// interpreter.h — Reference IR interpreter.
//
// Executes one decoded `Instr` against a `CPUState` and a `Bus`. This
// is what gives us a testable surface for the decoder before codegen
// exists, and what we'll run jsmolka/gba-tests against during Phase 1
// validation.
//
// Design points:
// - Stateless API: each call takes everything it needs. No hidden
//   global state.
// - Single CPU model: ARM and THUMB are the same struct; CPSR.T
//   distinguishes. PC pipeline offset is applied to register reads
//   of R15 according to the instruction's own `thumb` flag.
// - Branches set `cpu.R[15]` to the target's *current_pc*, i.e. the
//   value the next decoded instruction should treat as its `pc`
//   field. The caller's fetch loop is responsible for translating
//   PC into the next instruction word; the interpreter doesn't fetch.
// - PC advancement: if the instruction wrote to PC, the result is
//   `Branched` and PC is left exactly where the instruction put it.
//   Otherwise the result is `Normal` and PC is advanced by 4 (ARM)
//   or 2 (THUMB).
// - Set-flags semantics:
//     * Arithmetic ops set N/Z from result, C/V from arithmetic.
//     * Logical ops set N/Z from result, C from shifter carry-out,
//       V unchanged.
//     * `imm_carry_out == 2` means "carry unchanged" (the special
//       case of an unrotated immediate); the interpreter reads CPSR.C
//       in that case.
//
// Coverage in this pass: data processing (all 16 opcodes, imm + all
// shifter forms including by-register), B/BL/BX, LDR/STR (word/byte/
// halfword/sign-extended halfword/sign-extended byte) imm + reg
// offset with pre/post indexing and writeback, LDM/STM (basic case;
// no S-bit handling yet), MUL, SWI. Anything else returns
// `NotImplemented` and the caller can decide what to do.

#pragma once

#include <cstdint>

#include "arm_ir.h"
#include "bus.h"
#include "cpu_state.h"

namespace armv4t {

class Interpreter {
public:
    enum class Result : uint8_t {
        Normal,         // ran; PC advanced by instruction width
        Branched,       // ran; PC was set explicitly (don't auto-advance)
        Swi,            // SWI executed; caller handles BIOS entry
        Undefined,      // architecturally-undefined; trap
        NotImplemented, // op shape not yet handled
    };

    // `cycles_out` (optional) receives the number of host cycles this
    // instruction consumed. Approximation only — see interpreter.cpp's
    // `cycle_cost` table for the model. Pass nullptr to ignore.
    static Result step(CPUState& cpu, Bus& bus, const Instr& i,
                       uint32_t* cycles_out = nullptr);

    // Read a register with PC-pipeline adjustment baked in. Public so
    // tests can verify the rule directly.
    static uint32_t read_reg(const CPUState& cpu, uint8_t r, const Instr& i);

    // Evaluate a condition code against CPSR. Returns true if the
    // instruction should execute.
    static bool cond_passes(Cond c, const CPSR& cpsr);

    // Take an IRQ exception. Caller must check that IE/IF/IME and
    // CPSR.I permit the entry; this function does no gating.
    //
    // Mirrors ARM ARM A2.6.5 IRQ entry:
    //   SPSR_irq ← CPSR
    //   CPSR.mode ← IRQ
    //   CPSR.T ← 0           (handler always runs in ARM state)
    //   CPSR.I ← 1           (mask further IRQs while handling)
    //   LR_irq ← return_address + 4
    //   PC ← 0x00000018
    //
    // `return_address` is the PC of the next instruction that would
    // have executed if the IRQ hadn't fired. For IRQs taken between
    // instructions, that's `cpu.R[15]` at the call site.
    static void enter_irq(CPUState& cpu, uint32_t return_address);

    // Take a Software Interrupt (SWI) exception. Called when step()
    // returns Result::Swi. Mirrors ARM ARM A2.6.4 SWI entry:
    //   SPSR_svc ← CPSR
    //   CPSR.mode ← Supervisor
    //   CPSR.T ← 0           (handler always runs in ARM state)
    //   CPSR.I ← 1           (mask IRQs while in SWI handler)
    //   LR_svc ← address of next instruction (PC of SWI + 4 ARM, +2 THUMB)
    //   PC ← 0x00000008
    //
    // `return_address` is the PC immediately AFTER the SWI instruction
    // (already adjusted by the caller for ARM/THUMB instruction width).
    // The BIOS's standard `MOVS PC, R14` epilogue then returns to it.
    static void enter_swi(CPUState& cpu, uint32_t return_address,
                          bool from_thumb);
};

}  // namespace armv4t
