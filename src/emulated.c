/* SPDX-License-Identifier: MIT
 *
 * Functions answered by the dispatcher, as OVRPort's dispatcher answers them.
 *
 * Always local (the runtime is not asked, even if it has them):
 *   xrCreateFoveationProfileFB / xrDestroyFoveationProfileFB, xrUpdateSwapchainFB,
 *   xrGetSwapchainStateFB, xrSetAndroidApplicationThreadKHR, the four
 *   XR_META_performance_metrics functions, xrGetDeviceSampleRateFB (500 Hz) and
 *   xrGetRecommendedLayerResolutionMETA (instance.c, 1504x1504).
 * Fallbacks when the runtime fails or lacks the function:
 *   xrGetOpenGLESGraphicsRequirementsKHR (GLES 3.0..3.2, always),
 *   xrGetVulkanInstance/DeviceExtensionsKHR (the usual Android lists),
 *   xrEnumerateColorSpacesFB (UNMANAGED), xrSetColorSpaceFB (success).
 * Fix: xrGetHandMeshFB clamps joint parent indices above 25 to 0.
 */
#include <stdio.h>
#include <string.h>

#include "dispatch.h"
#include "hooks.h"

typedef XrResult(XRAPI_PTR *pfn_gles_req)(XrInstance, XrSystemId, xd_gles_requirements *);
typedef XrResult(XRAPI_PTR *pfn_vk_ext)(XrInstance, XrSystemId, uint32_t, uint32_t *, char *);

XRAPI_ATTR XrResult XRAPI_CALL xd_xrGetOpenGLESGraphicsRequirementsKHR(XrInstance i, XrSystemId s, xd_gles_requirements *r)
{
    pfn_gles_req fn = (pfn_gles_req)xd_next(FN_xrGetOpenGLESGraphicsRequirementsKHR);
    XrResult rr = fn && r ? fn(i, s, r) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (XR_FAILED(rr))
        xd_unsupported("xrGetOpenGLESGraphicsRequirementsKHR -> %s, answered with GLES 3.0 to 3.2", xd_result_name(rr));
    if (!r)
        return XR_ERROR_VALIDATION_FAILURE;
    r->minApiVersionSupported = XR_MAKE_VERSION(3, 0, 0);
    r->maxApiVersionSupported = XR_MAKE_VERSION(3, 2, 0);
    return XR_SUCCESS;
}

static XrResult vk_ext(enum xd_fn_id id, const char *fallback, XrInstance i, XrSystemId s, uint32_t cap, uint32_t *n,
                       char *buf)
{
    pfn_vk_ext fn = (pfn_vk_ext)xd_next(id);
    XrResult r = fn ? fn(i, s, cap, n, buf) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (XR_SUCCEEDED(r) || !n)
        return r;
    xd_unsupported("Vulkan extension query -> %s, answered with: %s", xd_result_name(r), fallback);
    uint32_t need = (uint32_t)strlen(fallback) + 1;
    *n = need;
    if (cap == 0 || !buf)
        return XR_SUCCESS;
    if (cap < need)
        return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(buf, fallback, need);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xd_xrGetVulkanInstanceExtensionsKHR(XrInstance i, XrSystemId s, uint32_t cap, uint32_t *n,
                                                                   char *buf)
{
    return vk_ext(FN_xrGetVulkanInstanceExtensionsKHR,
                  "VK_KHR_surface VK_KHR_android_surface VK_KHR_external_memory_capabilities "
                  "VK_KHR_get_physical_device_properties2",
                  i, s, cap, n, buf);
}

XRAPI_ATTR XrResult XRAPI_CALL xd_xrGetVulkanDeviceExtensionsKHR(XrInstance i, XrSystemId s, uint32_t cap, uint32_t *n,
                                                                 char *buf)
{
    return vk_ext(FN_xrGetVulkanDeviceExtensionsKHR,
                  "VK_KHR_swapchain VK_KHR_external_memory VK_KHR_get_memory_requirements2", i, s, cap, n, buf);
}

XRAPI_ATTR XrResult XRAPI_CALL xd_xrEnumerateColorSpacesFB(XrSession s, uint32_t cap, uint32_t *n, XrColorSpaceFB *c)
{
    PFN_xrEnumerateColorSpacesFB fn = NEXT(xrEnumerateColorSpacesFB);
    XrResult r = fn ? fn(s, cap, n, c) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (XR_SUCCEEDED(r))
        return r;
    xd_unsupported("xrEnumerateColorSpacesFB -> %s, answered with XR_COLOR_SPACE_UNMANAGED_FB", xd_result_name(r));
    if (n)
        *n = 1;
    if (cap && c)
        c[0] = XR_COLOR_SPACE_UNMANAGED_FB;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xd_xrSetColorSpaceFB(XrSession s, const XrColorSpaceFB c)
{
    PFN_xrSetColorSpaceFB fn = NEXT(xrSetColorSpaceFB);
    XrResult r = fn ? fn(s, c) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (XR_FAILED(r))
        xd_unsupported("xrSetColorSpaceFB(%d) -> %s, answered with success", (int)c, xd_result_name(r));
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xd_xrGetHandMeshFB(XrHandTrackerEXT t, XrHandTrackingMeshFB *mesh)
{
    PFN_xrGetHandMeshFB fn = NEXT(xrGetHandMeshFB);
    XrResult r = fn ? fn(t, mesh) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (mesh && mesh->jointParents)
        for (uint32_t i = 0; i < mesh->jointCapacityInput; i++)
            if (mesh->jointParents[i] > XR_HAND_JOINT_LITTLE_TIP_EXT)
                mesh->jointParents[i] = 0;
    xd_trace("xrGetHandMeshFB", r);
    return r;
}

/* ------------------------------------------------- always answered locally */

#define LOCAL(name) xd_trace(#name, XR_SUCCESS)

XRAPI_ATTR XrResult XRAPI_CALL xd_xrCreateFoveationProfileFB(XrSession s, const XrFoveationProfileCreateInfoFB *i,
                                                             XrFoveationProfileFB *p)
{
    (void)s;
    (void)i;
    if (p)
        *p = (XrFoveationProfileFB)(uintptr_t)0xdeadbeef; /* never reaches the runtime */
    LOCAL(xrCreateFoveationProfileFB);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xd_xrDestroyFoveationProfileFB(XrFoveationProfileFB p)
{
    (void)p;
    LOCAL(xrDestroyFoveationProfileFB);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xd_xrUpdateSwapchainFB(XrSwapchain s, const XrSwapchainStateBaseHeaderFB *st)
{
    (void)s;
    (void)st;
    LOCAL(xrUpdateSwapchainFB);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xd_xrGetSwapchainStateFB(XrSwapchain s, XrSwapchainStateBaseHeaderFB *st)
{
    (void)s;
    (void)st;
    LOCAL(xrGetSwapchainStateFB);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xd_xrSetAndroidApplicationThreadKHR(XrSession s, int32_t type, uint32_t tid)
{
    (void)s;
    (void)type;
    (void)tid;
    LOCAL(xrSetAndroidApplicationThreadKHR);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xd_xrSetPerformanceMetricsStateMETA(XrSession s, const XrPerformanceMetricsStateMETA *st)
{
    (void)s;
    (void)st;
    LOCAL(xrSetPerformanceMetricsStateMETA);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xd_xrEnumeratePerformanceMetricsCounterPathsMETA(XrInstance i, uint32_t cap, uint32_t *n,
                                                                               XrPath *p)
{
    (void)i;
    (void)cap;
    (void)p;
    if (n)
        *n = 0;
    LOCAL(xrEnumeratePerformanceMetricsCounterPathsMETA);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xd_xrQueryPerformanceMetricsCounterMETA(XrSession s, XrPath p,
                                                                       XrPerformanceMetricsCounterMETA *c)
{
    (void)s;
    (void)p;
    (void)c;
    LOCAL(xrQueryPerformanceMetricsCounterMETA);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xd_xrGetDeviceSampleRateFB(XrSession s, const XrHapticActionInfo *i,
                                                          XrDevicePcmSampleRateStateFB *rate)
{
    (void)s;
    (void)i;
    if (rate)
        rate->sampleRate = 500.0f; /* what the haptics emulation plays at */
    LOCAL(xrGetDeviceSampleRateFB);
    return XR_SUCCESS;
}
