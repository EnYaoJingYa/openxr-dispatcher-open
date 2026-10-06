/* SPDX-License-Identifier: MIT
 *
 * Haptics.
 *  - XrHapticVibration: amplitude and frequency scaled by vibromultiplier /
 *    vibrofreqmultiplier, then forwarded.
 *  - XR_FB_haptic_pcm and XR_FB_haptic_amplitude_envelope, which runtimes
 *    outside Meta's lack: the samples become 100 Hz amplitudes (absolute,
 *    linearly resampled), played by a worker thread as 10 ms vibrations at
 *    500 Hz. That is OVRPort's emulation, with two fixes: the envelope length
 *    is computed from its duration in seconds (OVRPort used nanoseconds and
 *    asked for gigabytes), and the thread only runs while a pattern plays
 *    (OVRPort's wakes every 10 ms for the life of the process).
 */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dispatch.h"

#define TICK_NS 10000000ll /* 10 ms per emulated sample */
#define RATE 100           /* samples per second */
#define MAX_SAMPLES (RATE * 60)

typedef struct {
    XrSession session;
    XrAction action;
    XrPath subaction;
    float *amp;
    size_t pos, count, cap;
} pattern;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static pattern *g_pat;
static size_t g_pat_count;
static bool g_thread;

size_t xd_resample_abs(const float *in, size_t n, float *out, size_t m)
{
    if (!n || !m)
        return 0;
    for (size_t k = 0; k < m; k++) {
        float x = m > 1 ? (float)(n - 1) / (float)(m - 1) * (float)k : 0.0f;
        size_t i = (size_t)x;
        if (i >= n)
            i = n - 1;
        size_t j = i + 1 < n ? i + 1 : n - 1;
        float f = x - (float)i;
        out[k] = fabsf(in[j] * f + in[i] * (1.0f - f));
    }
    return m;
}

size_t xd_envelope_samples(XrDuration duration_ns)
{
    if (duration_ns <= 0)
        return 1;
    long long n = (duration_ns * RATE + 999999999ll) / 1000000000ll;
    return n > MAX_SAMPLES ? MAX_SAMPLES : (size_t)n;
}

static pattern *find(XrAction action, XrPath subaction, bool create)
{
    for (size_t i = 0; i < g_pat_count; i++)
        if (g_pat[i].action == action && g_pat[i].subaction == subaction)
            return &g_pat[i];
    if (!create)
        return NULL;
    pattern *n = realloc(g_pat, (g_pat_count + 1) * sizeof *n);
    if (!n)
        return NULL;
    g_pat = n;
    memset(&g_pat[g_pat_count], 0, sizeof *g_pat);
    g_pat[g_pat_count].action = action;
    g_pat[g_pat_count].subaction = subaction;
    return &g_pat[g_pat_count++];
}

static bool reserve(pattern *p, size_t n)
{
    if (n <= p->cap)
        return true;
    float *a = realloc(p->amp, n * sizeof *a);
    if (!a)
        return false;
    p->amp = a;
    p->cap = n;
    return true;
}

static void *player(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        bool active = false;
        for (size_t i = 0; i < g_pat_count; i++) {
            pattern *p = &g_pat[i];
            if (p->pos >= p->count)
                continue;
            active = true;
            PFN_xrApplyHapticFeedback fn = NEXT(xrApplyHapticFeedback);
            XrHapticActionInfo info = {XR_TYPE_HAPTIC_ACTION_INFO, NULL, p->action, p->subaction};
            XrHapticVibration v = {XR_TYPE_HAPTIC_VIBRATION, NULL, TICK_NS, 500.0f * xd_cfg.vibration_frequency,
                                   p->amp[p->pos] * xd_cfg.vibration_amplitude};
            if (v.amplitude > 1.0f)
                v.amplitude = 1.0f;
            p->pos++;
            if (fn)
                fn(p->session, &info, (const XrHapticBaseHeader *)&v);
        }
        if (active) {
            pthread_mutex_unlock(&g_lock);
            struct timespec ts = {0, TICK_NS};
            nanosleep(&ts, NULL);
            pthread_mutex_lock(&g_lock);
        } else
            pthread_cond_wait(&g_wake, &g_lock);
    }
    return NULL;
}

/* Queue an emulated pattern; called with g_lock held. */
static void start_player(void)
{
    if (!g_thread) {
        pthread_t t;
        if (pthread_create(&t, NULL, player, NULL) == 0) {
            pthread_detach(t);
            g_thread = true;
            xd_log(XD_INFO, "haptics: playing PCM/envelope haptics as 100 Hz vibrations");
        }
    }
    pthread_cond_signal(&g_wake);
}

static void emulate(XrSession session, const XrHapticActionInfo *info, const XrHapticBaseHeader *h)
{
    pthread_mutex_lock(&g_lock);
    pattern *p = find(info->action, info->subactionPath, true);
    if (!p) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    p->session = session;
    if (h->type == XR_TYPE_HAPTIC_PCM_VIBRATION_FB) {
        const XrHapticPcmVibrationFB *pcm = (const XrHapticPcmVibrationFB *)h;
        size_t m = pcm->sampleRate > 0 ? (size_t)((double)pcm->bufferSize * RATE / pcm->sampleRate) : 0;
        if (m > MAX_SAMPLES)
            m = MAX_SAMPLES;
        if (!pcm->append || p->pos >= p->count)
            p->pos = p->count = 0;
        if (m && pcm->buffer && reserve(p, p->count + m))
            p->count += xd_resample_abs(pcm->buffer, pcm->bufferSize, p->amp + p->count, m);
        if (pcm->samplesConsumed)
            *pcm->samplesConsumed = pcm->bufferSize;
    } else {
        const XrHapticAmplitudeEnvelopeVibrationFB *env = (const XrHapticAmplitudeEnvelopeVibrationFB *)h;
        size_t m = xd_envelope_samples(env->duration);
        p->pos = p->count = 0;
        if (env->amplitudeCount && env->amplitudes && reserve(p, m))
            p->count = xd_resample_abs(env->amplitudes, env->amplitudeCount, p->amp, m);
    }
    if (p->count)
        start_player();
    pthread_mutex_unlock(&g_lock);
}

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrApplyHapticFeedback(XrSession session, const XrHapticActionInfo *info,
                                                              const XrHapticBaseHeader *haptic)
{
    if (!info || !haptic)
        return XR_ERROR_VALIDATION_FAILURE;
    if (haptic->type == XR_TYPE_HAPTIC_PCM_VIBRATION_FB || haptic->type == XR_TYPE_HAPTIC_AMPLITUDE_ENVELOPE_VIBRATION_FB) {
        emulate(session, info, haptic);
        xd_trace(haptic->type == XR_TYPE_HAPTIC_PCM_VIBRATION_FB ? "xrApplyHapticFeedback(PCM)"
                                                                 : "xrApplyHapticFeedback(envelope)", XR_SUCCESS);
        return XR_SUCCESS;
    }
    PFN_xrApplyHapticFeedback fn = NEXT(xrApplyHapticFeedback);
    if (!fn)
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r;
    if (haptic->type == XR_TYPE_HAPTIC_VIBRATION &&
        (xd_cfg.vibration_amplitude != 1.0f || xd_cfg.vibration_frequency != 1.0f)) {
        XrHapticVibration v = *(const XrHapticVibration *)haptic;
        v.amplitude *= xd_cfg.vibration_amplitude;
        if (v.amplitude > 1.0f)
            v.amplitude = 1.0f;
        v.frequency *= xd_cfg.vibration_frequency;
        r = fn(session, info, (const XrHapticBaseHeader *)&v);
    } else
        r = fn(session, info, haptic);
    if (XR_FAILED(r))
        xd_unsupported("xrApplyHapticFeedback(type %d) -> %s", (int)haptic->type, xd_result_name(r));
    return r;
}

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrStopHapticFeedback(XrSession session, const XrHapticActionInfo *info)
{
    if (info) {
        pthread_mutex_lock(&g_lock);
        pattern *p = find(info->action, info->subactionPath, false);
        if (p)
            p->pos = p->count = 0;
        pthread_mutex_unlock(&g_lock);
    }
    PFN_xrStopHapticFeedback fn = NEXT(xrStopHapticFeedback);
    return fn ? fn(session, info) : XR_ERROR_FUNCTION_UNSUPPORTED;
}

void xd_haptics_session_destroyed(XrSession session)
{
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_pat_count; i++)
        if (g_pat[i].session == session)
            g_pat[i].pos = g_pat[i].count = 0;
    pthread_mutex_unlock(&g_lock);
}
