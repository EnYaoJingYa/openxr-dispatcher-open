/* SPDX-License-Identifier: MIT
 * A fake next loader / runtime for the host test: answers a small set of
 * OpenXR functions and records what the dispatcher passed on (mock_state). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openxr/openxr.h>

#include "mock_state.h"

__attribute__((visibility("default"))) mock_state_t mock_state;

static const char *const k_ext[] = {"XR_KHR_android_create_instance", "XR_EXT_local_floor", "XR_FB_passthrough",
                                    "XR_KHR_opengl_es_enable", "XR_EXT_hand_tracking", "XR_FB_display_refresh_rate",
                                    "XR_VALVE_frame_controller_interaction"};
#define EXT_COUNT (sizeof k_ext / sizeof k_ext[0])

static char g_paths[256][XR_MAX_PATH_LENGTH];
static int g_path_count;

static XrResult XRAPI_CALL m_init(const XrLoaderInitInfoBaseHeaderKHR *i) { (void)i; mock_state.init_calls++; return XR_SUCCESS; }

static XrResult XRAPI_CALL m_enum_ext(const char *layer, uint32_t cap, uint32_t *n, XrExtensionProperties *p)
{
    (void)layer;
    *n = EXT_COUNT;
    if (!cap)
        return XR_SUCCESS;
    if (cap < EXT_COUNT)
        return XR_ERROR_SIZE_INSUFFICIENT;
    for (size_t i = 0; i < EXT_COUNT; i++) {
        snprintf(p[i].extensionName, sizeof p[i].extensionName, "%s", k_ext[i]);
        p[i].extensionVersion = 1;
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_create_instance(const XrInstanceCreateInfo *info, XrInstance *out)
{
    mock_state.create_calls++;
    mock_state.api_version = info->applicationInfo.apiVersion;
    if (XR_VERSION_MINOR(info->applicationInfo.apiVersion) == 1)
        return XR_ERROR_API_VERSION_UNSUPPORTED;
    mock_state.ext_count = info->enabledExtensionCount;
    for (uint32_t i = 0; i < info->enabledExtensionCount && i < 32; i++)
        snprintf(mock_state.ext[i], sizeof mock_state.ext[i], "%s", info->enabledExtensionNames[i]);
    mock_state.android_chained = 0;
    for (const XrBaseInStructure *p = info->next; p; p = p->next)
        if (p->type == XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR)
            mock_state.android_chained = 1;
    *out = (XrInstance)(uintptr_t)0x1234;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_destroy_instance(XrInstance i) { (void)i; return XR_SUCCESS; }

static XrResult XRAPI_CALL m_instance_props(XrInstance i, XrInstanceProperties *p)
{
    (void)i;
    p->runtimeVersion = XR_MAKE_VERSION(2, 18, 0);
    snprintf(p->runtimeName, sizeof p->runtimeName, "SteamVR/OpenXR");
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_system_props(XrInstance i, XrSystemId s, XrSystemProperties *p)
{
    (void)i;
    (void)s;
    mock_state.system_props_next = p->next;
    snprintf(p->systemName, sizeof p->systemName, "Steam Frame");
    p->vendorId = 0x28de;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_poll(XrInstance i, XrEventDataBuffer *e)
{
    (void)i;
    (void)e;
    return mock_state.poll_calls++ == 0 ? XR_ERROR_RUNTIME_FAILURE : XR_EVENT_UNAVAILABLE;
}

static XrResult XRAPI_CALL m_create_space(XrSession s, const XrReferenceSpaceCreateInfo *i, XrSpace *out)
{
    (void)s;
    mock_state.space_type = i->referenceSpaceType;
    mock_state.space_y = i->poseInReferenceSpace.position.y;
    *out = (XrSpace)(uintptr_t)(0x100 + i->referenceSpaceType);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_destroy_space(XrSpace s) { (void)s; return XR_SUCCESS; }

static XrResult XRAPI_CALL m_locate(XrSpace s, XrSpace b, XrTime t, XrSpaceLocation *l)
{
    (void)s;
    (void)b;
    mock_state.locate_time = t;
    l->locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    l->pose = (XrPosef){{0, 0, 0, 1}, {0, mock_state.head_height, 0}};
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_views(XrInstance i, XrSystemId s, XrViewConfigurationType t, uint32_t cap, uint32_t *n,
                                   XrViewConfigurationView *v)
{
    (void)i;
    (void)s;
    (void)t;
    *n = 2;
    for (uint32_t k = 0; k < cap && k < 2; k++) {
        if (v[k].type != XR_TYPE_VIEW_CONFIGURATION_VIEW)
            return XR_ERROR_VALIDATION_FAILURE;
        v[k].recommendedImageRectWidth = v[k].recommendedImageRectHeight = 2000;
        v[k].maxImageRectWidth = v[k].maxImageRectHeight = 4096;
        v[k].recommendedSwapchainSampleCount = v[k].maxSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_create_swapchain(XrSession s, const XrSwapchainCreateInfo *i, XrSwapchain *out)
{
    (void)s;
    mock_state.swapchain = *i;
    *out = (XrSwapchain)(uintptr_t)0x777;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_create_session(XrInstance i, const XrSessionCreateInfo *c, XrSession *s)
{
    (void)i;
    (void)c;
    *s = (XrSession)(uintptr_t)0x5e55;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_end_frame(XrSession s, const XrFrameEndInfo *info)
{
    (void)s;
    mock_state.frame_layers = info->layerCount;
    mock_state.frame_view_chain = -1;
    mock_state.frame_layer_chain = -1;
    for (uint32_t i = 0; i < info->layerCount && i < 8; i++) {
        const XrCompositionLayerBaseHeader *l = info->layers[i];
        mock_state.frame_types[i] = l->type;
        mock_state.frame_flags[i] = l->layerFlags;
        if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const XrCompositionLayerProjection *p = (const XrCompositionLayerProjection *)l;
            const XrBaseInStructure *n = p->views[0].next;
            mock_state.frame_view_chain = n ? (int)n->type : 0;
            mock_state.frame_view_chain_len = 0;
            for (; n; n = n->next)
                mock_state.frame_view_chain_len++;
            const XrBaseInStructure *ln = l->next;
            mock_state.frame_layer_chain = ln ? (int)ln->type : 0;
            mock_state.frame_layer_chain_len = 0;
            for (; ln; ln = ln->next)
                mock_state.frame_layer_chain_len++;
        }
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_string_to_path(XrInstance i, const char *s, XrPath *p)
{
    (void)i;
    for (int k = 0; k < g_path_count; k++)
        if (!strcmp(g_paths[k], s)) {
            *p = (XrPath)(k + 1);
            return XR_SUCCESS;
        }
    snprintf(g_paths[g_path_count], XR_MAX_PATH_LENGTH, "%s", s);
    *p = (XrPath)(++g_path_count);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_path_to_string(XrInstance i, XrPath p, uint32_t cap, uint32_t *n, char *buf)
{
    (void)i;
    if (!p || (int)p > g_path_count)
        return XR_ERROR_PATH_INVALID;
    *n = (uint32_t)strlen(g_paths[p - 1]) + 1;
    if (cap)
        snprintf(buf, cap, "%s", g_paths[p - 1]);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_suggest(XrInstance i, const XrInteractionProfileSuggestedBinding *s)
{
    char name[XR_MAX_PATH_LENGTH];
    uint32_t n;
    m_path_to_string(i, s->interactionProfile, sizeof name, &n, name);
    mock_state.suggest_calls++;
    if (strstr(name, "bytedance"))
        return XR_ERROR_PATH_UNSUPPORTED;
    if (!strcmp(name, "/interaction_profiles/oculus/touch_controller")) {
        mock_state.touch_bindings = s->countSuggestedBindings;
        for (uint32_t k = 0; k < s->countSuggestedBindings && k < 16; k++)
            m_path_to_string(i, s->suggestedBindings[k].binding, sizeof mock_state.touch_paths[k], &n,
                             mock_state.touch_paths[k]);
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_current_profile(XrSession s, XrPath u, XrInteractionProfileState *st)
{
    (void)s;
    (void)u;
    st->interactionProfile = mock_state.current_profile;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_haptic(XrSession s, const XrHapticActionInfo *i, const XrHapticBaseHeader *h)
{
    (void)s;
    (void)i;
    mock_state.haptic_calls++;
    if (h->type == XR_TYPE_HAPTIC_VIBRATION)
        mock_state.last_vibration = *(const XrHapticVibration *)h;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_perf(XrSession s, XrPerfSettingsDomainEXT d, XrPerfSettingsLevelEXT l)
{
    (void)s;
    mock_state.perf_calls++;
    mock_state.perf_domain = d;
    mock_state.perf_level = l;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_layer_res(XrSession s, const void *i, XrRecommendedLayerResolutionMETA *r)
{
    (void)s;
    (void)i;
    r->recommendedImageDimensions.width = 3000;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL m_fancy(void) { return (XrResult)42; }

static XrResult XRAPI_CALL m_gipa(XrInstance instance, const char *name, PFN_xrVoidFunction *fn);

static const struct {
    const char *name;
    void *fn;
    int global;
} k_fns[] = {
    {"xrGetInstanceProcAddr", (void *)m_gipa, 1},
    {"xrInitializeLoaderKHR", (void *)m_init, 1},
    {"xrEnumerateInstanceExtensionProperties", (void *)m_enum_ext, 1},
    {"xrCreateInstance", (void *)m_create_instance, 1},
    {"xrDestroyInstance", (void *)m_destroy_instance, 0},
    {"xrGetInstanceProperties", (void *)m_instance_props, 0},
    {"xrGetSystemProperties", (void *)m_system_props, 0},
    {"xrPollEvent", (void *)m_poll, 0},
    {"xrCreateReferenceSpace", (void *)m_create_space, 0},
    {"xrDestroySpace", (void *)m_destroy_space, 0},
    {"xrLocateSpace", (void *)m_locate, 0},
    {"xrEnumerateViewConfigurationViews", (void *)m_views, 0},
    {"xrCreateSwapchain", (void *)m_create_swapchain, 0},
    {"xrCreateSession", (void *)m_create_session, 0},
    {"xrEndFrame", (void *)m_end_frame, 0},
    {"xrStringToPath", (void *)m_string_to_path, 0},
    {"xrPathToString", (void *)m_path_to_string, 0},
    {"xrSuggestInteractionProfileBindings", (void *)m_suggest, 0},
    {"xrGetCurrentInteractionProfile", (void *)m_current_profile, 0},
    {"xrApplyHapticFeedback", (void *)m_haptic, 0},
    {"xrPerfSettingsSetPerformanceLevelEXT", (void *)m_perf, 0},
    {"xrGetRecommendedLayerResolutionMETA", (void *)m_layer_res, 0},
    {"xrFancyMockFunctionMOCK", (void *)m_fancy, 0},
};

static XrResult XRAPI_CALL m_gipa(XrInstance instance, const char *name, PFN_xrVoidFunction *fn)
{
    mock_state.gipa_calls++;
    *fn = NULL;
    for (size_t i = 0; i < sizeof k_fns / sizeof k_fns[0]; i++)
        if (!strcmp(k_fns[i].name, name)) {
            if (!instance && !k_fns[i].global)
                return XR_ERROR_HANDLE_INVALID;
            *fn = (PFN_xrVoidFunction)k_fns[i].fn;
            return XR_SUCCESS;
        }
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

__attribute__((visibility("default"))) XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char *name,
                                                                                 PFN_xrVoidFunction *fn)
{
    return m_gipa(instance, name, fn);
}
