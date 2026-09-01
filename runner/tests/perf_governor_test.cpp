#include "perf_governor.h"

namespace {

bool require(bool condition) {
    return condition;
}

NdsPerfGovernorSample over() {
    NdsPerfGovernorSample s{};
    s.emu_ms = 18.0;
    s.drain_ms = 0.0;
    return s;
}

NdsPerfGovernorSample under() {
    NdsPerfGovernorSample s{};
    s.emu_ms = 10.0;
    s.drain_ms = 5.0;
    return s;
}

}  // namespace

int main() {
    NdsPerfGovernorConfig cfg{};
    cfg.engage_frames = 3;
    cfg.restore_frames = 4;
    NdsPerfGovernorState state{};
    nds_perf_governor_init(&state, NdsPerfGovernorMode::Auto, cfg);
    if (!require(state.stage == 0)) return 1;

    for (int i = 0; i < 2; ++i)
        if (!require(!nds_perf_governor_update(&state, over())) ||
            !require(state.stage == 0))
            return 2;
    if (!require(nds_perf_governor_update(&state, over())) ||
        !require(state.stage == 1))
        return 3;
    for (int i = 0; i < 2; ++i)
        if (!require(!nds_perf_governor_update(&state, over())) ||
            !require(state.stage == 1))
            return 4;
    if (!require(nds_perf_governor_update(&state, over())) ||
        !require(state.stage == 2))
        return 5;

    for (int i = 0; i < 3; ++i)
        if (!require(!nds_perf_governor_update(&state, under())) ||
            !require(state.stage == 2))
            return 6;
    if (!require(nds_perf_governor_update(&state, under())) ||
        !require(state.stage == 1))
        return 7;
    for (int i = 0; i < 3; ++i)
        if (!require(!nds_perf_governor_update(&state, under())) ||
            !require(state.stage == 1))
            return 8;
    if (!require(nds_perf_governor_update(&state, under())) ||
        !require(state.stage == 0))
        return 9;

    nds_perf_governor_init(&state, NdsPerfGovernorMode::Off, cfg);
    if (!require(state.stage == 0) ||
        !require(!nds_perf_governor_update(&state, over())) ||
        !require(state.stage == 0))
        return 10;
    nds_perf_governor_init(&state, NdsPerfGovernorMode::ForceStage1, cfg);
    if (!require(state.stage == 1) ||
        !require(!nds_perf_governor_update(&state, under())) ||
        !require(state.stage == 1))
        return 11;
    nds_perf_governor_init(&state, NdsPerfGovernorMode::ForceStage2, cfg);
    if (!require(state.stage == 2) ||
        !require(!nds_perf_governor_update(&state, under())) ||
        !require(state.stage == 2))
        return 12;

    NdsPerfGovernorMode mode = NdsPerfGovernorMode::Invalid;
    if (!require(nds_parse_perf_governor_mode("auto", &mode)) ||
        !require(mode == NdsPerfGovernorMode::Auto) ||
        !require(nds_parse_perf_governor_mode("off", &mode)) ||
        !require(mode == NdsPerfGovernorMode::Off) ||
        !require(nds_parse_perf_governor_mode("stage1", &mode)) ||
        !require(mode == NdsPerfGovernorMode::ForceStage1) ||
        !require(nds_parse_perf_governor_mode("2", &mode)) ||
        !require(mode == NdsPerfGovernorMode::ForceStage2) ||
        !require(!nds_parse_perf_governor_mode("maybe", &mode)))
        return 13;

    return 0;
}
