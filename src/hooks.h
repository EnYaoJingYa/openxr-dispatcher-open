/* SPDX-License-Identifier: MIT
 * Extension hooks (prefixed xd_ so they never clash with loader exports) and
 * mirrors of the Android/graphics structs whose real headers need jni.h,
 * EGL or Vulkan. */
#ifndef OPENXR_DISPATCH_HOOKS_H
#define OPENXR_DISPATCH_HOOKS_H

#include "dispatch.h"

/* XrLoaderInitInfoAndroidKHR / XrInstanceCreateInfoAndroidKHR (openxr_platform.h) */
typedef struct {
    XrStructureType type;
    const void *next;
    void *vm;
    void *context;
} xd_android_info;
#define XD_TYPE_LOADER_INIT_INFO_ANDROID ((XrStructureType)1000089000)
/* XrGraphicsRequirementsOpenGLESKHR */
typedef struct {
    XrStructureType type;
    void *next;
    XrVersion minApiVersionSupported;
    XrVersion maxApiVersionSupported;
} xd_gles_requirements;

/* Meta types missing from the public registry */
#define XD_TYPE_EVENT_MOUNT_STATE_FB ((XrStructureType)1000075000) /* OVRPort: "XrEventDataMountStateChangedFB" */
#define XD_SPACE_META_FIRST 1000043000 /* XR_OCULUS_common_reference_spaces, 1000043000..1000043004 */
#define XD_SPACE_META_FLOOR 1000043001

uint32_t xd_runtime_extensions(const XrExtensionProperties **out);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrLocateViews(
    XrSession session, const XrViewLocateInfo *info, XrViewState *state, uint32_t capacity,
    uint32_t *count, XrView *views);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrGetActionStateBoolean(
    XrSession session, const XrActionStateGetInfo *info, XrActionStateBoolean *state);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrGetActionStateFloat(
    XrSession session, const XrActionStateGetInfo *info, XrActionStateFloat *state);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrGetActionStateVector2f(
    XrSession session, const XrActionStateGetInfo *info, XrActionStateVector2f *state);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrGetActionStatePose(
    XrSession session, const XrActionStateGetInfo *info, XrActionStatePose *state);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrEnumerateSwapchainFormats(
    XrSession session, uint32_t capacity, uint32_t *count, int64_t *formats);

XRAPI_ATTR XrResult XRAPI_CALL xd_xrGetOpenGLESGraphicsRequirementsKHR(XrInstance i, XrSystemId s, xd_gles_requirements *r);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrGetVulkanInstanceExtensionsKHR(XrInstance i, XrSystemId s, uint32_t cap, uint32_t *n,
                                                                   char *buf);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrGetVulkanDeviceExtensionsKHR(XrInstance i, XrSystemId s, uint32_t cap, uint32_t *n,
                                                                 char *buf);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrPerfSettingsSetPerformanceLevelEXT(XrSession s, XrPerfSettingsDomainEXT d,
                                                                       XrPerfSettingsLevelEXT l);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrEnumerateColorSpacesFB(XrSession s, uint32_t cap, uint32_t *n, XrColorSpaceFB *c);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrSetColorSpaceFB(XrSession s, const XrColorSpaceFB c);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrGetHandMeshFB(XrHandTrackerEXT t, XrHandTrackingMeshFB *mesh);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrCreateFoveationProfileFB(XrSession s, const XrFoveationProfileCreateInfoFB *i,
                                                             XrFoveationProfileFB *p);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrDestroyFoveationProfileFB(XrFoveationProfileFB p);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrUpdateSwapchainFB(XrSwapchain s, const XrSwapchainStateBaseHeaderFB *st);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrGetSwapchainStateFB(XrSwapchain s, XrSwapchainStateBaseHeaderFB *st);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrSetAndroidApplicationThreadKHR(XrSession s, int32_t type, uint32_t tid);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrSetPerformanceMetricsStateMETA(XrSession s, const XrPerformanceMetricsStateMETA *st);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrEnumeratePerformanceMetricsCounterPathsMETA(XrInstance i, uint32_t cap, uint32_t *n,
                                                                               XrPath *p);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrQueryPerformanceMetricsCounterMETA(XrSession s, XrPath p,
                                                                       XrPerformanceMetricsCounterMETA *c);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrGetDeviceSampleRateFB(XrSession s, const XrHapticActionInfo *i,
                                                          XrDevicePcmSampleRateStateFB *rate);
XRAPI_ATTR XrResult XRAPI_CALL xd_xrGetRecommendedLayerResolutionMETA(XrSession s,
        const XrRecommendedLayerResolutionGetInfoMETA *i, XrRecommendedLayerResolutionMETA *r);

#endif
