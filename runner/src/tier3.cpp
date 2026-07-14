// tier3.cpp — see tier3.h.
//
// The reference interpreter (recompiler/armv4t/interpreter.cpp) is the
// always-correct execution floor. It operates on armv4t::CPUState +
// armv4t::Bus; this file bridges those to the runner's g_cpu and C bus ABI,
// and drives the fetch→decode→step loop with cross-tier exits.

#include "tier3.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "interpreter.h"
#include "arm_decode.h"
#include "thumb_decode.h"
#include "cpu_state.h"
#include "bus.h"

#include "runtime_arm.h"
#include "state.h"
#include "io.h"

using armv4t::CPUState;
using armv4t::Interpreter;
using armv4t::Instr;

// Runtime/scheduler hooks (defined in runtime_arm.cpp).
extern "C" int  nds_has_bank(uint32_t pc, int thumb);
extern "C" int  nds_slice_over(void);
extern "C" uint32_t nds_exception_base(void);

namespace {

constexpr uint32_t kTier3TraceSize = 4096;
Tier3TraceEvent g_trace[kTier3TraceSize];
uint32_t g_trace_w = 0;
uint32_t g_trace_count = 0;
uint64_t g_trace_seq = 0;
Tier3Stats g_stats{};
std::vector<Tier3CoverageEntry> g_coverage;
std::unordered_map<uint64_t, uint32_t> g_coverage_index;

void coverage_note(uint32_t pc, bool thumb, uint8_t kind, uint32_t caller) {
    if (!g_discover_static_misses) return;
    pc &= ~1u;
    const uint8_t cpu = g_nds_active == NDS_ARM7 ? 1u : 0u;
    const uint64_t key = uint64_t{pc} |
                         (uint64_t{kind} << 32u) |
                         (uint64_t{thumb ? 1u : 0u} << 40u) |
                         (uint64_t{cpu} << 41u);
    auto found = g_coverage_index.find(key);
    if (found != g_coverage_index.end()) {
        ++g_coverage[found->second].hits;
        return;
    }
    g_coverage_index.emplace(key, static_cast<uint32_t>(g_coverage.size()));
    g_coverage.push_back({1u, pc, caller, cpu,
                          static_cast<uint8_t>(thumb ? 1u : 0u), kind});
}

uint32_t pack_cpsr(const CPUState& c) {
    return (uint32_t(c.cpsr.n) << 31) | (uint32_t(c.cpsr.z) << 30) |
           (uint32_t(c.cpsr.c) << 29) | (uint32_t(c.cpsr.v) << 28) |
           (uint32_t(c.cpsr.q) << 27) |
           (uint32_t(c.cpsr.i) << 7)  | (uint32_t(c.cpsr.f) << 6)  |
           (uint32_t(c.cpsr.t) << 5)  | uint32_t(c.cpsr.mode);
}

bool trace_pc(uint32_t pc) {
    return (pc >= 0x021F0000u && pc < 0x021F0040u) ||
           (pc >= 0x0231FE00u && pc < 0x0231FE80u) ||
           (pc >= 0x037FD7A0u && pc < 0x037FD7C0u) ||
           (pc >= 0x0380F800u && pc < 0x0380F900u);
}

bool static_bios_pc(uint32_t pc) {
    return (g_nds_active == NDS_ARM9)
        ? (pc >= 0xFFFF0000u && pc < 0xFFFF1000u)
        : (pc < 0x00004000u);
}

void trace_push(const CPUState& c, uint8_t phase, uint32_t pc,
                uint32_t raw, uint32_t next_pc, uint8_t result) {
    Tier3TraceEvent& e = g_trace[g_trace_w];
    e.seq = ++g_trace_seq;
    e.cpu = static_cast<uint8_t>(g_nds_active);
    e.thumb = c.cpsr.t ? 1 : 0;
    e.phase = phase;
    e.result = result;
    e.pc = pc;
    e.raw = raw;
    e.next_pc = next_pc;
    e.cpsr = pack_cpsr(c);
    e.r0 = c.R[0];
    e.r1 = c.R[1];
    e.r2 = c.R[2];
    e.r3 = c.R[3];
    e.r12 = c.R[12];
    e.sp = c.R[13];
    e.lr = c.R[14];
    e.cycles = g_runtime_cycles;
    g_trace_w = (g_trace_w + 1u) % kTier3TraceSize;
    if (g_trace_count < kTier3TraceSize) ++g_trace_count;
}

// Bus adapter: forward the interpreter's accesses to the C bus ABI (which
// applies the DS memory map, TCM, I/O, and the always-on access ring).
struct RtBus : armv4t::Bus {
    mutable uint32_t data_cycles = 0;
    void begin_instruction() { data_cycles = 0; }
    uint8_t  read8 (uint32_t a) override { return bus_read_u8(a); }
    uint16_t read16(uint32_t a) override { return bus_read_u16(a); }
    uint32_t read32(uint32_t a) override { return bus_read_u32(a); }
    void write8 (uint32_t a, uint8_t  v) override { bus_write_u8(a, v); }
    void write16(uint32_t a, uint16_t v) override { bus_write_u16(a, v); }
    void write32(uint32_t a, uint32_t v) override { bus_write_u32(a, v); }
    uint32_t access_cycles(uint32_t a, uint8_t width,
                           bool sequential) const override {
        const uint32_t c = runtime_mem_cycles(a, width, sequential ? 1u : 0u);
        data_cycles += c;
        return c;
    }
    void coproc_write(uint32_t cp_num, uint32_t op1, uint32_t crn,
                      uint32_t crm, uint32_t op2, uint32_t value) override {
        runtime_coproc_write(cp_num, op1, crn, crm, op2, value);
    }
    uint32_t coproc_read(uint32_t cp_num, uint32_t op1, uint32_t crn,
                         uint32_t crm, uint32_t op2) override {
        return runtime_coproc_read(cp_num, op1, crn, crm, op2);
    }
    void coproc_cdp(uint32_t cp_num, uint32_t op1, uint32_t crn,
                    uint32_t crm, uint32_t op2) override {
        runtime_coproc_cdp(cp_num, op1, crn, crm, op2);
    }
};
RtBus g_bus;

uint32_t arm9_internal_cost(const Instr& in) {
    uint32_t n = 0;
    if (in.op2.kind == armv4t::Op2::Kind::Shifted &&
        in.op2.shifted.by_register)
        n += 1u;
    switch (in.op) {
        case armv4t::IrOp::MUL: case armv4t::IrOp::MLA:
        case armv4t::IrOp::UMULL: case armv4t::IrOp::UMLAL:
        case armv4t::IrOp::SMULL: case armv4t::IrOp::SMLAL:
            n += in.set_flags ? 3u : 1u;
            break;
        case armv4t::IrOp::MCR: n += 2u; break;
        case armv4t::IrOp::MRC: n += 3u; break;
        default: break;
    }
    return n;
}

bool arm7_has_internal(const Instr& in) {
    if (in.op2.kind == armv4t::Op2::Kind::Shifted &&
        in.op2.shifted.by_register)
        return true;
    switch (in.op) {
        case armv4t::IrOp::MUL: case armv4t::IrOp::MLA:
        case armv4t::IrOp::UMULL: case armv4t::IrOp::UMLAL:
        case armv4t::IrOp::SMULL: case armv4t::IrOp::SMLAL:
            return true;
        default:
            return false;
    }
}

bool arm7_is_load(armv4t::IrOp op) {
    switch (op) {
        case armv4t::IrOp::LDR: case armv4t::IrOp::LDRB:
        case armv4t::IrOp::LDRH: case armv4t::IrOp::LDRSB:
        case armv4t::IrOp::LDRSH: case armv4t::IrOp::LDM:
        case armv4t::IrOp::LDRD: case armv4t::IrOp::SWP:
        case armv4t::IrOp::SWPB:
            return true;
        default:
            return false;
    }
}

// g_cpu (packed CPSR) ↔ CPUState (structured CPSR). Register windows and
// banked tables share layout (ARM_BANK_* == BankedSlot order).
void sync_in(CPUState& c) {
    for (int i = 0; i < 16; ++i) c.R[i] = g_cpu.R[i];
    uint32_t p = g_cpu.cpsr;
    c.cpsr.n = (p >> 31) & 1; c.cpsr.z = (p >> 30) & 1;
    c.cpsr.c = (p >> 29) & 1; c.cpsr.v = (p >> 28) & 1;
    c.cpsr.q = (p >> 27) & 1;
    c.cpsr.i = (p >> 7) & 1;  c.cpsr.f = (p >> 6) & 1;
    c.cpsr.t = (p >> 5) & 1;  c.cpsr.mode = p & 0x1Fu;
    c.thumb = c.cpsr.t;
    for (int i = 0; i < 6; ++i) {
        c.banked_sp[i] = g_cpu.banked_sp[i];
        c.banked_lr[i] = g_cpu.banked_lr[i];
        c.banked_spsr[i] = g_cpu.banked_spsr[i];
    }
    for (int i = 0; i < 5; ++i) {
        c.r8_12_user[i] = g_cpu.r8_12_user[i];
        c.r8_12_fiq[i]  = g_cpu.r8_12_fiq[i];
    }
}
void sync_out(const CPUState& c) {
    for (int i = 0; i < 16; ++i) g_cpu.R[i] = c.R[i];
    g_cpu.cpsr = pack_cpsr(c);
    for (int i = 0; i < 6; ++i) {
        g_cpu.banked_sp[i] = c.banked_sp[i];
        g_cpu.banked_lr[i] = c.banked_lr[i];
        g_cpu.banked_spsr[i] = c.banked_spsr[i];
    }
    for (int i = 0; i < 5; ++i) {
        g_cpu.r8_12_user[i] = c.r8_12_user[i];
        g_cpu.r8_12_fiq[i]  = c.r8_12_fiq[i];
    }
}

}  // namespace

void tier3_run(uint32_t /*entry*/) {
    const uint32_t cpu_index = g_nds_active == NDS_ARM7 ? 1u : 0u;
    // Every native-to-Tier3 boundary is a static-coverage root. Recording
    // only the first boundary per CPU forced an otherwise deterministic
    // traversal to reveal one missing dispatch entry per rebuild. The
    // coverage map already deduplicates (CPU, PC, mode, kind) and counts
    // hits, so retain the complete root surface in one discovery pass.
    coverage_note(g_cpu.R[15], (g_cpu.cpsr & CPSR_T_BIT) != 0u,
                  TIER3_COVERAGE_ROOT, g_cpu.R[14]);
    ++g_stats.entries[cpu_index];
    CPUState ic;
    sync_in(ic);
    const bool trace_this_entry = (g_nds_active == NDS_ARM9) || trace_pc(ic.R[15]);
    if (trace_this_entry) trace_push(ic, 0, ic.R[15], 0, ic.R[15], 0);

    long guard = 0;
    while (true) {
        uint32_t pc = ic.R[15];
        bool thumb = ic.cpsr.t;

        // Keep the runtime-visible architectural state current while Tier 3
        // is executing.  Device side effects and timing helpers consult
        // g_cpu (not the interpreter's private CPUState), so leaving it at
        // the tier-entry snapshot makes interworking code use the wrong
        // instruction-set state for subsequent fetches.
        sync_out(ic);

        // insn7/insn9 anchor reached during interpreted code → stop AT this
        // (not-yet-executed) instruction, symmetric with the bank path's
        // runtime_should_yield check. State is fully in `ic`; sync_out below.
        // A FIFO/IPCSYNC/SPI/DMA event can fire inside an interpreted
        // instruction. Finish that instruction (and any immediate IRQ entry),
        // then unwind so run_to_event stops at its exact boundary instead of
        // executing the remainder of the Tier-3 slice.
        if (nds_event_break_hit()) {
            sync_out(ic);
            nds_preserve_unwind_state();
            break;
        }
        if (g_nds_insn_stop) {
            // Propagate the per-instruction stop through any enclosing static
            // dispatch frames (notably BIOS IRQ -> copied-RAM handler).  A bare
            // break returns normally and lets the interrupted bank execute a
            // few more instructions, corrupting the exact-index observer.
            nds_preserve_unwind_state();
            break;
        }

        // Tier-1 takeover: a static bank covers this PC — hand back to the
        // dispatcher (it will call the recompiled function).
        if (nds_has_bank(pc & ~1u, thumb ? 1 : 0)) {
            nds_preserve_unwind_state();
            break;
        }
        // Only RAM-resident copied code belongs to Tier 3 (main RAM, WRAM,
        // or ARM9 ITCM); anything else is a genuine gap. The bus owns the
        // memory map, so ask it (covers the ITCM mirror the firmware uses
        // for its IRQ handler).
        const uint32_t fetch_addr = pc & (thumb ? ~1u : ~3u);
        const uint32_t fetch_size = thumb ? 2u : 4u;
        if (!bus_range_has_write_provenance(fetch_addr, fetch_size) &&
            !(g_discover_static_misses && static_bios_pc(pc & ~1u))) {
            // Hand the invalid/static target back to the scheduler so the
            // normal dispatcher can resolve it or report a real miss. Never
            // return normally into the stale host frame that entered Tier 3.
            nds_preserve_unwind_state();
            break;
        }

        // Retired-instruction counter (insn9/insn7), symmetric with the
        // recompiled-bank runtime_insn_fp hook: one bump per interpreted guest
        // instruction, counted as it begins (committed to execute).
        ++g_stats.instructions[g_nds_active == NDS_ARM7 ? 1 : 0];
        nds_note_insn_retired(g_nds_active);
        // Current-PC fetch correction.  For a taken branch melonDS JumpTo
        // replaces the source fetch with the target pipeline refill, so this
        // is applied below only when the instruction does not branch.
        const uint32_t code_correction = runtime_code_cycles(pc & ~1u);

        Instr in = thumb
            ? armv4t::ThumbDecoder::decode(g_bus.read16(pc & ~1u), pc & ~1u)
            : armv4t::ArmDecoder::decode(g_bus.read32(pc & ~3u), pc & ~3u);
        const bool condition_passed = Interpreter::cond_passes(in.cond, ic.cpsr);
        const bool traced = (g_nds_active == NDS_ARM9) || trace_pc(pc);
        if (traced) trace_push(ic, 1, pc, in.raw, ic.R[15], 0);

        g_bus.begin_instruction();
        uint32_t cyc = 1;
        Interpreter::Result r = Interpreter::step(ic, g_bus, in, &cyc);
        if (g_nds_active != NDS_ARM9 && thumb &&
            in.op == armv4t::IrOp::MUL) {
            // melonDS ARM7 T_MUL_REG models the architecturally-destroyed C
            // flag as clear. ARM9 preserves it, so keep this runtime-specific
            // rather than baking a CPU identity into the portable core.
            ic.cpsr.c = false;
        }

        const bool branch_op =
            in.op == armv4t::IrOp::B || in.op == armv4t::IrOp::BL ||
            in.op == armv4t::IrOp::BX || in.op == armv4t::IrOp::BLX_reg ||
            in.op == armv4t::IrOp::BLX_imm ||
            in.op == armv4t::IrOp::BL_suffix;
        if (r == Interpreter::Result::Swi) {
            const uint32_t target = nds_exception_base() + 0x08u;
            cyc = (g_nds_active == NDS_ARM9)
                ? arm9_refill_cycles(target)
                : arm7_refill_cycles(target);
        } else if (r == Interpreter::Result::Branched && branch_op) {
            const uint32_t target = (ic.R[15] & ~1u) |
                                    (ic.cpsr.t ? 1u : 0u);
            cyc = (g_nds_active == NDS_ARM9)
                ? arm9_refill_cycles(target)
                : arm7_refill_cycles(target);
        } else if (g_nds_active == NDS_ARM9) {
            const uint32_t numI = condition_passed ? arm9_internal_cost(in) : 0u;
            cyc = arm9_cycle_combine(code_correction, g_bus.data_cycles,
                                     numI, g_bus.data_cycles ? 1u : 0u);
            // Loads, ALU writes to PC and exception returns first pay their
            // source instruction's C/CD/CI cost, then JumpTo refills from the
            // runtime target.  The portable interpreter's fixed +2 refill is
            // deliberately replaced by the target-region ARM9 model above.
            if (r == Interpreter::Result::Branched) {
                const uint32_t target = (ic.R[15] & ~1u) |
                                        (ic.cpsr.t ? 1u : 0u);
                cyc += arm9_refill_cycles(target);
            }
        } else {
            const bool had_data = g_bus.data_cycles != 0u;
            cyc = arm7_cycle_combine(
                cyc, g_bus.data_cycles,
                arm7_is_load(in.op) ? 1u : 0u,
                condition_passed && arm7_has_internal(in) ? 1u : 0u);
            if (r == Interpreter::Result::Branched) {
                const uint32_t target = (ic.R[15] & ~1u) |
                                        (ic.cpsr.t ? 1u : 0u);
                if (had_data)
                    cyc += arm7_refill_cycles(target);
                else if (cyc >= 2u)
                    cyc = cyc - 2u + arm7_refill_cycles(target);
            }
        }
        // HALT can leave the prior instruction's melonDS ARM::Cycles pending
        // across sleep. Tier 3 commits that debt with the first interpreted
        // instruction, just as a generated bank's runtime_tick() does.
        g_runtime_cycles += cyc + runtime_deferred_cycles_take();
        if (traced) {
            trace_push(ic, 2, pc, in.raw, ic.R[15],
                       static_cast<uint8_t>(r));
        }

        // Immediate DMA begins during the control-register store and stalls
        // the CPU after that instruction retires.  Preserve the completed
        // guest state and unwind exactly like the generated-bank path; the
        // scheduler will move this instruction's cost into deferred debt and
        // run DMA bus units instead of the CPU until completion.
        if (nds_cpu_halted(g_nds_active) ||
            nds_dma_cpu_stalled(g_nds_active)) {
            sync_out(ic);
            nds_preserve_unwind_state();
            break;
        }

        // Keep the call-return stack balanced across tier boundaries: the
        // banks push on BL/BLX and pop (via runtime_call_should_return) on
        // a matching `bx lr`/`pop pc`. The interpreter must mirror that, or
        // a bank that BL'd into Tier-3 (whose return executes here) leaks
        // its push and eventually overflows the stack.
        if (r == Interpreter::Result::Branched && in.is_call &&
            in.op != armv4t::IrOp::BL_prefix) {
            coverage_note(ic.R[15], ic.cpsr.t, TIER3_COVERAGE_CALL, pc);
            runtime_call_push_return(pc + (thumb ? 2u : 4u));
        } else if (r == Interpreter::Result::Branched && !in.is_call) {
            // A BX/LDM/POP return may interwork.  The call was pushed in the
            // caller's pre-branch mode above, but matching must use the
            // branch target's post-step T bit.  g_cpu still contains the
            // entry snapshot until the next interpreter iteration, so publish
            // `ic` now or one ARM->Thumb return leaks on every loop.
            sync_out(ic);
            const bool matched_return =
                runtime_call_should_return(ic.R[15] & ~1u) != 0;
            if (!matched_return && in.op != armv4t::IrOp::B) {
                coverage_note(ic.R[15], ic.cpsr.t,
                              TIER3_COVERAGE_INDIRECT, pc);
            }
        }

        // Discovery-only: every previously uncovered immutable-ROM branch is
        // a candidate static entry. Log it, but never auto-promote it; normal
        // execution remains fatal until the target is validated and folded.
        if (g_discover_static_misses && r == Interpreter::Result::Branched &&
            static_bios_pc(ic.R[15] & ~1u) &&
            !nds_has_bank(ic.R[15] & ~1u, ic.cpsr.t ? 1 : 0)) {
            sync_out(ic);
            runtime_discovery_note_static(ic.R[15] & ~1u,
                                          ic.cpsr.t ? 1u : 0u);
        }

        switch (r) {
            case Interpreter::Result::Normal:
            case Interpreter::Result::Branched:
                break;
            case Interpreter::Result::Swi:
                // PC was not advanced by step(); LR_svc = next instruction.
                Interpreter::enter_swi(ic, pc + (thumb ? 2u : 4u), thumb);
                ic.R[15] = nds_exception_base() + 0x08u;
                break;  // PC now at the SWI vector (a bank) → exits next iter
            case Interpreter::Result::Undefined:
            case Interpreter::Result::NotImplemented: {
                sync_out(ic);
                static char reason[128];
                std::snprintf(reason, sizeof(reason),
                    "tier3: interpreter %s pc=%08X raw=%08X thumb=%d",
                    r == Interpreter::Result::Undefined ? "undefined"
                                                         : "not-implemented",
                    pc, in.raw, thumb ? 1 : 0);
                nds_halt(reason);
                return;
            }
        }

        // Deliver a pending IRQ to the interpreted CPU (vectors to the BIOS
        // handler bank next iteration).
        if (!ic.cpsr.i && nds_irq_pending(g_nds_active)) {
            const uint32_t target = nds_exception_base() + 0x18u;
            nds_note_irq_accept(g_nds_active, ic.R[15]);
            Interpreter::enter_irq(ic, ic.R[15]);
            ic.R[15] = target;
            // IRQ entry performs the same pipeline refill whether the
            // interrupted instruction came from a static bank or copied RAM.
            // The bank path charges this in runtime_irq(); Tier 3 must do it
            // here because it enters the exception directly.
            g_runtime_cycles += (g_nds_active == NDS_ARM9)
                ? arm9_refill_cycles(target)
                : arm7_refill_cycles(target);
        }

        // Cooperative slice boundary — clean here (all state is in `ic`).
        // The retire hook may have reached an exact-index breakpoint during
        // this instruction. Capture the just-retired state before a coincident
        // slice boundary can return through enclosing BIOS/IRQ host frames.
        if (g_nds_insn_stop) {
            sync_out(ic);
            nds_preserve_unwind_state();
            break;
        }
        if (nds_slice_over()) {
            // Tier 3 is often nested under a recompiled BIOS IRQ/vector call.
            // A normal return would resume that stale caller and overwrite the
            // interpreted PC. Preserve the guest state and unwind to the
            // scheduler, which will redispatch this exact PC next slice.
            sync_out(ic);
            nds_preserve_unwind_state();
            break;
        }
        if (++guard > 50'000'000) {
            sync_out(ic);
            nds_halt("tier3: slice guard (no exit)");
            return;
        }
    }
    if ((g_nds_active == NDS_ARM9) || trace_pc(ic.R[15]))
        trace_push(ic, 3, ic.R[15], 0, ic.R[15], 0);
    sync_out(ic);
}

uint32_t tier3_debug_trace_copy(Tier3TraceEvent* out, uint32_t max_entries) {
    if (!out || max_entries == 0) return 0;
    uint32_t count = g_trace_count;
    if (count > max_entries) count = max_entries;
    uint32_t start = (g_trace_w + kTier3TraceSize - count) % kTier3TraceSize;
    for (uint32_t i = 0; i < count; ++i)
        out[i] = g_trace[(start + i) % kTier3TraceSize];
    return count;
}

void tier3_reset() {
    g_stats = {};
    g_trace_w = 0;
    g_trace_count = 0;
    g_trace_seq = 0;
    g_coverage.clear();
    g_coverage_index.clear();
}

void tier3_note_clean_ram_reject() {
    ++g_stats.clean_ram_rejects[g_nds_active == NDS_ARM7 ? 1 : 0];
}

Tier3Stats tier3_stats() {
    return g_stats;
}

uint32_t tier3_coverage_copy(Tier3CoverageEntry* out, uint32_t max_entries) {
    if (!out || max_entries == 0u) return 0u;
    std::vector<Tier3CoverageEntry> sorted = g_coverage;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.cpu != b.cpu) return a.cpu < b.cpu;
        if (a.kind != b.kind) return a.kind < b.kind;
        if (a.pc != b.pc) return a.pc < b.pc;
        return a.thumb < b.thumb;
    });
    const uint32_t count = std::min<uint32_t>(
        max_entries, static_cast<uint32_t>(sorted.size()));
    for (uint32_t i = 0; i < count; ++i) out[i] = sorted[i];
    return count;
}
