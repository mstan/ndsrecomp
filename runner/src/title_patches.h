#pragma once

#include <cstdint>

struct NdsTitlePatchDebugState {
    bool mph_adventure_wide_enabled;
    bool mph_adventure_wide_active;
    uint16_t mph_adventure_wide_width;
    uint64_t mph_adventure_wide_site_applied[3];
    uint64_t mph_adventure_wide_frames_active;
    uint64_t mph_adventure_wide_frames_inactive;
};

// Title-specific, opt-in presentation patches. Native DS execution never
// enables these paths.
void nds_title_patches_set_sm64ds_adaptive(bool enabled);
void nds_title_patches_set_mph_mouse_aim(bool enabled);
// MPH adventure mode: rebuild the guest's own frusta at the host's adaptive
// top width so per-room sub-frusta and entity sphere culling cover the whole
// band. `adaptive_width` must equal the host 3D render width.
void nds_title_patches_set_mph_adventure_wide(bool enabled,
                                              uint16_t adaptive_width);
bool nds_title_patches_apply_mph_mouse_delta(int32_t dx, int32_t dy);
NdsTitlePatchDebugState nds_title_patches_debug_state();
void nds_title_patches_start_frame();
