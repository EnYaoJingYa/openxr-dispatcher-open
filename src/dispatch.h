/* SPDX-License-Identifier: MIT
 *
 * openxr-dispatcher-open: an open replacement for OVRPort's closed OpenXR
 * dispatcher (lib/<abi>/libopenxr_loader.so in OVRPort/FramePort builds).
 * See README.md.
 *
 * Design notes:
 *  - Same position in the chain as OVRPort's dispatcher: the game loads
 *    libopenxr_loader.so, we load the next loader picked by ro.product.brand
 *    (FrameBridge's libopenxr_loader_generic.so on the Steam Frame, a vendor
 *    loader elsewhere) and forward to it.
 *  - xrGetInstanceProcAddr asks the next loader first, always. Names in our
 *    table (functions.inc) get our hook, or the next loader's function, or a
 *    stub that fails with XR_ERROR_FUNCTION_UNSUPPORTED when the runtime lacks
 *    it (the lookup still succeeds, as with OVRPort). Every other name is
 *    forwarded unchanged: no "Unknown proc addr" refusals.
 *  - Behaviour that games rely on is kept (Quest identity, extension list,
 *    local answers for Meta-only functions, layer and binding fixes). Behaviour
 *    that crashes games is not (abort() on large swapchains or unknown formats,
 *    broken amplitude-envelope haptics). Nothing touches the network.
 *  - Log tag "OpenXRDispatch". "unsupported:" lines (once each) list every
 *    function, extension, layer type or binding the runtime lacks.
 */
#ifndef OPENXR_DISPATCH_H
#define OPENXR_DISPATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <openxr/openxr.h>

/* XR_KHR_loader_init is an extension, so openxr.h declares no prototype for it */
XRAPI_ATTR XrResult XRAPI_CALL xrInitializeLoaderKHR(const XrLoaderInitInfoBaseHeaderKHR *info);

#define XD_EXPORT __attribute__((visibility("default")))
#define XD_VERSION "1.0.0"

/* ---------------------------------------------------------------- logging */

enum { XD_INFO = 4, XD_WARN = 5, XD_ERROR = 6 };
void xd_log(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* verbose_logging only */
void xd_vlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* 1 the first time `key` is seen (bounded table; later keys always count as new) */
bool xd_first(const char *key);
/* "unsupported: ..." once per distinct message */
void xd_unsupported(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* verbose: first call of a hooked function; failing results */
void xd_trace(const char *fn, XrResult result);
const char *xd_result_name(XrResult r);

/* --------------------------------------------------------------- settings */

typedef struct xd_settings {
    int texture_width, texture_height; /* fixedtexturewidth/height, suggested_texture_* */
    int cpu_level, gpu_level;           /* 1..4, 0 = app's choice */
    float vibration_amplitude;          /* vibromultiplier */
    float vibration_frequency;          /* vibrofreqmultiplier */
    bool verbose;
    bool disable_space_warp;
    bool force_passthrough;
    bool disable_controller_offset;
    char custom_app_id[100];   /* read for compatibility, unused here */
    char platform_option[100]; /* read for compatibility, unused here */
    char config_name[100];
    char source[512];          /* where the config file came from, for the log */
} xd_settings;

extern xd_settings xd_cfg;
void xd_settings_load(void);
/* system property (empty string if unset). Host builds read the environment
 * variable XD_PROP_<name with dots as underscores> so tests can set them. */
void xd_property(const char *name, char *out, size_t size);
/* /proc/self/cmdline up to ':' */
const char *xd_package(void);
/* directory this library was loaded from (nativeLibraryDir) */
const char *xd_libdir(void);

/* ------------------------------------------------------- next loader, table */

#define XD_FN(name, kind) FN_##name,
enum xd_fn_id {
#include "functions.inc"
    FN_COUNT
};
#undef XD_FN

/* kinds in functions.inc */
enum { XD_PLAIN = 0, XD_HOOK = 1 };

bool xd_next_load(void);
PFN_xrGetInstanceProcAddr xd_next_gipa(void);
const char *xd_next_name(void);
/* The next loader's function for a table entry, resolved lazily with the
 * current instance; NULL if it does not provide it. */
PFN_xrVoidFunction xd_next(enum xd_fn_id id);
#define NEXT(name) ((PFN_##name)xd_next(FN_##name))
void xd_set_instance(XrInstance instance);
XrInstance xd_instance(void);

/* next loader's instance extensions (cached after the first query) */
bool xd_runtime_has_extension(const char *name);

/* -------------------------------------------------- shared state, features */

extern bool xd_runtime_local_floor; /* runtime offers XR_EXT_local_floor */
extern bool xd_runtime_bd;          /* XR_BD_controller_interaction (Pico) */
extern bool xd_vulkan_session;      /* the session was created with a Vulkan binding */

/* Android: JavaVM and activity (global ref) for XrInstanceCreateInfoAndroidKHR */
extern void *xd_java_vm;
extern void *xd_activity;
void xd_android_init(void *vm, void *context);

/* hook lookup for xrGetInstanceProcAddr: our implementation, or NULL */
PFN_xrVoidFunction xd_hook(enum xd_fn_id id);

/* frame.c */
void xd_passthrough_destroyed(void);
extern XrPassthroughFB xd_pt;
extern XrPassthroughLayerFB xd_pt_layer;
/* haptics.c */
void xd_haptics_session_destroyed(XrSession session);


/* VkFormat -> GL internal format, 0 if unknown (formats.c) */
int64_t xd_vk_to_gl(int64_t vk);
bool xd_looks_like_vk_format(int64_t format);

/* haptic resampling (haptics.c, exposed for tests) */
size_t xd_resample_abs(const float *in, size_t n, float *out, size_t m);
size_t xd_envelope_samples(XrDuration duration_ns);

#endif
