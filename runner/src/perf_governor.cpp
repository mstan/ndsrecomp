#include "perf_governor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>

const char* nds_perf_governor_mode_name(NdsPerfGovernorMode mode) {
    switch (mode) {
        case NdsPerfGovernorMode::Auto: return "auto";
        case NdsPerfGovernorMode::Off: return "off";
        case NdsPerfGovernorMode::ForceStage1: return "stage1";
        case NdsPerfGovernorMode::ForceStage2: return "stage2";
        default: return "invalid";
    }
}

const char* nds_perf_governor_reason_name(NdsPerfGovernorReason reason) {
    switch (reason) {
        case NdsPerfGovernorReason::Initial: return "initial";
        case NdsPerfGovernorReason::Forced: return "forced";
        case NdsPerfGovernorReason::Off: return "off";
        case NdsPerfGovernorReason::OverBudget: return "over_budget";
        case NdsPerfGovernorReason::Recovered: return "recovered";
        case NdsPerfGovernorReason::ApplyFailed: return "apply_failed";
        default: return "none";
    }
}

bool nds_parse_perf_governor_mode(const char* value,
                                  NdsPerfGovernorMode* out) {
    if (!value || !*value || !out) return false;
    if (std::strcmp(value, "auto") == 0) {
        *out = NdsPerfGovernorMode::Auto;
        return true;
    }
    if (std::strcmp(value, "off") == 0) {
        *out = NdsPerfGovernorMode::Off;
        return true;
    }
    if (std::strcmp(value, "stage1") == 0 || std::strcmp(value, "1") == 0) {
        *out = NdsPerfGovernorMode::ForceStage1;
        return true;
    }
    if (std::strcmp(value, "stage2") == 0 || std::strcmp(value, "2") == 0) {
        *out = NdsPerfGovernorMode::ForceStage2;
        return true;
    }
    return false;
}

void nds_perf_governor_init(NdsPerfGovernorState* state,
                            NdsPerfGovernorMode mode,
                            NdsPerfGovernorConfig config) {
    if (!state) return;
    *state = {};
    state->mode = mode;
    state->config = config;
    if (mode == NdsPerfGovernorMode::ForceStage1)
        state->stage = 1;
    else if (mode == NdsPerfGovernorMode::ForceStage2)
        state->stage = 2;
    if (state->stage != 0)
        state->last_reason = NdsPerfGovernorReason::Initial;
}

void nds_perf_governor_mark_apply_failed(NdsPerfGovernorState* state) {
    if (!state) return;
    state->apply_failed = true;
    state->over_frames = 0;
    state->under_frames = 0;
    state->last_reason = NdsPerfGovernorReason::ApplyFailed;
}

bool nds_perf_governor_update(NdsPerfGovernorState* state,
                              const NdsPerfGovernorSample& sample) {
    if (!state) return false;
    // Terminal: the installed stage is whatever survived the failure and the
    // governor no longer asks for anything.
    if (state->apply_failed) return false;
    if (state->mode == NdsPerfGovernorMode::Off) {
        const bool changed = state->stage != 0;
        state->stage = 0;
        state->over_frames = 0;
        state->under_frames = 0;
        state->stage2_engagements = 0;
        state->frames_since_stage2_engage = 0;
        state->stage2_held = false;
        if (changed) state->last_reason = NdsPerfGovernorReason::Off;
        return changed;
    }
    if (state->mode == NdsPerfGovernorMode::ForceStage1 ||
        state->mode == NdsPerfGovernorMode::ForceStage2) {
        const uint8_t forced =
            state->mode == NdsPerfGovernorMode::ForceStage1 ? 1u : 2u;
        const bool changed = state->stage != forced;
        state->stage = forced;
        state->over_frames = 0;
        state->under_frames = 0;
        if (changed) state->last_reason = NdsPerfGovernorReason::Forced;
        return changed;
    }

    const NdsPerfGovernorConfig& cfg = state->config;

    // Flap-guard window decay. A stretch spent BELOW stage 2 retires the
    // guard, so a single transient load spike cannot pin the machine at
    // stage 2 for the session. Frames spent at stage 2 never decay it: the
    // window measures "how soon did we have to come back", and time spent in
    // the reduced mode is not evidence that the full mode is sustainable.
    if (state->stage2_engagements != 0u && state->stage < 2u) {
        if (++state->frames_since_stage2_engage >= cfg.flap_window_frames) {
            state->stage2_engagements = 0;
            state->frames_since_stage2_engage = 0;
            state->stage2_held = false;
        }
    }

    // The drain clause only means anything when the drain is actually the
    // host's pacing sleep this frame. Without that signal it is neutral on
    // both directions, never over-budget: drain_audio() returns immediately
    // with no audio device, does not sleep before playback starts, and is
    // bypassed entirely under turbo.
    const bool drain_over =
        sample.headroom_valid && sample.drain_ms <= cfg.max_over_drain_ms;
    const bool drain_recovered =
        !sample.headroom_valid || sample.drain_ms >= cfg.min_recovery_drain_ms;

    const bool over_budget =
        sample.underruns_delta != 0 ||
        sample.emu_ms > sample.budget_ms + cfg.over_budget_margin_ms ||
        drain_over;
    const bool recovered =
        sample.underruns_delta == 0 &&
        sample.emu_ms < sample.budget_ms - cfg.under_budget_margin_ms &&
        drain_recovered;

    if (over_budget) {
        state->over_frames = std::min<uint32_t>(
            state->over_frames + 1u, cfg.engage_frames);
        state->under_frames = 0;
    } else if (recovered) {
        state->under_frames = std::min<uint32_t>(
            state->under_frames + 1u, cfg.restore_frames);
        state->over_frames = 0;
    } else {
        // Dead zone: a single interrupting frame breaks the streak, which is
        // what makes engage_frames mean "sustained".
        state->over_frames = 0;
        state->under_frames = 0;
    }

    if (over_budget && state->over_frames >= cfg.engage_frames &&
        state->stage < 2u) {
        ++state->stage;
        state->over_frames = 0;
        state->last_reason = NdsPerfGovernorReason::OverBudget;
        if (state->stage == 2u) {
            ++state->stage2_engagements;
            state->frames_since_stage2_engage = 0;
            if (state->stage2_engagements >= cfg.flap_engage_limit)
                state->stage2_held = true;
        }
        return true;
    }
    if (recovered && state->under_frames >= cfg.restore_frames &&
        state->stage > 0u) {
        if (state->stage == 2u && state->stage2_held) {
            // Held: this machine has already proved twice that it cannot
            // sustain the configured mode, and each restore costs a
            // synchronous rebuild hitch. Stay put and report the hold.
            return false;
        }
        --state->stage;
        state->under_frames = 0;
        state->last_reason = NdsPerfGovernorReason::Recovered;
        return true;
    }
    return false;
}

// ── Always-on transition ring ───────────────────────────────────────────

namespace {

std::array<NdsPerfGovernorTransition, kNdsPerfGovernorHistoryCapacity>
    g_history{};
uint64_t g_history_total = 0;

uint64_t unix_ms_now() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

}  // namespace

void nds_perf_governor_record_transition(uint8_t from_stage, uint8_t to_stage,
                                         NdsPerfGovernorReason reason,
                                         uint64_t frame_index,
                                         bool stage2_held,
                                         bool apply_failed) {
    NdsPerfGovernorTransition& slot =
        g_history[static_cast<size_t>(
            g_history_total % kNdsPerfGovernorHistoryCapacity)];
    slot.frame_index = frame_index;
    slot.ts_ms = unix_ms_now();
    slot.from_stage = from_stage;
    slot.to_stage = to_stage;
    slot.reason = reason;
    slot.stage2_held = stage2_held;
    slot.apply_failed = apply_failed;
    ++g_history_total;
}

uint32_t nds_perf_governor_history(NdsPerfGovernorTransition* out,
                                   uint32_t max) {
    if (!out || max == 0u) return 0u;
    const uint64_t held = std::min<uint64_t>(
        g_history_total, kNdsPerfGovernorHistoryCapacity);
    const uint32_t count = static_cast<uint32_t>(
        std::min<uint64_t>(held, max));
    // Oldest first, ending at the newest entry.
    const uint64_t first = g_history_total - count;
    for (uint32_t i = 0; i < count; ++i)
        out[i] = g_history[static_cast<size_t>(
            (first + i) % kNdsPerfGovernorHistoryCapacity)];
    return count;
}

uint64_t nds_perf_governor_history_total() { return g_history_total; }

void nds_perf_governor_history_reset() {
    g_history = {};
    g_history_total = 0;
}
