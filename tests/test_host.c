/* SPDX-License-Identifier: MIT
 * Host test for the open dispatcher: loads it with a fake next loader
 * (mock_next.c, forced through OVERPORT_FORCE_LOADER_PATH) and drives it the
 * way OVRPlugin does, checking what reaches the "runtime". */
#include <dlfcn.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openxr/openxr.h>

#include "mock_state.h"

int fake_log_count(const char *needle);

static void *lib;
static mock_state_t *mock;
static PFN_xrGetInstanceProcAddr gipa;
static int failures;

#define CHECK(c)                                                         \
    do {                                                                 \
        if (c)                                                           \
            printf("ok   %s\n", #c);                                     \
        else {                                                           \
            printf("FAIL %s (line %d)\n", #c, __LINE__);                 \
            failures++;                                                  \
        }                                                                \
    } while (0)

static void *sym(const char *n)
{
    void *p = dlsym(lib, n);
    if (!p) {
        fprintf(stderr, "missing export %s\n", n);
        exit(2);
    }
    return p;
}
#define FN(name) ((PFN_##name)sym(#name))

static PFN_xrVoidFunction lookup(XrInstance inst, const char *name, XrResult *r)
{
    PFN_xrVoidFunction fn = NULL;
    *r = gipa(inst, name, &fn);
    return fn;
}

static bool has_ext(const char *name)
{
    for (uint32_t i = 0; i < mock->ext_count; i++)
        if (!strcmp(mock->ext[i], name))
            return true;
    return false;
}

static XrPath path(XrInstance inst, const char *s)
{
    XrPath p = 0;
    FN(xrStringToPath)(inst, s, &p);
    return p;
}

int main(int argc, char **argv)
{
    lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    void *mock_lib = dlopen(argv[2], RTLD_NOW | RTLD_GLOBAL);
    if (!lib || !mock_lib) {
        fprintf(stderr, "%s\n", dlerror());
        return 2;
    }
    mock = dlsym(mock_lib, "mock_state");
    gipa = FN(xrGetInstanceProcAddr);
    bool (*is_vrx)(void) = (bool (*)(void))dlsym(lib, "xd_is_vrx");
    CHECK(is_vrx != NULL && is_vrx());
    mock->head_height = 1.6f;

    /* ---- loader init and extensions */
    struct { XrStructureType type; const void *next; void *vm, *ctx; } init = {
        XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR, NULL, (void *)0x11, (void *)0x22};
    CHECK(FN(xrInitializeLoaderKHR)((const XrLoaderInitInfoBaseHeaderKHR *)&init) == XR_SUCCESS);
    CHECK(mock->init_calls == 1);
    CHECK(fake_log_count("next loader") == 1);
    CHECK(fake_log_count("settings from") == 1);

    uint32_t n = 0;
    CHECK(FN(xrEnumerateInstanceExtensionProperties)(NULL, 0, &n, NULL) == XR_SUCCESS);
    CHECK(n == 66); /* 65 Quest extensions + XR_VALVE_frame_controller_interaction */
    XrExtensionProperties *props = calloc(n, sizeof *props);
    CHECK(FN(xrEnumerateInstanceExtensionProperties)(NULL, n, &n, props) == XR_SUCCESS);
    CHECK(!strcmp(props[0].extensionName, "XR_FB_common_events"));
    CHECK(!strcmp(props[64].extensionName, "XR_META_body_tracking_fidelity"));
    CHECK(!strcmp(props[65].extensionName, "XR_VALVE_frame_controller_interaction"));
    CHECK(FN(xrEnumerateInstanceExtensionProperties)(NULL, 3, &n, props) == XR_ERROR_SIZE_INSUFFICIENT);
    free(props);

    /* ---- instance: dropped/added extensions, Android info, 1.1 -> 1.0 retry */
    const char *want[] = {"XR_FB_body_tracking", "XR_KHR_opengl_es_enable", "XR_FB_passthrough"};
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "test");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 1, 40);
    ci.enabledExtensionCount = 3;
    ci.enabledExtensionNames = want;
    XrInstance inst = XR_NULL_HANDLE;
    CHECK(FN(xrCreateInstance)(&ci, &inst) == XR_SUCCESS);
    CHECK(inst == (XrInstance)(uintptr_t)0x1234);
    CHECK(mock->create_calls == 2 && XR_VERSION_MINOR(mock->api_version) == 0);
    CHECK(has_ext("XR_KHR_opengl_es_enable") && has_ext("XR_FB_passthrough") && has_ext("XR_EXT_local_floor"));
    CHECK(!has_ext("XR_FB_body_tracking"));
    CHECK(mock->android_chained == 1);
    CHECK(fake_log_count("not offered by the runtime, not enabled: XR_FB_body_tracking") == 1);

    /* ---- xrGetInstanceProcAddr */
    XrResult r;
    PFN_xrVoidFunction f = lookup(inst, "xrFancyMockFunctionMOCK", &r);
    CHECK(r == XR_SUCCESS && f && ((XrResult(*)(void))f)() == 42); /* unknown names are forwarded */
    f = lookup(inst, "xrNoSuchFunctionMOCK", &r);
    CHECK(r == XR_ERROR_FUNCTION_UNSUPPORTED && !f);
    CHECK(fake_log_count("unsupported: function xrNoSuchFunctionMOCK not provided by the runtime") == 1);
    f = lookup(inst, "xrCreateBodyTrackerFB", &r); /* table name, runtime lacks it: lookup ok, call fails */
    CHECK(r == XR_SUCCESS && f && ((XrResult(*)(void))f)() == XR_ERROR_FUNCTION_UNSUPPORTED);
    f = lookup(inst, "xrGetInstanceProcAddr", &r);
    CHECK(r == XR_SUCCESS && f == (PFN_xrVoidFunction)gipa);
    f = lookup(inst, "xrStringToPath", &r); /* plain table name the runtime has: the runtime's own */
    CHECK(r == XR_SUCCESS && f && f != sym("xrStringToPath"));
    f = lookup(XR_NULL_HANDLE, "xrBeginFrame", &r);
    CHECK(XR_FAILED(r) && !f);

    /* ---- identity */
    XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
    CHECK(FN(xrGetInstanceProperties)(inst, &ip) == XR_SUCCESS);
    CHECK(!strcmp(ip.runtimeName, "Oculus") && ip.runtimeVersion == XR_MAKE_VERSION(67, 522, 0));
    XrSystemHeadsetIdPropertiesMETA hid = {XR_TYPE_SYSTEM_HEADSET_ID_PROPERTIES_META};
    memset(hid.id.data, 0xff, sizeof hid.id.data); /* must be overwritten with zeros */
    XrSystemVirtualKeyboardPropertiesMETA kb = {XR_TYPE_SYSTEM_VIRTUAL_KEYBOARD_PROPERTIES_META, &hid};
    XrSystemHandTrackingPropertiesEXT ht = {XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT, &kb};
    XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES, &ht};
    CHECK(FN(xrGetSystemProperties)(inst, 1, &sp) == XR_SUCCESS);
    CHECK(!strcmp(sp.systemName, "Oculus Quest2") && sp.vendorId == 0x2833);
    CHECK(ht.supportsHandTracking && kb.supportsVirtualKeyboard && hid.id.data[0] == 0 && hid.id.data[15] == 0);
    CHECK(mock->system_props_next == &ht); /* not a Pico runtime: the chain reaches it */

    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    CHECK(FN(xrPollEvent)(inst, &ev) == XR_SUCCESS && ev.type == (XrStructureType)1000075000 &&
          ((uint32_t *)&ev)[4] == 1);
    CHECK(FN(xrPollEvent)(inst, &ev) == XR_EVENT_UNAVAILABLE); /* runtime error */
    CHECK(FN(xrPollEvent)(inst, &ev) == XR_EVENT_UNAVAILABLE);

    PFN_xrGetRecommendedLayerResolutionMETA lres = (PFN_xrGetRecommendedLayerResolutionMETA)lookup(
        inst, "xrGetRecommendedLayerResolutionMETA", &r);
    XrRecommendedLayerResolutionMETA res = {XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_META};
    CHECK(lres && lres(0, NULL, &res) == XR_SUCCESS && res.recommendedImageDimensions.width == 1504 && res.isValid);
    struct { XrStructureType t; void *n; XrVersion mn, mx; } gles = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    f = lookup(inst, "xrGetOpenGLESGraphicsRequirementsKHR", &r);
    CHECK(f && ((XrResult(*)(XrInstance, XrSystemId, void *))f)(inst, 1, &gles) == XR_SUCCESS &&
          gles.mx == XR_MAKE_VERSION(3, 2, 0));

    /* ---- session, perf levels (cpulevel 3 from persist, overriding debug's 2) */
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
    XrSession session;
    CHECK(FN(xrCreateSession)(inst, &sci, &session) == XR_SUCCESS);
    CHECK(mock->perf_calls == 1 && mock->perf_domain == XR_PERF_SETTINGS_DOMAIN_CPU_EXT && mock->perf_level == 50);
    PFN_xrPerfSettingsSetPerformanceLevelEXT perf = (PFN_xrPerfSettingsSetPerformanceLevelEXT)lookup(
        inst, "xrPerfSettingsSetPerformanceLevelEXT", &r);
    CHECK(perf(session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT) == XR_SUCCESS);
    CHECK(mock->perf_calls == 1); /* overridden to 50, same as before: answered locally */
    CHECK(perf(session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT) == XR_SUCCESS);
    CHECK(mock->perf_calls == 2 && mock->perf_level == 25);

    /* ---- reference spaces (runtime has local floor) */
    XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO, NULL, XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR,
                                     {{0, 0, 0, 1}, {0, 0, 0}}};
    XrSpace space;
    CHECK(FN(xrCreateReferenceSpace)(session, &rs, &space) == XR_SUCCESS &&
          mock->space_type == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR);
    rs.referenceSpaceType = (XrReferenceSpaceType)1000043000;
    CHECK(FN(xrCreateReferenceSpace)(session, &rs, &space) == XR_SUCCESS &&
          mock->space_type == XR_REFERENCE_SPACE_TYPE_LOCAL);
    rs.referenceSpaceType = (XrReferenceSpaceType)1000043001;
    CHECK(FN(xrCreateReferenceSpace)(session, &rs, &space) == XR_SUCCESS &&
          mock->space_type == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR);
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    CHECK(FN(xrCreateReferenceSpace)(session, &rs, &space) == XR_SUCCESS &&
          mock->space_type == XR_REFERENCE_SPACE_TYPE_STAGE);
    XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
    CHECK(FN(xrLocateSpace)(space, space, 0, &loc) == XR_SUCCESS && mock->locate_time > 0);

    /* ---- views: uninitialised structs are fixed up, fixedtexturewidth=1440 applied */
    XrViewConfigurationView views[2];
    memset(views, 0xab, sizeof views);
    CHECK(FN(xrEnumerateViewConfigurationViews)(inst, 1, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, views) ==
          XR_SUCCESS);
    CHECK(views[0].recommendedImageRectWidth == 1440 && views[1].recommendedImageRectHeight == 2000);

    /* ---- swapchains: VkFormat in GLES translated, usage stripped, no abort on 8192 px */
    XrSwapchainCreateInfo sc = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
    sc.format = 43; /* VK_FORMAT_R8G8B8A8_SRGB */
    sc.width = 8192;
    sc.height = 4096;
    sc.sampleCount = sc.faceCount = sc.arraySize = sc.mipCount = 1;
    XrSwapchain swapchain;
    CHECK(FN(xrCreateSwapchain)(session, &sc, &swapchain) == XR_SUCCESS);
    CHECK(mock->swapchain.format == 0x8c43 && mock->swapchain.width == 8192);
    CHECK(mock->swapchain.usageFlags == (XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT));
    sc.format = 50; /* B8G8R8A8_SRGB: no GLES equivalent, passed on */
    CHECK(FN(xrCreateSwapchain)(session, &sc, &swapchain) == XR_SUCCESS && mock->swapchain.format == 50);

    /* ---- frames: chains filtered, unknown layer types dropped, NULL skipped */
    XrCompositionLayerDepthInfoKHR depth = {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
    XrCompositionLayerSpaceWarpInfoFB warp = {XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB, &depth};
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, &warp},
                                              {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, &depth}};
    XrBaseInStructure odd = {(XrStructureType)1000999000, NULL};
    XrCompositionLayerAlphaBlendFB blend = {XR_TYPE_COMPOSITION_LAYER_ALPHA_BLEND_FB, &odd};
    XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION, &blend, 0, space, 2, pv};
    XrCompositionLayerBaseHeader weird = {(XrStructureType)1000555000};
    const XrCompositionLayerBaseHeader *layers[] = {(const XrCompositionLayerBaseHeader *)&proj, NULL, &weird};
    XrFrameEndInfo fe = {XR_TYPE_FRAME_END_INFO, NULL, 1, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 3, layers};
    CHECK(FN(xrEndFrame)(session, &fe) == XR_SUCCESS);
    CHECK(mock->frame_layers == 1 && mock->frame_types[0] == XR_TYPE_COMPOSITION_LAYER_PROJECTION);
    CHECK(mock->frame_view_chain == XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB && mock->frame_view_chain_len == 1);
    CHECK(mock->frame_layer_chain == XR_TYPE_COMPOSITION_LAYER_ALPHA_BLEND_FB && mock->frame_layer_chain_len == 1);
    CHECK(fake_log_count("unsupported: layer type 1000555000") == 1);
    CHECK(proj.views == pv && pv[0].next == &warp && warp.next == &depth); /* the game's structs are untouched */

    /* ---- bindings */
    XrAction act = (XrAction)(uintptr_t)0xac7;
    const char *touch_in[] = {"/user/hand/right/input/x/click", "/user/hand/left/input/thumbstick",
                              "/user/hand/right/input/dpad_up", "/user/hand/left/input/menu/touch",
                              "/user/hand/right/input/menu/click"};
    XrActionSuggestedBinding sb[5];
    for (int i = 0; i < 5; i++)
        sb[i] = (XrActionSuggestedBinding){act, path(inst, touch_in[i])};
    XrInteractionProfileSuggestedBinding sug = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, NULL,
                                                path(inst, "/interaction_profiles/oculus/touch_controller"), 5, sb};
    int calls = mock->suggest_calls;
    CHECK(FN(xrSuggestInteractionProfileBindings)(inst, &sug) == XR_SUCCESS);
    CHECK(mock->suggest_calls == calls + 4); /* 3 Pico aliases (rejected) + Touch */
    CHECK(mock->touch_bindings == 3);
    CHECK(!strcmp(mock->touch_paths[0], "/user/hand/left/input/x/click"));
    CHECK(!strcmp(mock->touch_paths[2], "/user/hand/left/input/menu/click"));
    sug.interactionProfile = path(inst, "/interaction_profiles/valve/frame_controller_valve");
    CHECK(FN(xrSuggestInteractionProfileBindings)(inst, &sug) == XR_SUCCESS);
    mock->current_profile = path(inst, "/interaction_profiles/oculus/touch_controller");
    XrInteractionProfileState ps = {XR_TYPE_INTERACTION_PROFILE_STATE};
    CHECK(FN(xrGetCurrentInteractionProfile)(session, path(inst, "/user/hand/left"), &ps) == XR_SUCCESS);
    CHECK(ps.interactionProfile == sug.interactionProfile); /* the alias maps back to the game's profile */

    /* ---- haptics: multiplier 0.5 from the config file; PCM and envelope emulated */
    XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO, NULL, act, 0};
    XrHapticVibration vib = {XR_TYPE_HAPTIC_VIBRATION, NULL, 10000000, 160.0f, 0.8f};
    CHECK(FN(xrApplyHapticFeedback)(session, &hi, (const XrHapticBaseHeader *)&vib) == XR_SUCCESS);
    CHECK(fabsf(mock->last_vibration.amplitude - 0.4f) < 1e-6f && mock->last_vibration.frequency == 160.0f);
    float pcm_buf[1000];
    for (int i = 0; i < 1000; i++)
        pcm_buf[i] = -0.5f; /* abs() makes it 0.5 */
    uint32_t consumed = 0;
    XrHapticPcmVibrationFB pcm = {XR_TYPE_HAPTIC_PCM_VIBRATION_FB, NULL, 1000, pcm_buf, 1000.0f, XR_FALSE, &consumed};
    int before = mock->haptic_calls;
    CHECK(FN(xrApplyHapticFeedback)(session, &hi, (const XrHapticBaseHeader *)&pcm) == XR_SUCCESS && consumed == 1000);
    struct timespec ts = {0, 150000000};
    nanosleep(&ts, NULL);
    CHECK(mock->haptic_calls > before + 5 && mock->last_vibration.duration == 10000000 &&
          fabsf(mock->last_vibration.amplitude - 0.25f) < 1e-3f);
    float env_amp[3] = {0.2f, 1.0f, 0.2f};
    XrHapticAmplitudeEnvelopeVibrationFB env = {XR_TYPE_HAPTIC_AMPLITUDE_ENVELOPE_VIBRATION_FB, NULL, 500000000, 3,
                                                env_amp};
    CHECK(FN(xrApplyHapticFeedback)(session, &hi, (const XrHapticBaseHeader *)&env) == XR_SUCCESS);
    CHECK(FN(xrStopHapticFeedback)(session, &hi) == XR_ERROR_FUNCTION_UNSUPPORTED); /* mock has no stop */
    before = mock->haptic_calls;
    nanosleep(&ts, NULL);
    CHECK(mock->haptic_calls == before); /* stopped */

    CHECK(FN(xrDestroyAction)(act) == XR_ERROR_FUNCTION_UNSUPPORTED); /* mock has none; bookkeeping still runs */
    CHECK(FN(xrDestroyInstance)(inst) == XR_SUCCESS);
    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
