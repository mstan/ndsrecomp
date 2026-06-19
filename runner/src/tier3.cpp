// tier3.cpp — see tier3.h.
//
// The reference interpreter (recompiler/armv4t/interpreter.cpp) is the
// always-correct execution floor. It operates on armv4t::CPUState +
// armv4t::Bus; this file bridges those to the runner's g_cpu and C bus ABI,
// and drives the fetch→decode→step loop with cross-tier exits.

#include "tier3.h"

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

namespace {

// Bus adapter: forward the interpreter's accesses to the C bus ABI (which
// applies the DS memory map, TCM, I/O, and the always-on access ring).
struct RtBus : armv4t::Bus {
    uint8_t  read8 (uint32_t a) override { return bus_read_u8(a); }
    uint16_t read16(uint32_t a) override { return bus_read_u16(a); }
    uint32_t read32(uint32_t a) override { return bus_read_u32(a); }
    void write8 (uint32_t a, uint8_t  v) override { bus_write_u8(a, v); }
    void write16(uint32_t a, uint16_t v) override { bus_write_u16(a, v); }
    void write32(uint32_t a, uint32_t v) override { bus_write_u32(a, v); }
};
RtBus g_bus;

// g_cpu (packed CPSR) ↔ CPUState (structured CPSR). Register windows and
// banked tables share layout (ARM_BANK_* == BankedSlot order).
void sync_in(CPUState& c) {
    for (int i = 0; i < 16; ++i) c.R[i] = g_cpu.R[i];
    uint32_t p = g_cpu.cpsr;
    c.cpsr.n = (p >> 31) & 1; c.cpsr.z = (p >> 30) & 1;
    c.cpsr.c = (p >> 29) & 1; c.cpsr.v = (p >> 28) & 1;
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
    g_cpu.cpsr =
        (uint32_t(c.cpsr.n) << 31) | (uint32_t(c.cpsr.z) << 30) |
        (uint32_t(c.cpsr.c) << 29) | (uint32_t(c.cpsr.v) << 28) |
        (uint32_t(c.cpsr.i) << 7)  | (uint32_t(c.cpsr.f) << 6)  |
        (uint32_t(c.cpsr.t) << 5)  | uint32_t(c.cpsr.mode);
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

    long guard = 0;
    while (true) {
        uint32_t pc = ic.R[15];
        bool thumb = ic.cpsr.t;

        // Tier-1 takeover: a static bank covers this PC — hand back to the
        // dispatcher (it will call the recompiled function).
        if (nds_has_bank(pc & ~1u, thumb ? 1 : 0)) break;
        // Only RAM-resident copied code belongs to Tier 3; anything else is
        // a genuine gap.
        if (!(pc >= 0x02000000u && pc < 0x04000000u)) break;

        Instr in = thumb
            ? armv4t::ThumbDecoder::decode(g_bus.read16(pc & ~1u), pc & ~1u)
            : armv4t::ArmDecoder::decode(g_bus.read32(pc & ~3u), pc & ~3u);

        uint32_t cyc = 1;
        Interpreter::Result r = Interpreter::step(ic, g_bus, in, &cyc);
        g_runtime_cycles += cyc;

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
                break;  // PC now at the SWI vector (a bank) → exits next iter
            case Interpreter::Result::Undefined:
            case Interpreter::Result::NotImplemented:
                sync_out(ic);
                nds_halt("tier3: interpreter undefined/not-implemented");
                return;
        }

        // Deliver a pending IRQ to the interpreted CPU (vectors to the BIOS
        // handler bank next iteration).
        if (!ic.cpsr.i && nds_irq_pending(g_nds_active))
            Interpreter::enter_irq(ic, ic.R[15]);

        // Cooperative slice boundary — clean here (all state is in `ic`).
        if (nds_slice_over()) break;
        if (++guard > 50'000'000) {
            sync_out(ic);
            nds_halt("tier3: slice guard (no exit)");
            return;
        }
    }
    sync_out(ic);
}
