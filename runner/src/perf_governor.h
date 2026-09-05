#pragma once

#include <cstdint>

enum class NdsPerfGovernorMode : uint8_t {
    Auto,
    Off,
    ForceStage1,
    ForceStage2,
    Invalid,
};

// Why the last transition happened. Derived from the predicate that fired, not
// from the resulting stage: a 2->1 restore and a 0->1 escalation both change
// the stage, and only the predicate says which direction the machine moved in.
enum class NdsPerfGovernorReason : uint8_t {
    None,
    Initial,
    Forced,
    Off,
    OverBudget,
    Recovered,
    ApplyFailed,
};

struct NdsPerfGovernorConfig {
    uint32_t engage_frames = 30;
    uint32_t restore_frames = 180;
    double over_budget_margin_ms = 0.5;
    double under_budget_margin_ms = 2.0;
    double min_recovery_drain_ms = 2.0;
    // The host has no pacing headroom left when the audio drain sleeps for
    // (essentially) nothing: the emulator is consuming the entire frame.
    double max_over_drain_ms = 0.25;
    // Flap guard. A machine that cannot sustain the configured internal
    // resolution otherwise cycles 2->1->2 forever, and every re-engagement is
    // a synchronous glFinish + shader rebuild hitch. After this many stage-2
    // engagements inside flap_window_frames, stage 2 is held: the restore
    // predicate stops being able to leave it. The window only counts frames
    // spent BELOW stage 2, so a single transient spike followed by a long
    // healthy stretch retires the guard, while time spent in the reduced mode
    // is never taken as evidence the full mode is sustainable.
    uint32_t flap_engage_limit = 2;
    uint32_t flap_window_frames = 3600;
};

struct NdsPerfGovernorSample {
    double emu_ms = 0.0;
    double drain_ms = 0.0;
    double budget_ms = 1000.0 / 60.0;
    uint64_t underruns_delta = 0;
    double compute_map_ms = 0.0;
    // True only when drain_ms is a real headroom signal: an audio device
    // exists, playback has started (so the drain actually paces), and turbo is
    // not bypassing the drain. When false the drain clause is NEUTRAL - it
    // neither reports over-budget nor blocks recovery - because a no-audio or
    // turbo frame reads drain ~= 0 for reasons that say nothing about load.
    bool headroom_valid = false;
};

struct NdsPerfGovernorState {
    NdsPerfGovernorMode mode = NdsPerfGovernorMode::Auto;
    NdsPerfGovernorConfig config{};
    uint8_t stage = 0;
    uint32_t over_frames = 0;
    uint32_t under_frames = 0;
    // Flap guard bookkeeping (see NdsPerfGovernorConfig).
    uint32_t stage2_engagements = 0;
    uint32_t frames_since_stage2_engage = 0;
    bool stage2_held = false;
    // Terminal state: a stage could not be applied to the renderer, so the
    // governor stops deciding anything rather than retrying a failing rebuild
    // every engage_frames forever.
    bool apply_failed = false;
    NdsPerfGovernorReason last_reason = NdsPerfGovernorReason::None;
};

const char* nds_perf_governor_mode_name(NdsPerfGovernorMode mode);
const char* nds_perf_governor_reason_name(NdsPerfGovernorReason reason);
bool nds_parse_perf_governor_mode(const char* value,
                                  NdsPerfGovernorMode* out);
void nds_perf_governor_init(NdsPerfGovernorState* state,
                            NdsPerfGovernorMode mode,
                            NdsPerfGovernorConfig config = {});
// Returns true when the caller must apply state->stage. Never returns true for
// a no-op N->N move, and never returns true again once apply has failed.
bool nds_perf_governor_update(NdsPerfGovernorState* state,
                              const NdsPerfGovernorSample& sample);
// Enter the terminal "apply failed" state. state->stage must already hold the
// stage that is actually installed.
void nds_perf_governor_mark_apply_failed(NdsPerfGovernorState* state);

// ── Always-on transition ring ───────────────────────────────────────────
//
// Stage transitions are rare and are exactly what a field report needs, so
// they are captured unconditionally - not behind the performance log. The
// diagnostics writer and the debug server both read this ring retroactively;
// neither has to be armed before the interesting transition happens.
//
// Written and read only from the emu thread (the frontend loop records, and
// both the diagnostics writer and the debug-server pump run at between-frames
// safe points on that same thread), so no lock is needed.
struct NdsPerfGovernorTransition {
    uint64_t frame_index;
    uint64_t ts_ms;
    uint8_t from_stage;
    uint8_t to_stage;
    NdsPerfGovernorReason reason;
    bool stage2_held;
    bool apply_failed;
};
constexpr uint32_t kNdsPerfGovernorHistoryCapacity = 64u;
void nds_perf_governor_record_transition(uint8_t from_stage, uint8_t to_stage,
                                         NdsPerfGovernorReason reason,
                                         uint64_t frame_index,
                                         bool stage2_held,
                                         bool apply_failed);
// Copies up to max entries, oldest first, into out. Returns how many were
// written. Total transitions ever recorded (which can exceed the ring
// capacity) is nds_perf_governor_history_total().
uint32_t nds_perf_governor_history(NdsPerfGovernorTransition* out,
                                   uint32_t max);
uint64_t nds_perf_governor_history_total();
void nds_perf_governor_history_reset();
