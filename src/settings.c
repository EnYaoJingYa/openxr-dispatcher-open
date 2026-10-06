/* SPDX-License-Identifier: MIT
 *
 * Settings, read once at xrInitializeLoaderKHR (or the first call). Same sources
 * and names as OVRPort's dispatcher, so existing builds and habits keep working:
 *
 *  1. lib<configname>.so (default liboverport.config.so), JSON with comments,
 *     top-level keys, from /sdcard/Android/data/<package>/files/ first, else
 *     from the APK's native library directory. The same file carries the
 *     platform loader's "database"; we only read our keys.
 *  2. System properties debug.sys.ovrport.<name>, then persist.sys.ovrport.<name>
 *     (a set persist value wins). They override the file.
 *
 *  file key                        property              meaning
 *  suggested_texture_width/height  fixedtexturewidth/height  recommended eye-buffer size
 *  suggested_cpu_level/gpu_level   cpulevel/gpulevel     1..4 = power saving..boost, 0 = app's choice
 *  vibration_multiplier            vibromultiplier       haptic amplitude factor
 *  vibration_frequency_multiplier  vibrofreqmultiplier   haptic frequency factor
 *  verbose_logging                 verboselogging        first calls and failures
 *  disable_space_warp              disablespacewarp      strip space-warp info from frames
 *  force_passthrough               forcepassthrough      passthrough behind the game
 *  disable_controller_offset       disablecontrolleroffset  read for compatibility, unused (no Pico grip offset)
 *  custom_app_id                   customappid           read, unused (platform loader's)
 *  -                               platformoption        read, unused (platform loader's)
 *  -                               configname            config file name (default overport.config)
 *
 * Unlike OVRPort, configname is read before the file, so it actually selects it,
 * and no JNI is needed: the library directory comes from dladdr().
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#include "dispatch.h"
#include "third_party/cJSON.h"
#ifndef PROP_VALUE_MAX
#define PROP_VALUE_MAX 92
#endif

xd_settings xd_cfg = {.vibration_amplitude = 1.0f, .vibration_frequency = 1.0f};

void xd_property(const char *name, char *out, size_t size)
{
    if (!size)
        return;
    *out = 0;
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = "";
    if (__system_property_get(name, v) > 0)
        snprintf(out, size, "%s", v);
#else
    char env[128];
    snprintf(env, sizeof env, "XD_PROP_%s", name);
    for (char *p = env; *p; p++)
        if (*p == '.')
            *p = '_';
    const char *v = getenv(env);
    if (v)
        snprintf(out, size, "%s", v);
#endif
}

static char g_package[256], g_libdir[PATH_MAX];
static pthread_once_t g_env_once = PTHREAD_ONCE_INIT;

static void env_init(void)
{
    const char *forced = getenv("XD_PACKAGE"); /* host tests */
    if (forced)
        snprintf(g_package, sizeof g_package, "%s", forced);
    else {
        FILE *f = fopen("/proc/self/cmdline", "r");
        if (f) {
            size_t n = fread(g_package, 1, sizeof g_package - 1, f);
            g_package[n] = 0;
            fclose(f);
        }
        char *colon = strchr(g_package, ':');
        if (colon)
            *colon = 0;
    }
    Dl_info info;
    if (dladdr((void *)env_init, &info) && info.dli_fname) {
        snprintf(g_libdir, sizeof g_libdir, "%s", info.dli_fname);
        char *slash = strrchr(g_libdir, '/');
        if (slash)
            *slash = 0;
        else
            g_libdir[0] = 0;
    }
}

const char *xd_package(void)
{
    pthread_once(&g_env_once, env_init);
    return g_package;
}

const char *xd_libdir(void)
{
    pthread_once(&g_env_once, env_init);
    return g_libdir;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    size_t cap = 4 << 20; /* OVRPort reads at most 4 MB too */
    char *buf = malloc(cap + 1);
    size_t n = buf ? fread(buf, 1, cap, f) : 0;
    fclose(f);
    if (!buf)
        return NULL;
    buf[n] = 0;
    return buf;
}

static bool json_flag(const cJSON *v, bool *out)
{
    if (cJSON_IsBool(v))
        *out = cJSON_IsTrue(v);
    else if (cJSON_IsNumber(v))
        *out = v->valuedouble != 0;
    else
        return false;
    return true;
}

static void apply_json(const char *text)
{
    char *copy = strdup(text);
    if (!copy)
        return;
    cJSON_Minify(copy); /* strips // and block comments, which OVRPort's parser accepts */
    cJSON *root = cJSON_Parse(copy);
    free(copy);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        xd_log(XD_WARN, "settings: %s is not a JSON object, ignored", xd_cfg.source);
        return;
    }
    const cJSON *v;
#define INT(key, field)                                                  \
    if ((v = cJSON_GetObjectItemCaseSensitive(root, key)) && cJSON_IsNumber(v)) \
        xd_cfg.field = v->valueint;
#define FLOAT(key, field)                                                \
    if ((v = cJSON_GetObjectItemCaseSensitive(root, key)) && cJSON_IsNumber(v)) \
        xd_cfg.field = (float)v->valuedouble;
#define FLAG(key, field)                                                 \
    if ((v = cJSON_GetObjectItemCaseSensitive(root, key)))               \
        json_flag(v, &xd_cfg.field);
    INT("suggested_texture_width", texture_width)
    INT("suggested_texture_height", texture_height)
    INT("suggested_cpu_level", cpu_level)
    INT("suggested_gpu_level", gpu_level)
    FLOAT("vibration_multiplier", vibration_amplitude)
    FLOAT("vibration_frequency_multiplier", vibration_frequency)
    FLAG("verbose_logging", verbose)
    FLAG("disable_space_warp", disable_space_warp)
    FLAG("force_passthrough", force_passthrough)
    FLAG("disable_controller_offset", disable_controller_offset)
    if ((v = cJSON_GetObjectItemCaseSensitive(root, "custom_app_id")) && cJSON_IsString(v))
        snprintf(xd_cfg.custom_app_id, sizeof xd_cfg.custom_app_id, "%s", v->valuestring);
#undef INT
#undef FLOAT
#undef FLAG
    cJSON_Delete(root);
}

/* debug.sys.ovrport.<name>, then persist.sys.ovrport.<name>; returns the winning value */
static bool prop(const char *name, char *out, size_t size)
{
    char key[96], v[PROP_VALUE_MAX];
    bool found = false;
    static const char *const prefixes[] = {"debug.sys.ovrport.", "persist.sys.ovrport."};
    for (int i = 0; i < 2; i++) {
        snprintf(key, sizeof key, "%s%s", prefixes[i], name);
        xd_property(key, v, sizeof v);
        if (*v) {
            snprintf(out, size, "%s", v);
            found = true;
        }
    }
    return found;
}

static void apply_properties(void)
{
    char v[100];
    if (prop("fixedtexturewidth", v, sizeof v)) xd_cfg.texture_width = atoi(v);
    if (prop("fixedtextureheight", v, sizeof v)) xd_cfg.texture_height = atoi(v);
    if (prop("cpulevel", v, sizeof v)) xd_cfg.cpu_level = atoi(v);
    if (prop("gpulevel", v, sizeof v)) xd_cfg.gpu_level = atoi(v);
    if (prop("vibromultiplier", v, sizeof v)) xd_cfg.vibration_amplitude = (float)atof(v);
    if (prop("vibrofreqmultiplier", v, sizeof v)) xd_cfg.vibration_frequency = (float)atof(v);
    if (prop("customappid", v, sizeof v)) snprintf(xd_cfg.custom_app_id, sizeof xd_cfg.custom_app_id, "%s", v);
    if (prop("platformoption", v, sizeof v)) snprintf(xd_cfg.platform_option, sizeof xd_cfg.platform_option, "%s", v);
    if (prop("verboselogging", v, sizeof v)) xd_cfg.verbose = atoi(v) != 0;
    if (prop("disablespacewarp", v, sizeof v)) xd_cfg.disable_space_warp = atoi(v) != 0;
    if (prop("forcepassthrough", v, sizeof v)) xd_cfg.force_passthrough = atoi(v) != 0;
    if (prop("disablecontrolleroffset", v, sizeof v)) xd_cfg.disable_controller_offset = atoi(v) != 0;
}

static pthread_once_t g_settings_once = PTHREAD_ONCE_INIT;

static void settings_init(void)
{
    char v[100];
    snprintf(xd_cfg.config_name, sizeof xd_cfg.config_name, "overport.config");
    if (prop("configname", v, sizeof v) && *v)
        snprintf(xd_cfg.config_name, sizeof xd_cfg.config_name, "%s", v);

    const char *root = getenv("XD_STORAGE"); /* host tests */
    char path[PATH_MAX + 128];
    char *text = NULL;
    snprintf(path, sizeof path, "%s/data/%s/files/lib%s.so", root ? root : "/sdcard/Android", xd_package(),
             xd_cfg.config_name);
    text = read_file(path);
    if (!text && *xd_libdir()) {
        snprintf(path, sizeof path, "%s/lib%s.so", xd_libdir(), xd_cfg.config_name);
        text = read_file(path);
    }
    if (text) {
        snprintf(xd_cfg.source, sizeof xd_cfg.source, "%s", path);
        apply_json(text);
        free(text);
    } else
        snprintf(xd_cfg.source, sizeof xd_cfg.source, "defaults (no lib%s.so)", xd_cfg.config_name);
    apply_properties();
    if (xd_cfg.cpu_level < 0 || xd_cfg.cpu_level > 4) xd_cfg.cpu_level = 0;
    if (xd_cfg.gpu_level < 0 || xd_cfg.gpu_level > 4) xd_cfg.gpu_level = 0;
    if (xd_cfg.texture_width < 0) xd_cfg.texture_width = 0;
    if (xd_cfg.texture_height < 0) xd_cfg.texture_height = 0;

    xd_log(XD_INFO, "settings from %s: texture=%dx%d cpu=%d gpu=%d vibration=%.2f/%.2f verbose=%d "
                    "disable_space_warp=%d force_passthrough=%d disable_controller_offset=%d",
           xd_cfg.source, xd_cfg.texture_width, xd_cfg.texture_height, xd_cfg.cpu_level, xd_cfg.gpu_level,
           xd_cfg.vibration_amplitude, xd_cfg.vibration_frequency, xd_cfg.verbose, xd_cfg.disable_space_warp,
           xd_cfg.force_passthrough, xd_cfg.disable_controller_offset);
}

void xd_settings_load(void) { pthread_once(&g_settings_once, settings_init); }
