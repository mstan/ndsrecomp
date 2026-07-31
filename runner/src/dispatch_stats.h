// Passive cumulative dispatch-composition counters. Always on (Release
// too): the per-event cost is one non-atomic increment on paths that
// already do orders of magnitude more work. Like hle_heat and
// mem_timing_profile there is deliberately no arm/reset command — probes
// snapshot twice and subtract (DEBUG.md query-after-the-fact model).
//
// Composition without any emission change:
//   crs_push          = BL/BLX call frequency (every call pushes)
//   crs_hit           = returns resolved as host C returns (no dispatch)
//   crs_miss          = return-idiom transfers that fell to the dispatcher
//   dispatch_total    = every runtime_dispatch entry (incl. via exchange)
//   dispatch_slice_yield = entries that returned before static lookup
//   dispatch_exchange = the runtime_dispatch_with_exchange subset
//   literal_branch/call/fallthrough = generated compile-time-target sources
//   exception_dispatch = runtime SWI/IRQ vector transfers
//   cache_hit / cache_hit_absent / cache_slow_lookup =
//                       lookup_static_cached slot fast-hits
//                       (present/absent) vs full searches
//   crs_scan_iters    = call-return-stack entries examined by
//                       runtime_call_should_return (depth cost of returns)
// The explicitly tagged source counts are intentionally separate from
// dispatch_exchange: exchange describes transfer semantics, while the source
// buckets describe why generated/runtime code entered the dispatcher. Their
// residual is dynamic PC writes and other as-yet-unclassified sources.
#pragma once

#include <cstdint>
#include <string>

struct NdsDispatchStats {
    // Scheduler-initiated dispatches: one per run_slice stepping-loop
    // iteration (slice resume / post-preemption continuation). The
    // difference dispatch_total - resume_dispatch is generated-code-
    // initiated volume (calls, tails, computed transfers, vectoring).
    uint64_t resume_dispatch = 0;
    uint64_t dispatch_total = 0;
    uint64_t dispatch_slice_yield = 0;
    uint64_t dispatch_exchange = 0;
    uint64_t literal_branch = 0;
    uint64_t literal_call = 0;
    uint64_t literal_fallthrough = 0;
    uint64_t exception_dispatch = 0;
    uint64_t cache_hit = 0;
    uint64_t cache_hit_absent = 0;
    uint64_t cache_slow_lookup = 0;
    uint64_t crs_push = 0;
    uint64_t crs_hit = 0;
    uint64_t crs_miss = 0;
    uint64_t crs_scan_iters = 0;
};

// One slot per CPU (0 = ARM9, 1 = ARM7), indexed by g_nds_active.
extern NdsDispatchStats g_nds_dispatch_stats[2];

std::string nds_dispatch_stats_json();
