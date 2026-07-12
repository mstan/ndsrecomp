// tier3.cpp — see tier3.h.
//
// The reference interpreter (recompiler/armv4t/interpreter.cpp) is the
// always-correct execution floor. It operates on armv4t::CPUState +
// armv4t::Bus; this file bridges those to the runner's g_cpu and C bus ABI,
// and drives the fetch→decode→step loop with cross-tier exits.

#include "tier3.h"

#include <cstdio>

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
           (pc >= 0x0380F800u && pc < 0x0380F900u);
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
    uint8_t  read8 (uint32_t a) override { return bus_read_u8(a); }
    uint16_t read16(uint32_t a) override { return bus_read_u16(a); }
    uint32_t read32(uint32_t a) override { return bus_read_u32(a); }
    void write8 (uint32_t a, uint8_t  v) override { bus_write_u8(a, v); }
    void write16(uint32_t a, uint16_t v) override { bus_write_u16(a, v); }
    void write32(uint32_t a, uint32_t v) override { bus_write_u32(a, v); }
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
    CPUState ic;
    sync_in(ic);
    const bool trace_this_entry = (g_nds_active == NDS_ARM9) || trace_pc(ic.R[15]);
    if (trace_this_entry) trace_push(ic, 0, ic.R[15], 0, ic.R[15], 0);

    long guard = 0;
    while (true) {
        uint32_t pc = ic.R[15];
        bool thumb = ic.cpsr.t;

        // insn7/insn9 anchor reached during interpreted code → stop AT this
        // (not-yet-executed) instruction, symmetric with the bank path's
        // runtime_should_yield check. State is fully in `ic`; sync_out below.
        if (g_nds_insn_stop) break;

        // Tier-1 takeover: a static bank covers this PC — hand back to the
        // dispatcher (it will call the recompiled function).
        if (nds_has_bank(pc & ~1u, thumb ? 1 : 0)) break;
        // Only RAM-resident copied code belongs to Tier 3 (main RAM, WRAM,
        // or ARM9 ITCM); anything else is a genuine gap. The bus owns the
        // memory map, so ask it (covers the ITCM mirror the firmware uses
        // for its IRQ handler).
        if (!bus_addr_is_exec_ram(pc & ~1u)) break;

        // Retired-instruction counter (insn9/insn7), symmetric with the
        // recompiled-bank runtime_insn_fp hook: one bump per interpreted guest
        // instruction, counted as it begins (committed to execute).
        nds_note_insn_retired(g_nds_active);

        Instr in = thumb
            ? armv4t::ThumbDecoder::decode(g_bus.read16(pc & ~1u), pc & ~1u)
            : armv4t::ArmDecoder::decode(g_bus.read32(pc & ~3u), pc & ~3u);
        const bool traced = (g_nds_active == NDS_ARM9) || trace_pc(pc);
        if (traced) trace_push(ic, 1, pc, in.raw, ic.R[15], 0);

        uint32_t cyc = 1;
        Interpreter::Result r = Interpreter::step(ic, g_bus, in, &cyc);
        g_runtime_cycles += cyc;
        if (traced) {
            trace_push(ic, 2, pc, in.raw, ic.R[15],
                       static_cast<uint8_t>(r));
        }

        // Keep the call-return stack balanced across tier boundaries: the
        // banks push on BL/BLX and pop (via runtime_call_should_return) on
        // a matching `bx lr`/`pop pc`. The interpreter must mirror that, or
        // a bank that BL'd into Tier-3 (whose return executes here) leaks
        // its push and eventually overflows the stack.
        if (in.is_call && in.op != armv4t::IrOp::BL_prefix) {
            runtime_call_push_return(pc + (thumb ? 2u : 4u));
        } else if (r == Interpreter::Result::Branched && !in.is_call) {
            runtime_call_should_return(ic.R[15] & ~1u);  // pops iff it matches
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
            Interpreter::enter_irq(ic, ic.R[15]);
            ic.R[15] = nds_exception_base() + 0x18u;
        }

        // Cooperative slice boundary — clean here (all state is in `ic`).
        if (nds_slice_over()) break;
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
