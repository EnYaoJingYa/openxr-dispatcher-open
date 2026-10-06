/* SPDX-License-Identifier: MIT
 *
 * The next loader and xrGetInstanceProcAddr.
 *
 * Next loader (same rule as OVRPort): ro.product.brand containing "pico",
 * "oculus"/"meta", "yvr" or "huawei" (later wins) picks that vendor's loader,
 * anything else libopenxr_loader_generic.so (FrameBridge on the Steam Frame).
 * OVERPORT_FORCE_LOADER_PATH overrides the choice. A loader that resolves back
 * to us is refused (OVRPort aborts the process instead).
 *
 * xrGetInstanceProcAddr always asks the next loader first:
 *  - table names (functions.inc): XD_HOOK -> our function; XD_PLAIN -> the
 *    next loader's function, or a stub failing with FUNCTION_UNSUPPORTED when
 *    it has none (the lookup succeeds, as games built for Quest expect);
 *  - any other name: the next loader's answer, unchanged.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dispatch.h"
#include "hooks.h"

/* ------------------------------------------------------------ next loader */

static void *g_lib;
static PFN_xrGetInstanceProcAddr g_gipa;
static char g_name[PATH_MAX];
static pthread_once_t g_load_once = PTHREAD_ONCE_INIT;

static const char *pick_loader(const char *brand)
{
    const char *name = "libopenxr_loader_generic.so";
    if (strcasestr(brand, "pico"))
        name = "libopenxr_loader_pico.so";
    if (strcasestr(brand, "oculus") || strcasestr(brand, "meta"))
        name = "libopenxr_loader_meta.so";
    if (strcasestr(brand, "yvr"))
        name = "libopenxr_loader_yvr.so";
    if (strcasestr(brand, "huawei"))
        name = "libopenxr_loader_huawei.so";
    return name;
}

static void load_next(void)
{
    char brand[100];
    xd_property("ro.product.brand", brand, sizeof brand);
    const char *forced = getenv("OVERPORT_FORCE_LOADER_PATH");
    snprintf(g_name, sizeof g_name, "%s", forced && *forced ? forced : pick_loader(brand));
    g_lib = dlopen(g_name, RTLD_NOW | RTLD_LOCAL);
    if (!g_lib && !strchr(g_name, '/') && *xd_libdir()) { /* not on the search path: next to us */
        char path[PATH_MAX * 2];
        snprintf(path, sizeof path, "%s/%s", xd_libdir(), g_name);
        g_lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    }
    if (!g_lib) {
        xd_log(XD_ERROR, "next loader %s (brand '%s') failed to load: %s", g_name, brand, dlerror());
        return;
    }
    g_gipa = (PFN_xrGetInstanceProcAddr)dlsym(g_lib, "xrGetInstanceProcAddr");
    if (g_gipa == xrGetInstanceProcAddr) {
        xd_log(XD_ERROR, "next loader %s resolves to this dispatcher; refusing it", g_name);
        g_gipa = NULL;
    }
    xd_log(g_gipa ? XD_INFO : XD_ERROR, "open dispatcher %s: next loader %s (brand '%s')%s", XD_VERSION, g_name,
           brand, g_gipa ? "" : " has no usable xrGetInstanceProcAddr");
}

bool xd_next_load(void)
{
    pthread_once(&g_load_once, load_next);
    return g_gipa != NULL;
}

PFN_xrGetInstanceProcAddr xd_next_gipa(void)
{
    xd_next_load();
    return g_gipa;
}

const char *xd_next_name(void) { return g_name; }

/* ------------------------------------------------------------------ table */

#define XD_FN(name, kind) {#name, kind},
static const struct {
    const char *name;
    int kind;
} g_table[FN_COUNT] = {
#include "functions.inc"
};
#undef XD_FN

/* Resolved next-loader functions. `gen` is the instance generation the slot
 * was resolved for; a new instance re-resolves everything lazily. */
static struct {
    PFN_xrVoidFunction fn;
    unsigned gen;
} g_slot[FN_COUNT];
static XrInstance g_instance;
static unsigned g_gen = 1;

void xd_set_instance(XrInstance instance)
{
    __atomic_store_n(&g_instance, instance, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_gen, 1, __ATOMIC_ACQ_REL);
}

XrInstance xd_instance(void) { return __atomic_load_n(&g_instance, __ATOMIC_ACQUIRE); }

static int find(const char *name)
{
    for (int i = 0; i < FN_COUNT; i++)
        if (!strcmp(g_table[i].name, name))
            return i;
    return -1;
}

static void store(int id, PFN_xrVoidFunction fn)
{
    __atomic_store_n(&g_slot[id].fn, fn, __ATOMIC_RELAXED);
    __atomic_store_n(&g_slot[id].gen, __atomic_load_n(&g_gen, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
}

PFN_xrVoidFunction xd_next(enum xd_fn_id id)
{
    unsigned gen = __atomic_load_n(&g_gen, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&g_slot[id].gen, __ATOMIC_ACQUIRE) == gen)
        return __atomic_load_n(&g_slot[id].fn, __ATOMIC_RELAXED);
    PFN_xrVoidFunction fn = NULL;
    if (xd_next_load() && XR_FAILED(g_gipa(xd_instance(), g_table[id].name, &fn)))
        fn = NULL;
    store(id, fn);
    return fn;
}

static XRAPI_ATTR XrResult XRAPI_CALL unsupported_stub(void) { return XR_ERROR_FUNCTION_UNSUPPORTED; }

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char *name,
                                                               PFN_xrVoidFunction *function)
{
    if (!name || !function)
        return XR_ERROR_VALIDATION_FAILURE;
    *function = NULL;
    PFN_xrVoidFunction real = NULL;
    XrResult result = XR_ERROR_FUNCTION_UNSUPPORTED;
    if (xd_next_load() && XR_FAILED(result = g_gipa(instance, name, &real)))
        real = NULL;
    int id = find(name);
    if (id < 0) { /* not ours: forward the next loader's answer as it is */
        *function = real;
        if (!real && instance != XR_NULL_HANDLE)
            xd_unsupported("function %s not provided by the runtime (%s)", name, xd_result_name(result));
        return real ? result : (XR_FAILED(result) ? result : XR_ERROR_FUNCTION_UNSUPPORTED);
    }
    if (real || instance != XR_NULL_HANDLE)
        store(id, real);
    if (g_table[id].kind == XD_HOOK) {
        *function = xd_hook((enum xd_fn_id)id);
        if (!real && instance != XR_NULL_HANDLE)
            xd_unsupported("function %s not provided by the runtime: answered by the dispatcher", name);
        return XR_SUCCESS;
    }
    if (real) {
        *function = real;
        return result;
    }
    if (instance == XR_NULL_HANDLE) /* a non-global function looked up without an instance */
        return result;
    xd_unsupported("function %s not provided by the runtime: the lookup succeeds, calls return "
                   "XR_ERROR_FUNCTION_UNSUPPORTED", name);
    *function = (PFN_xrVoidFunction)unsupported_stub;
    return XR_SUCCESS;
}

/* ----------------------------------------------------- forwarding exports */

/* Core functions the dispatcher does not change: exported for games that link
 * the loader directly, resolved lazily through the next loader. */
#define FORWARD(name, params, args)                                           \
    XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL name params                      \
    {                                                                         \
        PFN_##name fn = NEXT(name);                                           \
        return fn ? fn args : XR_ERROR_FUNCTION_UNSUPPORTED;                  \
    }

FORWARD(xrEnumerateApiLayerProperties, (uint32_t cap, uint32_t *count, XrApiLayerProperties *p), (cap, count, p))
FORWARD(xrResultToString, (XrInstance i, XrResult r, char buf[XR_MAX_RESULT_STRING_SIZE]), (i, r, buf))
FORWARD(xrStructureTypeToString, (XrInstance i, XrStructureType t, char buf[XR_MAX_STRUCTURE_NAME_SIZE]), (i, t, buf))
FORWARD(xrGetSystem, (XrInstance i, const XrSystemGetInfo *info, XrSystemId *id), (i, info, id))
FORWARD(xrEnumerateEnvironmentBlendModes,
        (XrInstance i, XrSystemId s, XrViewConfigurationType v, uint32_t cap, uint32_t *n, XrEnvironmentBlendMode *m),
        (i, s, v, cap, n, m))
FORWARD(xrEnumerateReferenceSpaces, (XrSession s, uint32_t cap, uint32_t *n, XrReferenceSpaceType *t), (s, cap, n, t))
FORWARD(xrGetReferenceSpaceBoundsRect, (XrSession s, XrReferenceSpaceType t, XrExtent2Df *b), (s, t, b))
FORWARD(xrDestroySpace, (XrSpace s), (s))
FORWARD(xrEnumerateViewConfigurations, (XrInstance i, XrSystemId s, uint32_t cap, uint32_t *n, XrViewConfigurationType *t),
        (i, s, cap, n, t))
FORWARD(xrGetViewConfigurationProperties,
        (XrInstance i, XrSystemId s, XrViewConfigurationType t, XrViewConfigurationProperties *p), (i, s, t, p))
FORWARD(xrEnumerateSwapchainFormats, (XrSession s, uint32_t cap, uint32_t *n, int64_t *f), (s, cap, n, f))
FORWARD(xrDestroySwapchain, (XrSwapchain s), (s))
FORWARD(xrEnumerateSwapchainImages, (XrSwapchain s, uint32_t cap, uint32_t *n, XrSwapchainImageBaseHeader *i),
        (s, cap, n, i))
FORWARD(xrAcquireSwapchainImage, (XrSwapchain s, const XrSwapchainImageAcquireInfo *i, uint32_t *idx), (s, i, idx))
FORWARD(xrWaitSwapchainImage, (XrSwapchain s, const XrSwapchainImageWaitInfo *i), (s, i))
FORWARD(xrReleaseSwapchainImage, (XrSwapchain s, const XrSwapchainImageReleaseInfo *i), (s, i))
FORWARD(xrRequestExitSession, (XrSession s), (s))
FORWARD(xrBeginFrame, (XrSession s, const XrFrameBeginInfo *i), (s, i))
FORWARD(xrLocateViews,
        (XrSession s, const XrViewLocateInfo *i, XrViewState *st, uint32_t cap, uint32_t *n, XrView *v),
        (s, i, st, cap, n, v))
FORWARD(xrStringToPath, (XrInstance i, const char *s, XrPath *p), (i, s, p))
FORWARD(xrPathToString, (XrInstance i, XrPath p, uint32_t cap, uint32_t *n, char *b), (i, p, cap, n, b))
FORWARD(xrCreateActionSet, (XrInstance i, const XrActionSetCreateInfo *c, XrActionSet *s), (i, c, s))
FORWARD(xrDestroyActionSet, (XrActionSet s), (s))
FORWARD(xrCreateAction, (XrActionSet s, const XrActionCreateInfo *c, XrAction *a), (s, c, a))
FORWARD(xrAttachSessionActionSets, (XrSession s, const XrSessionActionSetsAttachInfo *i), (s, i))
FORWARD(xrGetActionStateBoolean, (XrSession s, const XrActionStateGetInfo *i, XrActionStateBoolean *st), (s, i, st))
FORWARD(xrGetActionStateFloat, (XrSession s, const XrActionStateGetInfo *i, XrActionStateFloat *st), (s, i, st))
FORWARD(xrGetActionStateVector2f, (XrSession s, const XrActionStateGetInfo *i, XrActionStateVector2f *st), (s, i, st))
FORWARD(xrGetActionStatePose, (XrSession s, const XrActionStateGetInfo *i, XrActionStatePose *st), (s, i, st))
FORWARD(xrSyncActions, (XrSession s, const XrActionsSyncInfo *i), (s, i))
FORWARD(xrEnumerateBoundSourcesForAction,
        (XrSession s, const XrBoundSourcesForActionEnumerateInfo *i, uint32_t cap, uint32_t *n, XrPath *p),
        (s, i, cap, n, p))
FORWARD(xrGetInputSourceLocalizedName,
        (XrSession s, const XrInputSourceLocalizedNameGetInfo *i, uint32_t cap, uint32_t *n, char *b), (s, i, cap, n, b))
FORWARD(xrLocateSpaces, (XrSession s, const XrSpacesLocateInfo *i, XrSpaceLocations *l), (s, i, l))

/* ------------------------------------------------------------------ hooks */

PFN_xrVoidFunction xd_hook(enum xd_fn_id id)
{
    switch (id) {
#define CORE(n) case FN_##n: return (PFN_xrVoidFunction)n;
#define EXT(n) case FN_##n: return (PFN_xrVoidFunction)xd_##n;
        CORE(xrGetInstanceProcAddr) CORE(xrInitializeLoaderKHR) CORE(xrEnumerateInstanceExtensionProperties)
        CORE(xrCreateInstance) CORE(xrDestroyInstance) CORE(xrGetInstanceProperties) CORE(xrGetSystemProperties)
        CORE(xrPollEvent) CORE(xrCreateSession) CORE(xrDestroySession) CORE(xrBeginSession) CORE(xrEndSession)
        CORE(xrCreateReferenceSpace) CORE(xrCreateActionSpace) CORE(xrLocateSpace)
        CORE(xrEnumerateViewConfigurationViews) CORE(xrCreateSwapchain) CORE(xrEndFrame) CORE(xrWaitFrame)
        CORE(xrSuggestInteractionProfileBindings) CORE(xrGetCurrentInteractionProfile) CORE(xrDestroyAction)
        CORE(xrApplyHapticFeedback) CORE(xrStopHapticFeedback)
        EXT(xrGetOpenGLESGraphicsRequirementsKHR) EXT(xrGetVulkanInstanceExtensionsKHR)
        EXT(xrGetVulkanDeviceExtensionsKHR) EXT(xrPerfSettingsSetPerformanceLevelEXT) EXT(xrEnumerateColorSpacesFB)
        EXT(xrSetColorSpaceFB) EXT(xrGetHandMeshFB) EXT(xrCreateFoveationProfileFB) EXT(xrDestroyFoveationProfileFB)
        EXT(xrUpdateSwapchainFB) EXT(xrGetSwapchainStateFB) EXT(xrSetAndroidApplicationThreadKHR)
        EXT(xrSetPerformanceMetricsStateMETA) EXT(xrEnumeratePerformanceMetricsCounterPathsMETA)
        EXT(xrQueryPerformanceMetricsCounterMETA) EXT(xrGetDeviceSampleRateFB) EXT(xrGetRecommendedLayerResolutionMETA)
#undef CORE
#undef EXT
    default:
        return NULL;
    }
}

/* ---------------------------------------------------- runtime extensions */

static XrExtensionProperties *g_ext;
static uint32_t g_ext_count;
static pthread_mutex_t g_ext_lock = PTHREAD_MUTEX_INITIALIZER;

/* Query once the next loader answers (it needs xrInitializeLoaderKHR first). */
static bool runtime_extensions(void)
{
    pthread_mutex_lock(&g_ext_lock);
    if (!g_ext) {
        PFN_xrEnumerateInstanceExtensionProperties fn = NEXT(xrEnumerateInstanceExtensionProperties);
        uint32_t n = 0;
        if (fn && XR_SUCCEEDED(fn(NULL, 0, &n, NULL))) {
            XrExtensionProperties *p = calloc(n ? n : 1, sizeof *p);
            for (uint32_t i = 0; p && i < n; i++)
                p[i].type = XR_TYPE_EXTENSION_PROPERTIES;
            if (p && XR_SUCCEEDED(fn(NULL, n, &n, p))) {
                g_ext = p;
                g_ext_count = n;
            } else
                free(p);
        }
    }
    bool ok = g_ext != NULL;
    pthread_mutex_unlock(&g_ext_lock);
    return ok;
}

bool xd_runtime_has_extension(const char *name)
{
    if (!runtime_extensions())
        return false;
    for (uint32_t i = 0; i < g_ext_count; i++)
        if (!strcmp(g_ext[i].extensionName, name))
            return true;
    return false;
}

uint32_t xd_runtime_extensions(const XrExtensionProperties **out)
{
    runtime_extensions();
    *out = g_ext;
    return g_ext_count;
}
