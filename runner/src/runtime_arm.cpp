// runtime_arm.cpp — DS runner implementation of the recomp C ABI.
//
// The condition-code / shifter / flag-updater / PSR-banking / call-return
// helpers are reused verbatim from the (verified) single-CPU runtime in
// recompiler/armv4t/runtime_arm.cpp — they are CPU-architecture-agnostic.
// What is DS-specific and rewritten here: per-CPU dispatch registration,
// the exception-vector base (ARM9 high vectors vs ARM7 low), a simple
// always-on trace ring, the cooperative yield/halt, and lifecycle.

#include "runtime_arm.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

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

// ── Per-CPU dispatch registration ───────────────────────────────────────
// DispatchEntry is declared in state.h (layout-matches the generated table).

namespace {
struct CpuCtx {
    const DispatchEntry* table = nullptr;
    unsigned             len = 0;
    uint32_t             exc_base = 0;  // exception-vector base for this CPU
};
CpuCtx g_ctx[2];

// Cycle budget for the run slice; runtime_should_yield trips when reached
// so a guest spin loop can't hang the host.
unsigned long long g_cycle_cap = 0;  // 0 = unlimited

void (*lookup_in(const DispatchEntry* table, unsigned len,
                 uint32_t pc, bool thumb))(void) {
    if (!table) return nullptr;
    unsigned lo = 0, hi = len;
    while (lo < hi) {
        unsigned mid = (lo + hi) >> 1u;
        if (table[mid].addr < pc) lo = mid + 1u;
        else                       hi = mid;
    }
    for (unsigned i = lo; i < len && table[i].addr == pc; ++i)
        if ((table[i].thumb != 0) == thumb) return table[i].fn;
    return nullptr;
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

extern "C" void nds_register_dispatch(int cpu, const DispatchEntry* t,
                                      unsigned len, uint32_t exc_base) {
    g_ctx[cpu & 1].table = t;
    g_ctx[cpu & 1].len = len;
    g_ctx[cpu & 1].exc_base = exc_base;
}

extern "C" void nds_set_cycle_cap(unsigned long long cap) { g_cycle_cap = cap; }

// Tier-3 helpers: does the active CPU have a Tier-1 bank fn at (pc, thumb)?
// And is this slice's cycle cap reached? (The interpreter uses these to
// decide when to hand control back to the dispatcher / scheduler.)
extern "C" int nds_has_bank(uint32_t pc, int thumb) {
    const CpuCtx& c = g_ctx[g_nds_active];
    return lookup_in(c.table, c.len, pc & ~1u, thumb != 0) != nullptr ? 1 : 0;
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
}

// Slice control used by the scheduler: arm the cycle cap, clear terminal.
extern "C" void nds_slice_begin(unsigned long long cap) {
    g_nds_terminal = false;
    g_cycle_cap = cap;
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

extern "C" void runtime_trace_event(uint32_t kind, uint32_t pc, uint32_t addr,
                                    uint32_t value, uint32_t aux) {
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

// Per-instruction hook, fired once at the top of every recompiled-bank guest
// instruction (g_runtime_insn_trace on). Bumps the active CPU's retired-insn
// counter so insn9/insn7 can anchor the fp-stream bisector. Tier-3 bumps the
// same counters from its own step loop (tier3.cpp).
extern "C" void runtime_insn_fp(void) { nds_note_insn_retired(g_nds_active); }
extern "C" void runtime_fp_reset(void) {}
extern "C" uint32_t runtime_fp_count(void) { return 0; }
extern "C" uint32_t runtime_fp_save_file(const char*) { return 0; }

// ── Tick / yield ────────────────────────────────────────────────────────
extern "C" void runtime_tick(uint32_t cycles) {
    g_runtime_cycles += cycles;
    // Deliver a pending IRQ to the active CPU at this instruction boundary
    // (R15 already points at the next instruction = the return address).
    // runtime_irq masks CPSR.I before vectoring, so it cannot re-enter here
    // while the handler runs.
    if (!(g_cpu.cpsr & CPSR_I_BIT) && nds_irq_pending(g_nds_active))
        runtime_irq(g_cpu.R[15]);
}
// Per-instruction unwind: terminal halts only (a guest spin waiting on the
// other core is NOT a fault — it is preempted at a backward branch instead).
extern "C" bool runtime_should_yield(void) {
    // insn7/insn9 anchor reached → stop at this exact instruction (see io.cpp
    // g_nds_insn_stop). The bisector resets per K, so the mid-function unwind
    // (which does not preserve the call-return stack) is never resumed from.
    if (g_nds_insn_stop) return true;
    if (g_runtime_break_pc && (g_cpu.R[15] & ~1u) == (g_runtime_break_pc & ~1u))
        nds_halt("break pc");
    return g_nds_terminal;
}
// Cooperative slice preemption: trips once this slice's cycle cap is
// reached. Checked only at backward branches (loop tops = dispatch
// entries). When it fires it arms g_unwinding so the BL/BLX return-checks
// PRESERVE their pending returns as the host stack unwinds — the call-
// return stack is saved/restored per-CPU by the scheduler, so a spin at
// any call depth can be preempted and cleanly resumed.
namespace { bool g_unwinding = false; }
extern "C" bool runtime_slice_yield(void) {
    if (g_cycle_cap != 0 && g_runtime_cycles >= g_cycle_cap) {
        g_unwinding = true;
        return true;
    }
    return false;
}
extern "C" bool runtime_unwinding(void) { return g_unwinding; }
extern "C" void nds_clear_unwinding(void) { g_unwinding = false; }

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
// Multiply operand wait — flat for now (DS ARM9 timing lands later).
extern "C" uint32_t runtime_mul_cycles(uint32_t, uint32_t, uint32_t) { return 1u; }

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
    runtime_trace_event(RUNTIME_TRACE_DISPATCH, pc, target_pc, 0, 0);
    bool thumb = (g_cpu.cpsr & CPSR_T_BIT) != 0;
    const CpuCtx& c = g_ctx[g_nds_active];
    if (void (*fn)(void) = lookup_in(c.table, c.len, pc, thumb)) { fn(); return; }
    // Tier 3: code copied into RAM at runtime (firmware boot, menu, and the
    // ITCM-resident IRQ handler) has no static bank — run the guest's OWN
    // bytes through the interpreter (PRINCIPLES.md "the one exception"),
    // never an HLE model. The bus owns the memory map (covers ITCM mirror).
    if (bus_addr_is_exec_ram(pc)) { tier3_run(pc); return; }
    runtime_dispatch_miss(target_pc);
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
    const bool have_range = bracket_static_range(cc.table, cc.len, t, &rs, &re);

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
    if (t >= 0x02000000u && t < 0x04000000u) {
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
    runtime_tick(3u);
    runtime_dispatch(base + 0x08u);
}
extern "C" void runtime_irq(uint32_t return_address) {
    uint32_t saved = g_cpu.cpsr;
    runtime_trace_event(RUNTIME_TRACE_IRQ, return_address, 0, saved, 0);
    uint32_t nc = (saved & ~(0x1Fu | CPSR_T_BIT)) | 0x12u | CPSR_I_BIT;
    unsigned ob = mode_to_bank(saved), nb = mode_to_bank(nc);
    if (ob != nb) { bank_out(ob, saved); bank_in(nb, nc); }
    g_cpu.cpsr = nc; g_cpu.banked_spsr[nb] = saved; g_cpu.R[14] = return_address + 4u;
    uint32_t base = g_ctx[g_nds_active].exc_base;
    g_cpu.R[15] = base + 0x18u;
    runtime_dispatch(base + 0x18u);
}

// ── Fallback / lifecycle ────────────────────────────────────────────────
extern "C" void runtime_unimplemented_op(const char* op_name, uint32_t pc) {
    std::fprintf(stderr, "[UNIMPLEMENTED] op=%s pc=0x%08X cpu=%s cpsr=0x%08X\n",
                 op_name, pc, g_nds_active == NDS_ARM9 ? "arm9" : "arm7", g_cpu.cpsr);
    runtime_trace_dump_recent(12);
    nds_halt("unimplemented op");
}
extern "C" void runtime_init(void*) { g_crs_depth = 0; }
extern "C" void runtime_shutdown(void) { g_crs_depth = 0; }
