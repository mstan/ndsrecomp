// interpreter.cpp — see interpreter.h.

#include "interpreter.h"

#include <cstdint>

#include "pipeline_semantics.h"

namespace armv4t {

namespace {

// ─────────────────────────────────────────────────────────────────────
// Small helpers
// ─────────────────────────────────────────────────────────────────────

inline uint32_t rotr32(uint32_t v, uint32_t r) {
    r &= 31;
    if (r == 0) return v;
    return (v >> r) | (v << (32 - r));
}

inline bool sign_bit(uint32_t v) { return (v >> 31) & 1u; }

// Compute the shifter operand value and shifter carry-out for a
// register-shifted operand. cpsr_c is the current CPSR.C (used only
// for RRX). The "by_register" form's count is the low 8 bits of Rs;
// counts > 32 are handled per ARM ARM A5.1.5.
struct ShiftResult { uint32_t value; uint32_t carry; };  // carry: 0/1
ShiftResult do_shift(uint32_t rm_val, ShiftType type, uint32_t count,
                     bool count_is_zero_in_encoding, bool cpsr_c) {
    // For ARM register-form shifts where the count field in the
    // encoding is literally 0:
    //   LSL #0   → no shift, carry = CPSR.C
    //   LSR #0   → treated as LSR #32: value=0, carry = bit 31 of Rm
    //   ASR #0   → treated as ASR #32: arithmetic sign-extension
    //   ROR #0   → RRX (handled as type=RRX before we get here)
    // For shift-by-register, count is always the literal Rs[7:0].
    switch (type) {
        case ShiftType::LSL: {
            if (count == 0) return {rm_val, cpsr_c ? 1u : 0u};
            if (count < 32)  return {rm_val << count, (rm_val >> (32 - count)) & 1u};
            if (count == 32) return {0u, rm_val & 1u};
            return {0u, 0u};
        }
        case ShiftType::LSR: {
            if (count == 0) {
                if (count_is_zero_in_encoding) {
                    // imm-form LSR #0 == LSR #32
                    return {0u, (rm_val >> 31) & 1u};
                }
                return {rm_val, cpsr_c ? 1u : 0u};
            }
            if (count < 32)  return {rm_val >> count, (rm_val >> (count - 1)) & 1u};
            if (count == 32) return {0u, (rm_val >> 31) & 1u};
            return {0u, 0u};
        }
        case ShiftType::ASR: {
            int32_t s = static_cast<int32_t>(rm_val);
            if (count == 0) {
                if (count_is_zero_in_encoding) {
                    uint32_t v = (s < 0) ? 0xFFFFFFFFu : 0u;
                    return {v, (rm_val >> 31) & 1u};
                }
                return {rm_val, cpsr_c ? 1u : 0u};
            }
            if (count < 32) {
                uint32_t v = static_cast<uint32_t>(s >> count);
                return {v, (rm_val >> (count - 1)) & 1u};
            }
            uint32_t v = (s < 0) ? 0xFFFFFFFFu : 0u;
            return {v, (rm_val >> 31) & 1u};
        }
        case ShiftType::ROR: {
            if (count == 0) return {rm_val, cpsr_c ? 1u : 0u};
            uint32_t v = rotr32(rm_val, count);
            return {v, (v >> 31) & 1u};
        }
        case ShiftType::RRX: {
            uint32_t v = (rm_val >> 1) | ((cpsr_c ? 1u : 0u) << 31);
            return {v, rm_val & 1u};
        }
    }
    return {rm_val, cpsr_c ? 1u : 0u};
}

// Sign-extend lo-bit field to 32-bit.
inline int32_t sext(uint32_t v, int bits) {
    uint32_t mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    v &= mask;
    int shift = 32 - bits;
    return static_cast<int32_t>(v << shift) >> shift;
}

// Arithmetic flag computation.
struct ArithFlags { uint32_t result; bool n, z, c, v; };

ArithFlags add_with_carry(uint32_t a, uint32_t b, uint32_t carry_in) {
    uint64_t u = static_cast<uint64_t>(a) + static_cast<uint64_t>(b) + carry_in;
    uint32_t r = static_cast<uint32_t>(u);
    ArithFlags f{};
    f.result = r;
    f.n = sign_bit(r);
    f.z = (r == 0);
    f.c = (u >> 32) & 1ull;
    // V: overflow when signs of a and b match and differ from result.
    f.v = ((~(a ^ b) & (a ^ r)) >> 31) & 1u;
    return f;
}

ArithFlags sub_with_carry(uint32_t a, uint32_t b, uint32_t carry_in) {
    // Carry-in here is the AArch32 convention: carry_in == 1 means
    // "no borrow." Therefore a - b - (1 - carry_in) → use ADC(a, ~b, c).
    return add_with_carry(a, ~b, carry_in);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// Public helpers
// ─────────────────────────────────────────────────────────────────────

uint32_t Interpreter::read_reg(const CPUState& cpu, uint8_t r, const Instr& i) {
    if (r != 15) return cpu.R[r];
    return i.thumb ? thumb_pc_value(i.pc) : arm_pc_value(i.pc);
}

uint32_t stm_pc_store_value(const CPUState& cpu, const Instr& i) {
    uint32_t pc = Interpreter::read_reg(cpu, 15, i);
    if (i.thumb) pc &= ~2u;
    return pc + 4u;
}

void Interpreter::enter_irq(CPUState& cpu, uint32_t return_address) {
    // Pack CPSR into a uint32 view for SPSR storage.
    uint32_t cpsr_u32 = 0;
    if (cpu.cpsr.n) cpsr_u32 |= 1u << 31;
    if (cpu.cpsr.z) cpsr_u32 |= 1u << 30;
    if (cpu.cpsr.c) cpsr_u32 |= 1u << 29;
    if (cpu.cpsr.v) cpsr_u32 |= 1u << 28;
    if (cpu.cpsr.i) cpsr_u32 |= 1u << 7;
    if (cpu.cpsr.f) cpsr_u32 |= 1u << 6;
    if (cpu.cpsr.t) cpsr_u32 |= 1u << 5;
    cpsr_u32 |= cpu.cpsr.mode & 0x1Fu;

    // Bank out the current mode's R13/R14 into its banked slot.
    auto mode_to_bank = [](uint8_t m) -> BankedSlot {
        switch (static_cast<Mode>(m)) {
            case Mode::FIQ:        return Bank_FIQ;
            case Mode::IRQ:        return Bank_IRQ;
            case Mode::Supervisor: return Bank_Supervisor;
            case Mode::Abort:      return Bank_Abort;
            case Mode::Undefined:  return Bank_Undefined;
            default:               return Bank_User;
        }
    };
    auto old_bank = mode_to_bank(cpu.cpsr.mode);
    cpu.banked_sp[old_bank] = cpu.R[13];
    cpu.banked_lr[old_bank] = cpu.R[14];

    // Save CPSR into SPSR_irq.
    cpu.banked_spsr[Bank_IRQ] = cpsr_u32;

    // Switch to IRQ mode and load its banked SP.
    cpu.cpsr.mode = static_cast<uint8_t>(Mode::IRQ);
    cpu.R[13]     = cpu.banked_sp[Bank_IRQ];
    // LR_irq = return + 4 (so the standard `SUBS PC, LR, #4` epilogue
    // returns to `return_address`).
    cpu.R[14]     = return_address + 4u;

    // CPSR for the handler: ARM state, IRQ disabled.
    cpu.cpsr.t = false;
    cpu.cpsr.i = true;
    cpu.thumb  = false;

    // Vector.
    cpu.R[15] = 0x00000018u;
}

void Interpreter::enter_swi(CPUState& cpu, uint32_t return_address,
                            bool /*from_thumb*/) {
    // Pack CPSR for SPSR_svc.
    uint32_t cpsr_u32 = 0;
    if (cpu.cpsr.n) cpsr_u32 |= 1u << 31;
    if (cpu.cpsr.z) cpsr_u32 |= 1u << 30;
    if (cpu.cpsr.c) cpsr_u32 |= 1u << 29;
    if (cpu.cpsr.v) cpsr_u32 |= 1u << 28;
    if (cpu.cpsr.i) cpsr_u32 |= 1u << 7;
    if (cpu.cpsr.f) cpsr_u32 |= 1u << 6;
    if (cpu.cpsr.t) cpsr_u32 |= 1u << 5;
    cpsr_u32 |= cpu.cpsr.mode & 0x1Fu;

    // Bank out current mode's R13/R14.
    auto mode_to_bank = [](uint8_t m) -> BankedSlot {
        switch (static_cast<Mode>(m)) {
            case Mode::FIQ:        return Bank_FIQ;
            case Mode::IRQ:        return Bank_IRQ;
            case Mode::Supervisor: return Bank_Supervisor;
            case Mode::Abort:      return Bank_Abort;
            case Mode::Undefined:  return Bank_Undefined;
            default:               return Bank_User;
        }
    };
    auto old_bank = mode_to_bank(cpu.cpsr.mode);
    cpu.banked_sp[old_bank] = cpu.R[13];
    cpu.banked_lr[old_bank] = cpu.R[14];

    cpu.banked_spsr[Bank_Supervisor] = cpsr_u32;

    cpu.cpsr.mode = static_cast<uint8_t>(Mode::Supervisor);
    cpu.R[13]     = cpu.banked_sp[Bank_Supervisor];
    // LR_svc = address of next instruction. The caller adjusts for
    // ARM/THUMB instruction width before passing in.
    cpu.R[14]     = return_address;

    cpu.cpsr.t = false;
    cpu.cpsr.i = true;
    cpu.thumb  = false;

    cpu.R[15] = 0x00000008u;
}

bool Interpreter::cond_passes(Cond c, const CPSR& cpsr) {
    switch (c) {
        case Cond::EQ: return  cpsr.z;
        case Cond::NE: return !cpsr.z;
        case Cond::CS: return  cpsr.c;
        case Cond::CC: return !cpsr.c;
        case Cond::MI: return  cpsr.n;
        case Cond::PL: return !cpsr.n;
        case Cond::VS: return  cpsr.v;
        case Cond::VC: return !cpsr.v;
        case Cond::HI: return  cpsr.c && !cpsr.z;
        case Cond::LS: return !cpsr.c ||  cpsr.z;
        case Cond::GE: return  cpsr.n == cpsr.v;
        case Cond::LT: return  cpsr.n != cpsr.v;
        case Cond::GT: return !cpsr.z && (cpsr.n == cpsr.v);
        case Cond::LE: return  cpsr.z || (cpsr.n != cpsr.v);
        case Cond::AL: return true;
        case Cond::NV: return false;
    }
    return false;
}

namespace {

// Evaluate Op2: value + shifter carry-out.
struct Op2Eval { uint32_t value; uint32_t carry; };
Op2Eval eval_op2(const CPUState& cpu, const Instr& i) {
    if (i.op2.kind == Op2::Kind::Imm) {
        if (i.op2.imm_carry_out == 2u) {
            return {i.op2.imm_value, cpu.cpsr.c ? 1u : 0u};
        }
        return {i.op2.imm_value, i.op2.imm_carry_out};
    }
    const auto& sr = i.op2.shifted;
    uint32_t rm_val = Interpreter::read_reg(cpu, sr.rm, i);
    uint32_t count;
    bool count_zero_encoded;
    if (sr.by_register) {
        // ARM register-controlled shifts read R15 as PC+12 when it
        // is the shifted value (Rm). The extra +4 is the same
        // pipeline effect modeled below for PC-as-Rs.
        if (!i.thumb && sr.rm == 15) rm_val += 4u;
        // PC-as-Rs adds an extra +4 (per ARM ARM register-shift rule).
        // We don't expect this in practice for GBA games, but model
        // it correctly anyway.
        uint32_t rs = (sr.imm_or_rs == 15)
            ? (Interpreter::read_reg(cpu, 15, i) + 4u)
            : cpu.R[sr.imm_or_rs];
        count = rs & 0xFFu;
        count_zero_encoded = false;
    } else {
        count = sr.imm_or_rs;
        count_zero_encoded = (sr.imm_or_rs == 0);
    }
    auto sh = do_shift(rm_val, sr.type, count, count_zero_encoded, cpu.cpsr.c);
    return {sh.value, sh.carry};
}

void set_logical_flags(CPSR& cpsr, uint32_t result, uint32_t carry) {
    cpsr.n = sign_bit(result);
    cpsr.z = (result == 0);
    cpsr.c = (carry != 0);
    // V unchanged for logical ops.
}

void set_arith_flags(CPSR& cpsr, const ArithFlags& f) {
    cpsr.n = f.n;
    cpsr.z = f.z;
    cpsr.c = f.c;
    cpsr.v = f.v;
}

// Returns true if the instruction wrote PC (so caller skips auto-advance).
bool write_dest(CPUState& cpu, uint8_t rd, uint32_t value, const Instr& i) {
    if (rd == 15) {
        cpu.R[15] = i.thumb ? (value & ~1u) : (value & ~3u);
        return true;
    }
    cpu.R[rd] = value;
    return rd == 15;
}

BankedSlot mode_to_bank(uint8_t m) {
    switch (static_cast<Mode>(m)) {
        case Mode::FIQ:        return Bank_FIQ;
        case Mode::IRQ:        return Bank_IRQ;
        case Mode::Supervisor: return Bank_Supervisor;
        case Mode::Abort:      return Bank_Abort;
        case Mode::Undefined:  return Bank_Undefined;
        default:               return Bank_User;
    }
}

uint32_t read_user_reg(const CPUState& cpu, int reg) {
    if (reg < 8 || reg == 15) return cpu.R[reg];
    if (reg < 13) {
        return (cpu.cpsr.mode == static_cast<uint8_t>(Mode::FIQ))
            ? cpu.r8_12_user[reg - 8]
            : cpu.R[reg];
    }
    if (cpu.cpsr.mode == static_cast<uint8_t>(Mode::User) ||
        cpu.cpsr.mode == static_cast<uint8_t>(Mode::System)) {
        return cpu.R[reg];
    }
    return (reg == 13) ? cpu.banked_sp[Bank_User]
                       : cpu.banked_lr[Bank_User];
}

void write_user_reg(CPUState& cpu, int reg, uint32_t value) {
    if (reg < 8 || reg == 15) {
        cpu.R[reg] = value;
        return;
    }
    if (reg < 13) {
        if (cpu.cpsr.mode == static_cast<uint8_t>(Mode::FIQ)) {
            cpu.r8_12_user[reg - 8] = value;
        } else {
            cpu.R[reg] = value;
        }
        return;
    }
    if (cpu.cpsr.mode == static_cast<uint8_t>(Mode::User) ||
        cpu.cpsr.mode == static_cast<uint8_t>(Mode::System)) {
        cpu.R[reg] = value;
    } else if (reg == 13) {
        cpu.banked_sp[Bank_User] = value;
    } else {
        cpu.banked_lr[Bank_User] = value;
    }
}

// ARM ARM A2.6.5 / A4.1.45 "Exception return": when a data-processing
// instruction or LDM has Rd=R15 (PC) AND the S bit set AND we are in
// a privileged mode (anything except User/System), the SPSR of the
// current mode is restored into the CPSR, and PC is set to `new_pc`.
//
// This is the path the BIOS uses to leave IRQ/SVC handlers (e.g.
// `SUBS PC, LR, #4` or `LDMFD SP!, {..., PC}^`). Without it, the
// CPU stays in IRQ mode with I=1 forever after the first exception,
// and every subsequent IRQ is silently masked.
void exception_return(CPUState& cpu, uint32_t new_pc) {
    uint8_t old_mode = cpu.cpsr.mode;
    if (old_mode == static_cast<uint8_t>(Mode::User) ||
        old_mode == static_cast<uint8_t>(Mode::System)) {
        // No SPSR in these modes — exception return is undefined
        // on hardware. We just set PC and bail.
        cpu.R[15] = new_pc;
        return;
    }
    auto old_bank = mode_to_bank(old_mode);
    uint32_t spsr = cpu.banked_spsr[old_bank];

    // Bank out the IRQ-mode (or current mode's) R13/R14 before we
    // pivot to the restored mode's banked pair.
    cpu.banked_sp[old_bank] = cpu.R[13];
    cpu.banked_lr[old_bank] = cpu.R[14];

    // Unpack SPSR into CPSR.
    uint8_t new_mode = static_cast<uint8_t>(spsr & 0x1Fu);
    cpu.cpsr.n = (spsr >> 31) & 1u;
    cpu.cpsr.z = (spsr >> 30) & 1u;
    cpu.cpsr.c = (spsr >> 29) & 1u;
    cpu.cpsr.v = (spsr >> 28) & 1u;
    cpu.cpsr.i = (spsr >>  7) & 1u;
    cpu.cpsr.f = (spsr >>  6) & 1u;
    cpu.cpsr.t = (spsr >>  5) & 1u;
    cpu.cpsr.mode = new_mode;
    cpu.thumb = cpu.cpsr.t;

    auto new_bank = mode_to_bank(new_mode);
    cpu.R[13] = cpu.banked_sp[new_bank];
    cpu.R[14] = cpu.banked_lr[new_bank];

    cpu.R[15] = new_pc;
}

void restore_cpsr_from_spsr(CPUState& cpu) {
    uint8_t old_mode = cpu.cpsr.mode;
    if (old_mode == static_cast<uint8_t>(Mode::User) ||
        old_mode == static_cast<uint8_t>(Mode::System)) {
        return;
    }
    auto old_bank = mode_to_bank(old_mode);
    uint32_t spsr = cpu.banked_spsr[old_bank];

    cpu.banked_sp[old_bank] = cpu.R[13];
    cpu.banked_lr[old_bank] = cpu.R[14];

    uint8_t new_mode = static_cast<uint8_t>(spsr & 0x1Fu);
    cpu.cpsr.n = (spsr >> 31) & 1u;
    cpu.cpsr.z = (spsr >> 30) & 1u;
    cpu.cpsr.c = (spsr >> 29) & 1u;
    cpu.cpsr.v = (spsr >> 28) & 1u;
    cpu.cpsr.i = (spsr >>  7) & 1u;
    cpu.cpsr.f = (spsr >>  6) & 1u;
    cpu.cpsr.t = (spsr >>  5) & 1u;
    cpu.cpsr.mode = new_mode;
    cpu.thumb = cpu.cpsr.t;

    auto new_bank = mode_to_bank(new_mode);
    cpu.R[13] = cpu.banked_sp[new_bank];
    cpu.R[14] = cpu.banked_lr[new_bank];
}

bool is_priv_non_system(uint8_t mode) {
    return mode != static_cast<uint8_t>(Mode::User) &&
           mode != static_cast<uint8_t>(Mode::System);
}

// ─────────────────────────────────────────────────────────────────────
// Cycle accounting
// ─────────────────────────────────────────────────────────────────────
//
// `cycle_cost_base` returns the FIXED part of the instruction's cost —
// the fetch (1S) and any internal (I) cycles. Memory-access cycles
// (N for the data access, S for sequential accesses in LDM/STM) are
// added at execute time via `bus.access_cycles(addr, width, seq)` so
// they reflect the actual target region's bus width + waitstates.
//
// Pipeline refill (any op that writes to PC) is charged uniformly as
// +2 cycles on top of the base cost: the prefetch buffer is flushed
// and two new instruction fetches occur before the next decode. For
// branches the refill is folded into `cycle_cost_base` so we don't
// double-count.
//
// Reference: GBATEK § "GBA Memory Map - Bus Width and Speed",
// ARM7TDMI TRM § "Instruction Cycle Times", and `GbaBus::access_cycles`.

// The per-instruction cycle model (fixed base cost + multiply operand
// waits) lives in arm_ir.cpp as `instr_cycle_base` / `mul_wait_cycles`,
// shared verbatim with the recompiler codegen and the runtime tick
// helpers so the recompiled cycle stream matches this oracle exactly.
// Memory-access N/S cycles come from `Bus::access_cycles`.

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// Main step
// ─────────────────────────────────────────────────────────────────────

Interpreter::Result Interpreter::step(CPUState& cpu, Bus& bus, const Instr& i,
                                      uint32_t* cycles_out) {
    // Condition check first.
    if (!cond_passes(i.cond, cpu.cpsr)) {
        // Auto-advance and report Normal. A condition-failed
        // instruction still costs 1S for its fetch.
        cpu.R[15] += i.thumb ? kThumbInsnBytes : kArmInsnBytes;
        if (cycles_out) *cycles_out = 1;
        return Result::Normal;
    }

    if (i.is_undefined) {
        // Caller decides how to trap. PC is not auto-advanced; on
        // hardware, exception entry takes over.
        if (cycles_out) *cycles_out = 1;
        return Result::Undefined;
    }

    bool wrote_pc = false;
    // Memory-access cycles accumulated by LDR/STR/LDM/STM/SWP during
    // execute. Final cycles_out = cycle_cost_base + mem_cycles + any
    // pipeline-refill surcharge for PC-writing non-branch ops.
    uint32_t mem_cycles = 0;
    uint32_t extra_cycles = 0;

    switch (i.op) {
        // ── Data processing ────────────────────────────────────────
        case IrOp::AND: case IrOp::EOR: case IrOp::ORR:
        case IrOp::BIC: case IrOp::MOV: case IrOp::MVN:
        case IrOp::TST: case IrOp::TEQ: {
            auto o2  = eval_op2(cpu, i);
            uint32_t rn  = (i.op == IrOp::MOV || i.op == IrOp::MVN)
                            ? 0u
                            : read_reg(cpu, i.rn, i);
            if (!i.thumb && i.rn == 15 &&
                i.op2.kind == Op2::Kind::Shifted &&
                i.op2.shifted.by_register) {
                rn += 4u;
            }
            uint32_t r;
            switch (i.op) {
                case IrOp::AND: r = rn & o2.value;  break;
                case IrOp::EOR: r = rn ^ o2.value;  break;
                case IrOp::ORR: r = rn | o2.value;  break;
                case IrOp::BIC: r = rn & ~o2.value; break;
                case IrOp::MOV: r = o2.value;       break;
                case IrOp::MVN: r = ~o2.value;      break;
                case IrOp::TST: r = rn & o2.value;  break;
                case IrOp::TEQ: r = rn ^ o2.value;  break;
                default: r = 0;                     break;
            }
            // Exception return: Rd=R15 with S set, in a privileged
            // non-User/System mode → restore CPSR from SPSR_<mode>,
            // do NOT update flags from the result.
            bool excpt_return =
                i.set_flags && i.rd == 15 && is_priv_non_system(cpu.cpsr.mode);
            bool spsr_restore_test =
                i.set_flags && i.rd == 15 &&
                (i.op == IrOp::TST || i.op == IrOp::TEQ) &&
                is_priv_non_system(cpu.cpsr.mode);
            if (i.set_flags && !excpt_return) {
                set_logical_flags(cpu.cpsr, r, o2.carry);
                if (spsr_restore_test) restore_cpsr_from_spsr(cpu);
            }
            if (i.op != IrOp::TST && i.op != IrOp::TEQ) {
                if (excpt_return) {
                    exception_return(cpu, r);
                    wrote_pc = true;
                } else {
                    wrote_pc = write_dest(cpu, i.rd, r, i);
                }
            }
            break;
        }

        case IrOp::ADD: case IrOp::ADC:
        case IrOp::SUB: case IrOp::SBC:
        case IrOp::RSB: case IrOp::RSC:
        case IrOp::CMP: case IrOp::CMN: {
            auto o2 = eval_op2(cpu, i);
            uint32_t rn = read_reg(cpu, i.rn, i);
            if (!i.thumb && i.rn == 15 &&
                i.op2.kind == Op2::Kind::Shifted &&
                i.op2.shifted.by_register) {
                rn += 4u;
            }
            // THUMB ADD Rd, PC, #imm aligns the PC value to a 4-byte
            // boundary (ARM ARM A4.1.6). Same rule as the LDR PC-rel
            // path above. ARM mode does NOT align PC reads.
            if (i.thumb && i.rn == 15 && i.op2.kind == Op2::Kind::Imm) {
                rn &= ~3u;
            }
            ArithFlags f{};
            uint32_t c_in = cpu.cpsr.c ? 1u : 0u;
            switch (i.op) {
                case IrOp::ADD: f = add_with_carry(rn, o2.value, 0u);    break;
                case IrOp::ADC: f = add_with_carry(rn, o2.value, c_in);   break;
                case IrOp::SUB: f = sub_with_carry(rn, o2.value, 1u);    break;
                case IrOp::SBC: f = sub_with_carry(rn, o2.value, c_in);   break;
                case IrOp::RSB: f = sub_with_carry(o2.value, rn, 1u);    break;
                case IrOp::RSC: f = sub_with_carry(o2.value, rn, c_in);   break;
                case IrOp::CMP: f = sub_with_carry(rn, o2.value, 1u);    break;
                case IrOp::CMN: f = add_with_carry(rn, o2.value, 0u);    break;
                default: break;
            }
            bool excpt_return_arith =
                i.set_flags && i.rd == 15 && is_priv_non_system(cpu.cpsr.mode);
            bool spsr_restore_test =
                i.set_flags && i.rd == 15 &&
                (i.op == IrOp::CMP || i.op == IrOp::CMN) &&
                is_priv_non_system(cpu.cpsr.mode);
            if (i.set_flags && !excpt_return_arith) {
                set_arith_flags(cpu.cpsr, f);
                if (spsr_restore_test) restore_cpsr_from_spsr(cpu);
            }
            if (i.op != IrOp::CMP && i.op != IrOp::CMN) {
                if (excpt_return_arith) {
                    exception_return(cpu, f.result);
                    wrote_pc = true;
                } else {
                    wrote_pc = write_dest(cpu, i.rd, f.result, i);
                }
            }
            break;
        }

        // ── Branches ───────────────────────────────────────────────
        case IrOp::B: {
            cpu.R[15] = i.branch_target;
            wrote_pc = true;
            break;
        }
        case IrOp::BL: {
            uint32_t ret = i.pc + (i.thumb ? kThumbInsnBytes : kArmInsnBytes);
            cpu.R[14] = ret;
            cpu.R[15] = i.branch_target;
            wrote_pc = true;
            break;
        }
        case IrOp::BL_prefix: {
            // Upper half of a THUMB BL pair: stash the partial target
            // (PC+4 + sext(imm11) << 12) into LR. The lower half will
            // combine that with its own imm11 << 1 to produce the
            // final target. PC just advances by 2.
            cpu.R[14] = i.branch_target;
            break;
        }
        case IrOp::BL_suffix: {
            // Lower half: target = LR + (imm11 << 1).
            // New LR = (PC_of_lower + 2) | 1 so a BX LR resumes in
            // THUMB.
            uint32_t target = (cpu.R[14] + i.swi_imm) & ~1u;
            cpu.R[14] = (i.pc + kThumbInsnBytes) | 1u;
            cpu.R[15] = target;
            wrote_pc = true;
            break;
        }
        case IrOp::BX: {
            uint32_t target = read_reg(cpu, i.rm, i);
            // Bit 0 selects THUMB on entry; clear it for the PC value.
            cpu.cpsr.t = (target & 1u) != 0;
            cpu.thumb  = cpu.cpsr.t;
            cpu.R[15] = target & ~1u;
            wrote_pc = true;
            break;
        }

        // ── Loads / stores ─────────────────────────────────────────
        case IrOp::LDR: case IrOp::STR:
        case IrOp::LDRB: case IrOp::STRB:
        case IrOp::LDRH: case IrOp::STRH:
        case IrOp::LDRSB: case IrOp::LDRSH: {
            uint32_t base = read_reg(cpu, i.mem.rn, i);
            // THUMB PC-relative addressing (LDR Rd, [PC, #imm]) forces
            // bits[1:0] of the PC value to zero — ARM ARM A4.1.20.
            // Without this, an instruction at an odd halfword boundary
            // (e.g. PC=0x11E) reads a misaligned word and produces a
            // rotated value that matches no symbol in the constant
            // pool. (We discovered this against BIOS 0x11E loading
            // the IWRAM-clear loop counter.)
            if (i.thumb && i.mem.rn == 15) base &= ~3u;
            uint32_t offset;
            if (i.mem.by_register) {
                uint32_t rm_val = cpu.R[i.mem.reg_offset.rm];
                auto sh = do_shift(rm_val,
                                   i.mem.reg_offset.type,
                                   i.mem.reg_offset.imm_or_rs,
                                   i.mem.reg_offset.imm_or_rs == 0,
                                   cpu.cpsr.c);
                offset = sh.value;
            } else {
                offset = i.mem.imm_offset;
            }
            uint32_t effective_addr = i.mem.pre_indexed
                ? (i.mem.add ? base + offset : base - offset)
                : base;
            uint32_t post_addr =
                i.mem.add ? base + offset : base - offset;

            // Region-aware access cost. Width follows the IR op.
            uint8_t access_w = 4;
            switch (i.op) {
                case IrOp::LDRB: case IrOp::STRB: case IrOp::LDRSB:
                    access_w = 1; break;
                case IrOp::LDRH: case IrOp::STRH: case IrOp::LDRSH:
                    access_w = 2; break;
                default:
                    access_w = 4; break;
            }
            mem_cycles += bus.access_cycles(effective_addr, access_w, false);

            // Load form.
            if (i.op == IrOp::LDR || i.op == IrOp::LDRB ||
                i.op == IrOp::LDRH || i.op == IrOp::LDRSB ||
                i.op == IrOp::LDRSH) {
                uint32_t value = 0;
                switch (i.op) {
                    case IrOp::LDR: {
                        // ARMv4T: misaligned word loads rotate.
                        uint32_t v = bus.read32(effective_addr & ~3u);
                        uint32_t rot = (effective_addr & 3u) * 8u;
                        value = rotr32(v, rot);
                        break;
                    }
                    case IrOp::LDRB:
                        value = bus.read8(effective_addr);
                        break;
                    case IrOp::LDRH:
                        // ARMv4T: misaligned halfword load rotates.
                        if (effective_addr & 1u) {
                            uint32_t v = bus.read16(effective_addr & ~1u);
                            value = ((v >> 8) | (v << 24)) & 0xFFFFFFFFu;
                        } else {
                            value = bus.read16(effective_addr);
                        }
                        break;
                    case IrOp::LDRSB:
                        value = static_cast<uint32_t>(sext(bus.read8(effective_addr), 8));
                        break;
                    case IrOp::LDRSH: {
                        // ARMv4T: misaligned LDRSH acts as LDRSB on the
                        // odd byte (ARM ARM A4.1.27 "Operation").
                        if (effective_addr & 1u) {
                            value = static_cast<uint32_t>(sext(bus.read8(effective_addr), 8));
                        } else {
                            value = static_cast<uint32_t>(sext(bus.read16(effective_addr), 16));
                        }
                        break;
                    }
                    default: break;
                }
                wrote_pc = write_dest(cpu, i.rd, value, i);
            } else {
                // Store form. Note: STR of PC stores `pc + 12` on
                // ARMv4T (pipeline + one extra instruction).
                uint32_t val = (i.rd == 15)
                    ? (i.pc + (i.thumb ? 12u : 12u))
                    : cpu.R[i.rd];
                switch (i.op) {
                    case IrOp::STR:  bus.write32(effective_addr & ~3u, val); break;
                    case IrOp::STRB: bus.write8 (effective_addr, val & 0xFFu); break;
                    case IrOp::STRH: bus.write16(effective_addr & ~1u, val & 0xFFFFu); break;
                    default: break;
                }
            }

            // Writeback: pre-indexed only when explicit W; post-indexed
            // always (but ignore if Rn == Rd on a load — Rd wins).
            bool do_wb = i.mem.writeback;
            if (do_wb && !(i.mem.rn == i.rd &&
                           (i.op == IrOp::LDR || i.op == IrOp::LDRB ||
                            i.op == IrOp::LDRH || i.op == IrOp::LDRSB ||
                            i.op == IrOp::LDRSH))) {
                cpu.R[i.mem.rn] = i.mem.pre_indexed ? effective_addr : post_addr;
            }
            break;
        }

        // ── Block transfer ─────────────────────────────────────────
        case IrOp::LDM: case IrOp::STM: {
            uint32_t base = cpu.R[i.block.rn];
            uint16_t list = i.block.reg_list;
            int n = __builtin_popcount(list);
            if (n == 0) {
                uint32_t addr;
                if (i.block.add) {
                    addr = i.block.pre_indexed ? base + 4u : base;
                } else {
                    addr = i.block.pre_indexed ? base - 0x40u : base - 0x3Cu;
                }
                uint32_t final_base = i.block.add
                    ? base + 0x40u
                    : base - 0x40u;
                mem_cycles += bus.access_cycles(addr & ~3u, 4,
                                                /*sequential=*/false);
                if (i.block.load) {
                    uint32_t v = bus.read32(addr & ~3u);
                    if (i.block.writeback) cpu.R[i.block.rn] = final_base;
                    if (i.block.s_bit && is_priv_non_system(cpu.cpsr.mode)) {
                        exception_return(cpu, v & ~1u);
                    } else {
                        cpu.R[15] = v & ~1u;
                    }
                    wrote_pc = true;
                } else {
                    bus.write32(addr & ~3u, stm_pc_store_value(cpu, i));
                    if (i.block.writeback) cpu.R[i.block.rn] = final_base;
                }
                break;
            }
            uint32_t addr;
            // Compute starting address. The four addressing modes
            // (IA/IB/DA/DB) follow from P (pre) and U (add) per
            // ARM ARM A5.4.
            if (i.block.add) {
                addr = i.block.pre_indexed ? base + 4 : base;
            } else {
                addr = i.block.pre_indexed ? base - 4 * n : base - 4 * (n - 1);
            }
            uint32_t lowest = addr;
            uint32_t final_base = i.block.add
                ? base + 4 * n
                : base - 4 * n;
            // Iterate registers in ascending order; lowest-numbered
            // gets the lowest address regardless of direction.
            // Hardware semantics: store in ascending register order
            // to ascending addresses.
            uint32_t cursor = lowest;
            // First access is non-sequential (N), each subsequent
            // access is sequential (S). The region-aware cost model
            // makes this important for LDM/STM over slow regions
            // (e.g., EWRAM at 6 cycles per N + 3 cycles per S for
            // 32-bit). LDM keeps the 1I cycle from cycle_cost_base.
            bool first_access = true;
            for (int reg = 0; reg < 16; ++reg) {
                if (!(list & (1u << reg))) continue;
                mem_cycles += bus.access_cycles(cursor & ~3u, 4,
                                                /*sequential=*/!first_access);
                first_access = false;
                if (i.block.load) {
                    uint32_t v = bus.read32(cursor & ~3u);
                    if (reg == 15) {
                        // LDM with R15: when the S bit is set AND
                        // we're in a privileged non-User/System mode,
                        // this is an exception return — SPSR → CPSR
                        // (and bank-swap). Otherwise plain PC load.
                        // ARMv4T loads PC as a word; bit 0 is forced
                        // clear (no interworking).
                        if (i.block.s_bit && is_priv_non_system(cpu.cpsr.mode)) {
                            exception_return(cpu, v & ~1u);
                        } else {
                            cpu.R[15] = v & ~1u;
                        }
                        wrote_pc = true;
                    } else if (i.block.s_bit &&
                               (list & (1u << 15)) == 0) {
                        write_user_reg(cpu, reg, v);
                    } else {
                        cpu.R[reg] = v;
                    }
                } else {
                    bool store_writeback_base =
                        i.block.writeback && reg == i.block.rn &&
                        (list & ((1u << reg) - 1u)) != 0;
                    uint32_t v = (reg == 15)
                        ? stm_pc_store_value(cpu, i)
                        : (store_writeback_base
                            ? final_base
                            : (i.block.s_bit
                                ? read_user_reg(cpu, reg)
                                : cpu.R[reg]));
                    bus.write32(cursor & ~3u, v);
                }
                cursor += 4;
            }
            bool base_in_list = (list & (1u << i.block.rn)) != 0;
            if (i.block.writeback && !(i.block.load && base_in_list)) {
                cpu.R[i.block.rn] = final_base;
            }
            break;
        }

        // ── Multiply (32-bit) ──────────────────────────────────────
        case IrOp::MUL: {
            uint32_t wait_operand = i.thumb ? cpu.R[i.rm] : cpu.R[i.rs];
            extra_cycles += mul_wait_cycles(wait_operand, /*signed=*/true, 0);
            uint32_t r = cpu.R[i.rm] * cpu.R[i.rs];
            cpu.R[i.rd] = r;
            if (i.set_flags) {
                cpu.cpsr.n = sign_bit(r);
                cpu.cpsr.z = (r == 0);
                // C and V are unpredictable on ARMv4T for MUL/MLA;
                // leave them alone.
            }
            break;
        }
        case IrOp::MLA: {
            extra_cycles += mul_wait_cycles(cpu.R[i.rs], /*signed=*/true, 1);
            // Decoder layout: Rd=destination (and accumulator source
            // from Rn slot), Rm × Rs + Rn → Rd.
            uint32_t r = cpu.R[i.rm] * cpu.R[i.rs] + cpu.R[i.rn];
            cpu.R[i.rd] = r;
            if (i.set_flags) {
                cpu.cpsr.n = sign_bit(r);
                cpu.cpsr.z = (r == 0);
            }
            break;
        }

        // ── Multiply long ──────────────────────────────────────────
        // Decoder field layout for multiply-long:
        //   Rd = RdHi (bits 19..16)
        //   Rn = RdLo (bits 15..12)
        //   Rs (bits 11..8), Rm (bits 3..0)
        // Result low word → R[Rn]; high word → R[Rd].
        case IrOp::UMULL: {
            extra_cycles += mul_wait_cycles(cpu.R[i.rs], /*signed=*/false, 1);
            uint64_t prod = static_cast<uint64_t>(cpu.R[i.rm]) *
                            static_cast<uint64_t>(cpu.R[i.rs]);
            cpu.R[i.rn] = static_cast<uint32_t>(prod & 0xFFFFFFFFu);
            cpu.R[i.rd] = static_cast<uint32_t>(prod >> 32);
            if (i.set_flags) {
                cpu.cpsr.n = (prod >> 63) & 1u;
                cpu.cpsr.z = (prod == 0);
            }
            break;
        }
        case IrOp::UMLAL: {
            extra_cycles += mul_wait_cycles(cpu.R[i.rs], /*signed=*/false, 2);
            uint64_t prod = static_cast<uint64_t>(cpu.R[i.rm]) *
                            static_cast<uint64_t>(cpu.R[i.rs]);
            uint64_t acc = (static_cast<uint64_t>(cpu.R[i.rd]) << 32) |
                           cpu.R[i.rn];
            uint64_t sum = acc + prod;
            cpu.R[i.rn] = static_cast<uint32_t>(sum & 0xFFFFFFFFu);
            cpu.R[i.rd] = static_cast<uint32_t>(sum >> 32);
            if (i.set_flags) {
                cpu.cpsr.n = (sum >> 63) & 1u;
                cpu.cpsr.z = (sum == 0);
            }
            break;
        }
        case IrOp::SMULL: {
            extra_cycles += mul_wait_cycles(cpu.R[i.rs], /*signed=*/true, 1);
            int64_t prod = static_cast<int64_t>(static_cast<int32_t>(cpu.R[i.rm])) *
                           static_cast<int64_t>(static_cast<int32_t>(cpu.R[i.rs]));
            uint64_t u = static_cast<uint64_t>(prod);
            cpu.R[i.rn] = static_cast<uint32_t>(u & 0xFFFFFFFFu);
            cpu.R[i.rd] = static_cast<uint32_t>(u >> 32);
            if (i.set_flags) {
                cpu.cpsr.n = (u >> 63) & 1u;
                cpu.cpsr.z = (u == 0);
            }
            break;
        }
        case IrOp::SMLAL: {
            extra_cycles += mul_wait_cycles(cpu.R[i.rs], /*signed=*/true, 2);
            int64_t prod = static_cast<int64_t>(static_cast<int32_t>(cpu.R[i.rm])) *
                           static_cast<int64_t>(static_cast<int32_t>(cpu.R[i.rs]));
            uint64_t acc = (static_cast<uint64_t>(cpu.R[i.rd]) << 32) |
                           cpu.R[i.rn];
            uint64_t sum = acc + static_cast<uint64_t>(prod);
            cpu.R[i.rn] = static_cast<uint32_t>(sum & 0xFFFFFFFFu);
            cpu.R[i.rd] = static_cast<uint32_t>(sum >> 32);
            if (i.set_flags) {
                cpu.cpsr.n = (sum >> 63) & 1u;
                cpu.cpsr.z = (sum == 0);
            }
            break;
        }

        // ── Swap (atomic) ─────────────────────────────────────────
        case IrOp::SWP: {
            uint32_t addr = cpu.R[i.rn];
            uint32_t orig = bus.read32(addr & ~3u);
            uint32_t rot = (addr & 3u) * 8u;
            orig = rotr32(orig, rot);
            bus.write32(addr & ~3u, cpu.R[i.rm]);
            cpu.R[i.rd] = orig;
            break;
        }
        case IrOp::SWPB: {
            uint32_t addr = cpu.R[i.rn];
            uint8_t orig = bus.read8(addr);
            bus.write8(addr, static_cast<uint8_t>(cpu.R[i.rm] & 0xFFu));
            cpu.R[i.rd] = orig;
            break;
        }

        // ── PSR transfer ───────────────────────────────────────────
        case IrOp::MRS: {
            // Pack CPSR / SPSR_<mode> into a 32-bit value and store
            // into Rd. ARMv4T layout:
            //   [31] N  [30] Z  [29] C  [28] V
            //   [27..8] reserved (read as zero)
            //   [7] I  [6] F  [5] T  [4..0] mode
            CPSR src = cpu.cpsr;  // for SPSR, picked below
            if (i.psr.spsr) {
                // The current banked SPSR; map mode → bank.
                auto bank = Bank_User;
                switch (static_cast<Mode>(cpu.cpsr.mode)) {
                    case Mode::FIQ:        bank = Bank_FIQ;        break;
                    case Mode::IRQ:        bank = Bank_IRQ;        break;
                    case Mode::Supervisor: bank = Bank_Supervisor; break;
                    case Mode::Abort:      bank = Bank_Abort;      break;
                    case Mode::Undefined:  bank = Bank_Undefined;  break;
                    default: break;  // User / System: SPSR undefined
                }
                uint32_t s = cpu.banked_spsr[bank];
                // Reconstitute a CPSR view of the stored SPSR. We
                // store SPSR as a raw 32-bit value so we can just
                // forward it.
                cpu.R[i.rd] = s;
                break;
            }
            uint32_t v = 0;
            if (src.n) v |= 1u << 31;
            if (src.z) v |= 1u << 30;
            if (src.c) v |= 1u << 29;
            if (src.v) v |= 1u << 28;
            if (src.i) v |= 1u << 7;
            if (src.f) v |= 1u << 6;
            if (src.t) v |= 1u << 5;
            v |= (src.mode & 0x1Fu);
            cpu.R[i.rd] = v;
            break;
        }

        case IrOp::MSR: {
            // Determine source value.
            uint32_t src;
            if (i.op2.kind == Op2::Kind::Imm) {
                src = i.op2.imm_value;
            } else {
                src = cpu.R[i.op2.shifted.rm];
            }

            // Field mask: build a 32-bit byte-mask from psr.mask.
            uint32_t byte_mask = 0;
            if (i.psr.mask & 0x1) byte_mask |= 0x000000FFu;  // control byte
            if (i.psr.mask & 0x2) byte_mask |= 0x0000FF00u;  // ext1
            if (i.psr.mask & 0x4) byte_mask |= 0x00FF0000u;  // ext2
            if (i.psr.mask & 0x8) byte_mask |= 0xFF000000u;  // flags

            if (i.psr.spsr) {
                auto bank = Bank_User;
                switch (static_cast<Mode>(cpu.cpsr.mode)) {
                    case Mode::FIQ:        bank = Bank_FIQ;        break;
                    case Mode::IRQ:        bank = Bank_IRQ;        break;
                    case Mode::Supervisor: bank = Bank_Supervisor; break;
                    case Mode::Abort:      bank = Bank_Abort;      break;
                    case Mode::Undefined:  bank = Bank_Undefined;  break;
                    default:
                        // SPSR write in User/System is unpredictable
                        // on hardware; we drop it silently here.
                        // Phase 2.x will route this through the
                        // unmapped-style log.
                        break;
                }
                uint32_t old = cpu.banked_spsr[bank];
                cpu.banked_spsr[bank] = (old & ~byte_mask) | (src & byte_mask);
                break;
            }

            // CPSR write. Privileged fields (bits 7..0: I, F, T, mode)
            // are writable only in privileged modes. User mode can
            // only touch the flags byte. We're permissive — only
            // applying the masked bits — but in User mode we
            // additionally zero out the control byte from the mask.
            bool privileged = (static_cast<Mode>(cpu.cpsr.mode) !=
                               Mode::User);
            if (!privileged) byte_mask &= 0xFF000000u;

            // Build the candidate new CPSR as a uint32.
            uint32_t old = 0;
            if (cpu.cpsr.n) old |= 1u << 31;
            if (cpu.cpsr.z) old |= 1u << 30;
            if (cpu.cpsr.c) old |= 1u << 29;
            if (cpu.cpsr.v) old |= 1u << 28;
            if (cpu.cpsr.i) old |= 1u << 7;
            if (cpu.cpsr.f) old |= 1u << 6;
            if (cpu.cpsr.t) old |= 1u << 5;
            old |= (cpu.cpsr.mode & 0x1Fu);

            uint32_t newv = (old & ~byte_mask) | (src & byte_mask);

            // A mode change requires us to swap banked R13/R14/SPSR
            // in and out. We don't model the full banking matrix
            // yet (R8..R12 FIQ banking is also a thing); for the
            // BIOS bring-up subset, the most common transition is
            // SVC → System (no SP/LR swap because System shares
            // User's bank). When we hit the FIQ case or any other
            // bank-affecting move, this stub leaves stale R13/R14
            // — to be tightened in the next iteration.
            uint8_t new_mode = static_cast<uint8_t>(newv & 0x1Fu);
            uint8_t old_mode = cpu.cpsr.mode;
            cpu.cpsr.n = (newv >> 31) & 1u;
            cpu.cpsr.z = (newv >> 30) & 1u;
            cpu.cpsr.c = (newv >> 29) & 1u;
            cpu.cpsr.v = (newv >> 28) & 1u;
            cpu.cpsr.i = (newv >>  7) & 1u;
            cpu.cpsr.f = (newv >>  6) & 1u;
            cpu.cpsr.t = (newv >>  5) & 1u;
            cpu.cpsr.mode = new_mode;
            cpu.thumb = cpu.cpsr.t;

            // Bank swap helper: save active R13/R14 into the
            // outgoing mode's bank, then restore the incoming.
            auto mode_to_bank = [](uint8_t m) -> BankedSlot {
                switch (static_cast<Mode>(m)) {
                    case Mode::FIQ:        return Bank_FIQ;
                    case Mode::IRQ:        return Bank_IRQ;
                    case Mode::Supervisor: return Bank_Supervisor;
                    case Mode::Abort:      return Bank_Abort;
                    case Mode::Undefined:  return Bank_Undefined;
                    default:               return Bank_User;
                }
            };
            if (old_mode != new_mode) {
                auto old_bank = mode_to_bank(old_mode);
                auto new_bank = mode_to_bank(new_mode);
                if (old_bank != new_bank) {
                    cpu.banked_sp[old_bank] = cpu.R[13];
                    cpu.banked_lr[old_bank] = cpu.R[14];
                    cpu.R[13] = cpu.banked_sp[new_bank];
                    cpu.R[14] = cpu.banked_lr[new_bank];
                }
            }
            break;
        }

        // ── Software interrupt ─────────────────────────────────────
        case IrOp::SWI:
            if (cycles_out) *cycles_out = instr_cycle_base(i.op);
            return Result::Swi;

        default:
            if (cycles_out) *cycles_out = 1;
            return Result::NotImplemented;
    }

    // Compute cycles for the just-executed instruction. Final cost:
    //   base (fetch + internal cycles per cycle_cost_base)
    // + mem_cycles (data-access cycles accumulated by LDR/STR/LDM/
    //   STM/SWP execute paths via bus.access_cycles)
    // + 1I if op2 is a register-shifted operand (mGBA matches
    //   ARM7TDMI's behaviour here: ALU with shift-by-register adds
    //   one internal cycle for the shifter)
    // + pipeline refill (+2) for any non-branch op that wrote PC.
    // Branches (B/BL/BX/BL_*) fold the refill into base.
    if (cycles_out) {
        uint32_t c = instr_cycle_base(i.op) +
                     mem_cycles + extra_cycles;
        bool branch_op = (i.op == IrOp::B || i.op == IrOp::BL ||
                          i.op == IrOp::BX || i.op == IrOp::BLX_reg ||
                          i.op == IrOp::BL_prefix || i.op == IrOp::BL_suffix);
        if (i.op2.kind == Op2::Kind::Shifted &&
            i.op2.shifted.by_register) {
            c += 1;
        }
        if (wrote_pc && !branch_op) c += 2;
        *cycles_out = c;
    }

    if (!wrote_pc) {
        cpu.R[15] += i.thumb ? kThumbInsnBytes : kArmInsnBytes;
        return Result::Normal;
    }
    return Result::Branched;
}

}  // namespace armv4t
