#pragma once

#include <cstdint>

enum class NdsPerfGovernorMode : uint8_t {
    Auto,
    Off,
    ForceStage1,
    ForceStage2,
    Invalid,
};

struct NdsPerfGovernorConfig {
    uint32_t engage_frames = 30;
    uint32_t restore_frames = 180;
    double over_budget_margin_ms = 0.5;
    double under_budget_margin_ms = 2.0;
    double min_recovery_drain_ms = 2.0;
};

struct NdsPerfGovernorSample {
    double emu_ms = 0.0;
    double drain_ms = 0.0;
    double budget_ms = 1000.0 / 60.0;
    uint64_t underruns_delta = 0;
    double compute_map_ms = 0.0;
};

struct NdsPerfGovernorState {
    NdsPerfGovernorMode mode = NdsPerfGovernorMode::Auto;
    NdsPerfGovernorConfig config{};
    uint8_t stage = 0;
    uint32_t over_frames = 0;
    uint32_t under_frames = 0;
};

const char* nds_perf_governor_mode_name(NdsPerfGovernorMode mode);
bool nds_parse_perf_governor_mode(const char* value,
                                  NdsPerfGovernorMode* out);
void nds_perf_governor_init(NdsPerfGovernorState* state,
                            NdsPerfGovernorMode mode,
                            NdsPerfGovernorConfig config = {});
bool nds_perf_governor_update(NdsPerfGovernorState* state,
                              const NdsPerfGovernorSample& sample);
