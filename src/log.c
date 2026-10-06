/* SPDX-License-Identifier: MIT
 *
 * Logging. Tag "OpenXRDispatch".
 *  - info: next loader, settings, runtime extensions, emulation decisions;
 *  - "unsupported: ..." once per distinct message: functions, extensions, layer
 *    types and bindings the runtime lacks (the same style as FrameBridge's
 *    input_diag.c, so one grep covers both);
 *  - verbose_logging: first call of each hooked function and failing results.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dispatch.h"

#define TAG "OpenXRDispatch"

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap);

void xd_log(int prio, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(prio, TAG, fmt, ap);
    va_end(ap);
}

void xd_vlog(const char *fmt, ...)
{
    if (!xd_cfg.verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(XD_INFO, TAG, fmt, ap);
    va_end(ap);
}

#define SEEN_MAX 1024
static char g_seen[SEEN_MAX][96];
static int g_seen_count;
static pthread_mutex_t g_seen_lock = PTHREAD_MUTEX_INITIALIZER;

bool xd_first(const char *key)
{
    char k[96];
    snprintf(k, sizeof k, "%s", key);
    pthread_mutex_lock(&g_seen_lock);
    bool first = true;
    for (int i = 0; i < g_seen_count && first; i++)
        first = strcmp(g_seen[i], k) != 0;
    if (first && g_seen_count < SEEN_MAX)
        memcpy(g_seen[g_seen_count++], k, sizeof k);
    pthread_mutex_unlock(&g_seen_lock);
    return first;
}

void xd_unsupported(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    char key[96];
    snprintf(key, sizeof key, "u:%s", msg);
    if (xd_first(key))
        xd_log(XD_INFO, "unsupported: %s", msg);
}

void xd_trace(const char *fn, XrResult result)
{
    if (!xd_cfg.verbose)
        return;
    char key[96];
    snprintf(key, sizeof key, "t:%s", fn);
    if (xd_first(key))
        xd_log(XD_INFO, "first call %s -> %s", fn, xd_result_name(result));
    else if (XR_FAILED(result)) {
        snprintf(key, sizeof key, "f:%s:%d", fn, result);
        if (xd_first(key))
            xd_log(XD_INFO, "%s -> %s", fn, xd_result_name(result));
    }
}

const char *xd_result_name(XrResult r)
{
    switch (r) {
#define R(x) case x: return #x;
    R(XR_SUCCESS) R(XR_TIMEOUT_EXPIRED) R(XR_SESSION_LOSS_PENDING) R(XR_EVENT_UNAVAILABLE)
    R(XR_SPACE_BOUNDS_UNAVAILABLE) R(XR_SESSION_NOT_FOCUSED) R(XR_FRAME_DISCARDED)
    R(XR_ERROR_VALIDATION_FAILURE) R(XR_ERROR_RUNTIME_FAILURE) R(XR_ERROR_OUT_OF_MEMORY)
    R(XR_ERROR_API_VERSION_UNSUPPORTED) R(XR_ERROR_INITIALIZATION_FAILED) R(XR_ERROR_FUNCTION_UNSUPPORTED)
    R(XR_ERROR_FEATURE_UNSUPPORTED) R(XR_ERROR_EXTENSION_NOT_PRESENT) R(XR_ERROR_LIMIT_REACHED)
    R(XR_ERROR_SIZE_INSUFFICIENT) R(XR_ERROR_HANDLE_INVALID) R(XR_ERROR_INSTANCE_LOST)
    R(XR_ERROR_SESSION_RUNNING) R(XR_ERROR_SESSION_NOT_RUNNING) R(XR_ERROR_SESSION_LOST)
    R(XR_ERROR_SYSTEM_INVALID) R(XR_ERROR_PATH_INVALID) R(XR_ERROR_PATH_COUNT_EXCEEDED)
    R(XR_ERROR_PATH_FORMAT_INVALID) R(XR_ERROR_PATH_UNSUPPORTED) R(XR_ERROR_LAYER_INVALID)
    R(XR_ERROR_LAYER_LIMIT_EXCEEDED) R(XR_ERROR_SWAPCHAIN_RECT_INVALID)
    R(XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED) R(XR_ERROR_ACTION_TYPE_MISMATCH)
    R(XR_ERROR_SESSION_NOT_READY) R(XR_ERROR_SESSION_NOT_STOPPING) R(XR_ERROR_TIME_INVALID)
    R(XR_ERROR_REFERENCE_SPACE_UNSUPPORTED) R(XR_ERROR_FILE_ACCESS_ERROR) R(XR_ERROR_FILE_CONTENTS_INVALID)
    R(XR_ERROR_FORM_FACTOR_UNSUPPORTED) R(XR_ERROR_FORM_FACTOR_UNAVAILABLE) R(XR_ERROR_API_LAYER_NOT_PRESENT)
    R(XR_ERROR_CALL_ORDER_INVALID) R(XR_ERROR_GRAPHICS_DEVICE_INVALID) R(XR_ERROR_POSE_INVALID)
    R(XR_ERROR_INDEX_OUT_OF_RANGE) R(XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED)
    R(XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED) R(XR_ERROR_NAME_DUPLICATED) R(XR_ERROR_NAME_INVALID)
    R(XR_ERROR_ACTIONSET_NOT_ATTACHED) R(XR_ERROR_ACTIONSETS_ALREADY_ATTACHED)
    R(XR_ERROR_LOCALIZED_NAME_DUPLICATED) R(XR_ERROR_LOCALIZED_NAME_INVALID)
    R(XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING) R(XR_ERROR_RUNTIME_UNAVAILABLE)
#undef R
    default: {
        static __thread char buf[24];
        snprintf(buf, sizeof buf, "%d", (int)r);
        return buf;
    }
    }
}
