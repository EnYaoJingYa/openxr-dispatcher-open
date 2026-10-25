/* SPDX-License-Identifier: MIT
 *
 * Sessions, spaces, views and swapchains.
 *
 *  - xrCreateSession: notes Vulkan sessions; applies cpulevel/gpulevel; with
 *    force_passthrough creates a running passthrough and a reconstruction layer
 *    (started at xrBeginSession, paused at xrEndSession, destroyed with the
 *    session), shown behind the game by xrEndFrame.
 *  - xrPerfSettingsSetPerformanceLevelEXT: cpulevel/gpulevel 1..4 replace the
 *    game's level (power savings .. boost); repeats are answered locally.
 *  - xrCreateReferenceSpace: LOCAL_FLOOR and Meta's private types
 *    1000043000..1000043004 (XR_OCULUS_common_reference_spaces) become native
 *    LOCAL_FLOOR (1000426000, 1000043001) or LOCAL (the others) when the
 *    runtime has XR_EXT_local_floor; otherwise LOCAL, lowered by the head
 *    height above the STAGE floor for the floor types (1.7 m if implausible).
 *  - xrLocateSpace: time 0 means "now".
 *  - xrEnumerateViewConfigurationViews: output structs initialised; fixed
 *    texture size from the settings.
 *  - xrCreateSwapchain: usage flags OVRPort strips are stripped (mutable format,
 *    transfer, storage, input attachment); a VkFormat in a GLES session is
 *    translated. Unlike OVRPort nothing aborts: oversized swapchains and
 *    unknown formats go to the runtime, which decides.
 */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dispatch.h"
#include "hooks.h"

bool xd_vulkan_session;
static XrSession g_session;
static XrTime g_last_predicted;

static int64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

/* ------------------------------------------------------------- perf levels */

static int g_last_level[2] = {0x7fffffff, 0x7fffffff}; /* CPU, GPU */

XRAPI_ATTR XrResult XRAPI_CALL xd_xrPerfSettingsSetPerformanceLevelEXT(XrSession s, XrPerfSettingsDomainEXT domain,
                                                                       XrPerfSettingsLevelEXT level)
{
    int configured = domain == XR_PERF_SETTINGS_DOMAIN_CPU_EXT ? xd_cfg.cpu_level
                     : domain == XR_PERF_SETTINGS_DOMAIN_GPU_EXT ? xd_cfg.gpu_level : 0;
    if (configured > 0)
        level = (XrPerfSettingsLevelEXT)(configured * 25 - 25); /* 1..4 -> 0, 25, 50, 75 */
    int slot = domain == XR_PERF_SETTINGS_DOMAIN_CPU_EXT ? 0 : domain == XR_PERF_SETTINGS_DOMAIN_GPU_EXT ? 1 : -1;
    if (slot >= 0) {
        if (g_last_level[slot] == (int)level)
            return XR_SUCCESS;
        g_last_level[slot] = (int)level;
    }
    PFN_xrPerfSettingsSetPerformanceLevelEXT fn = NEXT(xrPerfSettingsSetPerformanceLevelEXT);
    XrResult r = fn ? fn(s, domain, level) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (XR_FAILED(r))
        xd_unsupported("xrPerfSettingsSetPerformanceLevelEXT(domain %d, level %d) -> %s", domain, level,
                       xd_result_name(r));
    xd_trace("xrPerfSettingsSetPerformanceLevelEXT", r);
    return r;
}

/* ---------------------------------------------------------------- sessions */

XrPassthroughFB xd_pt;
XrPassthroughLayerFB xd_pt_layer;

static void force_passthrough_create(XrSession session)
{
    PFN_xrCreatePassthroughFB create = NEXT(xrCreatePassthroughFB);
    PFN_xrCreatePassthroughLayerFB layer = NEXT(xrCreatePassthroughLayerFB);
    if (!create || !layer) {
        xd_unsupported("force_passthrough: the runtime has no XR_FB_passthrough");
        return;
    }
    XrPassthroughCreateInfoFB pci = {XR_TYPE_PASSTHROUGH_CREATE_INFO_FB, NULL, XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB};
    XrResult r = create(session, &pci, &xd_pt);
    if (XR_SUCCEEDED(r)) {
        XrPassthroughLayerCreateInfoFB lci = {XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB, NULL, xd_pt,
                                              XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB,
                                              XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB};
        r = layer(session, &lci, &xd_pt_layer);
    }
    xd_log(XD_INFO, "force_passthrough: passthrough %p layer %p (%s)", (void *)(uintptr_t)xd_pt,
           (void *)(uintptr_t)xd_pt_layer, xd_result_name(r));
}

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrCreateSession(XrInstance instance, const XrSessionCreateInfo *info,
                                                        XrSession *session)
{
    for (const XrBaseInStructure *p = info ? info->next : NULL; p; p = p->next)
        if (p->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR)
            xd_vulkan_session = true;
    PFN_xrCreateSession fn = NEXT(xrCreateSession);
    XrResult r = fn ? fn(instance, info, session) : XR_ERROR_FUNCTION_UNSUPPORTED;
    xd_log(XD_INFO, "xrCreateSession (%s) -> %s", xd_vulkan_session ? "Vulkan" : "GLES", xd_result_name(r));
    if (XR_FAILED(r) || !session)
        return r;
    g_session = *session;
    g_last_level[0] = g_last_level[1] = 0x7fffffff;
    if (xd_cfg.cpu_level > 0)
        xd_xrPerfSettingsSetPerformanceLevelEXT(*session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, 0);
    if (xd_cfg.gpu_level > 0)
        xd_xrPerfSettingsSetPerformanceLevelEXT(*session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, 0);
    if (xd_cfg.force_passthrough)
        force_passthrough_create(*session);
    return r;
}

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrBeginSession(XrSession session, const XrSessionBeginInfo *info)
{
    PFN_xrBeginSession fn = NEXT(xrBeginSession);
    XrResult r = fn ? fn(session, info) : XR_ERROR_FUNCTION_UNSUPPORTED;
    PFN_xrPassthroughStartFB start = NEXT(xrPassthroughStartFB);
    if (xd_cfg.force_passthrough && xd_pt && start)
        start(xd_pt);
    xd_trace("xrBeginSession", r);
    return r;
}

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrEndSession(XrSession session)
{
    PFN_xrPassthroughPauseFB pause = NEXT(xrPassthroughPauseFB);
    if (xd_cfg.force_passthrough && xd_pt && pause)
        pause(xd_pt);
    PFN_xrEndSession fn = NEXT(xrEndSession);
    XrResult r = fn ? fn(session) : XR_ERROR_FUNCTION_UNSUPPORTED;
    xd_trace("xrEndSession", r);
    return r;
}

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrDestroySession(XrSession session)
{
    if (xd_pt) {
        PFN_xrDestroyPassthroughLayerFB dl = NEXT(xrDestroyPassthroughLayerFB);
        PFN_xrDestroyPassthroughFB dp = NEXT(xrDestroyPassthroughFB);
        if (xd_pt_layer && dl)
            dl(xd_pt_layer);
        if (dp)
            dp(xd_pt);
        xd_pt = XR_NULL_HANDLE;
        xd_pt_layer = XR_NULL_HANDLE;
    }
    xd_haptics_session_destroyed(session);
    PFN_xrDestroySession fn = NEXT(xrDestroySession);
    XrResult r = fn ? fn(session) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (session == g_session)
        g_session = XR_NULL_HANDLE;
    xd_trace("xrDestroySession", r);
    return r;
}

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrWaitFrame(XrSession session, const XrFrameWaitInfo *info, XrFrameState *state)
{
    PFN_xrWaitFrame fn = NEXT(xrWaitFrame);
    XrResult r = fn ? fn(session, info, state) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (XR_SUCCEEDED(r) && state)
        __atomic_store_n(&g_last_predicted, state->predictedDisplayTime, __ATOMIC_RELAXED);
    return r;
}

/* ------------------------------------------------------------------ spaces */

/* Head height above the STAGE floor, for floor-level spaces on runtimes without
 * XR_EXT_local_floor. OVRPort calls xrWaitFrame here to get a time; we use the
 * last predicted display time, or now. */
static float head_height(XrSession session)
{
    PFN_xrCreateReferenceSpace create = NEXT(xrCreateReferenceSpace);
    PFN_xrLocateSpace locate = NEXT(xrLocateSpace);
    PFN_xrDestroySpace destroy = NEXT(xrDestroySpace);
    float height = 0;
    XrSpace stage = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
    XrReferenceSpaceCreateInfo ci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO, NULL, XR_REFERENCE_SPACE_TYPE_STAGE,
                                     {{0, 0, 0, 1}, {0, 0, 0}}};
    if (create && locate && XR_SUCCEEDED(create(session, &ci, &stage))) {
        ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        if (XR_SUCCEEDED(create(session, &ci, &view))) {
            XrTime t = __atomic_load_n(&g_last_predicted, __ATOMIC_RELAXED);
            XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION, NULL, 0, {{0, 0, 0, 1}, {0, 0, 0}}};
            if (XR_SUCCEEDED(locate(view, stage, t ? t : monotonic_ns(), &loc)) &&
                (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
                height = loc.pose.position.y;
        }
    }
    if (destroy && view)
        destroy(view);
    if (destroy && stage)
        destroy(stage);
    if (height < 0.3f) {
        xd_log(XD_INFO, "floor emulation: head height %.2f m is implausible, using 1.70 m", height);
        height = 1.7f;
    }
    return height;
}

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo *info,
                                                               XrSpace *space)
{
    PFN_xrCreateReferenceSpace fn = NEXT(xrCreateReferenceSpace);
    if (!fn)
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!info)
        return fn(session, info, space);
    int type = (int)info->referenceSpaceType;
    bool meta = type >= XD_SPACE_META_FIRST && type < XD_SPACE_META_FIRST + 5;
    if (!meta && type != XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR)
        return fn(session, info, space);
    bool floor = type == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR || type == XD_SPACE_META_FLOOR;
    XrReferenceSpaceCreateInfo fixed = *info;
    fixed.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    float lowered = 0;
    if (floor && xd_runtime_local_floor)
        fixed.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR;
    else if (floor) {
        static float height; /* measured once, as OVRPort does */
        if (!height)
            height = head_height(session);
        lowered = height;
        fixed.poseInReferenceSpace.position.y -= height;
    }
    XrResult r = fn(session, &fixed, space);
    char key[48];
    snprintf(key, sizeof key, "space:%d", type);
    if (xd_first(key))
        xd_log(XD_INFO, "reference space %d created as %d%s (%s)", type, (int)fixed.referenceSpaceType,
               lowered ? ", lowered by the head height" : "", xd_result_name(r));
    return r;
}

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo *info,
                                                            XrSpace *space)
{
    PFN_xrCreateActionSpace fn = NEXT(xrCreateActionSpace);
    if (!fn)
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = fn(session, info, space);
    xd_trace("xrCreateActionSpace", r);
    return r;
}

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation *loc)
{
    PFN_xrLocateSpace fn = NEXT(xrLocateSpace);
    if (!fn)
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (time == 0)
        time = monotonic_ns(); /* as OVRPort: some Quest titles pass 0 for "now" */
    XrResult r = fn(space, base, time, loc);
    static XrSpace seen[16];
    static uint32_t seen_count;
    if (XR_SUCCEEDED(r) && loc) {
        bool fresh = true;
        for (uint32_t i = 0; i < seen_count; i++)
            fresh &= seen[i] != space;
        bool pose_words = (loc->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                          (loc->locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        if (fresh && pose_words && seen_count < sizeof seen / sizeof seen[0]) {
            seen[seen_count++] = space;
            xd_log(XD_INFO, "tracked space %p: position valid, orientation valid (flags 0x%llx)",
                   space, (unsigned long long)loc->locationFlags);
        }
    }
    return r;
}

/* ------------------------------------------------------------------- views */

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(XrInstance instance, XrSystemId system,
        XrViewConfigurationType type, uint32_t capacity, uint32_t *count, XrViewConfigurationView *views)
{
    for (uint32_t i = 0; views && i < capacity; i++) { /* some titles pass uninitialised structs */
        memset(&views[i], 0, sizeof views[i]);
        views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    }
    PFN_xrEnumerateViewConfigurationViews fn = NEXT(xrEnumerateViewConfigurationViews);
    XrResult r = fn ? fn(instance, system, type, capacity, count, views) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (XR_SUCCEEDED(r) && views && count && (xd_cfg.texture_width > 0 || xd_cfg.texture_height > 0)) {
        for (uint32_t i = 0; i < *count && i < capacity; i++) {
            if (xd_cfg.texture_width > 0)
                views[i].recommendedImageRectWidth = (uint32_t)xd_cfg.texture_width;
            if (xd_cfg.texture_height > 0)
                views[i].recommendedImageRectHeight = (uint32_t)xd_cfg.texture_height;
        }
        if (xd_first("fixed-texture"))
            xd_log(XD_INFO, "recommended eye-buffer size set to %ux%u (fixedtexturewidth/height)",
                   views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight);
    }
    xd_trace("xrEnumerateViewConfigurationViews", r);
    return r;
}

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xd_xrLocateViews(
    XrSession session, const XrViewLocateInfo *info, XrViewState *state, uint32_t capacity,
    uint32_t *count, XrView *views)
{
    PFN_xrLocateViews fn = NEXT(xrLocateViews);
    XrResult r = fn ? fn(session, info, state, capacity, count, views) : XR_ERROR_FUNCTION_UNSUPPORTED;
    XrSpaceLocationFlags required =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (XR_SUCCEEDED(r) && state && (state->viewStateFlags & required) == required &&
        xd_first("head-pose-valid"))
        xd_log(XD_INFO, "head pose valid: 6DoF position and orientation flags 0x%llx",
               (unsigned long long)state->viewStateFlags);
    return r;
}

/* -------------------------------------------------------------- swapchains */

#define USAGE_STRIPPED (XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT |        \
                        XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT |               \
                        XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR)

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xd_xrEnumerateSwapchainFormats(
    XrSession session, uint32_t capacity, uint32_t *count, int64_t *formats)
{
    PFN_xrEnumerateSwapchainFormats fn = NEXT(xrEnumerateSwapchainFormats);
    XrResult r = fn ? fn(session, capacity, count, formats) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (XR_SUCCEEDED(r) && formats && count && *count && xd_first("runtime-swapchain-formats")) {
        char line[800] = "";
        size_t used = 0;
        uint32_t shown = *count < capacity ? *count : capacity;
        for (uint32_t i = 0; i < shown; i++) {
            int written = snprintf(line + used, sizeof line - used, "%s0x%llx", i ? " " : "",
                                   (unsigned long long)formats[i]);
            if (written <= 0 || used + (size_t)written >= sizeof line)
                break;
            used += (size_t)written;
        }
        xd_log(XD_INFO, "runtime swapchain formats (%u): %s", *count, line);
    }
    return r;
}

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo *info,
                                                          XrSwapchain *swapchain)
{
    PFN_xrCreateSwapchain fn = NEXT(xrCreateSwapchain);
    if (!fn)
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!info)
        return fn(session, info, swapchain);
    XrSwapchainCreateInfo fixed = *info;
    fixed.usageFlags &= ~(XrSwapchainUsageFlags)USAGE_STRIPPED;
    if (!xd_vulkan_session && fixed.format == 0x93d0) {
        /* GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4 is accepted by some Quest
         * compositors as a render target. Monado/Spaces rejects it, so use
         * the uncompressed sRGB renderable format with the same colour
         * interpretation. */
        fixed.format = 0x8c43; /* GL_SRGB8_ALPHA8 */
        xd_log(XD_INFO, "swapchain format 0x93d0 -> 0x8c43 (VRX cannot render to ASTC)");
    }
    if (!xd_vulkan_session && xd_looks_like_vk_format(fixed.format)) {
        int64_t gl = xd_vk_to_gl(fixed.format);
        char key[48];
        snprintf(key, sizeof key, "vkfmt:%lld", (long long)fixed.format);
        if (gl) {
            if (xd_first(key))
                xd_log(XD_INFO, "swapchain format: VkFormat %lld in a GLES session -> GL 0x%llx",
                       (long long)fixed.format, (long long)gl);
            fixed.format = gl;
        } else if (xd_first(key))
            xd_unsupported("swapchain format: VkFormat %lld in a GLES session has no GL equivalent here, passed on",
                           (long long)fixed.format);
    }
    if (fixed.width > 4096 || fixed.height > 4096) {
        char key[48];
        snprintf(key, sizeof key, "big:%ux%u", fixed.width, fixed.height);
        if (xd_first(key))
            xd_log(XD_INFO, "swapchain %ux%u is larger than a Quest allows; passed to the runtime (OVRPort aborts here)",
                   fixed.width, fixed.height);
    }
    XrResult r = fn(session, &fixed, swapchain);
    xd_vlog("xrCreateSwapchain %ux%u format %lld samples %u usage 0x%llx -> %s", fixed.width, fixed.height,
            (long long)fixed.format, fixed.sampleCount, (unsigned long long)fixed.usageFlags, xd_result_name(r));
    return r;
}
