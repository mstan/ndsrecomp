// GPU texture upscaling for the vendored melonDS texture cache.
//
// This file and TextureUpscale.cpp are project-owned. The GLSL rule set in
// the .cpp is adapted from Hyllian's xBR-lv2 shader, which is MIT licensed
// -- see THIRD_PARTY_ATTRIBUTION.md. It is deliberately NOT placed under
// runner/vendor/melonds/, so the MIT source stays outside the GPL-3.0
// vendored tree and its provenance stays legible.
//
// Why here rather than at sample time: the DS reuses each decoded texture
// across many polygons and many frames, so filtering once per cache miss is
// far cheaper than filtering per sampled pixel, and it leaves the renderer's
// hot texture-fetch path untouched.
#pragma once

#include <cstdint>

// Valid factors are 1 (disabled), 2, and 4. Must be set before the compute
// renderer builds its texture cache: the factor sizes every texture array
// allocation, and the cache's free-list is derived from it.
void nds_texture_upscale_set_factor(int factor);
int nds_texture_upscale_factor();

// True once the compute program exists. Failure leaves the factor at 1 so
// the texture cache silently keeps native uploads rather than breaking.
bool nds_texture_upscale_ready();

// Compiles the compute program and allocates the staging texture. Requires a
// current GL 4.3 context. Safe to call repeatedly.
bool nds_texture_upscale_init();
void nds_texture_upscale_shutdown();

// Filters one decoded RGB6A5 texture (width x height, four bytes per texel:
// r, g, b at 0..63 and alpha at 0..31) into the given array layer, which the
// caller must have allocated at width*factor x height*factor.
//
// Returns false if the dispatch could not run, in which case the caller must
// fall back to a plain native upload.
bool nds_texture_upscale_dispatch(uint32_t dst_array, uint32_t width,
                                  uint32_t height, uint32_t layer,
                                  const void* decoded);
