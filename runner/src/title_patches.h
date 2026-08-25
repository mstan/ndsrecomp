#pragma once

#include <cstdint>

// Title-specific, opt-in presentation patches. Native DS execution never
// enables these paths.
void nds_title_patches_set_sm64ds_adaptive(bool enabled);
void nds_title_patches_set_mph_mouse_aim(bool enabled);
bool nds_title_patches_apply_mph_mouse_delta(int32_t dx, int32_t dy);
void nds_title_patches_start_frame();
