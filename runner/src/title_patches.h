#pragma once

#include <cstdint>

struct NdsTitlePatchDebugState {
    bool mph_adaptive_room_culling;
    uint64_t mph_portal_clip_calls;
    uint64_t mph_portal_min_x_relaxed;
    uint64_t mph_viewport_patches;
    int32_t mph_last_viewport_min_x;
    int32_t mph_last_viewport_max_x;
    int32_t mph_last_viewport_min_y;
    int32_t mph_last_viewport_max_y;
};

// Title-specific, opt-in presentation patches. Native DS execution never
// enables these paths.
void nds_title_patches_set_sm64ds_adaptive(bool enabled);
void nds_title_patches_set_mph_mouse_aim(bool enabled);
void nds_title_patches_set_mph_adaptive_room_culling(bool enabled);
bool nds_title_patches_apply_mph_mouse_delta(int32_t dx, int32_t dy);
bool nds_title_patches_handle_literal_branch(uint32_t source_pc,
                                             uint32_t target_pc);
bool nds_title_patches_handle_literal_call(uint32_t target_pc);
NdsTitlePatchDebugState nds_title_patches_debug_state();
void nds_title_patches_start_frame();
