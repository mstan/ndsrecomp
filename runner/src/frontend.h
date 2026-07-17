#pragma once

#include <cstdint>

// Run the native, human-facing firmware preview. The emulation remains on the
// same scheduler/device path used by the deterministic debug verifier; SDL is
// only the host presentation and input/audio transport.
int nds_run_interactive_frontend();

// Live frontend counters for the play-mode debug surface (`frontend_stats`).
// Cumulative since frontend start; a client samples twice and derives fps /
// phase shares over its own window. Zeros (active=0) when no frontend runs.
// Written and read on the frontend thread only (debug_pump() safe point).
struct NdsFrontendLiveStats {
    int active;
    uint64_t frames;          // presented frames
    uint64_t emu_ticks;       // cumulative emulation phase (perf ticks)
    uint64_t present_ticks;   // cumulative present phase
    uint64_t drain_ticks;     // cumulative audio-drain phase
    uint64_t now_ticks;       // performance counter at query time
    uint64_t freq;            // performance frequency (ticks/second)
    uint64_t underruns;       // audio underruns so far
};
void nds_frontend_live_stats(NdsFrontendLiveStats* out);
