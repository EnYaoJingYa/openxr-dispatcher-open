/* SPDX-License-Identifier: MIT
 *
 * Interaction profiles, as OVRPort's dispatcher handles them.
 *
 * xrSuggestInteractionProfileBindings:
 *  1. Bindings Touch does not have are removed (paths containing proximity_fb,
 *     input/dpad_*, input/view, input/bumper, input/squeeze/touch,
 *     input/menu/touch), so a strict runtime accepts the rest.
 *  2. X, Y and menu bound on the right hand move to the left hand, where Touch
 *     has them.
 *  3. The same bindings are suggested again for alias profiles: Pico 4 / 4S /
 *     Neo 3 for Touch, Frame and Touch Plus games, and Touch for Frame games.
 *     Any accepted alias makes the call succeed.
 * xrGetCurrentInteractionProfile reports the game's own profile when the
 * runtime picked an alias.
 * OVRPort also moves Pico action spaces by a fixed grip offset; that offset is
 * OVRPort's own calibration and is not reproduced here (no Pico grip offset).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dispatch.h"

#define TOUCH "/interaction_profiles/oculus/touch_controller"
#define FRAME "/interaction_profiles/valve/frame_controller_valve"
#define TOUCH_PLUS "/interaction_profiles/meta/touch_controller_plus"
#define NEO3 "/interaction_profiles/bytedance/pico_neo3_controller"
#define PICO_NEO3 "/interaction_profiles/pico/neo3_controller"
#define PICO4 "/interaction_profiles/bytedance/pico4_controller"
#define PICO4S "/interaction_profiles/bytedance/pico4s_controller"
#define SHORT(p) (strncmp((p), "/interaction_profiles/", 22) ? (p) : (p) + 22)

static struct alias {
    const char *alias, *match;
    XrPath alias_path, game_profile;
} g_alias[] = {
    {NEO3, TOUCH, 0, 0}, {PICO4, TOUCH, 0, 0}, {PICO4S, TOUCH, 0, 0},
    {NEO3, FRAME, 0, 0}, {PICO4, FRAME, 0, 0}, {PICO4S, FRAME, 0, 0},
    {TOUCH, FRAME, 0, 0},
    {NEO3, TOUCH_PLUS, 0, 0}, {PICO4, TOUCH_PLUS, 0, 0}, {PICO4S, TOUCH_PLUS, 0, 0},
    {TOUCH, NEO3, 0, 0}, {TOUCH, PICO4, 0, 0}, {TOUCH, PICO4S, 0, 0},
    {TOUCH, PICO_NEO3, 0, 0},
};
#define ALIAS_COUNT (sizeof g_alias / sizeof g_alias[0])

static const char *const k_removed[] = {
    "proximity_fb", "input/dpad_up", "input/dpad_down", "input/dpad_left", "input/dpad_right",
    "input/view", "input/bumper", "input/squeeze/touch", "input/menu/touch",
};
static const char *const k_left[][2] = {
    {"/user/hand/right/input/y/touch", "/user/hand/left/input/y/touch"},
    {"/user/hand/right/input/x/touch", "/user/hand/left/input/x/touch"},
    {"/user/hand/right/input/menu/click", "/user/hand/left/input/menu/click"},
    {"/user/hand/right/input/y/click", "/user/hand/left/input/y/click"},
    {"/user/hand/right/input/x/click", "/user/hand/left/input/x/click"},
    {"/user/hand/right/input/a/click", "/user/hand/left/input/x/click"},
    {"/user/hand/right/input/b/click", "/user/hand/left/input/y/click"},
    {"/user/hand/right/input/a/touch", "/user/hand/left/input/x/touch"},
    {"/user/hand/right/input/b/touch", "/user/hand/left/input/y/touch"},
    {"/user/hand/left/input/back/click", "/user/hand/left/input/menu/click"},
    {"/user/hand/right/input/back/click", "/user/hand/left/input/menu/click"},
    {"/user/hand/left/input/home/click", "/user/hand/left/input/menu/click"},
    {"/user/hand/right/input/home/click", "/user/hand/left/input/menu/click"},
    {"/user/hand/left/input/system/click", "/user/hand/left/input/menu/click"},
    {"/user/hand/right/input/system/click", "/user/hand/left/input/menu/click"},
};

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(
    XrInstance instance, const XrInteractionProfileSuggestedBinding *suggested)
{
    PFN_xrSuggestInteractionProfileBindings fn = NEXT(xrSuggestInteractionProfileBindings);
    PFN_xrPathToString to_string = NEXT(xrPathToString);
    PFN_xrStringToPath to_path = NEXT(xrStringToPath);
    if (!fn)
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (!suggested || !to_string || !to_path || (suggested->countSuggestedBindings && !suggested->suggestedBindings))
        return fn(instance, suggested);

    char profile[XR_MAX_PATH_LENGTH] = "", path[XR_MAX_PATH_LENGTH];
    uint32_t len = 0;
    to_string(instance, suggested->interactionProfile, sizeof profile, &len, profile);

    uint32_t n = suggested->countSuggestedBindings, kept = 0, moved = 0;
    XrActionSuggestedBinding *b = calloc(n ? n : 1, sizeof *b);
    if (!b)
        return XR_ERROR_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < n; i++) {
        XrActionSuggestedBinding one = suggested->suggestedBindings[i];
        if (XR_FAILED(to_string(instance, one.binding, sizeof path, &len, path))) {
            b[kept++] = one;
            continue;
        }
        bool removed = false;
        for (size_t k = 0; k < sizeof k_removed / sizeof k_removed[0] && !removed; k++)
            removed = strstr(path, k_removed[k]) != NULL;
        if (removed) {
            char key[96];
            snprintf(key, sizeof key, "rm:%.40s:%.40s", SHORT(profile), path);
            if (xd_first(key))
                xd_log(XD_INFO, "bindings: %s: %s removed (no such input on Touch)", profile, path);
            continue;
        }
        for (size_t k = 0; k < sizeof k_left / sizeof k_left[0]; k++)
            if (strstr(path, k_left[k][0]) && XR_SUCCEEDED(to_path(instance, k_left[k][1], &one.binding))) {
                moved++;
                break;
            }
        b[kept++] = one;
    }

    XrInteractionProfileSuggestedBinding copy = *suggested;
    copy.suggestedBindings = b;
    copy.countSuggestedBindings = kept;
    bool alias_ok = false;
    char aliases[512] = "";
    size_t used = 0;
    for (size_t i = 0; i < ALIAS_COUNT; i++) {
        struct alias *a = &g_alias[i];
        if (!strcasestr(profile, a->match))
            continue;
        a->game_profile = suggested->interactionProfile;
        if (XR_FAILED(to_path(instance, a->alias, &a->alias_path)))
            continue;
        XrInteractionProfileSuggestedBinding as = copy;
        as.interactionProfile = a->alias_path;
        XrResult r = fn(instance, &as);
        alias_ok |= XR_SUCCEEDED(r);
        int w = snprintf(aliases + used, sizeof aliases - used, " %s=%s", a->alias + 22, XR_SUCCEEDED(r) ? "ok" : "no");
        if (w > 0 && used + (size_t)w < sizeof aliases)
            used += (size_t)w;
    }
    XrResult r = fn(instance, &copy);
    free(b);
    char key[96];
    snprintf(key, sizeof key, "suggest:%.60s:%d", profile, r);
    if (xd_first(key))
        xd_log(XD_INFO, "bindings: %s: %u of %u kept, %u moved to the left hand -> %s; aliases:%s", profile, kept, n,
               moved, xd_result_name(r), *aliases ? aliases : " none");
    return alias_ok ? XR_SUCCESS : r;
}

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrGetCurrentInteractionProfile(XrSession session, XrPath user,
                                                                       XrInteractionProfileState *state)
{
    PFN_xrGetCurrentInteractionProfile fn = NEXT(xrGetCurrentInteractionProfile);
    XrResult r = fn ? fn(session, user, state) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (state && state->interactionProfile)
        for (size_t i = 0; i < ALIAS_COUNT; i++)
            if (g_alias[i].alias_path && state->interactionProfile == g_alias[i].alias_path) {
                state->interactionProfile = g_alias[i].game_profile;
                break;
            }
    xd_trace("xrGetCurrentInteractionProfile", r);
    return r;
}

XD_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrDestroyAction(XrAction action)
{
    PFN_xrDestroyAction fn = NEXT(xrDestroyAction);
    XrResult r = fn ? fn(action) : XR_ERROR_FUNCTION_UNSUPPORTED;
    xd_trace("xrDestroyAction", r);
    return r;
}
