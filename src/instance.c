/* SPDX-License-Identifier: MIT
 *
 * Loader init, instance creation and the Quest identity.
 *
 *  - xrEnumerateInstanceExtensionProperties: the 65 extensions OVRPort's
 *    dispatcher advertises (Quest titles look for them) plus everything the
 *    next loader has.
 *  - xrCreateInstance: requested extensions the next loader lacks are dropped
 *    (logged as "unsupported: extension ..."), the game still gets XR_SUCCESS;
 *    XR_BD_controller_interaction and XR_EXT_local_floor are added when the
 *    runtime has them, XR_FB_passthrough with force_passthrough; an
 *    XrInstanceCreateInfoAndroidKHR is chained if the game has none; a 1.1 app
 *    on a 1.0 runtime is retried as 1.0.
 *  - xrGetInstanceProperties / xrGetSystemProperties: the runtime is "Oculus"
 *    67.522.0 and the system an "Oculus Quest2" (vendor 0x2833); feature
 *    structs say hand and body tracking, passthrough and the virtual keyboard
 *    are supported, as on a Quest. Calls to functions the runtime lacks fail
 *    later with XR_ERROR_FUNCTION_UNSUPPORTED instead.
 *  - xrPollEvent: one "headset mounted" event first; runtime errors become
 *    XR_EVENT_UNAVAILABLE.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dispatch.h"
#include "hooks.h"

bool xd_runtime_local_floor, xd_runtime_bd;
void *xd_java_vm, *xd_activity;

/* -------------------------------------------------------------- extensions */

static const struct {
    const char *name;
    uint32_t version;
} k_quest_extensions[] = {
    {"XR_FB_common_events", 2}, {"XR_EXT_user_presence", 1}, {"XR_KHR_android_create_instance", 3},
    {"XR_KHR_android_surface_swapchain", 4}, {"XR_KHR_android_thread_settings", 6},
    {"XR_FB_android_surface_swapchain_create", 1}, {"XR_KHR_opengl_es_enable", 8}, {"XR_KHR_vulkan_enable", 8},
    {"XR_KHR_vulkan_enable2", 2}, {"XR_KHR_composition_layer_cube", 8}, {"XR_KHR_composition_layer_cylinder", 4},
    {"XR_KHR_composition_layer_equirect2", 1}, {"XR_KHR_composition_layer_color_scale_bias", 5},
    {"XR_KHR_composition_layer_depth", 6}, {"XR_FB_color_space", 3}, {"XR_EXT_performance_settings", 4},
    {"XR_EXT_hand_tracking", 4}, {"XR_EXT_debug_utils", 5}, {"XR_FB_display_refresh_rate", 1},
    {"XR_FB_swapchain_update_state", 3}, {"XR_FB_swapchain_update_state_opengl_es", 1},
    {"XR_FB_swapchain_update_state_vulkan", 1}, {"XR_FB_swapchain_update_state_android_surface", 1},
    {"XR_OCULUS_common_reference_spaces", 1}, {"XR_FB_composition_layer_image_layout", 1},
    {"XR_FB_composition_layer_alpha_blend", 2}, {"XR_FB_hand_tracking_capsules", 3}, {"XR_FB_foveation", 1},
    {"XR_FB_foveation_configuration", 1}, {"XR_FB_foveation_vulkan", 1}, {"XR_FB_composition_layer_secure_content", 1},
    {"XR_KHR_convert_timespec_time", 1}, {"XR_FB_triangle_mesh", 2}, {"XR_FB_passthrough", 3},
    {"XR_META_passthrough_color_lut", 1}, {"XR_META_passthrough_layer_resumed_event", 1},
    {"XR_META_passthrough_preferences", 1}, {"XR_FB_touch_controller_proximity", 1}, {"XR_FB_touch_controller_pro", 1},
    {"XR_META_touch_controller_plus", 1}, {"XR_FB_haptic_amplitude_envelope", 1}, {"XR_FB_haptic_pcm", 1},
    {"XR_FB_spatial_entity_user", 1}, {"XR_FB_space_warp", 2}, {"XR_META_vulkan_swapchain_create_info", 1},
    {"XR_FB_passthrough_keyboard_hands", 2}, {"XR_FB_composition_layer_settings", 1}, {"XR_META_feature_fidelity", 1},
    {"XR_FB_composition_layer_depth_test", 1}, {"XR_EXT_hand_joints_motion_range", 1},
    {"XR_EXT_hand_tracking_data_source", 1}, {"XR_META_performance_metrics", 2}, {"XR_META_virtual_keyboard", 1},
    {"XR_MSFT_hand_interaction", 1}, {"XR_META_headset_id", 2}, {"XR_META_recommended_layer_resolution", 1},
    {"XR_EXT_active_action_set_priority", 1}, {"XR_META_automatic_layer_filter", 1},
    {"XR_META_simultaneous_hands_and_controllers", 1}, {"XR_EXT_local_floor", 1}, {"XR_FB_hand_tracking_aim", 2},
    {"XR_META_body_tracking_full_body", 1}, {"XR_FB_body_tracking", 1}, {"XR_META_body_tracking_calibration", 1},
    {"XR_META_body_tracking_fidelity", 1},
};
#define QUEST_EXT_COUNT (sizeof k_quest_extensions / sizeof k_quest_extensions[0])

/* "<prefix>: a b c", wrapped into lines that fit logcat */
static void log_list(const char *prefix, const char *const *items, uint32_t n)
{
    char line[900];
    size_t used = 0;
    for (uint32_t i = 0; i < n; i++) {
        size_t len = strlen(items[i]);
        if (used && used + len + 2 >= sizeof line) {
            xd_log(XD_INFO, "%s: %s", prefix, line);
            used = 0;
        }
        used += (size_t)snprintf(line + used, sizeof line - used, "%s%s", used ? " " : "", items[i]);
    }
    if (used)
        xd_log(XD_INFO, "%s: %s", prefix, line);
}

static void log_runtime_extensions(void)
{
    const XrExtensionProperties *ext;
    uint32_t n = xd_runtime_extensions(&ext);
    if (!ext || !xd_first("runtime-extensions"))
        return;
    const char **names = calloc(n ? n : 1, sizeof *names);
    for (uint32_t i = 0; names && i < n; i++)
        names[i] = ext[i].extensionName;
    if (names)
        log_list("runtime extensions", names, n);
    free(names);
    xd_runtime_local_floor = xd_runtime_has_extension("XR_EXT_local_floor");
    xd_runtime_bd = xd_runtime_has_extension("XR_BD_controller_interaction");
    xd_log(XD_INFO, "runtime has %u extensions; local floor %d, Pico controllers %d", n, xd_runtime_local_floor,
           xd_runtime_bd);
}

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(const char *layer, uint32_t capacity,
                                                                               uint32_t *count,
                                                                               XrExtensionProperties *props)
{
    if (!count)
        return XR_ERROR_VALIDATION_FAILURE;
    xd_settings_load();
    xd_next_load();
    log_runtime_extensions();
    const XrExtensionProperties *rt;
    uint32_t rt_count = xd_runtime_extensions(&rt);
    uint32_t n = 0;
#define EMIT(nm, ver)                                                           \
    do {                                                                        \
        if (props && n < capacity) {                                            \
            props[n].type = XR_TYPE_EXTENSION_PROPERTIES;                       \
            props[n].next = NULL;                                               \
            snprintf(props[n].extensionName, sizeof props[n].extensionName, "%s", nm); \
            props[n].extensionVersion = ver;                                    \
        }                                                                       \
        n++;                                                                    \
    } while (0)
    for (size_t i = 0; i < QUEST_EXT_COUNT; i++)
        EMIT(k_quest_extensions[i].name, k_quest_extensions[i].version);
    for (uint32_t i = 0; rt && i < rt_count; i++) {
        bool listed = false;
        for (size_t j = 0; j < QUEST_EXT_COUNT && !listed; j++)
            listed = !strcmp(rt[i].extensionName, k_quest_extensions[j].name);
        if (!listed)
            EMIT(rt[i].extensionName, rt[i].extensionVersion);
    }
#undef EMIT
    (void)layer;
    *count = n;
    XrResult r = props && capacity && capacity < n ? XR_ERROR_SIZE_INSUFFICIENT : XR_SUCCESS;
    xd_trace("xrEnumerateInstanceExtensionProperties", r);
    return r;
}

/* ---------------------------------------------------------------- loader init */

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrInitializeLoaderKHR(const XrLoaderInitInfoBaseHeaderKHR *info)
{
    xd_settings_load();
    xd_next_load();
    if (info && info->type == XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR) {
        const xd_android_info *a = (const xd_android_info *)info;
        xd_android_init(a->vm, a->context);
    }
    PFN_xrInitializeLoaderKHR fn = NEXT(xrInitializeLoaderKHR);
    XrResult r = fn ? fn(info) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (XR_FAILED(r))
        xd_log(XD_WARN, "next loader's xrInitializeLoaderKHR -> %s", xd_result_name(r));
    xd_trace("xrInitializeLoaderKHR", r);
    return r;
}

/* ----------------------------------------------------------- instance */

static bool requested(const char *const *names, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(names[i], name))
            return true;
    return false;
}

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo *info, XrInstance *instance)
{
    if (!info || !instance)
        return XR_ERROR_VALIDATION_FAILURE;
    xd_settings_load();
    xd_next_load();
    log_runtime_extensions();
    PFN_xrCreateInstance fn = NEXT(xrCreateInstance);
    if (!fn)
        return XR_ERROR_INITIALIZATION_FAILED;

    const char **names = calloc(info->enabledExtensionCount + 4, sizeof *names);
    const char **dropped = calloc(info->enabledExtensionCount + 1, sizeof *dropped);
    if (!names || !dropped) {
        free(names);
        free(dropped);
        return XR_ERROR_OUT_OF_MEMORY;
    }
    uint32_t kept = 0, ndropped = 0;
    for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
        const char *ext = info->enabledExtensionNames[i];
        if (!ext)
            continue;
        if (xd_runtime_has_extension(ext)) {
            if (!requested(names, kept, ext))
                names[kept++] = ext;
        } else
            dropped[ndropped++] = ext;
    }
    if (ndropped) /* the game still gets XR_SUCCESS; calls into these extensions fail later */
        log_list("unsupported: extensions requested but not offered by the runtime, not enabled", dropped, ndropped);
    free(dropped);
    if (xd_runtime_bd && !requested(names, kept, "XR_BD_controller_interaction"))
        names[kept++] = "XR_BD_controller_interaction";
    if (xd_runtime_local_floor && !requested(names, kept, "XR_EXT_local_floor"))
        names[kept++] = "XR_EXT_local_floor";
    if (xd_cfg.force_passthrough && !requested(names, kept, "XR_FB_passthrough"))
        names[kept++] = "XR_FB_passthrough";

    XrInstanceCreateInfo fixed = *info;
    fixed.enabledExtensionNames = names;
    fixed.enabledExtensionCount = kept;

    /* Chain the Android create info if the game has none (it may only have called xrInitializeLoaderKHR). */
    bool chained = false;
    for (const XrBaseInStructure *p = info->next; p; p = p->next)
        chained |= p->type == XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
    xd_android_info android = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR, info->next, xd_java_vm, xd_activity};
    if (!chained && xd_java_vm && xd_activity) {
        fixed.next = &android;
        xd_log(XD_INFO, "xrCreateInstance: added XrInstanceCreateInfoAndroidKHR");
    }

    XrResult r = fn(&fixed, instance);
    if (r == XR_ERROR_API_VERSION_UNSUPPORTED &&
        XR_VERSION_MINOR(fixed.applicationInfo.apiVersion) > 0) { /* OpenXR 1.1 app, 1.0 runtime */
        fixed.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 41);
        r = fn(&fixed, instance);
        xd_log(XD_INFO, "xrCreateInstance: retried as OpenXR 1.0 -> %s", xd_result_name(r));
    }
    free(names);
    if (XR_SUCCEEDED(r))
        xd_set_instance(*instance);
    xd_log(XD_INFO, "xrCreateInstance(%s, %u of %u extensions) -> %s", info->applicationInfo.applicationName, kept,
           info->enabledExtensionCount, xd_result_name(r));
    return r;
}

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrDestroyInstance(XrInstance instance)
{
    PFN_xrDestroyInstance fn = NEXT(xrDestroyInstance);
    XrResult r = fn ? fn(instance) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (instance == xd_instance())
        xd_set_instance(XR_NULL_HANDLE);
    xd_trace("xrDestroyInstance", r);
    return r;
}

/* ---------------------------------------------------------------- identity */

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProperties(XrInstance instance, XrInstanceProperties *props)
{
    PFN_xrGetInstanceProperties fn = NEXT(xrGetInstanceProperties);
    XrResult r = fn ? fn(instance, props) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (props) {
        if (XR_SUCCEEDED(r) && xd_first("runtime-name"))
            xd_log(XD_INFO, "runtime %s %u.%u.%u reported as Oculus 67.522.0", props->runtimeName,
                   XR_VERSION_MAJOR(props->runtimeVersion), XR_VERSION_MINOR(props->runtimeVersion),
                   XR_VERSION_PATCH(props->runtimeVersion));
        props->runtimeVersion = XR_MAKE_VERSION(67, 522, 0);
        snprintf(props->runtimeName, sizeof props->runtimeName, "Oculus");
    }
    xd_trace("xrGetInstanceProperties", r);
    return r; /* as OVRPort: the identity is written even if the runtime failed */
}

/* Quest 2 defaults OVRPort reports (its view-size tracking never updates). */
#define QUEST_VIEW_SIZE 1504
/* XR_META_headset_id: all zeros = "unknown headset". OVRPort reports a
 * UUID of its own here; it is not in Meta's OVRPlugin builds shipped with
 * games, so we do not copy it. */
static const uint8_t k_headset_id[XR_UUID_SIZE] = {0};

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrGetSystemProperties(XrInstance instance, XrSystemId system,
                                                              XrSystemProperties *props)
{
    if (!props)
        return XR_ERROR_VALIDATION_FAILURE;
    PFN_xrGetSystemProperties fn = NEXT(xrGetSystemProperties);
    void *chain = props->next;
    if (xd_runtime_bd)
        props->next = NULL; /* Pico's runtime is given no Meta structs (as OVRPort does) */
    XrResult r = fn ? fn(instance, system, props) : XR_ERROR_FUNCTION_UNSUPPORTED;
    props->next = chain;
    if (xd_first("system-name"))
        xd_log(XD_INFO, "system '%s' vendor 0x%x reported as Oculus Quest2", props->systemName, props->vendorId);
    snprintf(props->systemName, sizeof props->systemName, "Oculus Quest2");
    props->vendorId = 0x2833;
    for (XrBaseOutStructure *p = chain; p; p = p->next) {
        switch ((int)p->type) {
        case XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT:
            ((XrSystemHandTrackingPropertiesEXT *)p)->supportsHandTracking = XR_TRUE;
            break;
        case XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB:
            ((XrSystemBodyTrackingPropertiesFB *)p)->supportsBodyTracking = XR_TRUE;
            break;
        case XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB:
            ((XrSystemPassthroughPropertiesFB *)p)->supportsPassthrough = XR_TRUE;
            break;
        case XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB:
            ((XrSystemPassthroughProperties2FB *)p)->capabilities = 0;
            break;
        case XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB:
            ((XrSystemSpaceWarpPropertiesFB *)p)->recommendedMotionVectorImageRectWidth = QUEST_VIEW_SIZE / 4;
            ((XrSystemSpaceWarpPropertiesFB *)p)->recommendedMotionVectorImageRectHeight = QUEST_VIEW_SIZE / 4;
            break;
        case XR_TYPE_SYSTEM_VIRTUAL_KEYBOARD_PROPERTIES_META:
            ((XrSystemVirtualKeyboardPropertiesMETA *)p)->supportsVirtualKeyboard = XR_TRUE;
            break;
        case XR_TYPE_SYSTEM_HEADSET_ID_PROPERTIES_META:
            memcpy(((XrSystemHeadsetIdPropertiesMETA *)p)->id.data, k_headset_id, XR_UUID_SIZE);
            break;
        case XR_TYPE_SYSTEM_PASSTHROUGH_COLOR_LUT_PROPERTIES_META:
            ((XrSystemPassthroughColorLutPropertiesMETA *)p)->maxColorLutResolution = QUEST_VIEW_SIZE;
            break;
        default:
            break;
        }
    }
    xd_trace("xrGetSystemProperties", r);
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL xd_xrGetRecommendedLayerResolutionMETA(XrSession s,
        const XrRecommendedLayerResolutionGetInfoMETA *info, XrRecommendedLayerResolutionMETA *res)
{
    (void)s;
    (void)info;
    if (res) {
        res->recommendedImageDimensions.width = QUEST_VIEW_SIZE;
        res->recommendedImageDimensions.height = QUEST_VIEW_SIZE;
        res->isValid = XR_TRUE;
    }
    xd_trace("xrGetRecommendedLayerResolutionMETA", XR_SUCCESS);
    return XR_SUCCESS;
}

/* ------------------------------------------------------------------ events */

static int g_mount_sent;

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrPollEvent(XrInstance instance, XrEventDataBuffer *event)
{
    if (!event)
        return XR_ERROR_VALIDATION_FAILURE;
    if (!__atomic_exchange_n(&g_mount_sent, 1, __ATOMIC_ACQ_REL)) {
        /* Quest's "headset mounted" (XR_FB_common_events, value 1), before anything else */
        memset(event, 0, 24);
        event->type = XD_TYPE_EVENT_MOUNT_STATE_FB;
        ((uint32_t *)event)[4] = 1;
        return XR_SUCCESS;
    }
    PFN_xrPollEvent fn = NEXT(xrPollEvent);
    XrResult r = fn ? fn(instance, event) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (XR_FAILED(r)) {
        char key[48];
        snprintf(key, sizeof key, "poll:%d", r);
        if (xd_first(key))
            xd_log(XD_WARN, "xrPollEvent -> %s, reported as XR_EVENT_UNAVAILABLE", xd_result_name(r));
        r = XR_EVENT_UNAVAILABLE;
    } else if (r == XR_SUCCESS && event->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        xd_vlog("session state %d", ((XrEventDataSessionStateChanged *)event)->state);
    return r;
}

/* ------------------------------------------------------------------ Android */

#ifdef __ANDROID__
#include <jni.h>

static jobject static_object(JNIEnv *env, const char *cls, const char *field, const char *sig)
{
    jclass c = (*env)->FindClass(env, cls);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return NULL;
    }
    if (!c)
        return NULL;
    jfieldID f = (*env)->GetStaticFieldID(env, c, field, sig);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return NULL;
    }
    jobject o = f ? (*env)->GetStaticObjectField(env, c, f) : NULL;
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        o = NULL;
    }
    return o;
}

/* The activity of a Unity, Unreal or Godot game, for games that pass no context
 * (the same static fields OVRPort looks at). */
static jobject find_activity(JNIEnv *env)
{
    jobject a = static_object(env, "com/epicgames/ue4/GameActivity", "R0", "Lcom/epicgames/ue4/GameActivity;");
    if (!a)
        a = static_object(env, "com/epicgames/unreal/GameActivity", "P0", "Lcom/epicgames/unreal/GameActivity;");
    if (!a)
        a = static_object(env, "com/unity3d/player/UnityPlayer", "currentActivity", "Landroid/app/Activity;");
    if (!a)
        a = static_object(env, "com/unity3d/player/UnityPlayer", "mActivity", "Landroid/app/Activity;");
    if (!a) {
        jobject io = static_object(env, "org/godotengine/godot/Godot", "io", "Lorg/godotengine/godot/GodotIO;");
        jclass c = io ? (*env)->FindClass(env, "org/godotengine/godot/GodotIO") : NULL;
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            c = NULL;
        }
        jfieldID f = c ? (*env)->GetFieldID(env, c, "activity", "Landroid/app/Activity;") : NULL;
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            f = NULL;
        }
        a = f ? (*env)->GetObjectField(env, io, f) : NULL;
    }
    return a;
}

void xd_android_init(void *vm_ptr, void *context)
{
    JavaVM *vm = vm_ptr;
    if (!vm || xd_java_vm)
        return;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED)
        (*vm)->AttachCurrentThread(vm, &env, NULL);
    if (!env)
        return;
    jobject a = context ? (jobject)context : find_activity(env);
    xd_java_vm = vm;
    xd_activity = a ? (*env)->NewGlobalRef(env, a) : NULL;
    xd_log(XD_INFO, "Android: VM %p, %s %p", vm, context ? "context" : "activity found by class", xd_activity);
}
#else
void xd_android_init(void *vm, void *context)
{
    xd_java_vm = vm;
    xd_activity = context;
}
#endif
