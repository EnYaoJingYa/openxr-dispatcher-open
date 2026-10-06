/* SPDX-License-Identifier: MIT
 * What the fake runtime (mock_next.c) saw; read by test_host.c through dlsym. */
#ifndef MOCK_STATE_H
#define MOCK_STATE_H
#include <openxr/openxr.h>

typedef struct {
    int init_calls, create_calls, gipa_calls, poll_calls, suggest_calls, haptic_calls, perf_calls;
    XrVersion api_version;
    uint32_t ext_count;
    char ext[32][XR_MAX_EXTENSION_NAME_SIZE];
    int android_chained;
    void *system_props_next;
    XrReferenceSpaceType space_type;
    float space_y, head_height;
    XrTime locate_time;
    XrSwapchainCreateInfo swapchain;
    uint32_t frame_layers;
    XrStructureType frame_types[8];
    XrCompositionLayerFlags frame_flags[8];
    int frame_view_chain, frame_view_chain_len, frame_layer_chain, frame_layer_chain_len;
    uint32_t touch_bindings;
    char touch_paths[16][XR_MAX_PATH_LENGTH];
    XrPath current_profile;
    XrHapticVibration last_vibration;
    XrPerfSettingsDomainEXT perf_domain;
    XrPerfSettingsLevelEXT perf_level;
} mock_state_t;

#endif
