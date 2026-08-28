// dispatch_timing.h -- sampled per-class dispatch COST, alongside the
// per-class counts in dispatch_stats.h.
//
// Why this exists: the counters answer "how often", never "how expensive".
// Field analysis of a dispatch regression had to infer ns-per-dispatch from
// a count delta between two builds (dev ~91 ns vs field ~188 ns), which is
// an inference, not a measurement. These accumulators measure it directly.
//
// Ring-buffer philosophy (DEBUG.md): always on, Release included, never
// armed. There is no start/stop and no reset command -- probes snapshot
// twice and subtract, exactly like dispatch_stats / hle_heat.
//
// WHAT IS TIMED (and what is deliberately NOT): each measured region is the
// dispatch MACHINERY for one entry, EXCLUSIVE of the guest code it goes on
// to run. The region runs from the dispatch entry point through target
// alignment, the slice-yield poll, the trace hook, the static lookup and the
// execution-guard setup, and closes immediately before control is handed to
// the recompiled function. Timing a `literal_call` inclusively would time
// the entire callee -- and every dispatch nested inside it -- so the buckets
// would nest and their sum would exceed wall time. Because the regions are
// exclusive they never overlap, so:
//
//     sum(class_ns) <= wall time of the emulated thread
//
// which is the invariant the diagnostics gate checks.
//
// SAMPLING: one entry in every `modulus` (default 1009) is timed, following
// the NDS_PROFILE_SCHED idiom in scheduler.cpp. Unlike the scheduler
// sampler this is on by default; NDS_PROFILE_DISPATCH overrides the
// modulus ("0"/"off" disables, "exact"/"every" times every entry, N >= 2
// sets the modulus). The ungated cost is one counter increment and one
// compare on a path that already does hundreds of times more work.
//
// DENOMINATORS ARE EMITTED. The scheduler buckets shipped ns without the
// exact round count, which forced field analysis to scale sampled ns by an
// assumed sampling ratio and produced a nonsensical "106.6 percent of wall
// clock". Every bucket here carries BOTH:
//   events  -- exact count of entries of this class (never sampled)
//   samples -- how many of those were actually timed
// so mean cost is ns/samples and whole-run cost is (ns/samples)*events,
// with samples/events reporting the realised sampling ratio.
#pragma once

#include <cstdint>
#include <string>

// Why a dispatch entry was entered. Mirrors the source buckets in
// NdsDispatchStats; GENERIC is the residual (dynamic PC writes and other
// as-yet-unclassified transfers) that arrives with no tag set.
enum NdsDispatchClass : uint8_t {
    NDS_DISPATCH_CLASS_GENERIC = 0,
    NDS_DISPATCH_CLASS_LITERAL_BRANCH,
    NDS_DISPATCH_CLASS_LITERAL_CALL,
    NDS_DISPATCH_CLASS_LITERAL_FALLTHROUGH,
    NDS_DISPATCH_CLASS_EXCHANGE,
    NDS_DISPATCH_CLASS_RESUME,
    NDS_DISPATCH_CLASS_EXCEPTION,
    NDS_DISPATCH_CLASS_COUNT
};

// The three lookup_static_cached outcomes. These regions are NESTED INSIDE
// the class regions above (the lookup happens within the dispatch prologue),
// so they are a breakdown of class_ns, never an addend to it. Do not add
// cache_ns into the sum-vs-wall-time check.
//
// Timed only for lookups reached from runtime_dispatch. lookup_static_cached
// has a second consumer -- Tier 3's per-instruction "has a bank appeared yet"
// poll -- which outnumbers dispatch lookups roughly 8:1 (measured 23.5M vs
// 3.3M on a firmware boot). Instrumenting that consumer cost ~1.7 percent of
// total emulation time for a number that is not a dispatch cost at all: the
// interpreter poll is a separate phenomenon, already counted exactly in
// NdsDispatchStats::cache_*. So the poll path is left untouched.
enum NdsDispatchCachePath : uint8_t {
    NDS_DISPATCH_CACHE_HIT = 0,
    NDS_DISPATCH_CACHE_HIT_ABSENT,
    NDS_DISPATCH_CACHE_SLOW,
    NDS_DISPATCH_CACHE_PATH_COUNT
};

struct NdsDispatchTiming {
    uint64_t class_ns[NDS_DISPATCH_CLASS_COUNT] = {};
    uint64_t class_samples[NDS_DISPATCH_CLASS_COUNT] = {};
    uint64_t class_events[NDS_DISPATCH_CLASS_COUNT] = {};
    uint64_t cache_ns[NDS_DISPATCH_CACHE_PATH_COUNT] = {};
    uint64_t cache_samples[NDS_DISPATCH_CACHE_PATH_COUNT] = {};
    // Lookups made by the DISPATCHER only -- deliberately not the same
    // population as NdsDispatchStats::cache_hit/absent/slow, which also count
    // Tier 3's per-instruction poll and outnumber these ~70:1. A denominator
    // has to match the population its samples were drawn from: pairing 60
    // dispatcher samples with 4.65M all-consumer events would make any
    // mean-times-events extrapolation wrong by orders of magnitude. Both
    // populations end up in the record (this one here, the other in
    // dispatch_delta), so neither number is ambiguous.
    uint64_t cache_events[NDS_DISPATCH_CACHE_PATH_COUNT] = {};
};

// One slot per CPU (0 = ARM9, 1 = ARM7), indexed by g_nds_active, matching
// g_nds_dispatch_stats.
extern NdsDispatchTiming g_nds_dispatch_timing[2];

const char* nds_dispatch_class_name(NdsDispatchClass cls);
const char* nds_dispatch_cache_path_name(NdsDispatchCachePath path);

// Effective sampling modulus (0 = timing disabled), and the calibrated
// nanoseconds-per-tick / measured per-sample clock overhead, for reporting.
uint64_t nds_dispatch_timing_modulus();
double nds_dispatch_timing_ns_per_tick();
uint64_t nds_dispatch_timing_clock_overhead_ticks();

std::string nds_dispatch_timing_json();

// ── Internals used by the timed regions (header-inline: these sit on the
// hottest path in the runtime, so the sampling gate must not become a call).

namespace nds_dispatch_timing_detail {

// Monotonic tick source. rdtsc is used where available because the regions
// being measured are ~100 ns: a QueryPerformanceCounter pair costs ~20-30 ns
// each and would perturb a sample by a large fraction of its own value.
// The measured cost of the tick pair is subtracted from every sample.
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || \
    defined(_M_IX86)
#define NDS_DISPATCH_TIMING_TSC 1
#endif

uint64_t tick_now_slow();

inline uint64_t tick_now() {
#if defined(NDS_DISPATCH_TIMING_TSC)
    return __builtin_ia32_rdtsc();
#else
    return tick_now_slow();
#endif
}

// Sampling gate state. Per CPU and per class so a rare class still gets
// sampled at the same 1-in-modulus rate as a hot one rather than being
// starved by a shared counter.
//
// These are COUNTDOWNS, not free-running counters compared with `%`. The
// modulus is a runtime value, so `counter++ % modulus` compiles to a hardware
// 64-bit division -- tens of cycles, on a path whose whole cost is ~100 ns.
// The scheduler sampler can afford that idiom because it runs once per
// scheduler round; a dispatch gate cannot. A countdown is a decrement, a
// compare and a perfectly-predicted branch. Zero means "reload and sample",
// which also makes the zero-initialised state sample the first event.
extern uint64_t g_class_gate[2][NDS_DISPATCH_CLASS_COUNT];
extern uint64_t g_cache_gate[2];

// True when this event should be timed; decrements the gate otherwise.
inline bool gate_fires(uint64_t& gate, uint64_t modulus) {
    if (gate > 1u) {
        --gate;
        return false;
    }
    gate = modulus;
    return true;
}
extern uint64_t g_modulus;
extern double g_ns_per_tick;
extern uint64_t g_overhead_ticks;

void ensure_calibrated();

// Convert a raw tick delta to ns, removing the clock's own cost.
inline uint64_t ticks_to_ns(uint64_t delta) {
    const uint64_t overhead = g_overhead_ticks;
    const uint64_t net = delta > overhead ? delta - overhead : 0u;
    return static_cast<uint64_t>(static_cast<double>(net) * g_ns_per_tick);
}

}  // namespace nds_dispatch_timing_detail

// Initialise the tick calibration. Safe to call more than once; called from
// runner start-up so the first sampled dispatch is not the one paying for
// calibration.
void nds_dispatch_timing_init();

// The pending class tag. A dispatch wrapper (runtime_dispatch_literal_call
// and friends) records why it is entering, and the region inside
// runtime_dispatch consumes it. A plain runtime_dispatch call with no
// wrapper leaves the tag clear and is accounted GENERIC.
extern NdsDispatchClass g_nds_dispatch_pending_class;

inline void nds_dispatch_tag(NdsDispatchClass cls) {
    g_nds_dispatch_pending_class = cls;
}

// RAII timer for one exclusive dispatch-machinery region. close() ends the
// region early (before guest code runs); the destructor is a backstop for
// any path that returns without closing.
class NdsDispatchRegion {
  public:
    explicit NdsDispatchRegion(int cpu) : cpu_(cpu & 1) {
        using namespace nds_dispatch_timing_detail;
        cls_ = g_nds_dispatch_pending_class;
        g_nds_dispatch_pending_class = NDS_DISPATCH_CLASS_GENERIC;
        ++g_nds_dispatch_timing[cpu_].class_events[cls_];
        const uint64_t modulus = g_modulus;
        if (!modulus) return;
        if (!gate_fires(g_class_gate[cpu_][cls_], modulus)) return;
        sampling_ = true;
        start_ = tick_now();
    }

    ~NdsDispatchRegion() { close(); }

    NdsDispatchRegion(const NdsDispatchRegion&) = delete;
    NdsDispatchRegion& operator=(const NdsDispatchRegion&) = delete;

    void close() {
        if (!sampling_) return;
        using namespace nds_dispatch_timing_detail;
        const uint64_t delta = tick_now() - start_;
        sampling_ = false;
        NdsDispatchTiming& t = g_nds_dispatch_timing[cpu_];
        t.class_ns[cls_] += ticks_to_ns(delta);
        ++t.class_samples[cls_];
    }

  private:
    int cpu_;
    NdsDispatchClass cls_ = NDS_DISPATCH_CLASS_GENERIC;
    bool sampling_ = false;
    uint64_t start_ = 0;
};

// Timer for one cache lookup performed by the dispatcher. The outcome is not
// known until the lookup returns, so the path is supplied to close_as().
//
// The gate is decided at CONSTRUCTION, on one counter shared by the three
// outcomes, because the outcome is not yet known and the start tick has to be
// taken before the work begins. Sampling is therefore per-lookup rather than
// per-outcome, so a rare outcome collects proportionally fewer samples --
// which is exactly why `samples` sits next to every `ns` in the record.
//
// open() lets the caller skip the outcome-determination work entirely on
// unsampled lookups, so an unsampled lookup costs one countdown and one
// perfectly-predicted branch and nothing else.
class NdsDispatchCacheRegion {
  public:
    explicit NdsDispatchCacheRegion(int cpu) : cpu_(cpu & 1) {
        using namespace nds_dispatch_timing_detail;
        const uint64_t modulus = g_modulus;
        if (!modulus) return;
        if (!gate_fires(g_cache_gate[cpu_], modulus)) return;
        start_ = tick_now();
        open_ = true;
    }

    bool open() const { return open_; }

    void close_as(NdsDispatchCachePath path) {
        using namespace nds_dispatch_timing_detail;
        if (!open_) return;
        open_ = false;
        NdsDispatchTiming& t = g_nds_dispatch_timing[cpu_];
        t.cache_ns[path] += ticks_to_ns(tick_now() - start_);
        ++t.cache_samples[path];
    }

    NdsDispatchCacheRegion(const NdsDispatchCacheRegion&) = delete;
    NdsDispatchCacheRegion& operator=(const NdsDispatchCacheRegion&) = delete;

  private:
    int cpu_;
    bool open_ = false;
    uint64_t start_ = 0;
};
