#include "perf_governor.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(condition)                                                  \
    do {                                                                  \
        if (!(condition)) {                                               \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n",             \
                         __FILE__, __LINE__, #condition);                 \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

constexpr double kBudgetMs = 1000.0 / 60.0;

NdsPerfGovernorSample base() {
    NdsPerfGovernorSample s{};
    s.budget_ms = kBudgetMs;
    s.headroom_valid = true;
    return s;
}

// Over budget on the emu-time clause only: the drain is a healthy pacing
// sleep, so the drain clause reports neither over nor blocked recovery.
NdsPerfGovernorSample over_emu() {
    NdsPerfGovernorSample s = base();
    s.emu_ms = 18.0;
    s.drain_ms = 5.0;
    return s;
}

// Over budget on the drain clause only: emu time is comfortable but the host
// has no pacing headroom left.
NdsPerfGovernorSample over_drain() {
    NdsPerfGovernorSample s = base();
    s.emu_ms = 10.0;
    s.drain_ms = 0.0;
    return s;
}

// The same frame with no headroom signal available. The drain clause must be
// neutral here, never over-budget, and must not block recovery.
NdsPerfGovernorSample over_drain_no_headroom() {
    NdsPerfGovernorSample s = over_drain();
    s.headroom_valid = false;
    return s;
}

// Over budget on the underrun clause only.
NdsPerfGovernorSample over_underruns() {
    NdsPerfGovernorSample s = base();
    s.emu_ms = 10.0;
    s.drain_ms = 5.0;
    s.underruns_delta = 1;
    return s;
}

NdsPerfGovernorSample under() {
    NdsPerfGovernorSample s = base();
    s.emu_ms = 10.0;
    s.drain_ms = 5.0;
    return s;
}

// Dead zone: exactly at budget, with a drain between the two thresholds.
// Neither predicate fires.
NdsPerfGovernorSample neutral() {
    NdsPerfGovernorSample s = base();
    s.emu_ms = kBudgetMs;
    s.drain_ms = 1.0;
    return s;
}

NdsPerfGovernorConfig small_config() {
    NdsPerfGovernorConfig cfg{};
    cfg.engage_frames = 3;
    cfg.restore_frames = 4;
    return cfg;
}

void feed(NdsPerfGovernorState& state, const NdsPerfGovernorSample& sample,
          uint32_t times, bool expect_change = false) {
    for (uint32_t i = 0; i < times; ++i)
        CHECK(nds_perf_governor_update(&state, sample) == expect_change);
}

// ---- The original escalate/restore ladder ----

void test_ladder() {
    NdsPerfGovernorState state{};
    nds_perf_governor_init(&state, NdsPerfGovernorMode::Auto, small_config());
    CHECK(state.stage == 0);
    CHECK(state.last_reason == NdsPerfGovernorReason::None);

    feed(state, over_emu(), 2);
    CHECK(state.stage == 0);
    CHECK(nds_perf_governor_update(&state, over_emu()));
    CHECK(state.stage == 1);
    CHECK(state.last_reason == NdsPerfGovernorReason::OverBudget);

    feed(state, over_emu(), 2);
    CHECK(state.stage == 1);
    CHECK(nds_perf_governor_update(&state, over_emu()));
    CHECK(state.stage == 2);

    feed(state, under(), 3);
    CHECK(state.stage == 2);
    CHECK(nds_perf_governor_update(&state, under()));
    CHECK(state.stage == 1);
    // Finding H: a 2->1 restore is a recovery, not an over-budget event.
    CHECK(state.last_reason == NdsPerfGovernorReason::Recovered);

    feed(state, under(), 3);
    CHECK(state.stage == 1);
    CHECK(nds_perf_governor_update(&state, under()));
    CHECK(state.stage == 0);
    CHECK(state.last_reason == NdsPerfGovernorReason::Recovered);
}

// (1) A single interrupting frame resets the engage counter.
void test_streak_is_broken_by_one_frame() {
    NdsPerfGovernorConfig cfg{};  // default 30/180
    NdsPerfGovernorState state{};
    nds_perf_governor_init(&state, NdsPerfGovernorMode::Auto, cfg);

    feed(state, over_emu(), 29);
    CHECK(state.over_frames == 29);
    // One neutral frame in the middle of the streak.
    CHECK(!nds_perf_governor_update(&state, neutral()));
    CHECK(state.over_frames == 0);
    CHECK(state.stage == 0);
    // 29 more must still not engage.
    feed(state, over_emu(), 29);
    CHECK(state.stage == 0);
    CHECK(nds_perf_governor_update(&state, over_emu()));
    CHECK(state.stage == 1);

    // Same on the restore side.
    feed(state, under(), 179);
    CHECK(state.under_frames == 179);
    CHECK(!nds_perf_governor_update(&state, neutral()));
    CHECK(state.under_frames == 0);
    CHECK(state.stage == 1);
    feed(state, under(), 179);
    CHECK(state.stage == 1);
    CHECK(nds_perf_governor_update(&state, under()));
    CHECK(state.stage == 0);
}

// (2) The dead zone is neutral in both directions.
void test_dead_zone() {
    NdsPerfGovernorState state{};
    nds_perf_governor_init(&state, NdsPerfGovernorMode::Auto, small_config());
    feed(state, neutral(), 50);
    CHECK(state.stage == 0);
    CHECK(state.over_frames == 0);
    CHECK(state.under_frames == 0);

    // A drain between max_over_drain_ms and min_recovery_drain_ms is neutral
    // even when emu time is comfortable.
    NdsPerfGovernorSample s = under();
    s.drain_ms = 1.0;
    feed(state, s, 50);
    CHECK(state.stage == 0);
    CHECK(state.under_frames == 0);

    // ... and at stage 1 it neither escalates nor restores.
    nds_perf_governor_init(&state, NdsPerfGovernorMode::Auto, small_config());
    feed(state, over_emu(), 2);
    CHECK(nds_perf_governor_update(&state, over_emu()));
    CHECK(state.stage == 1);
    feed(state, neutral(), 50);
    CHECK(state.stage == 1);
}

// (3) Each over-budget clause in isolation.
void test_clauses_in_isolation() {
    // emu-time clause.
    NdsPerfGovernorState state{};
    nds_perf_governor_init(&state, NdsPerfGovernorMode::Auto, small_config());
    feed(state, over_emu(), 2);
    CHECK(nds_perf_governor_update(&state, over_emu()));
    CHECK(state.stage == 1);

    // drain clause, headroom available.
    nds_perf_governor_init(&state, NdsPerfGovernorMode::Auto, small_config());
    feed(state, over_drain(), 2);
    CHECK(state.over_frames == 2);
    CHECK(nds_perf_governor_update(&state, over_drain()));
    CHECK(state.stage == 1);

    // drain clause, NO headroom available (finding A). A no-audio, pre-
    // playback or turbo frame must never escalate on the drain, and must not
    // be prevented from recovering either: this sample is a recovery.
    nds_perf_governor_init(&state, NdsPerfGovernorMode::Auto, small_config());
    feed(state, over_drain_no_headroom(), 100);
    CHECK(state.stage == 0);
    CHECK(state.over_frames == 0);
    CHECK(state.under_frames == small_config().restore_frames);

    // The same drain, with no headroom, restores an engaged stage rather than
    // pinning it: otherwise a run without audio could never come back.
    nds_perf_governor_init(&state, NdsPerfGovernorMode::Auto, small_config());
    feed(state, over_emu(), 2);
    CHECK(nds_perf_governor_update(&state, over_emu()));
    CHECK(state.stage == 1);
    feed(state, over_drain_no_headroom(), 3);
    CHECK(state.stage == 1);
    CHECK(nds_perf_governor_update(&state, over_drain_no_headroom()));
    CHECK(state.stage == 0);
}

// (4) underruns_delta alone.
void test_underruns_clause() {
    NdsPerfGovernorState state{};
    nds_perf_governor_init(&state, NdsPerfGovernorMode::Auto, small_config());
    feed(state, over_underruns(), 2);
    CHECK(state.stage == 0);
    CHECK(nds_perf_governor_update(&state, over_underruns()));
    CHECK(state.stage == 1);
    CHECK(state.last_reason == NdsPerfGovernorReason::OverBudget);

    // An underrun also blocks recovery on its own: emu time and drain are both
    // healthy in this sample.
    NdsPerfGovernorSample s = under();
    s.underruns_delta = 1;
    nds_perf_governor_init(&state, NdsPerfGovernorMode::Auto, small_config());
    feed(state, over_emu(), 2);
    CHECK(nds_perf_governor_update(&state, over_emu()));
    CHECK(state.stage == 1);
    // Every one of these is over-budget (underruns), so it escalates to 2 and
    // then clamps -- it never restores.
    CHECK(nds_perf_governor_update(&state, s) == false);
    CHECK(nds_perf_governor_update(&state, s) == false);
    CHECK(nds_perf_governor_update(&state, s) == true);
    CHECK(state.stage == 2);
}

// (5) Continued over-budget at stage 2 stays clamped.
void test_clamped_at_stage2() {
    NdsPerfGovernorState state{};
    nds_perf_governor_init(&state, NdsPerfGovernorMode::Auto, small_config());
    feed(state, over_emu(), 2);
    CHECK(nds_perf_governor_update(&state, over_emu()));
    feed(state, over_emu(), 2);
    CHECK(nds_perf_governor_update(&state, over_emu()));
    CHECK(state.stage == 2);
    feed(state, over_emu(), 500);
    CHECK(state.stage == 2);
    CHECK(state.over_frames == small_config().engage_frames);
}

// (6) The shipped default config.
void test_default_config() {
    NdsPerfGovernorState state{};
    nds_perf_governor_init(&state, NdsPerfGovernorMode::Auto);
    CHECK(state.config.engage_frames == 30);
    CHECK(state.config.restore_frames == 180);
    CHECK(state.config.flap_engage_limit == 2);

    feed(state, over_emu(), 29);
    CHECK(state.stage == 0);
    CHECK(nds_perf_governor_update(&state, over_emu()));
    CHECK(state.stage == 1);
    feed(state, over_emu(), 29);
    CHECK(state.stage == 1);
    CHECK(nds_perf_governor_update(&state, over_emu()));
    CHECK(state.stage == 2);
    feed(state, under(), 179);
    CHECK(state.stage == 2);
    CHECK(nds_perf_governor_update(&state, under()));
    CHECK(state.stage == 1);
}

// (7) The flap backoff (finding D).
void test_flap_backoff() {
    NdsPerfGovernorConfig cfg{};
    cfg.engage_frames = 2;
    cfg.restore_frames = 2;
    cfg.flap_engage_limit = 2;
    cfg.flap_window_frames = 10;

    NdsPerfGovernorState state{};
    nds_perf_governor_init(&state, NdsPerfGovernorMode::Auto, cfg);
    auto escalate_one_stage = [&]() {
        CHECK(!nds_perf_governor_update(&state, over_emu()));
        CHECK(nds_perf_governor_update(&state, over_emu()));
    };
    // 0 -> 1 -> 2: first stage-2 engagement, not held yet.
    escalate_one_stage();
    CHECK(state.stage == 1);
    CHECK(!nds_perf_governor_update(&state, over_emu()));
    CHECK(nds_perf_governor_update(&state, over_emu()));
    CHECK(state.stage == 2);
    CHECK(state.stage2_engagements == 1);
    CHECK(!state.stage2_held);

    // 2 -> 1 restore is allowed.
    CHECK(!nds_perf_governor_update(&state, under()));
    CHECK(nds_perf_governor_update(&state, under()));
    CHECK(state.stage == 1);

    // Second stage-2 engagement inside the window: hold.
    CHECK(!nds_perf_governor_update(&state, over_emu()));
    CHECK(nds_perf_governor_update(&state, over_emu()));
    CHECK(state.stage == 2);
    CHECK(state.stage2_engagements == 2);
    CHECK(state.stage2_held);

    // The restore predicate can no longer leave stage 2, so no further
    // rebuild hitch is ever paid.
    feed(state, under(), 500);
    CHECK(state.stage == 2);
    CHECK(state.under_frames == cfg.restore_frames);

    // A long healthy stretch BELOW stage 2 retires the guard.
    nds_perf_governor_init(&state, NdsPerfGovernorMode::Auto, cfg);
    escalate_one_stage();
    CHECK(!nds_perf_governor_update(&state, over_emu()));
    CHECK(nds_perf_governor_update(&state, over_emu()));
    CHECK(state.stage == 2);
    CHECK(state.stage2_engagements == 1);
    CHECK(!nds_perf_governor_update(&state, under()));
    CHECK(nds_perf_governor_update(&state, under()));
    CHECK(state.stage == 1);
    feed(state, neutral(), cfg.flap_window_frames);
    CHECK(state.stage2_engagements == 0);
    CHECK(!state.stage2_held);
    // So the next stage-2 engagement counts as the first again.
    CHECK(!nds_perf_governor_update(&state, over_emu()));
    CHECK(nds_perf_governor_update(&state, over_emu()));
    CHECK(state.stage == 2);
    CHECK(state.stage2_engagements == 1);
    CHECK(!state.stage2_held);
    CHECK(!nds_perf_governor_update(&state, under()));
    CHECK(nds_perf_governor_update(&state, under()));
    CHECK(state.stage == 1);
}

// Finding F: the terminal apply-failure state stops deciding entirely.
void test_apply_failed_is_terminal() {
    NdsPerfGovernorState state{};
    nds_perf_governor_init(&state, NdsPerfGovernorMode::Auto, small_config());
    feed(state, over_emu(), 2);
    CHECK(nds_perf_governor_update(&state, over_emu()));
    CHECK(state.stage == 1);

    nds_perf_governor_mark_apply_failed(&state);
    CHECK(state.apply_failed);
    CHECK(state.last_reason == NdsPerfGovernorReason::ApplyFailed);
    feed(state, over_emu(), 500);
    CHECK(state.stage == 1);
    feed(state, under(), 500);
    CHECK(state.stage == 1);
    CHECK(state.over_frames == 0);
    CHECK(state.under_frames == 0);
}

// Forced and off modes.
void test_forced_modes() {
    NdsPerfGovernorState state{};
    nds_perf_governor_init(&state, NdsPerfGovernorMode::Off, small_config());
    CHECK(state.stage == 0);
    feed(state, over_emu(), 100);
    CHECK(state.stage == 0);

    nds_perf_governor_init(&state, NdsPerfGovernorMode::ForceStage1,
                           small_config());
    CHECK(state.stage == 1);
    CHECK(state.last_reason == NdsPerfGovernorReason::Initial);
    feed(state, under(), 100);
    CHECK(state.stage == 1);
    feed(state, over_emu(), 100);
    CHECK(state.stage == 1);

    nds_perf_governor_init(&state, NdsPerfGovernorMode::ForceStage2,
                           small_config());
    CHECK(state.stage == 2);
    feed(state, under(), 100);
    CHECK(state.stage == 2);
}

// (9) Off mode entered while a non-zero stage is installed must ask the caller
// to restore stage 0 exactly once.
void test_off_mode_from_engaged_stage() {
    NdsPerfGovernorState state{};
    nds_perf_governor_init(&state, NdsPerfGovernorMode::Auto, small_config());
    feed(state, over_emu(), 2);
    CHECK(nds_perf_governor_update(&state, over_emu()));
    feed(state, over_emu(), 2);
    CHECK(nds_perf_governor_update(&state, over_emu()));
    CHECK(state.stage == 2);
    state.stage2_held = true;

    state.mode = NdsPerfGovernorMode::Off;
    CHECK(nds_perf_governor_update(&state, over_emu()));
    CHECK(state.stage == 0);
    CHECK(state.last_reason == NdsPerfGovernorReason::Off);
    CHECK(!state.stage2_held);
    CHECK(state.stage2_engagements == 0);
    // No further change is requested: no no-op N->N transitions.
    feed(state, over_emu(), 100);
    CHECK(state.stage == 0);

    // Same for a forced mode adopted over an engaged stage.
    nds_perf_governor_init(&state, NdsPerfGovernorMode::Auto, small_config());
    feed(state, over_emu(), 2);
    CHECK(nds_perf_governor_update(&state, over_emu()));
    CHECK(state.stage == 1);
    state.mode = NdsPerfGovernorMode::ForceStage2;
    CHECK(nds_perf_governor_update(&state, under()));
    CHECK(state.stage == 2);
    CHECK(state.last_reason == NdsPerfGovernorReason::Forced);
    feed(state, under(), 100);
    CHECK(state.stage == 2);
}

// (8) Mode parsing, including the numeric aliases.
void test_mode_parsing() {
    NdsPerfGovernorMode mode = NdsPerfGovernorMode::Invalid;
    CHECK(nds_parse_perf_governor_mode("auto", &mode));
    CHECK(mode == NdsPerfGovernorMode::Auto);
    CHECK(nds_parse_perf_governor_mode("off", &mode));
    CHECK(mode == NdsPerfGovernorMode::Off);
    CHECK(nds_parse_perf_governor_mode("stage1", &mode));
    CHECK(mode == NdsPerfGovernorMode::ForceStage1);
    CHECK(nds_parse_perf_governor_mode("1", &mode));
    CHECK(mode == NdsPerfGovernorMode::ForceStage1);
    CHECK(nds_parse_perf_governor_mode("stage2", &mode));
    CHECK(mode == NdsPerfGovernorMode::ForceStage2);
    CHECK(nds_parse_perf_governor_mode("2", &mode));
    CHECK(mode == NdsPerfGovernorMode::ForceStage2);
    CHECK(!nds_parse_perf_governor_mode("maybe", &mode));
    CHECK(!nds_parse_perf_governor_mode("", &mode));
    CHECK(!nds_parse_perf_governor_mode(nullptr, &mode));

    CHECK(std::string("auto") ==
          nds_perf_governor_mode_name(NdsPerfGovernorMode::Auto));
    CHECK(std::string("stage2") ==
          nds_perf_governor_mode_name(NdsPerfGovernorMode::ForceStage2));
    CHECK(std::string("over_budget") ==
          nds_perf_governor_reason_name(NdsPerfGovernorReason::OverBudget));
    CHECK(std::string("apply_failed") ==
          nds_perf_governor_reason_name(NdsPerfGovernorReason::ApplyFailed));
}

// Finding I: the always-on transition ring.
void test_transition_ring() {
    nds_perf_governor_history_reset();
    CHECK(nds_perf_governor_history_total() == 0);
    NdsPerfGovernorTransition entries[kNdsPerfGovernorHistoryCapacity]{};
    CHECK(nds_perf_governor_history(entries,
                                    kNdsPerfGovernorHistoryCapacity) == 0);

    nds_perf_governor_record_transition(
        0, 1, NdsPerfGovernorReason::OverBudget, 100, false, false);
    nds_perf_governor_record_transition(
        1, 2, NdsPerfGovernorReason::OverBudget, 200, true, false);
    nds_perf_governor_record_transition(
        2, 2, NdsPerfGovernorReason::ApplyFailed, 300, true, true);
    CHECK(nds_perf_governor_history_total() == 3);
    CHECK(nds_perf_governor_history(entries,
                                    kNdsPerfGovernorHistoryCapacity) == 3);
    // Oldest first.
    CHECK(entries[0].frame_index == 100);
    CHECK(entries[0].to_stage == 1);
    CHECK(entries[1].frame_index == 200);
    CHECK(entries[1].stage2_held);
    CHECK(entries[2].frame_index == 300);
    CHECK(entries[2].apply_failed);
    CHECK(entries[2].reason == NdsPerfGovernorReason::ApplyFailed);
    CHECK(entries[0].ts_ms != 0);

    // Eviction keeps the newest window and the total keeps counting.
    for (uint64_t i = 3; i < kNdsPerfGovernorHistoryCapacity + 10u; ++i)
        nds_perf_governor_record_transition(
            0, 1, NdsPerfGovernorReason::Recovered, i * 1000u, false, false);
    const uint64_t total = kNdsPerfGovernorHistoryCapacity + 10u;
    CHECK(nds_perf_governor_history_total() == total);
    const uint32_t count = nds_perf_governor_history(
        entries, kNdsPerfGovernorHistoryCapacity);
    CHECK(count == kNdsPerfGovernorHistoryCapacity);
    CHECK(entries[count - 1].frame_index == (total - 1u) * 1000u);
    CHECK(entries[0].frame_index ==
          (total - kNdsPerfGovernorHistoryCapacity) * 1000u);

    // A truncated request returns the newest entries.
    CHECK(nds_perf_governor_history(entries, 2) == 2);
    CHECK(entries[1].frame_index == (total - 1u) * 1000u);
    CHECK(entries[0].frame_index == (total - 2u) * 1000u);

    nds_perf_governor_history_reset();
    CHECK(nds_perf_governor_history_total() == 0);
}

}  // namespace

int main() {
    test_ladder();
    test_streak_is_broken_by_one_frame();
    test_dead_zone();
    test_clauses_in_isolation();
    test_underruns_clause();
    test_clamped_at_stage2();
    test_default_config();
    test_flap_backoff();
    test_apply_failed_is_terminal();
    test_forced_modes();
    test_off_mode_from_engaged_stage();
    test_mode_parsing();
    test_transition_ring();
    if (g_failures)
        std::fprintf(stderr, "perf_governor_test: %d failure(s)\n",
                     g_failures);
    return g_failures ? 1 : 0;
}

