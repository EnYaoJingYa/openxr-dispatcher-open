/* SPDX-License-Identifier: MIT
 *
 * xrEndFrame: the frame is rebuilt the way OVRPort's dispatcher rebuilds it,
 * because the games tested so far were tuned against that:
 *  - layer types the dispatcher knows (projection, quad, cube, cylinder,
 *    equirect, equirect2, FB and HTC passthrough) are copied; any other type is
 *    dropped ("unsupported: layer type N");
 *  - each layer's next chain keeps only colour scale/bias, image layout, alpha
 *    blend, secure content and layer settings (FB) structs;
 *  - each projection view's next chain keeps only XrCompositionLayerSpaceWarpInfoFB,
 *    and not even that with disable_space_warp, so depth info never reaches the
 *    runtime (as with OVRPort);
 *  - force_passthrough: the passthrough layer goes first and the other layers
 *    get BLEND_TEXTURE_SOURCE_ALPHA;
 *  - Pico: an infinite space-warp far Z becomes 10000.
 * Unlike OVRPort, chain order is kept and NULL layers are skipped. The copy
 * lives in a per-call arena; a frame that does not fit is submitted unchanged.
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dispatch.h"

typedef struct {
    char *buf;
    size_t used, size;
    bool full;
} arena;

static void *take(arena *a, size_t n)
{
    n = (n + 15) & ~(size_t)15;
    if (a->used + n > a->size) {
        a->full = true;
        return NULL;
    }
    void *p = a->buf + a->used;
    a->used += n;
    return p;
}

static void *copy(arena *a, const void *src, size_t n)
{
    void *p = take(a, n);
    if (p)
        memcpy(p, src, n);
    return p;
}

static size_t layer_chain_size(XrStructureType t)
{
    switch ((int)t) {
    case XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR: return sizeof(XrCompositionLayerColorScaleBiasKHR);
    case XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB: return sizeof(XrCompositionLayerImageLayoutFB);
    case XR_TYPE_COMPOSITION_LAYER_ALPHA_BLEND_FB: return sizeof(XrCompositionLayerAlphaBlendFB);
    case XR_TYPE_COMPOSITION_LAYER_SECURE_CONTENT_FB: return sizeof(XrCompositionLayerSecureContentFB);
    case XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB: return sizeof(XrCompositionLayerSettingsFB);
    default: return 0;
    }
}

/* Copy the structs of `next` that `size_of` accepts, in order; drop the rest. */
static const void *filter_chain(arena *a, const void *next, size_t (*size_of)(XrStructureType))
{
    const XrBaseInStructure *head = NULL;
    XrBaseInStructure *tail = NULL;
    for (const XrBaseInStructure *p = next; p; p = p->next) {
        size_t n = size_of(p->type);
        if (!n) {
            char key[48];
            snprintf(key, sizeof key, "chain:%d", (int)p->type);
            if (xd_first(key))
                xd_log(XD_INFO, "frame: chained struct %d removed (as OVRPort does)", (int)p->type);
            continue;
        }
        XrBaseInStructure *c = copy(a, p, n);
        if (!c)
            return NULL;
        c->next = NULL;
        if (tail)
            tail->next = c;
        else
            head = c;
        tail = c;
    }
    return head;
}

static size_t view_chain_size(XrStructureType t)
{
    if (t == XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB && !xd_cfg.disable_space_warp)
        return sizeof(XrCompositionLayerSpaceWarpInfoFB);
    return 0;
}

static size_t layer_size(XrStructureType t)
{
    switch ((int)t) {
    case XR_TYPE_COMPOSITION_LAYER_PROJECTION: return sizeof(XrCompositionLayerProjection);
    case XR_TYPE_COMPOSITION_LAYER_QUAD: return sizeof(XrCompositionLayerQuad);
    case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR: return sizeof(XrCompositionLayerCubeKHR);
    case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR: return sizeof(XrCompositionLayerCylinderKHR);
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR: return sizeof(XrCompositionLayerEquirectKHR);
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR: return sizeof(XrCompositionLayerEquirect2KHR);
    case XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB: return sizeof(XrCompositionLayerPassthroughFB);
    case XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_HTC: return sizeof(XrCompositionLayerPassthroughHTC);
    default: return 0;
    }
}

static bool layer_has_content(const XrCompositionLayerBaseHeader *layer)
{
    switch ((int)layer->type) {
    case XR_TYPE_COMPOSITION_LAYER_PROJECTION: {
        const XrCompositionLayerProjection *p = (const XrCompositionLayerProjection *)layer;
        if (!p->viewCount || !p->views)
            return false;
        for (uint32_t i = 0; i < p->viewCount; i++)
            if (p->views[i].subImage.swapchain == XR_NULL_HANDLE)
                return false;
        return true;
    }
    case XR_TYPE_COMPOSITION_LAYER_QUAD:
        return ((const XrCompositionLayerQuad *)layer)->subImage.swapchain != XR_NULL_HANDLE;
    case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:
        return ((const XrCompositionLayerCubeKHR *)layer)->swapchain != XR_NULL_HANDLE;
    case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
        return ((const XrCompositionLayerCylinderKHR *)layer)->subImage.swapchain != XR_NULL_HANDLE;
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
        return ((const XrCompositionLayerEquirectKHR *)layer)->subImage.swapchain != XR_NULL_HANDLE;
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:
        return ((const XrCompositionLayerEquirect2KHR *)layer)->subImage.swapchain != XR_NULL_HANDLE;
    case XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB:
    case XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_HTC:
        return true;
    default:
        return true;
    }
}

static const XrCompositionLayerBaseHeader *copy_layer(arena *a, const XrCompositionLayerBaseHeader *src)
{
    size_t n = layer_size(src->type);
    if (!n) {
        xd_unsupported("layer type %d dropped from frames", (int)src->type);
        return NULL;
    }
    if (!layer_has_content(src)) {
        char key[64];
        snprintf(key, sizeof key, "empty-layer:%d", (int)src->type);
        if (xd_first(key))
            xd_log(XD_WARN, "frame: dropped empty composition layer type %d (NULL swapchain/content)",
                   (int)src->type);
        return NULL;
    }
    XrCompositionLayerBaseHeader *l = copy(a, src, n);
    if (!l)
        return NULL;
    if (src->type == XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB) {
        l->next = NULL; /* as OVRPort: nothing chained to passthrough layers */
        return l;
    }
    l->next = filter_chain(a, src->next, layer_chain_size);
    if (xd_cfg.force_passthrough)
        l->layerFlags |= XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    if (src->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
        XrCompositionLayerProjection *p = (XrCompositionLayerProjection *)l;
        if (p->viewCount && p->views) {
            XrCompositionLayerProjectionView *v = copy(a, p->views, p->viewCount * sizeof *v);
            if (!v)
                return NULL;
            for (uint32_t i = 0; i < p->viewCount; i++) {
                v[i].next = filter_chain(a, v[i].next, view_chain_size);
                for (XrBaseInStructure *c = (XrBaseInStructure *)v[i].next; xd_runtime_bd && c; c = (XrBaseInStructure *)c->next) {
                    XrCompositionLayerSpaceWarpInfoFB *w = (XrCompositionLayerSpaceWarpInfoFB *)c;
                    if (c->type == XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB && (isinf(w->farZ) || w->farZ >= 3.4e38f))
                        w->farZ = 10000.0f;
                }
            }
            p->views = v;
        }
    }
    return l;
}

static pthread_mutex_t g_frame_lock = PTHREAD_MUTEX_INITIALIZER;
static arena g_arena;

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrEndFrame(XrSession session, const XrFrameEndInfo *info)
{
    PFN_xrEndFrame fn = NEXT(xrEndFrame);
    if (!fn)
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!info || (info->layerCount && !info->layers))
        return fn(session, info);
    pthread_mutex_lock(&g_frame_lock);
    if (!g_arena.buf) {
        g_arena.size = 64 << 10;
        g_arena.buf = malloc(g_arena.size);
    }
    g_arena.used = 0;
    g_arena.full = !g_arena.buf;
    uint32_t extra = xd_cfg.force_passthrough && xd_pt_layer ? 1 : 0;
    const XrCompositionLayerBaseHeader **layers = take(&g_arena, (info->layerCount + extra + 1) * sizeof *layers);
    uint32_t count = 0;
    if (layers && extra) {
        XrCompositionLayerPassthroughFB *pt = take(&g_arena, sizeof *pt);
        if (pt) {
            *pt = (XrCompositionLayerPassthroughFB){XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB, NULL, 0, XR_NULL_HANDLE,
                                                    xd_pt_layer};
            layers[count++] = (const XrCompositionLayerBaseHeader *)pt;
        }
    }
    for (uint32_t i = 0; layers && i < info->layerCount && !g_arena.full; i++) {
        if (!info->layers[i])
            continue;
        const XrCompositionLayerBaseHeader *l = copy_layer(&g_arena, info->layers[i]);
        if (l)
            layers[count++] = l;
    }
    XrResult r;
    if (g_arena.full) { /* cannot happen with sane frames; never drop the game's frame for it */
        if (xd_first("arena-full"))
            xd_log(XD_WARN, "frame too large to rewrite (%u layers), submitted unchanged", info->layerCount);
        r = fn(session, info);
    } else {
        XrFrameEndInfo fixed = *info;
        fixed.layers = layers;
        fixed.layerCount = count;
        r = fn(session, &fixed);
    }
    pthread_mutex_unlock(&g_frame_lock);
    if (XR_FAILED(r)) {
        char key[48];
        snprintf(key, sizeof key, "endframe:%d", r);
        if (xd_first(key))
            xd_log(XD_WARN, "xrEndFrame -> %s (%u layers)", xd_result_name(r), info->layerCount);
    } else if (xd_first("endframe-success")) {
        xd_log(XD_INFO, "first frame submitted successfully (%u of %u layers)", count, info->layerCount);
    }
    return r;
}
