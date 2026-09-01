#include "perf_governor.h"

#include <algorithm>
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
}

bool nds_perf_governor_update(NdsPerfGovernorState* state,
                              const NdsPerfGovernorSample& sample) {
    if (!state) return false;
    if (state->mode == NdsPerfGovernorMode::Off) {
        const bool changed = state->stage != 0;
        state->stage = 0;
        state->over_frames = 0;
        state->under_frames = 0;
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
        return changed;
    }

    const NdsPerfGovernorConfig& cfg = state->config;
    const bool over_budget =
        sample.underruns_delta != 0 ||
        sample.emu_ms > sample.budget_ms + cfg.over_budget_margin_ms ||
        sample.drain_ms <= 0.25;
    const bool recovered =
        sample.underruns_delta == 0 &&
        sample.emu_ms < sample.budget_ms - cfg.under_budget_margin_ms &&
        sample.drain_ms >= cfg.min_recovery_drain_ms;

    if (over_budget) {
        state->over_frames = std::min<uint32_t>(
            state->over_frames + 1u, cfg.engage_frames);
        state->under_frames = 0;
    } else if (recovered) {
        state->under_frames = std::min<uint32_t>(
            state->under_frames + 1u, cfg.restore_frames);
        state->over_frames = 0;
    } else {
        state->over_frames = 0;
        state->under_frames = 0;
    }

    if (over_budget && state->over_frames >= cfg.engage_frames &&
        state->stage < 2u) {
        ++state->stage;
        state->over_frames = 0;
        return true;
    }
    if (recovered && state->under_frames >= cfg.restore_frames &&
        state->stage > 0u) {
        --state->stage;
        state->under_frames = 0;
        return true;
    }
    return false;
}
