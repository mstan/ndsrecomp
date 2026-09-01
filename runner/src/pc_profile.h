// pc_profile.h -- always-on guest-PC histogram: WHERE the two cores sit, as
// opposed to emu_profile.h's HOW LONG each host-side phase took.
//
// WHY THIS EXISTS. emu_profile.h partitions emu time by MACHINERY -- guest
// execution, the geometry engine, Tier 3, DMA, the raster, each device tick.
// That answers "which subsystem is expensive" and cannot answer the next
// question a dip raises: exec_arm9 is 18 ms/frame, WHICH GUEST CODE. A field
// bundle with a fat EXEC_ARM9 bucket names no address, so the only way to get
// from a player's log to a function was to reproduce the session locally under
// a host profiler, which is exactly the step field diagnostics exist to
// remove. This module closes that gap: the perf record carries the hottest
// guest PCs per interval, so a bundle points at a bank/overlay/interpreted
// span directly.
//
// TWO POPULATIONS, because one of them answers only half the question. Both
// are per CPU, both use the same table shape, and neither ever resets.
//
// ── PARK (NDS_PC_HOT_PARK): the round-boundary PC ─────────────────────────
// Once per 31 scheduler rounds -- its OWN countdown, taken BEFORE the
// emu-profile round region opens, because a first cut that rode the emu
// sampler's gate inside the region charged the hash inserts to SCHED_OTHER on
// exactly the rounds that bucket is measured (0.29 ms/f of pure artifact on
// the Kanden A/B) -- each CPU's CURRENT PC is recorded. A round boundary is a
// rendezvous, not a random instant, so this is not a uniform time sample of
// guest execution: it is a uniform sample over ROUNDS, and a round's PC is
// wherever that core was parked when its slice ended.
//
// FIELD RESULT (MPH Kanden, 2026-08-31), and the reason the second population
// below exists: ARM9 42 percent at 0x0208834C (the `bx lr` after a CP15
// wait-for-interrupt, i.e. the OS idle loop) and 40 percent at an IRQ-restore
// return; ARM7 99 percent at the BIOS halt loop. That is a truthful and useful
// measurement of HALT SHARE -- how much of a core's rounds end with it waiting
// -- and it is useless for ranking hot COMPUTE functions, because a slice ends
// where the scheduler stopped it and the scheduler stops it where the guest is
// idle. Read PARK for "how idle is this core"; never for "which function is
// expensive".
//
// The PC recorded for the CPU whose register file is LIVE (scheduler g_cur)
// comes from the live file, not from its parked slot copy -- the slot copy is
// only refreshed at a context switch, so for the live core it can be a whole
// round stale and would smear the histogram onto the last switch point.
//
// ── EXEC (NDS_PC_HOT_EXEC): the dispatch-ENTRY PC ─────────────────────────
// One sample per kNdsPcExecGate guest-function entries, taken at the single
// point every entry passes through: the `dispatch_total` increment at the top
// of runtime_dispatch_impl()'s loop (runtime_arm.cpp). Static-bank bodies,
// live-overlay bank bodies, Tier-3 stretches, BIOS bodies, B2 link-slot
// transfers, exchange/literal transfers and scheduler slice resumes all reach
// guest code through that one loop iteration, so no execution path is missing
// from the population. (The scheduler's own `resume_dispatch` counter is
// deliberately NOT a second sample site: every resume calls straight into this
// loop, so noting there too would double-weight resumed PCs.)
//
// TWO CAVEATS A READER MUST CARRY, because neither is visible in the numbers:
//
//   1. It is ENTRY-FREQUENCY weighted, NOT time weighted. A 20-instruction
//      leaf entered 100k times outranks a 50k-instruction function entered
//      twice, and for the question this answers -- which entry points dominate
//      dispatch -- that is correct. It is NOT a host profiler and must never
//      be read as "where the cycles went"; for that, cross it with
//      emu_attrib's buckets and dispatch_cost_delta's classes, which ARE time.
//      The honest use is ranking CANDIDATES: whatever sits at the top of exec
//      is where the next bank, inline or link win is, whether it got there by
//      being slow or by being called constantly.
//   2. The 1-in-N gate loses nothing SYSTEMATICALLY. The modulus is prime
//      (kNdsPcExecGate) precisely so it cannot phase-lock to a periodicity in
//      entry order: a guest loop whose body is a power-of-two number of
//      dispatches is exactly the shape that would otherwise let a power-of-two
//      countdown land on the same entry of the cycle forever, reporting one PC
//      at N times its true share while its neighbours vanished entirely. With
//      a prime gate the residue drifts across any such cycle, so the sample is
//      unbiased with respect to entry ORDER and what remains is ordinary
//      sampling noise, which total_samples lets a reader bound.
//
// COST. One multiply, one mask and at most eight compares per recorded sample;
// per skipped entry, one decrement and one perfectly-predicted branch. PARK
// costs ~19k inserts/s per CPU at 1-in-31. EXEC at MPH's 15k-70k dispatches
// per frame (0.9M-4.2M/s) records ~7k-33k/s per CPU -- tens of thousands of
// samples per 2 s diagnostics interval, far more than a top-8 needs -- for
// well under 0.1 percent of execution time. Nothing here reads a clock and
// nothing here opens a timed region, so unlike the timed regions it perturbs
// nothing it measures; EXEC's residual cost lands inside the enclosing
// NDS_EMU_EXEC_* bucket, which is exactly where the work it samples already
// is, and PARK's lands outside every bucket in the emu_attrib residual.
//
// Ring-buffer philosophy (DEBUG.md): always on, Release included, never armed,
// no start/stop, no reset -- NOT EVEN on a savestate load, unlike the timing
// accumulators. Counts only ever grow, so every consumer is a subtract of two
// snapshots and a reset would only be able to destroy history a later probe
// wanted. Probes snapshot twice and subtract.
#pragma once

#include <cstdint>
#include <string>

// Open-addressing table, one per CPU. Sized so a whole session's hot set fits
// without eviction: MPH gameplay touches a few thousand distinct round-boundary
// PCs, so 32768 slots keeps the load factor low enough that the 8-probe window
// almost never runs out (and when it does, the overflow counter says so rather
// than the table silently lying).
//
// There is no eviction and no rehash: a claimed slot keeps its PC for the life
// of the process. That is what makes a per-interval DELTA a simple per-slot
// subtraction against a previous full-table snapshot -- entries never move, so
// slot i means the same PC in both snapshots.
constexpr uint32_t kNdsPcHotSlots = 32768u;
constexpr uint32_t kNdsPcHotProbes = 8u;

// Which population a table holds. Both kinds exist per CPU and are reported
// separately everywhere; see the header note for what each one can and cannot
// answer.
enum NdsPcHotKind : uint8_t {
    NDS_PC_HOT_PARK = 0,   // round-boundary PC: halt share
    NDS_PC_HOT_EXEC,       // dispatch-entry PC: hot entry points
    // MMIO slow-path accesses. The "pc" slot key is NOT a PC here: it is the
    // register address's low 24 bits (the whole 0x04xxxxxx aperture) with bit
    // 31 set for a WRITE, so one table ranks reads and writes separately.
    // Exists because emu_attrib's bus block prices the MMIO slow path as one
    // number (0.43-0.65 ms/f on MPH, growing in fights) and an inline fast
    // path needs to know WHICH registers to serve -- a question the field
    // bundle should answer without a local reproduction, same as EXEC.
    NDS_PC_HOT_MMIO,
    NDS_PC_HOT_KIND_COUNT
};

// One EXEC sample per this many guest-function entries, per CPU.
//
// PRIME, and that is the whole design of this number rather than a detail: a
// power-of-two gate phase-locks to guest loops whose bodies are a power-of-two
// count of dispatches, which are common, and then reports one entry of the
// cycle at N times its share while the rest of the cycle never gets sampled at
// all. 127 also lands the sample rate where it needs to be: at MPH's 0.9M-4.2M
// dispatches/s it yields ~7k-33k samples/s per CPU, i.e. 14k-66k per 2 s
// diagnostics interval, which resolves a top-8 to well under a percent while
// leaving the per-entry cost at a decrement and a predicted branch.
constexpr uint64_t kNdsPcExecGate = 127u;

// One MMIO sample per this many slow-path I/O accesses, per CPU. Prime for the
// same phase-lock reason as kNdsPcExecGate; smaller because the population is
// smaller (MPH fights: ~20k slow MMIO events/frame = ~1.2M/s, so 1-in-61 is
// ~20k samples per 2 s interval) and because MMIO access sequences are the
// most rigidly periodic population of the three -- a per-scanline register
// poll IS a fixed cycle, which is exactly what a composite gate would alias.
constexpr uint64_t kNdsPcMmioGate = 61u;

// 16 bytes with the natural padding, so four slots share a cache line and the
// linear probe stays inside one or two lines.
struct NdsPcHotSlot {
    uint32_t pc = 0;
    uint64_t count = 0;   // 0 means "never claimed"; a claimed slot is >= 1
};

struct NdsPcHotTable {
    NdsPcHotSlot slots[kNdsPcHotSlots];
    // Every insert attempt, including the ones that overflowed the probe
    // window. Required to read a count as a share rather than as a bare
    // number: 4000 samples at one PC means nothing until you know whether the
    // interval took 40000 samples or 4100.
    uint64_t total_samples = 0;
    // Inserts that found eight occupied non-matching slots and were dropped.
    // Non-zero means the table is crowded and the top list is missing weight.
    uint64_t overflow = 0;
};

// One (pc, count) pair for reporting.
struct NdsPcHotEntry {
    uint32_t pc;
    uint64_t count;
};

// Live table for one population and `cpu` (0 = ARM9, 1 = ARM7). The emulation
// thread is the only writer, and every reader (the perf record, the
// debug-server handler) runs on that same thread at a safe point, so a
// reference is enough and there is no copy to pay for. The diagnostics
// prev-snapshot IS the second sample of the snapshot-twice-and-subtract
// pattern.
const NdsPcHotTable& nds_pc_profile_table(NdsPcHotKind kind, int cpu);

// "park" / "exec" -> kind, for the debug server's request parsing. Anything
// unrecognised (including an absent field) is PARK, which is the compatible
// default: pc_hot answered the park question before exec existed.
NdsPcHotKind nds_pc_profile_kind_from_name(const char* name);
const char* nds_pc_profile_kind_name(NdsPcHotKind kind);

// Top `max` entries of (now - prev), sorted count-descending. `prev` may be
// null, which means "against zero", i.e. the whole run so far. Returns how
// many entries were written. Because slots never move, the per-slot delta is
// exact whenever prev holds the same PC in that slot; a slot claimed since the
// previous snapshot contributes its whole count, which is correct.
unsigned nds_pc_profile_top_delta(const NdsPcHotTable& now,
                                  const NdsPcHotTable* prev,
                                  NdsPcHotEntry* out, unsigned max);

// Whole-run report for the debug server: {"cpu":9,"kind":"park",
// "total_samples":...,"overflow":...,"top":[{"pc":...,"count":...},...]}.
//
// `cpu` is the INDEX (0 = ARM9, 1 = ARM7), the same convention as every other
// function here and as g_slot/g_nds_dispatch_stats -- NOT the 9/7 core number
// the JSON reports back. A caller holding a 9/7 (the debug-server request, for
// one) converts first. `top` is clamped to [1, 256].
std::string nds_pc_profile_json(NdsPcHotKind kind, int cpu, unsigned top);

namespace nds_pc_detail {

extern NdsPcHotTable g_table[NDS_PC_HOT_KIND_COUNT][2];
// EXEC countdown, per CPU. Per-CPU rather than shared so a core that dispatches
// rarely (the ARM7 in a 3D-heavy title) still gets its own 1-in-N of its own
// entries instead of having its samples crowded out by the other core's rate.
extern uint64_t g_exec_gate[2];
// MMIO countdown, per CPU, same rationale.
extern uint64_t g_mmio_gate[2];

// Knuth multiplicative hash, taking the HIGH bits of the product so the
// low-order alignment structure of an ARM PC (always 2- or 4-byte aligned, so
// bits 0-1 carry almost no information) cannot collapse the index.
inline uint32_t hash_pc(uint32_t pc) {
    return (pc * 2654435761u) >> 17;   // 15 bits == kNdsPcHotSlots
}

// The shared insert. Both populations use it, so a change to the probe policy
// cannot apply to one and not the other.
inline void insert(NdsPcHotTable& table, uint32_t pc) {
    ++table.total_samples;
    const uint32_t home = hash_pc(pc);
    for (uint32_t probe = 0; probe < kNdsPcHotProbes; ++probe) {
        NdsPcHotSlot& slot =
            table.slots[(home + probe) & (kNdsPcHotSlots - 1u)];
        // Claim before match: an unclaimed slot cannot be a match, and testing
        // the count first keeps the common steady-state path (a hit on the
        // home slot) to one load and one compare.
        if (slot.count == 0u) {
            slot.pc = pc;
            slot.count = 1u;
            return;
        }
        if (slot.pc == pc) {
            ++slot.count;
            return;
        }
    }
    ++table.overflow;
}

}  // namespace nds_pc_detail

// Record one PARKED PC (scheduler round boundary). Header-inline for the same
// reason NdsEmuRound's ctor/dtor are: this sits in the scheduler round, and
// the whole operation is smaller than the call that would reach it. Ungated
// here -- the scheduler owns the 1-in-31 countdown, because it also owns the
// requirement that the note happen OUTSIDE the emu round region.
inline void nds_pc_profile_note(int cpu, uint32_t pc) {
    nds_pc_detail::insert(nds_pc_detail::g_table[NDS_PC_HOT_PARK][cpu & 1], pc);
}

// Record one guest-function ENTRY, gated 1-in-kNdsPcExecGate per CPU. Called
// from the dispatch loop's dispatch_total site (runtime_arm.cpp), which every
// entry passes through, so the gate is what keeps a site on a 4M/s path free:
// a skipped entry costs one decrement, one store and one predicted branch, and
// touches neither the 512 KB table nor a clock.
//
// The countdown is the same decrement-to-one shape as
// nds_dispatch_timing_detail::gate_fires -- no modulo on the hot path, and the
// modulus may therefore be any prime without paying for a division.
inline void nds_pc_profile_note_exec(int cpu, uint32_t pc) {
    uint64_t& gate = nds_pc_detail::g_exec_gate[cpu & 1];
    if (gate > 1u) {
        --gate;
        return;
    }
    gate = kNdsPcExecGate;
    nds_pc_detail::insert(nds_pc_detail::g_table[NDS_PC_HOT_EXEC][cpu & 1], pc);
}

// Record one slow-path MMIO access, gated 1-in-kNdsPcMmioGate per CPU. Called
// from the bus slow path's I/O arm (bus.cpp io_read/io_write), which is the
// population emu_attrib's NDS_EMU_BUS_MMIO bucket prices -- so the two are
// read together: the bucket says what the slow path costs, this table says
// which registers it serves. The key packs the register address's low 24 bits
// with bit 31 set for a write; a consumer splits it back the same way.
inline void nds_pc_profile_note_mmio(int cpu, uint32_t addr, bool write) {
    uint64_t& gate = nds_pc_detail::g_mmio_gate[cpu & 1];
    if (gate > 1u) {
        --gate;
        return;
    }
    gate = kNdsPcMmioGate;
    const uint32_t key = (addr & 0x00FFFFFFu) | (write ? 0x80000000u : 0u);
    nds_pc_detail::insert(nds_pc_detail::g_table[NDS_PC_HOT_MMIO][cpu & 1],
                          key);
}
