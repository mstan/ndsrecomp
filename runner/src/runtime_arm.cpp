// runtime_arm.cpp — DS runner implementation of the recomp C ABI.
//
// The condition-code / shifter / flag-updater / PSR-banking / call-return
// helpers are reused verbatim from the (verified) single-CPU runtime in
// recompiler/armv4t/runtime_arm.cpp — they are CPU-architecture-agnostic.
// What is DS-specific and rewritten here: per-CPU dispatch registration,
// the exception-vector base (ARM9 high vectors vs ARM7 low), a simple
// always-on trace ring, the cooperative yield/halt, and lifecycle.

#include "runtime_arm.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "state.h"
#include "io.h"
#include "tier3.h"

// ── Globals the ABI exposes ─────────────────────────────────────────────
extern "C" ArmCpuState g_cpu = {};
extern "C" unsigned long long g_runtime_cycles = 0;
// On by default: the recompiled banks gate their per-instruction hook on this,
// and the hook is what keeps the always-on insn9/insn7 retired-instruction
// counters live (DEBUG.md "all on in Release"). The hook only bumps a counter
// + break-check — no control-flow / cycle effect — so behaviour is unchanged.
extern "C" unsigned g_runtime_insn_trace = 1;
extern "C" uint32_t g_runtime_break_pc = 0;

NdsCpu      g_nds_active = NDS_ARM9;
bool        g_nds_terminal = false;
const char* g_nds_halt_reason = nullptr;
bool        g_discover_static_misses = false;

// ── Per-CPU dispatch registration ───────────────────────────────────────
// DispatchEntry is declared in state.h (layout-matches the generated table).

namespace {
struct DispatchBank {
    const DispatchEntry* table = nullptr;
    unsigned             len = 0;
};
struct CpuCtx {
    std::vector<DispatchBank> banks;
    uint32_t             exc_base = 0;  // exception-vector base for this CPU
};
CpuCtx g_ctx[2];

struct StaticLookup {
    void (*fn)(void) = nullptr;
    const NdsStaticValidation* validation = nullptr;
};

// Generated firmware functions are capped at 512 guest bytes, so a function
// can overlap at most two 4 KiB executable pages. A larger future validation
// is rejected safely instead of running without an active-code guard.
struct StaticExecutionGuard {
    const NdsStaticValidation* validation = nullptr;
    uint32_t page_addr[2] = {};
    uint32_t generation[2] = {};
    uint32_t page_count = 0u;
    bool invalidated = false;
};
StaticExecutionGuard g_static_guard;

constexpr uint32_t kDispatchCacheSize = 4096u;
struct CachedStaticLookup {
    uint32_t pc = 0u;
    uint32_t generation[2]{};
    StaticLookup hit{};
    uint8_t page_count = 0u;
    uint8_t thumb = 0u;
    uint8_t occupied = 0u;
};
std::array<std::array<CachedStaticLookup, kDispatchCacheSize>, 2>
    g_dispatch_cache{};

// Cycle budget for the run slice; runtime_should_yield trips when reached
// so a guest spin loop can't hang the host.
unsigned long long g_cycle_cap = 0;  // 0 = unlimited
std::vector<uint64_t> g_discovery_seen;
uint32_t g_yield_poll_hint = 1u;
bool g_cpu_fast_poll = true;

void request_yield_poll() { g_yield_poll_hint = 1u; }

bool configured_cpu_fast_poll() {
    static const bool enabled = [] {
        const char* value = std::getenv("NDS_CPU_FAST_POLL");
        if (!value || (value[0] == '1' && value[1] == '\0')) return true;
        if (value[0] == '0' && value[1] == '\0') return false;
        std::fprintf(stderr,
                     "invalid NDS_CPU_FAST_POLL value (expected 0 or 1); "
                     "using faithful full polling\n");
        return false;
    }();
    return enabled;
}

bool static_bios_pc(uint32_t pc) {
    return (g_nds_active == NDS_ARM9)
        ? (pc >= 0xFFFF0000u && pc < 0xFFFF1000u)
        : (pc < 0x00004000u);
}

StaticLookup lookup_in(const DispatchEntry* table, unsigned len,
                       uint32_t pc, bool thumb) {
    if (!table) return {};
    unsigned lo = 0, hi = len;
    while (lo < hi) {
        unsigned mid = (lo + hi) >> 1u;
        if (table[mid].addr < pc) lo = mid + 1u;
        else                       hi = mid;
    }
    for (unsigned i = lo; i < len && table[i].addr == pc; ++i) {
        if ((table[i].thumb != 0) != thumb) continue;
        const NdsStaticValidation* validation = table[i].validation;
        if (validation) {
            // Captured firmware variants are executable only after the LLE
            // guest or one of its hardware bus masters has actually
            // installed every backing page. Byte equality alone is not
            // provenance: pristine zero-filled RAM can otherwise select a
            // late all-zero variant before firmware boot has materialized it.
            if (!bus_range_has_write_provenance(validation->addr,
                                                validation->size) ||
                !bus_live_bytes_equal(validation->addr,
                                      validation->expected,
                                      validation->size))
                continue;
        }
        return {table[i].fn, validation};
    }
    return {};
}

StaticLookup lookup_static(const CpuCtx& c, uint32_t pc, bool thumb) {
    for (const DispatchBank& bank : c.banks) {
        StaticLookup hit = lookup_in(bank.table, bank.len, pc, thumb);
        if (hit.fn) return hit;
    }
    return {};
}

bool cached_lookup_live(const CachedStaticLookup& cached) {
    if (!cached.hit.validation) return true;
    const uint32_t first_page = cached.hit.validation->addr & ~0xFFFu;
    for (uint32_t i = 0; i < cached.page_count; ++i)
        if (bus_exec_page_generation(first_page + (i << 12u)) !=
            cached.generation[i])
            return false;
    return true;
}

StaticLookup lookup_static_cached(const CpuCtx& c, uint32_t pc, bool thumb) {
    auto& cache = g_dispatch_cache[g_nds_active];
    CachedStaticLookup& slot =
        cache[((pc >> 1u) ^ (pc >> 13u) ^ uint32_t{thumb}) &
              (kDispatchCacheSize - 1u)];
    if (slot.occupied && slot.pc == pc && slot.thumb == uint8_t{thumb} &&
        cached_lookup_live(slot))
        return slot.hit;

    const StaticLookup hit = lookup_static(c, pc, thumb);
    if (!hit.fn) return {};
    slot = {};
    slot.pc = pc;
    slot.thumb = static_cast<uint8_t>(thumb);
    slot.hit = hit;
    slot.occupied = 1u;
    if (hit.validation) {
        const uint64_t end = uint64_t{hit.validation->addr} +
                             hit.validation->size;
        const uint32_t first_page = hit.validation->addr & ~0xFFFu;
        const uint32_t last_page =
            static_cast<uint32_t>(end - 1u) & ~0xFFFu;
        slot.page_count = static_cast<uint8_t>(
            ((last_page - first_page) >> 12u) + 1u);
        if (slot.page_count > 2u) {
            slot = {};
            return hit;
        }
        for (uint32_t i = 0; i < slot.page_count; ++i)
            slot.generation[i] =
                bus_exec_page_generation(first_page + (i << 12u));
    }
    return hit;
}

bool arm_static_guard(const NdsStaticValidation* validation) {
    g_static_guard = {};
    if (!validation) return true;
    const uint64_t begin = validation->addr;
    const uint64_t end = begin + validation->size;
    if (validation->size == 0u || end > 0x1'0000'0000ull) return false;
    const uint32_t first_page = validation->addr & ~0xFFFu;
    const uint32_t last_page =
        static_cast<uint32_t>(end - 1u) & ~0xFFFu;
    const uint32_t page_count = ((last_page - first_page) >> 12u) + 1u;
    if (page_count > 2u) return false;
    g_static_guard.validation = validation;
    g_static_guard.page_count = page_count;
    for (uint32_t i = 0; i < page_count; ++i) {
        const uint32_t page = first_page + (i << 12u);
        g_static_guard.page_addr[i] = page;
        g_static_guard.generation[i] = bus_exec_page_generation(page);
    }
    return true;
}

bool guard_generation_changed(const StaticExecutionGuard& guard) {
    if (!guard.validation) return false;
    for (uint32_t i = 0; i < guard.page_count; ++i)
        if (bus_exec_page_generation(guard.page_addr[i]) !=
            guard.generation[i])
            return true;
    return false;
}

bool active_static_code_changed() {
    return g_static_guard.invalidated;
}

struct StaticGuardScope {
    StaticExecutionGuard saved;
    StaticGuardScope() : saved(g_static_guard) { g_static_guard = {}; }
    ~StaticGuardScope() {
        // A nested static call may write through an alias of its caller's
        // backing page. Revalidate the saved guard once on unwind so that
        // write cannot be lost when the outer guard becomes active again.
        if (!saved.invalidated && guard_generation_changed(saved))
            saved.invalidated = true;
        g_static_guard = saved;
        if (saved.invalidated) request_yield_poll();
    }
};

bool guarded_static_call(const StaticLookup& hit) {
    if (!hit.fn || !arm_static_guard(hit.validation)) return false;
    hit.fn();
    return true;
}

// Bracket the static function range containing `pc` in a dispatch table
// (sorted by addr): [*start, *end). Returns false if the table is empty or
// `pc` precedes the first entry. Used only by the dispatch-miss diagnostic
// to localize a non-function-start entry to its containing recompiled func.
bool bracket_static_range(const DispatchEntry* table, unsigned len,
                          uint32_t pc, uint32_t* start, uint32_t* end) {
    if (!table || len == 0) return false;
    unsigned lo = 0, hi = len;
    while (lo < hi) {                       // first index with addr > pc
        unsigned mid = (lo + hi) >> 1u;
        if (table[mid].addr <= pc) lo = mid + 1u; else hi = mid;
    }
    if (lo == 0) return false;              // pc precedes the first entry
    *start = table[lo - 1u].addr;
    unsigned j = lo;                        // first entry with a larger addr
    while (j < len && table[j].addr == *start) ++j;
    *end = (j < len) ? table[j].addr : *start;
    return true;
}
}  // namespace

extern "C" void runtime_request_yield_poll(void) { request_yield_poll(); }

extern "C" void runtime_note_code_write(void) {
    if (!g_static_guard.invalidated &&
        guard_generation_changed(g_static_guard)) {
        g_static_guard.invalidated = true;
        request_yield_poll();
    }
}

extern "C" void nds_register_dispatch(int cpu, const DispatchEntry* t,
                                      unsigned len, uint32_t exc_base) {
    CpuCtx& ctx = g_ctx[cpu & 1];
    ctx.banks.push_back({t, len});
    ctx.exc_base = exc_base;
}

extern "C" void nds_set_cycle_cap(unsigned long long cap) { g_cycle_cap = cap; }
extern "C" void nds_reschedule_slice(unsigned long long system_deadline) {
    const unsigned long long cpu_deadline =
        (g_nds_active == NDS_ARM9) ? (system_deadline << 1u) : system_deadline;
    if (g_cycle_cap == 0 || cpu_deadline < g_cycle_cap)
        g_cycle_cap = cpu_deadline;
}

// Tier-3 helpers: does the active CPU have a Tier-1 bank fn at (pc, thumb)?
// And is this slice's cycle cap reached? (The interpreter uses these to
// decide when to hand control back to the dispatcher / scheduler.)
extern "C" int nds_has_bank(uint32_t pc, int thumb) {
    const CpuCtx& c = g_ctx[g_nds_active];
    return lookup_static_cached(c, pc & ~1u, thumb != 0).fn ? 1 : 0;
}
extern "C" int nds_slice_over(void) {
    return (g_cycle_cap != 0 && g_runtime_cycles >= g_cycle_cap) ? 1 : 0;
}
extern "C" uint32_t nds_exception_base(void) {
    return g_ctx[g_nds_active].exc_base;
}

// Terminal halt for the active CPU: unwind now and don't resume it.
extern "C" void nds_halt(const char* reason) {
    g_nds_terminal = true;
    g_nds_halt_reason = reason;
    request_yield_poll();
}

// Slice control used by the scheduler: arm the cycle cap, clear terminal.
extern "C" void nds_slice_begin(unsigned long long cap) {
    g_nds_terminal = false;
    g_cycle_cap = cap;
    request_yield_poll();
}

// ── Trace ring (dispatch / branch / swi / irq events) ───────────────────
namespace {
constexpr uint32_t kTraceSize = 4096;
RuntimeTraceEntry g_trace[kTraceSize] = {};
uint32_t g_trace_w = 0, g_trace_count = 0, g_trace_seq = 0;
const char* trace_kind_name(uint32_t k) {
    switch (k) {
        case RUNTIME_TRACE_DISPATCH: return "dispatch";
        case RUNTIME_TRACE_EXCHANGE: return "exchange";
        case RUNTIME_TRACE_SWI:      return "swi";
        case RUNTIME_TRACE_MEM_WRITE:return "mem_w";
        case RUNTIME_TRACE_BRANCH:   return "branch";
        case RUNTIME_TRACE_IRQ:      return "irq";
        case RUNTIME_TRACE_CALL:     return "call";
        case RUNTIME_TRACE_MEM_READ: return "mem_r";
        default:                     return "?";
    }
}
}  // namespace

extern "C" uint32_t g_runtime_deep_trace = 1u;

extern "C" void runtime_set_deep_trace(uint32_t on) {
    g_runtime_deep_trace = on ? 1u : 0u;
    nds_insn_hook_recompute();
}

extern "C" void runtime_trace_event(uint32_t kind, uint32_t pc, uint32_t addr,
                                    uint32_t value, uint32_t aux) {
    // Per-store/per-load events fire for every guest memory access, so they
    // are the one trace class gated by the deep-trace policy: every mode
    // with a query surface (--serve, batch with its exit tail dump) keeps
    // them on; the interactive frontend has no debug server, so recording
    // them there costs real time with no way to ever read them back.
    // Block-level events (dispatch/exchange/call/swi/irq) stay unconditional.
    if ((kind == RUNTIME_TRACE_MEM_WRITE || kind == RUNTIME_TRACE_MEM_READ) &&
        !g_runtime_deep_trace)
        return;
    RuntimeTraceEntry& e = g_trace[g_trace_w];
    e.seq = ++g_trace_seq; e.cycles = g_runtime_cycles; e.kind = kind;
    e.pc = pc; e.cpsr = g_cpu.cpsr; e.addr = addr; e.value = value; e.aux = aux;
    e.r0 = g_cpu.R[0]; e.r1 = g_cpu.R[1]; e.r2 = g_cpu.R[2]; e.r3 = g_cpu.R[3];
    e.r4 = g_cpu.R[4]; e.r5 = g_cpu.R[5]; e.r12 = g_cpu.R[12];
    e.r13 = g_cpu.R[13]; e.r14 = g_cpu.R[14];
    g_trace_w = (g_trace_w + 1u) % kTraceSize;
    if (g_trace_count < kTraceSize) ++g_trace_count;
}

extern "C" void runtime_trace_reset(void) {
    g_trace_w = g_trace_count = g_trace_seq = 0;
    g_runtime_cycles = 0;
    // Always on: the per-insn hook owns the insn9/insn7 retired counters
    // (event ordinals for run_to_event / traversal / selftest), which must
    // count in every mode. The deep-trace policy only gates the hook's
    // ring-entry payload (see nds_note_insn_retired).
    g_runtime_insn_trace = 1u;
}

extern "C" void runtime_trace_dump_recent(uint32_t max_entries) {
    if (max_entries > g_trace_count) max_entries = g_trace_count;
    std::fprintf(stderr, "[trace] last %u event(s):\n", max_entries);
    uint32_t start = (g_trace_w + kTraceSize - max_entries) % kTraceSize;
    for (uint32_t i = 0; i < max_entries; ++i) {
        const RuntimeTraceEntry& e = g_trace[(start + i) % kTraceSize];
        std::fprintf(stderr,
            "  #%u %-8s pc=0x%08X cpsr=0x%08X addr=0x%08X val=0x%08X aux=0x%X "
            "sp=0x%08X lr=0x%08X\n",
            e.seq, trace_kind_name(e.kind), e.pc, e.cpsr, e.addr, e.value,
            e.aux, e.r13, e.r14);
    }
}

extern "C" uint32_t runtime_trace_copy_recent(RuntimeTraceEntry* out,
                                              uint32_t max_entries) {
    if (!out || !max_entries) return 0;
    if (max_entries > g_trace_count) max_entries = g_trace_count;
    uint32_t start = (g_trace_w + kTraceSize - max_entries) % kTraceSize;
    for (uint32_t i = 0; i < max_entries; ++i)
        out[i] = g_trace[(start + i) % kTraceSize];
    return max_entries;
}

namespace {
// Pending melonDS ARM::Cycles debt carried across a HALT. This is distinct
// from g_runtime_cycles (the committed CPU timestamp) and is saved/restored by
// the dual-CPU scheduler just like the call-return stack.
uint32_t g_deferred_cycles = 0;
}

// Per-instruction hook, fired once at the top of every recompiled-bank guest
// instruction (g_runtime_insn_trace on). Bumps the active CPU's retired-insn
// counter so insn9/insn7 can anchor the fp-stream bisector. Tier-3 bumps the
// same counters from its own step loop (tier3.cpp).
extern "C" void runtime_insn_fp(void) {
    nds_note_insn_retired(g_nds_active);
    // Timing belongs to each generated instruction's runtime_tick expression,
    // not this observer. ARM7 AddCycles_CD/CDI reconstructs the complete
    // nonsequential code cost; adding the sequential correction here too
    // double-charges loads when ARM7 executes on the 16-bit main-RAM bus.
}
extern "C" void runtime_fp_reset(void) {}
extern "C" uint32_t runtime_fp_count(void) { return 0; }
extern "C" uint32_t runtime_fp_save_file(const char*) { return 0; }

// ── Tick / yield ────────────────────────────────────────────────────────
extern "C" void runtime_tick(uint32_t cycles) {
    // Generated-code ticks are guest instruction boundaries. Commit any
    // ARM::Cycles debt carried across HALT together with this instruction.
    g_runtime_cycles += cycles + g_deferred_cycles;
    g_deferred_cycles = 0;
    // Deliver a pending IRQ to the active CPU at this instruction boundary
    // (R15 already points at the next instruction = the return address).
    // runtime_irq masks CPSR.I before vectoring, so it cannot re-enter here
    // while the handler runs.
    const uint32_t pending = g_cpu_fast_poll
        ? g_nds_irq_pending_cache[g_nds_active]
        : nds_irq_pending(g_nds_active);
    if (!(g_cpu.cpsr & CPSR_I_BIT) && pending)
        runtime_irq(g_cpu.R[15]);
}
// Per-instruction unwind: terminal halts only (a guest spin waiting on the
// other core is NOT a fault — it is preempted at a backward branch instead).
namespace {
bool g_unwinding = false;
bool g_preserved_unwind_state_valid = false;
ArmCpuState g_preserved_unwind_state{};
}

extern "C" bool runtime_should_yield(void) {
    // In fast mode rare state transitions eagerly set the hint. While it is
    // clear, only the two per-instruction dynamic predicates remain. The full
    // scan below is unchanged and is also the NDS_CPU_FAST_POLL=0 reference.
    if (g_cpu_fast_poll && !g_yield_poll_hint) {
        const bool break_pc_hit =
            g_runtime_break_pc &&
            (g_cpu.R[15] & ~1u) == (g_runtime_break_pc & ~1u);
        const bool cycle_cap_hit =
            g_cycle_cap != 0 && g_runtime_cycles >= g_cycle_cap;
        if (!break_pc_hit && !cycle_cap_hit) return false;
    }

    // insn7/insn9 anchor reached → stop at this exact instruction (see io.cpp
    // g_nds_insn_stop). The bisector resets per K, so the mid-function unwind
    // (which does not preserve the call-return stack) is never resumed from.
    if (g_nds_insn_stop || nds_event_break_hit()) {
        g_unwinding = true;
        return true;
    }
    // HALTCNT/CP15 sleep is a resumable hardware state. Unwind the current
    // static function before its next instruction; the scheduler owns wakeup
    // and timestamp advancement while no guest instructions retire.
    if (nds_cpu_halted(g_nds_active) || nds_dma_cpu_stalled(g_nds_active)) {
        g_unwinding = true;
        return true;
    }
    // A guest store touched a page containing the currently executing
    // content-validated function. Stop before the next guest instruction;
    // redispatch will either select a matching generation or enter Tier 3 for
    // the guest's newly written bytes. Never continue stale native semantics.
    if (active_static_code_changed()) {
        g_unwinding = true;
        return true;
    }
    if (g_runtime_break_pc &&
        (g_cpu.R[15] & ~1u) == (g_runtime_break_pc & ~1u))
        nds_halt("break pc");
    if (g_cycle_cap != 0 && g_runtime_cycles >= g_cycle_cap) {
        g_unwinding = true;
        return true;
    }
    if (g_nds_terminal) { g_unwinding = true; return true; }
    g_yield_poll_hint = 0u;
    return false;
}
// Cooperative slice preemption: trips once this slice's cycle cap is
// reached. Checked only at backward branches (loop tops = dispatch
// entries). When it fires it arms g_unwinding so the BL/BLX return-checks
// PRESERVE their pending returns as the host stack unwinds — the call-
// return stack is saved/restored per-CPU by the scheduler, so a spin at
// any call depth can be preempted and cleanly resumed.
extern "C" bool runtime_slice_yield(void) {
    if (g_cycle_cap != 0 && g_runtime_cycles >= g_cycle_cap) {
        g_unwinding = true;
        return true;
    }
    return false;
}
extern "C" bool runtime_unwinding(void) { return g_unwinding; }
extern "C" void nds_clear_unwinding(void) {
    g_unwinding = false;
    g_preserved_unwind_state_valid = false;
}
void nds_preserve_unwind_state() {
    g_preserved_unwind_state = g_cpu;
    g_preserved_unwind_state_valid = true;
    g_unwinding = true;
}
void nds_restore_unwind_state() {
    if (!g_preserved_unwind_state_valid) return;
    g_cpu = g_preserved_unwind_state;
    g_preserved_unwind_state_valid = false;
}

// ── Condition codes (verbatim) ──────────────────────────────────────────
extern "C" int arm_cond_passes(unsigned cond) {
    const uint32_t n = cpsr_n(), z = cpsr_z(), c = cpsr_c(), v = cpsr_v();
    switch (cond & 0xFu) {
        case 0x0: return z != 0;                 case 0x1: return z == 0;
        case 0x2: return c != 0;                 case 0x3: return c == 0;
        case 0x4: return n != 0;                 case 0x5: return n == 0;
        case 0x6: return v != 0;                 case 0x7: return v == 0;
        case 0x8: return (c != 0) && (z == 0);   case 0x9: return (c == 0) || (z != 0);
        case 0xA: return n == v;                 case 0xB: return n != v;
        case 0xC: return (z == 0) && (n == v);   case 0xD: return (z != 0) || (n != v);
        case 0xE: return 1;                      default:  return 0;  // NV
    }
}

// ── Shifters (verbatim) ─────────────────────────────────────────────────
extern "C" uint32_t arm_shift_lsl(uint32_t v, uint32_t n, int sc) {
    if (n == 0) return v;
    if (n >= 32) { if (sc) { uint32_t c = (n == 32) ? (v & 1u) : 0u;
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (c ? CPSR_C_BIT : 0u); } return 0; }
    if (sc) { uint32_t c = (v >> (32u - n)) & 1u;
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (c ? CPSR_C_BIT : 0u); }
    return v << n;
}
extern "C" uint32_t arm_shift_lsr(uint32_t v, uint32_t n, int sc) {
    if (n == 0) return v;
    if (n >= 32) { if (sc) { uint32_t c = (n == 32) ? ((v >> 31) & 1u) : 0u;
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (c ? CPSR_C_BIT : 0u); } return 0; }
    if (sc) { uint32_t c = (v >> (n - 1u)) & 1u;
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (c ? CPSR_C_BIT : 0u); }
    return v >> n;
}
extern "C" uint32_t arm_shift_asr(uint32_t v, uint32_t n, int sc) {
    if (n == 0) return v;
    if (n >= 32) { uint32_t c = (v >> 31) & 1u;
        if (sc) g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (c ? CPSR_C_BIT : 0u);
        return c ? 0xFFFFFFFFu : 0u; }
    if (sc) { uint32_t c = (v >> (n - 1u)) & 1u;
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (c ? CPSR_C_BIT : 0u); }
    return static_cast<uint32_t>(static_cast<int32_t>(v) >> n);
}
extern "C" uint32_t arm_shift_ror(uint32_t v, uint32_t n, int sc) {
    if (n == 0) return v;
    n &= 31u;
    if (n == 0) { if (sc) g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) |
        ((v & 0x80000000u) ? CPSR_C_BIT : 0u); return v; }
    uint32_t r = (v >> n) | (v << (32u - n));
    if (sc) g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | ((r & 0x80000000u) ? CPSR_C_BIT : 0u);
    return r;
}

// CLZ helper (ARMv5).
extern "C" uint32_t runtime_clz(uint32_t v) {
    return v ? static_cast<uint32_t>(__builtin_clz(v)) : 32u;
}
// ARM7TDMI early-termination multiply timing. ARM9 uses its separate static
// numI combine, so this runtime value is ignored on that path.
extern "C" uint32_t runtime_mul_cycles(uint32_t v, uint32_t signed_variant,
                                        uint32_t extra) {
    if (g_nds_active == NDS_ARM9) return 1u;
    if (g_cpu.cpsr & CPSR_T_BIT) {
        // melonDS T_MUL_REG tests the original destination operand by unsigned
        // significant bytes and documents C as destroyed (modeled as clear).
        g_cpu.cpsr &= ~CPSR_C_BIT;
        if (v & 0xFF000000u) return 4u;
        if (v & 0x00FF0000u) return 3u;
        if (v & 0x0000FF00u) return 2u;
        return 1u;
    }

    uint32_t m;
    if (signed_variant) {
        if ((v & 0xFFFFFF00u) == 0u || (v & 0xFFFFFF00u) == 0xFFFFFF00u) m = 1u;
        else if ((v & 0xFFFF0000u) == 0u || (v & 0xFFFF0000u) == 0xFFFF0000u) m = 2u;
        else if ((v & 0xFF000000u) == 0u || (v & 0xFF000000u) == 0xFF000000u) m = 3u;
        else m = 4u;
    } else {
        if ((v & 0xFFFFFF00u) == 0u) m = 1u;
        else if ((v & 0xFFFF0000u) == 0u) m = 2u;
        else if ((v & 0xFF000000u) == 0u) m = 3u;
        else m = 4u;
    }
    return m + extra;
}

// ── Flag updaters (verbatim) ────────────────────────────────────────────
extern "C" void arm_set_nz(uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT; if (r == 0) c |= CPSR_Z_BIT;
    g_cpu.cpsr = c;
}
extern "C" void arm_set_nzc_logic(uint32_t r, uint32_t sh) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT | CPSR_C_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT; if (r == 0) c |= CPSR_Z_BIT;
    if (sh) c |= CPSR_C_BIT; g_cpu.cpsr = c;
}
extern "C" void arm_set_nzcv_add(uint32_t a, uint32_t b, uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT | CPSR_C_BIT | CPSR_V_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT; if (r == 0) c |= CPSR_Z_BIT;
    if (r < a) c |= CPSR_C_BIT;
    if ((~(a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT; g_cpu.cpsr = c;
}
extern "C" void arm_set_nzcv_adc(uint32_t a, uint32_t b, uint32_t ci, uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT | CPSR_C_BIT | CPSR_V_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT; if (r == 0) c |= CPSR_Z_BIT;
    uint64_t w = static_cast<uint64_t>(a) + b + ci; if (w >> 32) c |= CPSR_C_BIT;
    if ((~(a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT; g_cpu.cpsr = c;
}
extern "C" void arm_set_nzcv_sub(uint32_t a, uint32_t b, uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT | CPSR_C_BIT | CPSR_V_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT; if (r == 0) c |= CPSR_Z_BIT;
    if (a >= b) c |= CPSR_C_BIT;
    if (((a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT; g_cpu.cpsr = c;
}
extern "C" void arm_set_nzcv_sbc(uint32_t a, uint32_t b, uint32_t ci, uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT | CPSR_C_BIT | CPSR_V_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT; if (r == 0) c |= CPSR_Z_BIT;
    uint64_t w = static_cast<uint64_t>(a) + (~b & 0xFFFFFFFFu) + ci;
    if (w >> 32) c |= CPSR_C_BIT;
    if (((a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT; g_cpu.cpsr = c;
}

// ── Dispatch ────────────────────────────────────────────────────────────
extern "C" void runtime_dispatch(uint32_t target_pc) {
    uint32_t pc = target_pc & ~1u;
    // Slice-preemption point. `pc` is a dispatch entry, so this is a safe
    // place to yield to the scheduler — including for loops whose back-edge
    // is an INDIRECT transfer (BX / pop pc / computed jump) and therefore
    // has no backward-branch yield site. runtime_slice_yield() arms the
    // unwind so pending BL/BLX returns are preserved.
    if (runtime_slice_yield()) { g_cpu.R[15] = pc; return; }
    // CPSR.T owns the instruction-set state. Some interpreted BX/POP paths
    // preserve the interworking bit in their target value; generated bank
    // prologues and the architectural register view require aligned R15.
    g_cpu.R[15] = pc;
    runtime_trace_event(RUNTIME_TRACE_DISPATCH, pc, target_pc, 0, 0);
    bool thumb = (g_cpu.cpsr & CPSR_T_BIT) != 0;
    const CpuCtx& c = g_ctx[g_nds_active];
    StaticGuardScope guard_scope;
    if (guarded_static_call(lookup_static_cached(c, pc, thumb))) return;
    if (g_discover_static_misses && static_bios_pc(pc)) {
        runtime_discovery_note_static(pc, thumb ? 1u : 0u);
        tier3_run(pc);
        return;
    }
    // Tier 3: code copied into RAM at runtime (firmware boot, menu, and the
    // ITCM-resident IRQ handler) has no static bank — run the guest's OWN
    // bytes through the interpreter (PRINCIPLES.md "the one exception"),
    // never an HLE model. The bus owns the memory map (covers ITCM mirror).
    if (bus_range_has_write_provenance(pc, thumb ? 2u : 4u)) {
        tier3_run(pc);
        return;
    }
    if (bus_addr_is_writable_ram(pc)) tier3_note_clean_ram_reject();
    runtime_dispatch_miss(target_pc);
}

extern "C" void runtime_discovery_note_static(uint32_t pc, uint32_t thumb) {
    pc &= ~1u;
    if (!static_bios_pc(pc)) return;
    const uint64_t key = (uint64_t(g_nds_active) << 33u) |
                         (uint64_t(thumb != 0u) << 32u) | pc;
    for (uint64_t seen : g_discovery_seen)
        if (seen == key) return;
    g_discovery_seen.push_back(key);

    const char* cpu = (g_nds_active == NDS_ARM9) ? "arm9" : "arm7";
    const char* mode = thumb ? "thumb" : "arm";
    std::fprintf(stderr,
        "[static-discovery] cpu=%s pc=0x%08X %s lr=0x%08X\n",
        cpu, pc, mode, g_cpu.R[14]);
    if (std::FILE* f = std::fopen("dispatch_candidates.log", "ab")) {
        std::fprintf(f,
            "# cpu=%s lr=0x%08X (discovery interpreter; validate before promotion)\n"
            "[[entry_point]]\naddr = 0x%08X\nmode = \"%s\"\n"
            "kind = \"runtime_candidate\"\n\n",
            cpu, g_cpu.R[14], pc, mode);
        std::fclose(f);
    }
}
extern "C" void runtime_dispatch_with_exchange(uint32_t target_pc) {
    if (target_pc & 1u) g_cpu.cpsr |= CPSR_T_BIT; else g_cpu.cpsr &= ~CPSR_T_BIT;
    runtime_trace_event(RUNTIME_TRACE_EXCHANGE, target_pc & ~1u, target_pc, 0, 0);
    runtime_dispatch(target_pc);
}

extern "C" void runtime_dispatch_miss(uint32_t target_pc) {
    const char* cpu = (g_nds_active == NDS_ARM9) ? "arm9" : "arm7";
    const bool thumb = (g_cpu.cpsr & CPSR_T_BIT) != 0;
    const char* mode = thumb ? "thumb" : "arm";
    const uint32_t t = target_pc & ~1u;
    std::fprintf(stderr,
        "[dispatch-miss] cpu=%s pc=0x%08X %s (lr=0x%08X)\n",
        cpu, t, mode, g_cpu.R[14]);

    // A miss INSIDE recompiled static ROM is a real non-function-start entry
    // PC (a BIOS-ABI landing pad reached by a runtime-computed branch), NOT a
    // Tier-3/HLE case. The finder's landing-pad discovery should normally seed
    // it; if one slips through, it is a genuine entry fact — declare it. Never
    // route static ROM to Tier-3.
    const bool arm9 = (g_nds_active == NDS_ARM9);
    const bool in_static_rom =
        (arm9 && t >= 0xFFFF0000u) || (!arm9 && t < 0x00004000u);

    // Localize the pad to its containing recompiled function.
    uint32_t rs = 0, re = 0;
    const CpuCtx& cc = g_ctx[g_nds_active];
    bool have_range = false;
    for (const DispatchBank& bank : cc.banks) {
        if (bracket_static_range(bank.table, bank.len, t, &rs, &re)) {
            have_range = true;
            break;
        }
    }

    if (in_static_rom) {
        std::fprintf(stderr,
            "  [!] dispatch miss INSIDE executable static %s BIOS ROM at 0x%08X\n"
            "      real non-function-start entry reached by a computed branch.\n"
            "      The finder's landing-pad discovery should seed it; if not,\n"
            "      add this entry_point to the bank config (do NOT interpret ROM):\n",
            arm9 ? "ARM9" : "ARM7", t);
        if (have_range)
            std::fprintf(stderr,
                "      # containing static range: 0x%08X..0x%08X\n", rs, re);
        std::fprintf(stderr,
            "        [[entry_point]]\n"
            "        addr = 0x%08X\n"
            "        mode = \"%s\"\n"
            "        kind = \"runtime_confirmed\"\n",
            t, mode);
    }

    // Discovery-loop log (CLAUDE.md BUILD LOOP step 5): a copy-pasteable
    // [[entry_point]] block per miss, directly appendable to the config.
    if (std::FILE* f = std::fopen("dispatch_misses.log", "ab")) {
        std::fprintf(f, "# cpu=%s pc=0x%08X lr=0x%08X%s\n",
                     cpu, t, g_cpu.R[14],
                     in_static_rom ? " (static ROM non-function-start entry)"
                                   : "");
        if (in_static_rom && have_range)
            std::fprintf(f, "#   containing static range 0x%08X..0x%08X\n",
                         rs, re);
        std::fprintf(f,
            "[[entry_point]]\naddr = 0x%08X\nmode = \"%s\"\n"
            "kind = \"runtime_confirmed\"\n\n",
            t, mode);
        std::fclose(f);
    }

    // RAM-resident target (copied firmware code): this is the Tier-3 dirty-RAM
    // case — dump the bytes there to show what would run.
    if (bus_addr_is_writable_ram(t)) {
        std::fprintf(stderr, "  [ram-provenance] generation=%u (%s)\n",
                     bus_exec_page_generation(t),
                     bus_addr_has_write_provenance(t) ? "written" : "clean");
        std::fprintf(stderr, "  [ram@0x%08X]", t);
        for (int i = 0; i < 16; i += 4)
            std::fprintf(stderr, " %08X", bus_read_u32(t + i));
        std::fprintf(stderr, "\n");
    }

    runtime_trace_dump_recent(16);
    nds_halt("dispatch miss");
}

// ── Call-return stack (verbatim) ────────────────────────────────────────
namespace {
constexpr uint32_t kCRS = 1024;
uint32_t g_crs[kCRS] = {};
uint32_t g_crs_depth = 0;
}  // namespace
extern "C" void runtime_call_push_return(uint32_t return_pc) {
    uint32_t pc = return_pc & ~1u;
    uint32_t key = pc | ((g_cpu.cpsr & CPSR_T_BIT) ? 1u : 0u);
    if (g_crs_depth >= kCRS) { nds_halt("call-return overflow"); return; }
    g_crs[g_crs_depth++] = key;
    runtime_trace_event(RUNTIME_TRACE_CALL, pc, pc, g_crs_depth, 1u);
}
extern "C" int runtime_call_should_return(uint32_t target_pc) {
    uint32_t pc = target_pc & ~1u;
    uint32_t key = pc | ((g_cpu.cpsr & CPSR_T_BIT) ? 1u : 0u);
    for (uint32_t i = g_crs_depth; i != 0; --i)
        if (g_crs[i - 1u] == key) {
            runtime_trace_event(RUNTIME_TRACE_CALL, pc, pc, g_crs_depth,
                                (i == g_crs_depth) ? 2u : 5u);
            g_crs_depth = i - 1u; return 1;
        }
    runtime_trace_event(RUNTIME_TRACE_CALL, pc,
        g_crs_depth ? (g_crs[g_crs_depth - 1u] & ~1u) : 0xFFFFFFFFu,
        g_crs_depth, 3u);
    return 0;
}
extern "C" void runtime_call_cancel_return(uint32_t return_pc) {
    uint32_t pc = return_pc & ~1u;
    if (g_crs_depth && (g_crs[g_crs_depth - 1u] & ~1u) == pc) {
        runtime_trace_event(RUNTIME_TRACE_CALL, pc, pc, g_crs_depth, 4u);
        --g_crs_depth;
    }
}
extern "C" uint32_t runtime_call_stack_depth(void) { return g_crs_depth; }
extern "C" const uint32_t* runtime_call_stack_data(void) { return g_crs; }
extern "C" void runtime_call_stack_restore(const uint32_t* e, uint32_t d) {
    if (d > kCRS) d = kCRS; g_crs_depth = d;
    for (uint32_t i = 0; i < d; ++i) g_crs[i] = e[i];
}
extern "C" uint32_t runtime_deferred_cycles(void) {
    return g_deferred_cycles;
}
extern "C" void runtime_deferred_cycles_set(uint32_t cycles) {
    g_deferred_cycles = cycles;
}
extern "C" uint32_t runtime_deferred_cycles_take(void) {
    const uint32_t cycles = g_deferred_cycles;
    g_deferred_cycles = 0;
    return cycles;
}

// ── PSR transfer + banking (verbatim) ───────────────────────────────────
namespace {
unsigned mode_to_bank(uint32_t mode) {
    switch (mode & 0x1Fu) {
        case 0x11u: return ARM_BANK_FIQ;        case 0x12u: return ARM_BANK_IRQ;
        case 0x13u: return ARM_BANK_SUPERVISOR; case 0x17u: return ARM_BANK_ABORT;
        case 0x1Bu: return ARM_BANK_UNDEFINED;  default:    return ARM_BANK_USER;
    }
}
void bank_out(unsigned ob, uint32_t om) {
    g_cpu.banked_sp[ob] = g_cpu.R[13]; g_cpu.banked_lr[ob] = g_cpu.R[14];
    if ((om & 0x1Fu) == 0x11u) for (unsigned i = 0; i < 5; ++i) g_cpu.r8_12_fiq[i] = g_cpu.R[8 + i];
    else                       for (unsigned i = 0; i < 5; ++i) g_cpu.r8_12_user[i] = g_cpu.R[8 + i];
}
void bank_in(unsigned nb, uint32_t nm) {
    g_cpu.R[13] = g_cpu.banked_sp[nb]; g_cpu.R[14] = g_cpu.banked_lr[nb];
    if ((nm & 0x1Fu) == 0x11u) for (unsigned i = 0; i < 5; ++i) g_cpu.R[8 + i] = g_cpu.r8_12_fiq[i];
    else                       for (unsigned i = 0; i < 5; ++i) g_cpu.R[8 + i] = g_cpu.r8_12_user[i];
}
}  // namespace

extern "C" uint32_t runtime_read_user_reg(uint32_t reg) {
    reg &= 15u; uint32_t mode = g_cpu.cpsr & 0x1Fu;
    if (reg < 8u || reg == 15u) return g_cpu.R[reg];
    if (reg < 13u) return (mode == 0x11u) ? g_cpu.r8_12_user[reg - 8u] : g_cpu.R[reg];
    if (mode == 0x10u || mode == 0x1Fu) return g_cpu.R[reg];
    return (reg == 13u) ? g_cpu.banked_sp[ARM_BANK_USER] : g_cpu.banked_lr[ARM_BANK_USER];
}
extern "C" void runtime_write_user_reg(uint32_t reg, uint32_t value) {
    reg &= 15u; uint32_t mode = g_cpu.cpsr & 0x1Fu;
    if (reg < 8u || reg == 15u) { g_cpu.R[reg] = value; return; }
    if (reg < 13u) { if (mode == 0x11u) g_cpu.r8_12_user[reg - 8u] = value; else g_cpu.R[reg] = value; return; }
    if (mode == 0x10u || mode == 0x1Fu) g_cpu.R[reg] = value;
    else if (reg == 13u) g_cpu.banked_sp[ARM_BANK_USER] = value;
    else                 g_cpu.banked_lr[ARM_BANK_USER] = value;
}
extern "C" uint32_t runtime_mrs_cpsr(void) { return g_cpu.cpsr; }
extern "C" uint32_t runtime_mrs_spsr(void) { return g_cpu.banked_spsr[mode_to_bank(g_cpu.cpsr)]; }
extern "C" void runtime_msr_cpsr(uint32_t value, uint32_t mask) {
    uint32_t bw = 0;
    if (mask & 1u) bw |= 0x000000FFu; if (mask & 2u) bw |= 0x0000FF00u;
    if (mask & 4u) bw |= 0x00FF0000u; if (mask & 8u) bw |= 0xFF000000u;
    if ((g_cpu.cpsr & 0x1Fu) == 0x10u) bw &= 0xFF000000u;  // User: flags only
    uint32_t oc = g_cpu.cpsr, nc = (oc & ~bw) | (value & bw);
    unsigned ob = mode_to_bank(oc), nb = mode_to_bank(nc);
    g_cpu.cpsr = nc;
    if (ob != nb) { bank_out(ob, oc); bank_in(nb, nc); }
}
extern "C" void runtime_msr_spsr(uint32_t value, uint32_t mask) {
    unsigned bank = mode_to_bank(g_cpu.cpsr);
    if (bank == ARM_BANK_USER) return;
    uint32_t bw = 0;
    if (mask & 1u) bw |= 0x000000FFu; if (mask & 2u) bw |= 0x0000FF00u;
    if (mask & 4u) bw |= 0x00FF0000u; if (mask & 8u) bw |= 0xFF000000u;
    uint32_t old = g_cpu.banked_spsr[bank];
    g_cpu.banked_spsr[bank] = (old & ~bw) | (value & bw);
}
extern "C" void runtime_exception_return(uint32_t new_pc) {
    uint32_t oc = g_cpu.cpsr, om = oc & 0x1Fu;
    if (om == 0x10u || om == 0x1Fu) { g_cpu.R[15] = new_pc; return; }
    unsigned ob = mode_to_bank(oc); uint32_t spsr = g_cpu.banked_spsr[ob];
    bank_out(ob, oc); g_cpu.cpsr = spsr; bank_in(mode_to_bank(spsr), spsr);
    g_cpu.R[15] = new_pc;
}
extern "C" void runtime_restore_cpsr_from_spsr(void) {
    uint32_t oc = g_cpu.cpsr, om = oc & 0x1Fu;
    if (om == 0x10u || om == 0x1Fu) return;
    unsigned ob = mode_to_bank(oc); uint32_t spsr = g_cpu.banked_spsr[ob];
    bank_out(ob, oc); g_cpu.cpsr = spsr; bank_in(mode_to_bank(spsr), spsr);
}

// ── SWI / IRQ (per-CPU exception base) ──────────────────────────────────
extern "C" void runtime_swi(uint32_t swi_imm) {
    uint32_t ret = g_cpu.R[15], saved = g_cpu.cpsr;
    runtime_trace_event(RUNTIME_TRACE_SWI, ret, swi_imm, saved, 0);
    uint32_t nc = (saved & ~(0x1Fu | CPSR_T_BIT)) | 0x13u | CPSR_I_BIT;
    unsigned ob = mode_to_bank(saved), nb = mode_to_bank(nc);
    if (ob != nb) { bank_out(ob, saved); bank_in(nb, nc); }
    g_cpu.cpsr = nc; g_cpu.banked_spsr[nb] = saved; g_cpu.R[14] = ret;
    uint32_t base = g_ctx[g_nds_active].exc_base;
    g_cpu.R[15] = base + 0x08u;
    // ARM7: flat 3 (2S+1N), matching the static base table exactly (unchanged).
    // ARM9: melonDS charges SWI entry as a taken branch to the exception vector
    // — 2*numC(exception base+8) (no class cost of its own; the SWI instruction
    // itself already ticked nothing here, see arm_codegen.cpp emit_swi). CPSR.T
    // was just cleared above (ARM mode, exception entry), so runtime_code_cycles
    // sees the correct target-mode state.
    runtime_tick(g_nds_active == NDS_ARM9
                     ? arm9_refill_cycles(base + 0x08u)
                     : arm7_refill_cycles(base + 0x08u));
    runtime_dispatch(base + 0x08u);
}
extern "C" void runtime_irq(uint32_t return_address) {
    nds_note_irq_accept(g_nds_active, return_address);
    uint32_t saved = g_cpu.cpsr;
    runtime_trace_event(RUNTIME_TRACE_IRQ, return_address, 0, saved, 0);
    // melonDS ARM::TriggerIRQ clears CPSR[7:0] then installs 0xD2: IRQ mode,
    // ARM state, with both IRQ and FIQ masked.  Preserving the old F bit makes
    // the BIOS IRQ prologue observably diverge on its first exception.
    uint32_t nc = (saved & ~0xFFu) | 0xD2u;
    unsigned ob = mode_to_bank(saved), nb = mode_to_bank(nc);
    if (ob != nb) { bank_out(ob, saved); bank_in(nb, nc); }
    g_cpu.cpsr = nc; g_cpu.banked_spsr[nb] = saved; g_cpu.R[14] = return_address + 4u;
    uint32_t base = g_ctx[g_nds_active].exc_base;
    g_cpu.R[15] = base + 0x18u;
    const uint32_t refill = g_nds_active == NDS_ARM9
        ? arm9_refill_cycles(base + 0x18u)
        : arm7_refill_cycles(base + 0x18u);
    if (g_deferred_cycles) {
        // IRQ accepted directly out of HALT: melonDS JumpTo appends the refill
        // to the still-pending ARM::Cycles debt. The first vector instruction
        // commits both; it must not become visible before that instruction.
        g_deferred_cycles += refill;
    } else {
        runtime_tick(refill);
    }
    runtime_dispatch(base + 0x18u);
}

// ── Fallback / lifecycle ────────────────────────────────────────────────
extern "C" void runtime_unimplemented_op(const char* op_name, uint32_t pc) {
    std::fprintf(stderr, "[UNIMPLEMENTED] op=%s pc=0x%08X cpu=%s cpsr=0x%08X\n",
                 op_name, pc, g_nds_active == NDS_ARM9 ? "arm9" : "arm7", g_cpu.cpsr);
    runtime_trace_dump_recent(12);
    nds_halt("unimplemented op");
}
extern "C" void runtime_init(void*) {
    g_cpu_fast_poll = configured_cpu_fast_poll();
    request_yield_poll();
    g_crs_depth = 0;
    g_deferred_cycles = 0;
    g_discovery_seen.clear();
    for (CpuCtx& ctx : g_ctx) {
        ctx.banks.clear();
        ctx.exc_base = 0u;
    }
    g_static_guard = {};
    g_dispatch_cache = {};
    tier3_reset();
}
extern "C" void runtime_shutdown(void) {
    g_crs_depth = 0;
    g_deferred_cycles = 0;
}
