#pragma once

// Title-specific, opt-in presentation patches. Native DS execution never
// enables these paths.
void nds_title_patches_set_sm64ds_adaptive(bool enabled);
void nds_title_patches_start_frame();
