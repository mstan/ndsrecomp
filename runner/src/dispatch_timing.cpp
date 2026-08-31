// dispatch_timing.cpp -- see dispatch_timing.h.

#include "dispatch_timing.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

NdsDispatchTiming g_nds_dispatch_timing[2] = {};
NdsDispatchClass g_nds_dispatch_pending_class = NDS_DISPATCH_CLASS_GENERIC;

namespace nds_dispatch_timing_detail {

uint64_t g_class_gate[2][NDS_DISPATCH_CLASS_COUNT] = {};
uint64_t g_cache_gate[2] = {};

// Default on, Release included: this is an always-on ring-buffer-style
// counter surface, not an armed probe. 1009 is the modulus already used by
// the scheduler sampler (prime, so it does not phase-lock to any power-of-two
// periodicity in dispatch volume).
uint64_t g_modulus = 1009u;
// Seeded to 1.0 (ticks treated as ns), never 0.0: the sampling gate on the
// hot path must not call ensure_calibrated(), so if a build ever reaches a
// dispatch before nds_dispatch_timing_init() runs, the fallback must still
// produce a non-zero, monotonically-correct number rather than silently
// accumulating zeros that would read as "dispatch is free".
double g_ns_per_tick = 1.0;
uint64_t g_overhead_ticks = 0u;

namespace {

using WallClock = std::chrono::steady_clock;

bool g_calibrated = false;

uint64_t parse_modulus() {
    const char* v = std::getenv("NDS_PROFILE_DISPATCH");
    if (!v || !v[0]) return 1009u;
    if (std::strcmp(v, "off") == 0 || std::strcmp(v, "0") == 0)
        return 0u;
    if (std::strcmp(v, "exact") == 0 || std::strcmp(v, "every") == 0)
        return 1u;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(v, &end, 10);
    if (end != v && *end == '\0' && parsed >= 2u)
        return static_cast<uint64_t>(parsed);
    return 1009u;
}

// Cost of one tick_now() pair, in ticks. Taken as the MINIMUM over many
// back-to-back reads: the minimum is the un-preempted cost, whereas a mean
// would fold in scheduler noise and over-subtract from real samples.
uint64_t measure_overhead_ticks() {
    uint64_t best = ~uint64_t{0};
    for (int i = 0; i < 4096; ++i) {
        const uint64_t a = tick_now();
        const uint64_t b = tick_now();
        const uint64_t d = b - a;
        if (d < best) best = d;
    }
    return best == ~uint64_t{0} ? 0u : best;
}

// Nanoseconds per tick, against the steady clock. On a TSC-less build
// tick_now() already returns nanoseconds, so the ratio is 1.
double measure_ns_per_tick() {
#if defined(NDS_DISPATCH_TIMING_TSC)
    // Two short passes; keep the one with the larger observed wall interval,
    // which is the one least likely to have been cut short by a clock
    // granularity artefact.
    double best = 0.0;
    uint64_t best_wall = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const WallClock::time_point w0 = WallClock::now();
        const uint64_t t0 = tick_now();
        // Spin rather than sleep: a sleep can be de-scheduled onto a
        // different core, and on some hosts that invalidates the pairing.
        while (std::chrono::duration_cast<std::chrono::microseconds>(
                   WallClock::now() - w0).count() < 2000) {
        }
        const uint64_t t1 = tick_now();
        const uint64_t wall_ns =
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    WallClock::now() - w0).count());
        const uint64_t ticks = t1 - t0;
        if (ticks && wall_ns > best_wall) {
            best_wall = wall_ns;
            best = static_cast<double>(wall_ns) / static_cast<double>(ticks);
        }
    }
    // A plausible invariant TSC is 0.1 .. 10 ns/tick (100 MHz .. 10 GHz).
    // Outside that the calibration is untrustworthy; fall back to treating
    // ticks as nanoseconds rather than publishing a wild scale factor.
    if (best < 0.1 || best > 10.0) return 1.0;
    return best;
#else
    return 1.0;
#endif
}

}  // namespace

uint64_t tick_now_slow() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            WallClock::now().time_since_epoch()).count());
}

void ensure_calibrated() {
    if (g_calibrated) return;
    g_calibrated = true;
    g_modulus = parse_modulus();
    g_ns_per_tick = measure_ns_per_tick();
    g_overhead_ticks = measure_overhead_ticks();
}

}  // namespace nds_dispatch_timing_detail

void nds_dispatch_timing_init() {
    nds_dispatch_timing_detail::ensure_calibrated();
}

void nds_dispatch_timing_reset() {
    g_nds_dispatch_timing[0] = {};
    g_nds_dispatch_timing[1] = {};
    g_nds_dispatch_pending_class = NDS_DISPATCH_CLASS_GENERIC;
    std::memset(nds_dispatch_timing_detail::g_class_gate, 0,
                sizeof(nds_dispatch_timing_detail::g_class_gate));
    std::memset(nds_dispatch_timing_detail::g_cache_gate, 0,
                sizeof(nds_dispatch_timing_detail::g_cache_gate));
}

uint64_t nds_dispatch_timing_modulus() {
    nds_dispatch_timing_detail::ensure_calibrated();
    return nds_dispatch_timing_detail::g_modulus;
}

double nds_dispatch_timing_ns_per_tick() {
    nds_dispatch_timing_detail::ensure_calibrated();
    return nds_dispatch_timing_detail::g_ns_per_tick;
}

uint64_t nds_dispatch_timing_clock_overhead_ticks() {
    nds_dispatch_timing_detail::ensure_calibrated();
    return nds_dispatch_timing_detail::g_overhead_ticks;
}

const char* nds_dispatch_class_name(NdsDispatchClass cls) {
    switch (cls) {
        case NDS_DISPATCH_CLASS_GENERIC: return "generic";
        case NDS_DISPATCH_CLASS_LITERAL_BRANCH: return "literal_branch";
        case NDS_DISPATCH_CLASS_LITERAL_CALL: return "literal_call";
        case NDS_DISPATCH_CLASS_LITERAL_FALLTHROUGH:
            return "literal_fallthrough";
        case NDS_DISPATCH_CLASS_EXCHANGE: return "exchange";
        case NDS_DISPATCH_CLASS_RESUME: return "resume_dispatch";
        case NDS_DISPATCH_CLASS_EXCEPTION: return "exception_dispatch";
        default: return "unknown";
    }
}

const char* nds_dispatch_cache_path_name(NdsDispatchCachePath path) {
    switch (path) {
        case NDS_DISPATCH_CACHE_HIT: return "cache_hit";
        case NDS_DISPATCH_CACHE_HIT_ABSENT: return "cache_hit_absent";
        case NDS_DISPATCH_CACHE_SLOW: return "cache_slow_lookup";
        default: return "unknown";
    }
}

std::string nds_dispatch_timing_json() {
    char buf[512];
    std::string out = "{";
    std::snprintf(buf, sizeof(buf),
                  "\"modulus\":%llu,\"ns_per_tick\":%.6f,"
                  "\"clock_overhead_ticks\":%llu,\"cpus\":[",
                  (unsigned long long)nds_dispatch_timing_modulus(),
                  nds_dispatch_timing_ns_per_tick(),
                  (unsigned long long)
                      nds_dispatch_timing_clock_overhead_ticks());
    out += buf;
    for (int cpu = 0; cpu < 2; ++cpu) {
        if (cpu) out += ',';
        const NdsDispatchTiming& t = g_nds_dispatch_timing[cpu];
        std::snprintf(buf, sizeof(buf), "{\"cpu\":\"%s\",\"classes\":{",
                      cpu == 0 ? "arm9" : "arm7");
        out += buf;
        for (int i = 0; i < NDS_DISPATCH_CLASS_COUNT; ++i) {
            if (i) out += ',';
            std::snprintf(buf, sizeof(buf),
                          "\"%s\":{\"ns\":%llu,\"samples\":%llu,"
                          "\"events\":%llu}",
                          nds_dispatch_class_name(
                              static_cast<NdsDispatchClass>(i)),
                          (unsigned long long)t.class_ns[i],
                          (unsigned long long)t.class_samples[i],
                          (unsigned long long)t.class_events[i]);
            out += buf;
        }
        // `events` is the dispatcher-only lookup population (see
        // dispatch_timing.h); the all-consumers totals are in
        // nds_dispatch_stats_json().
        out += "},\"cache_paths\":{";
        for (int i = 0; i < NDS_DISPATCH_CACHE_PATH_COUNT; ++i) {
            if (i) out += ',';
            std::snprintf(buf, sizeof(buf),
                          "\"%s\":{\"ns\":%llu,\"samples\":%llu,"
                          "\"events\":%llu}",
                          nds_dispatch_cache_path_name(
                              static_cast<NdsDispatchCachePath>(i)),
                          (unsigned long long)t.cache_ns[i],
                          (unsigned long long)t.cache_samples[i],
                          (unsigned long long)t.cache_events[i]);
            out += buf;
        }
        out += "}}";
    }
    out += "]}";
    return out;
}
