// emu_profile.h -- always-on host-time attribution for the EMULATED phase of
// a frame, i.e. the window frontend.cpp times into NdsFrontendLiveStats::
// emu_ticks (frontend.cpp:2731-2756). That window contains exactly one thing:
// the `while (...) scheduler_run_round();` loop. So an exhaustive partition of
// one scheduler round is an exhaustive partition of emu time, and that is what
// this module produces.
//
// WHY THIS EXISTS (beads-yjp.54). Before this module the only decomposition of
// emu time was NdsSchedulerProfile's five buckets (next_event / arm9 / arm7 /
// devices / switch). Field analysis of the 2026-08-28 MPH logs could not use
// them for three separate reasons:
//
//   1. `profile_totals` shipped the CUMULATIVE run totals while every other
//      block in the perf record ships a per-interval delta. Dividing a
//      monotonically-growing run total by one interval's frame_delta produces a
//      slow ramp that reads as "flat" next to a noisy per-interval emu figure.
//      Fixed by emitting `profile_totals_delta` next to the totals.
//   2. `scheduler_arm9_ns` conflated ARM9 instruction execution, ARM9 timer
//      ticks and the CPU-side geometry engine (scheduler.cpp:363-382), so the
//      single biggest bucket named no consumer.
//   3. The 1-in-1009 round sampler is an unbiased but very high-variance
//      estimator of a heavy-tailed per-round cost. Scaled to the interval it
//      scattered +/-60 percent against wall time and on one interval
//      attributed 53 ms/frame of devices to a 35.8 ms/frame emu budget.
//
// THE ESTIMATOR. Two accumulation modes, chosen per bucket by how often the
// region is entered:
//
//   EXACT buckets are timed on EVERY entry. Reserved for regions that are
//   rare per round but heavy and heavy-TAILED when they do fire (a 2D
//   scanline, a 3D frame boundary, a DMA burst, a geometry-command drain, a
//   Tier-3 stretch). Sampling those is what produced the +/-60 percent
//   scatter: a 0.9 ms compute readback that fires 60 times a second is a
//   lottery ticket at 1-in-1009. Their totals need no scaling.
//
//   SAMPLED buckets are timed only on rounds the scheduler has selected
//   (NDS_PROFILE_EMU, default 1 round in 31). Reserved for regions entered
//   once or twice per round whose per-entry cost is small and tightly
//   distributed, where the variance of a mean is what matters and 1-in-31
//   over ~600k rounds/s gives ~20k samples per 2 s interval. Their totals are
//   scaled by rounds/sampled_rounds at report time.
//
// The two compose because the buckets are EXCLUSIVE: an exact child opened
// inside a sampled parent stops the parent's clock, so the parent's sampled
// mean already excludes it and adding the child's exact total double-counts
// nothing. On an unsampled round the parent simply is not measured while the
// child still is; the parent's magnitude comes from the rounds where it WAS
// measured. See nds_emu_enter().
//
// OVERHEAD IS COMPENSATED, NOT ASSUMED. Every region boundary is exactly one
// tick read, and every boundary closes exactly one region, so subtracting one
// measured clock-pair overhead from the region being closed removes the whole
// instrumentation cost from the accumulated numbers (nds_dispatch_timing's
// g_overhead_ticks, measured as a minimum over 4096 back-to-back reads). The
// raw read count is exported as `reads` so the residual perturbation is
// checkable from a log rather than argued about: reads/sampled_rounds times
// the overhead is the instrumentation's own share of a sampled round.
//
// WHY THE RESIDUAL SITS SLIGHTLY NEGATIVE (~2-3 percent over-attribution),
// measured on a firmware boot 2026-08-28 and worth understanding before anyone
// tries to "fix" it. A boundary's tick read is compensated by subtracting
// g_overhead_ticks, the MINIMUM cost of a back-to-back read pair. That is the
// throughput cost; a read placed between real work costs a little more, and
// that unsubtractable remainder sits inside the sampled buckets, where the
// rounds/sampled_rounds scaling multiplies it by the modulus. So:
//
//   reported over-attribution ~= (reads_per_sampled_round * residual_per_read)
//                                / mean_round_cost
//
// which is INDEPENDENT of the modulus -- the amplification and the sampling
// rate cancel -- while the REAL wall-clock overhead is that figure divided by
// the modulus. Measured: 22.7 reads per sampled round, ~7.3 us mean round,
// ~3 percent reported over-attribution, therefore ~0.1 percent real cost. The
// modulus is thus purely a cost/variance knob and cannot buy accuracy here.
// Twenty of those 22.7 reads measure the per-round device ticks, whose buckets
// together are under 2 percent of emu time; if this term ever needs to shrink,
// coarsening THOSE is the lever, not the modulus.
//
// The rdtsc-to-ns calibration is NOT a contributor: NDS_EMU_GPU3D_FRAME
// (rdtsc, this module) and gpu3d_compute_sync_ns (steady_clock, gpu3d.cpp)
// measure overlapping work through independent clocks and independent code and
// agreed to 0.03 percent (14.695 vs 14.690 ms/frame) over eight intervals.
//
// KNOWN LIMIT OF THE HYBRID, stated so a reader does not have to rediscover
// it. A SAMPLED region nested inside an EXACT region on an UNSAMPLED round is
// not measured, so its time stays inside the exact parent -- while the sampled
// bucket's own scaled total also covers it. The two therefore overlap by
// (1 - 1/modulus) times that nested time. There is exactly one such path in
// the runtime today: nds_tick_timers (sampled) reached from a guest MMIO store
// executed by tier3_run (exact). It is bounded by the interpreter's share of
// the frame, which in a title running from static banks is ~0 (MPH gameplay:
// ~32 interpreted instructions per frame). It matters only for a workload the
// interpreter dominates -- a firmware boot or a menu -- where the honest
// reading is that TIER3_* is inclusive of the device ticks its own guest
// stores trigger. Making TIMERS exact would fix it and cost four tick reads
// per round (~1.3 percent), which is a worse trade than documenting it.
//
// NOT ADDITIVE, DELIBERATELY. `bus_*` and the pre-existing dispatch-machinery
// buckets in dispatch_timing.h are BREAKDOWNS of NDS_EMU_EXEC_*, not addends
// to the partition -- exactly the relationship cache_ns has to class_ns. They
// have their own independent countdown gates because they are entered
// thousands of times inside a single round: riding the round gate would time
// every one of them on a sampled round and inflate that round's own total,
// which is precisely the bias this module exists to remove.
//
// Ring-buffer philosophy (DEBUG.md): always on, Release included, never
// armed, no start/stop, no reset. Probes snapshot twice and subtract.
#pragma once

#include <cstdint>
#include <string>

#include "dispatch_timing.h"

// The partition. Every bucket is EXCLUSIVE of every other: time spent in a
// nested region is charged to the nested region only.
//
// Sum of all buckets (exact ones as-is, sampled ones scaled by
// rounds/sampled_rounds) estimates the whole emu phase. NDS_EMU_SCHED_OTHER is
// the round's own machinery -- the idle fast-forward planner, the ARM7
// catch-up loop conditions, the halt/DMA predicates -- and is MEASURED as the
// round region's self time, never derived by subtraction, so a residual
// against emu_ticks indicts the estimator rather than hiding in a bucket.
enum NdsEmuBucket : uint8_t {
    // ── SAMPLED (scaled by rounds/sampled_rounds at report time) ──────────
    // Guest code execution: the run_slice dispatch loop (scheduler.cpp:197).
    // Native recompiled bank bodies, the dispatch machinery, the bus, and any
    // Tier-3 stretch are all inside it; TIER3 is carved back out below, and
    // dispatch/bus have their own non-additive breakdowns.
    NDS_EMU_EXEC_ARM9 = 0,
    NDS_EMU_EXEC_ARM7,
    NDS_EMU_TIMERS_ARM9,
    NDS_EMU_TIMERS_ARM7,
    // Raster timeline advance, exclusive of the 2D scanline and 3D frame
    // boundary work it drives (those are exact buckets below).
    NDS_EMU_DISPLAY,
    NDS_EMU_SPU,
    NDS_EMU_WIFI,
    NDS_EMU_RTC,
    NDS_EMU_SYSEV,
    // next_scheduled_event_time(): five deadline computations per round, one
    // of them a 64-bit divide pair with a fixup loop (scheduler.cpp:80-125).
    NDS_EMU_NEXTEV,
    // switch_to(): register file plus call-return-stack save/restore.
    NDS_EMU_CTXSW,
    // live_overlay_poll(), once per round (scheduler.cpp:471).
    NDS_EMU_OVERLAY,
    // The round's own interleave machinery. Self time of the round region.
    NDS_EMU_SCHED_OTHER,
    NDS_EMU_SAMPLED_COUNT,

    // ── EXACT (timed on every entry; totals need no scaling) ──────────────
    // CPU-side geometry engine: the GX command drain, vertex transform,
    // lighting, clipping and polygon assembly (melonDS GPU3D::Run ->
    // ExecuteCommand -> SubmitVertex/SubmitPolygon/CalculateLighting). Opened
    // only when the bridge sees real pending geometry work, so an idle round
    // pays nothing. This is the consumer the old scheduler_arm9_ns hid.
    NDS_EMU_GEOM = NDS_EMU_SAMPLED_COUNT,
    // Tier-3 dirty-RAM interpreter, per entry, carved out of EXEC_*.
    NDS_EMU_TIER3_ARM9,
    NDS_EMU_TIER3_ARM7,
    // DMA bursts owning a CPU's bus slot (scheduler.cpp:166-172).
    NDS_EMU_DMA_ARM9,
    NDS_EMU_DMA_ARM7,
    // Main-thread cost of one 2D scanline: the render itself in inline mode,
    // or the submit plus any pool fence in threaded mode. (In threaded mode
    // the pixel work itself runs on workers and is NOT emu wall time -- which
    // is why gpu2d_render_ns can be large while emu time is not.)
    NDS_EMU_RASTER2D,
    // 3D frame boundaries reached from the display tick: start_frame,
    // VCount144, VCount215. The compute-renderer submit/sync/map buckets in
    // NdsGpu3dProfile are a breakdown of THIS bucket.
    NDS_EMU_GPU3D_FRAME,
    NDS_EMU_BUCKET_COUNT,

    // Sentinel: no region is open. A gap charged to this is discarded.
    NDS_EMU_NONE = 0xFFu,
    // Sentinel returned by nds_emu_enter() when THIS entry was not measured
    // (a sampled bucket on an unsampled round). It must be distinct from
    // NDS_EMU_NONE: an exact region can be open while an unmeasured sampled
    // region nests inside it -- Tier 3 (exact) interpreting a guest MMIO store
    // that reaches nds_tick_timers (sampled) is the concrete case -- and if
    // "not measured" and "nothing was open" shared a value, that inner exit
    // would close and mis-attribute the ENCLOSING exact region.
    NDS_EMU_INACTIVE = 0xFEu
};

// Non-additive breakdown of NDS_EMU_EXEC_*: which guest memory-access SLOW
// path the executing code took, i.e. which arm of the if-chain in
// bus_read_*_slow / bus_write_*_slow (bus.cpp:763-854) served the access.
// Independently gated (see the header note).
//
// The B3 inline fast path (recompiler/armv4t/runtime_arm.h:290-374) is NOT
// covered and deliberately so: it is a window compare plus a memcpy on the
// hottest path in the runtime, its cost is inseparable from the instruction
// that issued it, and it is already inside NDS_EMU_EXEC_*. What these buckets
// answer is the question the fast path cannot: when an access MISSES the
// inline windows, how expensive is the fallback and which region caused it.
enum NdsEmuBusPath : uint8_t {
    // resolve() served it -- main RAM / shared WRAM / TCM / BIOS. Reaching
    // the slow path at all means the inline window missed (a straddling or
    // out-of-window address) or deep trace forced it.
    NDS_EMU_BUS_RAM = 0,
    NDS_EMU_BUS_VRAM,     // 0x05-0x07: palette, VRAM banks, OAM
    NDS_EMU_BUS_MMIO,     // 0x04: I/O register dispatch (incl. the Wi-Fi window)
    NDS_EMU_BUS_OTHER,    // GBA slot-2, and the unmapped residual
    NDS_EMU_BUS_PATH_COUNT
};

struct NdsEmuProfile {
    // Accumulated in TICKS, not ns: the hot path must not do a float
    // multiply per boundary. Converted once, at report time.
    uint64_t ticks[NDS_EMU_BUCKET_COUNT] = {};
    // Region entries. Exact for exact buckets, sampled-round-only for
    // sampled buckets -- the same population as the ticks beside them.
    uint64_t entries[NDS_EMU_BUCKET_COUNT] = {};
    // Rounds the sampled buckets were measured on, and the exact total.
    // BOTH are required to scale: shipping only the sampled count is what
    // forced field analysis to guess the ratio.
    uint64_t sampled_rounds = 0;
    uint64_t rounds = 0;
    // Rounds that hit the GXFIFO-stall branch instead of executing ARM9
    // (scheduler.cpp:365-373). Exact; an increment is too cheap to gate.
    uint64_t gxstall_rounds = 0;
    // Tick reads taken. reads * overhead_ticks is the instrumentation's own
    // measured cost, already subtracted from the buckets above; exported so
    // the residual perturbation is checkable rather than assumed.
    uint64_t reads = 0;
    // Non-additive bus breakdown. `bus_events` is the EXACT total of slow-path
    // accesses; the per-path split is sampled, so a path's whole-run cost is
    // (bus_ticks[p] / sum(bus_samples)) * bus_events. Classifying every access
    // exactly would put three range compares on a path taken millions of times
    // a second for a number the samples already give.
    uint64_t bus_events = 0;
    uint64_t bus_ticks[NDS_EMU_BUS_PATH_COUNT] = {};
    uint64_t bus_samples[NDS_EMU_BUS_PATH_COUNT] = {};
};

const char* nds_emu_bucket_name(NdsEmuBucket bucket);
const char* nds_emu_bus_path_name(NdsEmuBusPath path);
// True for buckets whose totals are exact and must NOT be scaled.
bool nds_emu_bucket_is_exact(NdsEmuBucket bucket);

void nds_emu_profile(NdsEmuProfile* out);
void nds_emu_profile_reset();
std::string nds_emu_profile_json();

// Effective round-sampling modulus (0 disables the sampled buckets; the exact
// buckets keep working). NDS_PROFILE_EMU overrides: "off"/"0" disables,
// "exact"/"every" samples every round, N >= 2 sets the modulus.
uint64_t nds_emu_modulus();

// ── Internals used by the timed regions. Header-inline: these sit inside the
// scheduler round and the guest-execution path, so a boundary must not become
// an out-of-line call.

namespace nds_emu_detail {

extern NdsEmuProfile g_profile;
// True for the duration of a round selected by the sampler.
extern bool     g_sampling;
// Bucket currently accruing, and the tick at which it started accruing.
extern uint8_t  g_cur;
extern uint64_t g_last;
// Bit set for the exact buckets, so is_exact() is a shift-and-test rather
// than a call or a table load.
constexpr uint64_t kExactMask =
    (uint64_t{1} << NDS_EMU_GEOM) |
    (uint64_t{1} << NDS_EMU_TIER3_ARM9) |
    (uint64_t{1} << NDS_EMU_TIER3_ARM7) |
    (uint64_t{1} << NDS_EMU_DMA_ARM9) |
    (uint64_t{1} << NDS_EMU_DMA_ARM7) |
    (uint64_t{1} << NDS_EMU_RASTER2D) |
    (uint64_t{1} << NDS_EMU_GPU3D_FRAME);

extern uint64_t g_bus_gate;
extern uint64_t g_bus_modulus;
// Round sampler state. Header-visible so NdsEmuRound's ctor/dtor inline: they
// run once per scheduler round (~600k/s), and an out-of-line call there costs
// more than the tick reads it exists to take.
extern uint64_t g_modulus;
extern uint64_t g_round_counter;

void ensure_configured();

}  // namespace nds_emu_detail

// Initialise the modulus and the shared tick calibration. Safe to call more
// than once; called from runner start-up so the first measured round is not
// the one paying for calibration.
void nds_emu_profile_init();

// One round retired no ARM9 instruction because the GXFIFO stall was asserted
// (scheduler.cpp GXFIFO branch). Exact whole-run counter; an increment is too
// cheap to gate.
inline void nds_emu_note_gxstall_round() {
    ++nds_emu_detail::g_profile.gxstall_rounds;
}

// Open a region. Returns the bucket to restore, or NDS_EMU_INACTIVE when this
// entry is not being measured (an unsampled round for a sampled bucket).
//
// The single tick read here closes the parent's accrual and opens this
// bucket's. One read, one region closed: subtracting one clock-pair overhead
// from the region being closed compensates the instrumentation exactly.
inline uint8_t nds_emu_enter(NdsEmuBucket bucket) {
    using namespace nds_emu_detail;
    const bool exact = (kExactMask >> bucket) & 1u;
    if (!exact && !g_sampling) return NDS_EMU_INACTIVE;
    const uint64_t t = nds_dispatch_timing_detail::tick_now();
    ++g_profile.reads;
    const uint8_t prev = g_cur;
    if (prev != NDS_EMU_NONE) {
        const uint64_t delta = t - g_last;
        const uint64_t overhead = nds_dispatch_timing_detail::g_overhead_ticks;
        g_profile.ticks[prev] += delta > overhead ? delta - overhead : 0u;
    }
    ++g_profile.entries[bucket];
    g_cur = bucket;
    g_last = t;
    return prev;
}

// Close the region opened by the matching nds_emu_enter(). `prev` carries
// whether that entry was measured at all, so neither a sampling flip between
// the two nor an unmeasured region nested inside a measured one can corrupt
// the accumulators.
inline void nds_emu_exit(uint8_t prev) {
    using namespace nds_emu_detail;
    if (prev == NDS_EMU_INACTIVE) return;
    const uint64_t t = nds_dispatch_timing_detail::tick_now();
    ++g_profile.reads;
    const uint64_t delta = t - g_last;
    const uint64_t overhead = nds_dispatch_timing_detail::g_overhead_ticks;
    g_profile.ticks[g_cur] += delta > overhead ? delta - overhead : 0u;
    g_cur = prev;
    g_last = t;
}

// RAII form. Every call site uses this; there are no bare enter/exit pairs.
class NdsEmuScope {
  public:
    explicit NdsEmuScope(NdsEmuBucket bucket)
        : prev_(nds_emu_enter(bucket)) {}
    ~NdsEmuScope() { nds_emu_exit(prev_); }
    NdsEmuScope(const NdsEmuScope&) = delete;
    NdsEmuScope& operator=(const NdsEmuScope&) = delete;

  private:
    uint8_t prev_;
};

// A conditionally-opened region, for a bridge that can cheaply see whether the
// callee will do real work (nds_gpu3d_run replicating GPU3D::Run's early-out).
// Timing the no-work case would cost two tick reads per round for a bucket
// that is zero.
class NdsEmuScopeIf {
  public:
    NdsEmuScopeIf(NdsEmuBucket bucket, bool open)
        : prev_(open ? nds_emu_enter(bucket)
                     : uint8_t{NDS_EMU_INACTIVE}) {}
    ~NdsEmuScopeIf() { nds_emu_exit(prev_); }
    NdsEmuScopeIf(const NdsEmuScopeIf&) = delete;
    NdsEmuScopeIf& operator=(const NdsEmuScopeIf&) = delete;

  private:
    uint8_t prev_;
};

// Opens/closes one scheduler round, and IS the round sampler. The round region
// is the SCHED_OTHER bucket: its self time is the interleave machinery that no
// named phase covers. RAII because scheduler_run_round() has an early return
// (the power-off short circuit) that must not leave a region open.
class NdsEmuRound {
  public:
    NdsEmuRound() {
        using namespace nds_emu_detail;
        ++g_profile.rounds;
        const uint64_t modulus = g_modulus;
        sampling_ = modulus && ((g_round_counter++ % modulus) == 0u);
        if (!sampling_) return;
        // A round is the outermost region, so nothing may be open when one
        // starts. If something is, decline to sample rather than corrupt that
        // region's accrual -- a leak then shows up as a fat bucket, not as a
        // wrong total.
        if (g_cur != NDS_EMU_NONE) {
            sampling_ = false;
            return;
        }
        g_sampling = true;
        ++g_profile.sampled_rounds;
        ++g_profile.entries[NDS_EMU_SCHED_OTHER];
        ++g_profile.reads;
        g_last = nds_dispatch_timing_detail::tick_now();
        g_cur = NDS_EMU_SCHED_OTHER;
    }

    ~NdsEmuRound() {
        using namespace nds_emu_detail;
        if (!sampling_) return;
        ++g_profile.reads;
        const uint64_t t = nds_dispatch_timing_detail::tick_now();
        const uint64_t delta = t - g_last;
        const uint64_t overhead =
            nds_dispatch_timing_detail::g_overhead_ticks;
        g_profile.ticks[g_cur] += delta > overhead ? delta - overhead : 0u;
        g_cur = NDS_EMU_NONE;
        g_sampling = false;
    }

    NdsEmuRound(const NdsEmuRound&) = delete;
    NdsEmuRound& operator=(const NdsEmuRound&) = delete;

  private:
    bool sampling_;
};

// Which arm of the slow-path if-chain an address will take. Address ranges
// only, so it never repeats resolve()'s work: an address resolve() can serve
// is exactly one no other arm claims.
inline NdsEmuBusPath nds_emu_bus_classify(uint32_t addr) {
    // 0x04800000-0x0481FFFF (Wi-Fi) sits inside the I/O aperture and is
    // counted as MMIO: it is register dispatch, just a different device's.
    if (addr >= 0x04000000u && addr < 0x05000000u) return NDS_EMU_BUS_MMIO;
    if (addr >= 0x05000000u && addr < 0x08000000u) return NDS_EMU_BUS_VRAM;
    if (addr >= 0x08000000u && addr < 0x0B000000u) return NDS_EMU_BUS_OTHER;
    return NDS_EMU_BUS_RAM;
}

// One guest memory-access slow path. Independently gated (NOT on the round
// sampler) because these fire thousands of times inside one round; riding the
// round gate would time every one of them on a sampled round and inflate that
// round's own total, which is the bias this module exists to remove.
//
// The path is classified only on a sampled access, so an unsampled one pays
// one counter increment, one countdown and a predicted branch.
class NdsEmuBusRegion {
  public:
    explicit NdsEmuBusRegion(uint32_t addr) {
        using namespace nds_emu_detail;
        ++g_profile.bus_events;
        const uint64_t modulus = g_bus_modulus;
        if (!modulus) return;
        if (!nds_dispatch_timing_detail::gate_fires(g_bus_gate, modulus))
            return;
        path_ = nds_emu_bus_classify(addr);
        start_ = nds_dispatch_timing_detail::tick_now();
        open_ = true;
    }
    ~NdsEmuBusRegion() {
        using namespace nds_emu_detail;
        if (!open_) return;
        const uint64_t delta =
            nds_dispatch_timing_detail::tick_now() - start_;
        const uint64_t overhead =
            nds_dispatch_timing_detail::g_overhead_ticks;
        g_profile.bus_ticks[path_] += delta > overhead ? delta - overhead : 0u;
        ++g_profile.bus_samples[path_];
    }
    NdsEmuBusRegion(const NdsEmuBusRegion&) = delete;
    NdsEmuBusRegion& operator=(const NdsEmuBusRegion&) = delete;

  private:
    NdsEmuBusPath path_ = NDS_EMU_BUS_RAM;
    bool open_ = false;
    uint64_t start_ = 0;
};
