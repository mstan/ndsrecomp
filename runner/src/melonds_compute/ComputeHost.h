// Host-context owner for the opt-in melonDS compute renderer.
// GPL-3.0-or-later; see THIRD_PARTY_ATTRIBUTION.md.
#pragma once

struct SDL_Window;
struct NdsGpu2dDirectFrame;

struct NdsComputePresentTicks {
    unsigned long long upload = 0;
    unsigned long long draw = 0;
    unsigned long long swap = 0;
};

bool nds_compute_host_start(SDL_Window* presentation_window = nullptr);
bool nds_compute_host_make_current();
bool nds_compute_host_has_visible_context();
bool nds_compute_host_present_top(const unsigned int* fallback_pixels,
                                  unsigned short fallback_width,
                                  const NdsGpu2dDirectFrame* direct_frame,
                                  NdsComputePresentTicks* ticks);
void nds_compute_host_stop();
bool nds_compute_host_active();

// GL identification strings, captured once when the context comes up. These
// were previously only printed to stderr, which meant a field diagnostics
// bundle could not say which GPU/driver produced it -- the single most
// load-bearing fact when a performance report arrives from a machine nobody
// has access to. Empty strings until a context has been created.
const char* nds_compute_host_gl_renderer();
const char* nds_compute_host_gl_vendor();
const char* nds_compute_host_gl_version();
