// thumb_decode.cpp — THUMB-state decoder.
//
// Scaffolding pass. Covers the formats exercised by the smoke test:
// shift-by-imm (LSL/LSR/ASR), add/sub register & immediate, ALU imm,
// ALU reg, hi-reg/BX, PC-relative load, immediate load/store,
// push/pop, conditional branch, unconditional branch, SWI, BL pair.
//
// Like the ARM decoder, the layout is reference-implementation style.

#include "thumb_decode.h"

#include <cstring>

namespace armv4t {

namespace {

constexpr uint32_t bits(uint32_t w, int hi, int lo) {
    return (w >> lo) & ((1u << (hi - lo + 1)) - 1u);
}

constexpr bool bit(uint32_t w, int b) {
    return ((w >> b) & 1u) != 0;
}

void zero_instr(Instr& i, uint16_t hw, uint32_t pc) {
    std::memset(&i, 0, sizeof(i));
    i.pc    = pc;
    i.thumb = true;
    i.raw   = hw;
    i.cond  = Cond::AL;
}

}  // namespace

Instr ThumbDecoder::undefined(uint16_t hw, uint32_t pc) {
    Instr i; zero_instr(i, hw, pc);
    i.op = IrOp::Undefined;
    i.is_undefined = true;
    return i;
}

// Format 1: 000 OP imm5 Rs Rd  (LSL/LSR/ASR imm)
Instr ThumbDecoder::fmt1_shift_imm(uint16_t hw, uint32_t pc) {
    Instr i; zero_instr(i, hw, pc);
    uint32_t op   = bits(hw, 12, 11);
    uint32_t imm5 = bits(hw, 10, 6);
    uint8_t  rs   = static_cast<uint8_t>(bits(hw, 5, 3));
    uint8_t  rd   = static_cast<uint8_t>(bits(hw, 2, 0));
    i.op = IrOp::MOV;
    i.set_flags = true;
    i.rd = rd;
    i.op2.kind = Op2::Kind::Shifted;
    i.op2.shifted.rm = rs;
    i.op2.shifted.by_register = false;
    i.op2.shifted.imm_or_rs = static_cast<uint8_t>(imm5);
    switch (op) {
        case 0: i.op2.shifted.type = ShiftType::LSL; break;
        case 1: i.op2.shifted.type = ShiftType::LSR; break;
        case 2: i.op2.shifted.type = ShiftType::ASR; break;
        default: return undefined(hw, pc);
    }
    return i;
}

// Format 2: 00011 I OP Rn|imm Rs Rd  (ADD/SUB with optional immediate-3)
Instr ThumbDecoder::fmt2_add_sub(uint16_t hw, uint32_t pc) {
    Instr i; zero_instr(i, hw, pc);
    bool     i_bit = bit(hw, 10);
    bool     sub   = bit(hw, 9);
    uint8_t  rn_or_imm = static_cast<uint8_t>(bits(hw, 8, 6));
    uint8_t  rs    = static_cast<uint8_t>(bits(hw, 5, 3));
    uint8_t  rd    = static_cast<uint8_t>(bits(hw, 2, 0));
    i.op = sub ? IrOp::SUB : IrOp::ADD;
    i.set_flags = true;
    i.rd = rd;
    i.rn = rs;
    if (i_bit) {
        i.op2.kind = Op2::Kind::Imm;
        i.op2.imm_value = rn_or_imm;
        i.op2.imm_carry_out = 2;
    } else {
        i.op2.kind = Op2::Kind::Shifted;
        i.op2.shifted.rm = rn_or_imm;
        i.op2.shifted.type = ShiftType::LSL;
        i.op2.shifted.imm_or_rs = 0;
    }
    return i;
}

// Format 3: 001 OP Rd imm8  (MOV/CMP/ADD/SUB Rd, #imm8)
Instr ThumbDecoder::fmt3_alu_imm(uint16_t hw, uint32_t pc) {
    Instr i; zero_instr(i, hw, pc);
    uint32_t op  = bits(hw, 12, 11);
    uint8_t  rd  = static_cast<uint8_t>(bits(hw, 10, 8));
    uint32_t imm = bits(hw, 7, 0);
    static constexpr IrOp ops[4] = {IrOp::MOV, IrOp::CMP, IrOp::ADD, IrOp::SUB};
    i.op = ops[op];
    i.set_flags = true;
    i.rd = rd;
    i.rn = rd;  // CMP/ADD/SUB: rn == rd
    i.op2.kind = Op2::Kind::Imm;
    i.op2.imm_value = imm;
    i.op2.imm_carry_out = 2;
    return i;
}

// Format 4: 010000 OP4 Rs Rd  (ALU register-register)
//
// THUMB Format-4 has 16 ALU op codes. Most are straight ARM data-
// processing ops where the second operand is Rs. The four shift-by-
// register variants (LSL/LSR/ASR/ROR) are special: they encode as
// "Rd = Rd <SHIFT> Rs[7:0]" — the shift TYPE is the op, the COUNT
// is in Rs. In ARM-IR terms they map to `MOVS Rd, Rd <SHIFT> Rs`,
// with the shift type selected by op and `by_register = true` so
// the shifter pulls the count from Rs[7:0] at execute time.
//
// Previously these four ops mapped to a plain LSL-by-0, which is
// just a copy from Rs to Rd and produces the wrong result for any
// non-zero shift. Found via the mGBA oracle lockstep harness at
// BIOS instruction #38209 (`ROR R3, R4` with R3=0x18, R4=3 → real
// ROR result 3, but our decoder produced R3 = R4 = 3 via the wrong
// MOV path, with completely wrong flags).
Instr ThumbDecoder::fmt4_alu_reg(uint16_t hw, uint32_t pc) {
    Instr i; zero_instr(i, hw, pc);
    uint32_t op = bits(hw, 9, 6);
    uint8_t rs  = static_cast<uint8_t>(bits(hw, 5, 3));
    uint8_t rd  = static_cast<uint8_t>(bits(hw, 2, 0));
    static constexpr IrOp ops[16] = {
        IrOp::AND, IrOp::EOR, IrOp::MOV, IrOp::MOV,  // 0=AND 1=EOR 2=LSL 3=LSR
        IrOp::MOV, IrOp::ADC, IrOp::SBC, IrOp::MOV,  // 4=ASR 5=ADC 6=SBC 7=ROR
        IrOp::TST, IrOp::RSB, IrOp::CMP, IrOp::CMN,  // 8=TST 9=NEG 10=CMP 11=CMN
        IrOp::ORR, IrOp::MUL, IrOp::BIC, IrOp::MVN,  // 12=ORR 13=MUL 14=BIC 15=MVN
    };
    i.op = ops[op];
    i.set_flags = true;
    i.rd = rd;
    i.rn = rd;
    if (i.op == IrOp::MUL) {
        i.rm = rd;
        i.rs = rs;
        return i;
    }
    // NEG Rd, Rs (op 9): Rd = -Rs. ARM equivalent is
    // `RSB Rd, Rs, #0` — Rn = Rs (subtraction source), Op2 = 0
    // (immediate). The default "Rn = Rd, Op2 = Rs" path computes
    // Rs - Rd instead, which is wrong (e.g., NEG R0,R0 with R0=0x7E
    // gives 0 instead of 0xFFFFFF82). Found via oracle lockstep at
    // BIOS instruction #570503.
    if (op == 9) {
        i.rn = rs;
        i.op2.kind = Op2::Kind::Imm;
        i.op2.imm_value     = 0;
        i.op2.imm_carry_out = 2;  // "carry unchanged" sentinel
        return i;
    }
    // Shift-by-register variants (op 2/3/4/7): encode Rd as the
    // shifted operand and Rs as the shift count.
    if (op == 2 || op == 3 || op == 4 || op == 7) {
        ShiftType st = ShiftType::LSL;
        switch (op) {
            case 2: st = ShiftType::LSL; break;
            case 3: st = ShiftType::LSR; break;
            case 4: st = ShiftType::ASR; break;
            case 7: st = ShiftType::ROR; break;
        }
        i.op2.kind = Op2::Kind::Shifted;
        i.op2.shifted.rm          = rd;   // value to shift = Rd
        i.op2.shifted.type        = st;
        i.op2.shifted.by_register = true;
        i.op2.shifted.imm_or_rs   = rs;   // count from Rs[7:0]
        return i;
    }
    // Everything else: second operand is Rs (no shift).
    i.op2.kind = Op2::Kind::Shifted;
    i.op2.shifted.rm          = rs;
    i.op2.shifted.type        = ShiftType::LSL;
    i.op2.shifted.by_register = false;
    i.op2.shifted.imm_or_rs   = 0;
    return i;
}

// Format 5: 010001 OP H1 H2 Rs/Hs Rd/Hd  (hi-register ops + BX/BLX)
Instr ThumbDecoder::fmt5_hi_reg(uint16_t hw, uint32_t pc) {
    Instr i; zero_instr(i, hw, pc);
    uint32_t op = bits(hw, 9, 8);
    bool h1 = bit(hw, 7);
    bool h2 = bit(hw, 6);
    uint8_t rs = static_cast<uint8_t>(bits(hw, 5, 3) | (h2 ? 8u : 0u));
    uint8_t rd = static_cast<uint8_t>(bits(hw, 2, 0) | (h1 ? 8u : 0u));
    switch (op) {
        case 0b00: i.op = IrOp::ADD; i.set_flags = false; break;
        case 0b01: i.op = IrOp::CMP; i.set_flags = true;  break;
        case 0b10: i.op = IrOp::MOV; i.set_flags = false; break;
        case 0b11: {  // H1=0: BX; H1=1: BLX (register, ARMv5T)
            i.op = h1 ? IrOp::BLX_reg : IrOp::BX;
            i.rm = rs;
            i.branch_link = h1;
            i.branch_exchange = true;
            i.is_branch = true;
            i.is_indirect = true;
            i.is_call = h1;
            if (!h1 && i.rm == 14) i.is_return = true;
            return i;
        }
    }
    i.rd = rd;
    i.rn = rd;
    i.op2.kind = Op2::Kind::Shifted;
    i.op2.shifted.rm = rs;
    i.op2.shifted.type = ShiftType::LSL;
    i.op2.shifted.imm_or_rs = 0;
    if (i.rd == 15 && i.op != IrOp::CMP) {
        i.is_pc_writing = true;
        i.is_branch = true;
        i.is_indirect = true;
    }
    return i;
}

// Format 6: 01001 Rd imm8  (LDR Rd, [PC, #imm8 << 2])
Instr ThumbDecoder::fmt6_pc_rel_load(uint16_t hw, uint32_t pc) {
    Instr i; zero_instr(i, hw, pc);
    uint8_t  rd  = static_cast<uint8_t>(bits(hw, 10, 8));
    uint32_t imm = bits(hw, 7, 0) << 2;
    i.op = IrOp::LDR;
    i.rd = rd;
    i.mem.rn          = 15;  // PC
    i.mem.pre_indexed = true;
    i.mem.add         = true;
    i.mem.writeback   = false;
    i.mem.by_register = false;
    // THUMB PC-relative loads use (PC + 4) & ~3 + offset.
    i.mem.imm_offset  = imm;
    return i;
}

// Format 9: 011 B L imm5 Rb Rd  (LDR/STR / LDRB/STRB immediate)
Instr ThumbDecoder::fmt9_ldst_imm(uint16_t hw, uint32_t pc) {
    Instr i; zero_instr(i, hw, pc);
    bool b = bit(hw, 12);
    bool l = bit(hw, 11);
    uint32_t imm5 = bits(hw, 10, 6);
    uint8_t  rb   = static_cast<uint8_t>(bits(hw, 5, 3));
    uint8_t  rd   = static_cast<uint8_t>(bits(hw, 2, 0));
    if (l) i.op = b ? IrOp::LDRB : IrOp::LDR;
    else   i.op = b ? IrOp::STRB : IrOp::STR;
    i.rd = rd;
    i.mem.rn          = rb;
    i.mem.pre_indexed = true;
    i.mem.add         = true;
    i.mem.writeback   = false;
    i.mem.by_register = false;
    i.mem.imm_offset  = b ? imm5 : (imm5 << 2);
    return i;
}

// Format 7: 0101 LB 0 Ro Rb Rd  (LDR/STR/LDRB/STRB register-offset)
Instr ThumbDecoder::fmt7_ldst_reg(uint16_t hw, uint32_t pc) {
    Instr i; zero_instr(i, hw, pc);
    bool l = bit(hw, 11);
    bool b = bit(hw, 10);
    uint8_t ro = static_cast<uint8_t>(bits(hw, 8, 6));
    uint8_t rb = static_cast<uint8_t>(bits(hw, 5, 3));
    uint8_t rd = static_cast<uint8_t>(bits(hw, 2, 0));
    if (l) i.op = b ? IrOp::LDRB : IrOp::LDR;
    else   i.op = b ? IrOp::STRB : IrOp::STR;
    i.rd = rd;
    i.mem.rn          = rb;
    i.mem.pre_indexed = true;
    i.mem.add         = true;
    i.mem.writeback   = false;
    i.mem.byte_access = b;
    i.mem.by_register = true;
    i.mem.reg_offset.rm = ro;
    i.mem.reg_offset.type = ShiftType::LSL;
    i.mem.reg_offset.imm_or_rs = 0;
    i.mem.reg_offset.by_register = false;
    return i;
}

// Format 8: 0101 HS 1 Ro Rb Rd  (sign/halfword reg-offset)
//   H S  →   00=STRH, 01=LDRH, 10=LDRSB, 11=LDRSH
Instr ThumbDecoder::fmt8_ldst_sext_reg(uint16_t hw, uint32_t pc) {
    Instr i; zero_instr(i, hw, pc);
    bool h = bit(hw, 11);
    bool s = bit(hw, 10);
    uint8_t ro = static_cast<uint8_t>(bits(hw, 8, 6));
    uint8_t rb = static_cast<uint8_t>(bits(hw, 5, 3));
    uint8_t rd = static_cast<uint8_t>(bits(hw, 2, 0));
    if (!s && !h)      i.op = IrOp::STRH;
    else if (!s &&  h) i.op = IrOp::LDRH;
    else if ( s && !h) i.op = IrOp::LDRSB;
    else               i.op = IrOp::LDRSH;
    i.rd = rd;
    i.mem.rn          = rb;
    i.mem.pre_indexed = true;
    i.mem.add         = true;
    i.mem.writeback   = false;
    i.mem.by_register = true;
    i.mem.reg_offset.rm = ro;
    i.mem.reg_offset.type = ShiftType::LSL;
    i.mem.reg_offset.imm_or_rs = 0;
    return i;
}

// Format 10: 1000 L imm5 Rb Rd  (LDRH/STRH immediate, scaled by 2)
Instr ThumbDecoder::fmt10_ldst_halfword_imm(uint16_t hw, uint32_t pc) {
    Instr i; zero_instr(i, hw, pc);
    bool l = bit(hw, 11);
    uint32_t imm5 = bits(hw, 10, 6);
    uint8_t rb = static_cast<uint8_t>(bits(hw, 5, 3));
    uint8_t rd = static_cast<uint8_t>(bits(hw, 2, 0));
    i.op = l ? IrOp::LDRH : IrOp::STRH;
    i.rd = rd;
    i.mem.rn          = rb;
    i.mem.pre_indexed = true;
    i.mem.add         = true;
    i.mem.writeback   = false;
    i.mem.by_register = false;
    i.mem.imm_offset  = imm5 << 1;
    return i;
}

// Format 11: 1001 L Rd imm8  (SP-rel LDR/STR, imm8 << 2)
Instr ThumbDecoder::fmt11_sp_rel(uint16_t hw, uint32_t pc) {
    Instr i; zero_instr(i, hw, pc);
    bool l = bit(hw, 11);
    uint8_t  rd  = static_cast<uint8_t>(bits(hw, 10, 8));
    uint32_t imm = bits(hw, 7, 0) << 2;
    i.op = l ? IrOp::LDR : IrOp::STR;
    i.rd = rd;
    i.mem.rn          = 13;  // SP
    i.mem.pre_indexed = true;
    i.mem.add         = true;
    i.mem.writeback   = false;
    i.mem.by_register = false;
    i.mem.imm_offset  = imm;
    return i;
}

// Format 12: 1010 SP Rd imm8  (ADD Rd, PC|SP, #imm8 << 2)
//   SP=0 → ADD Rd, PC, #imm   (PC value is (curr+4) & ~3)
//   SP=1 → ADD Rd, SP, #imm
Instr ThumbDecoder::fmt12_load_address(uint16_t hw, uint32_t pc) {
    Instr i; zero_instr(i, hw, pc);
    bool sp = bit(hw, 11);
    uint8_t rd = static_cast<uint8_t>(bits(hw, 10, 8));
    uint32_t imm = bits(hw, 7, 0) << 2;
    i.op = IrOp::ADD;
    i.rd = rd;
    i.rn = sp ? 13 : 15;
    i.op2.kind = Op2::Kind::Imm;
    i.op2.imm_value = imm;
    i.op2.imm_carry_out = 2;
    return i;
}

// Format 13: 1011 0000 S imm7  (ADD/SUB SP, #imm7 << 2)
Instr ThumbDecoder::fmt13_add_sub_sp(uint16_t hw, uint32_t pc) {
    Instr i; zero_instr(i, hw, pc);
    bool sub = bit(hw, 7);
    uint32_t imm = bits(hw, 6, 0) << 2;
    i.op = sub ? IrOp::SUB : IrOp::ADD;
    i.rd = 13;
    i.rn = 13;
    i.op2.kind = Op2::Kind::Imm;
    i.op2.imm_value = imm;
    i.op2.imm_carry_out = 2;
    return i;
}

// Format 14: 1011 L 10 R reglist  (PUSH / POP)
Instr ThumbDecoder::fmt14_pushpop(uint16_t hw, uint32_t pc) {
    Instr i; zero_instr(i, hw, pc);
    bool load = bit(hw, 11);
    bool r    = bit(hw, 8);
    uint16_t rl = static_cast<uint16_t>(bits(hw, 7, 0));
    if (r) rl |= load ? (1u << 15) : (1u << 14);
    i.op = load ? IrOp::LDM : IrOp::STM;
    i.block.rn          = 13;  // SP
    i.block.pre_indexed = !load;          // PUSH = pre-decrement, POP = post-increment
    i.block.add         = load;           // POP increments; PUSH decrements
    i.block.writeback   = true;
    i.block.load        = load;
    i.block.reg_list    = rl;
    if (load && r) {  // POP { ..., PC }
        i.is_pc_writing = true;
        i.is_branch = true;
        i.is_indirect = true;
        i.is_return = true;
    }
    return i;
}

// Format 15: 1100 L Rb reglist  (LDMIA/STMIA)
Instr ThumbDecoder::fmt15_multi_ldst(uint16_t hw, uint32_t pc) {
    Instr i; zero_instr(i, hw, pc);
    bool load = bit(hw, 11);
    uint8_t rb = static_cast<uint8_t>(bits(hw, 10, 8));
    uint16_t rl = static_cast<uint16_t>(bits(hw, 7, 0));
    i.op = load ? IrOp::LDM : IrOp::STM;
    i.block.rn          = rb;
    i.block.pre_indexed = false;
    i.block.add         = true;
    i.block.writeback   = true;
    i.block.load        = load;
    i.block.reg_list    = rl;
    return i;
}

// Format 16: 1101 cond imm8  (Bcc)
Instr ThumbDecoder::fmt16_cond_branch(uint16_t hw, uint32_t pc) {
    Instr i; zero_instr(i, hw, pc);
    uint8_t c = static_cast<uint8_t>(bits(hw, 11, 8));
    if (c == 0xF) return fmt17_swi(hw, pc);  // SWI shares the prefix
    int32_t off = static_cast<int8_t>(bits(hw, 7, 0));
    i.cond = static_cast<Cond>(c);
    i.op = IrOp::B;
    i.branch_target = static_cast<uint32_t>(static_cast<int32_t>(pc + 4) + (off << 1));
    i.is_branch = true;
    return i;
}

// Format 17: 1101 1111 imm8  (SWI)
Instr ThumbDecoder::fmt17_swi(uint16_t hw, uint32_t pc) {
    Instr i; zero_instr(i, hw, pc);
    i.op = IrOp::SWI;
    i.swi_imm = bits(hw, 7, 0);
    return i;
}

// Format 18: 11100 imm11  (unconditional branch)
Instr ThumbDecoder::fmt18_uncond_branch(uint16_t hw, uint32_t pc) {
    Instr i; zero_instr(i, hw, pc);
    int32_t off = static_cast<int32_t>(bits(hw, 10, 0));
    if (off & 0x400) off |= 0xFFFFF800;  // sign-extend 11 bits
    i.op = IrOp::B;
    i.branch_target = static_cast<uint32_t>(static_cast<int32_t>(pc + 4) + (off << 1));
    i.is_branch = true;
    return i;
}

// Format 19: 1111 H imm11  (THUMB BL pair).
//
// A full BL is a 32-bit instruction spread across two consecutive
// halfwords. Neither half stands alone:
//
//   Upper (H=0): LR  = (PC + 4) + (sext(imm11) << 12)
//                PC  += 2
//   Lower (H=1): target = LR + (imm11 << 1)
//                LR  = (PC_of_lower + 2) | 1   (resume in THUMB on BX)
//                PC  = target
//
// We keep the two halves as distinct IR records so the interpreter
// can run them sequentially in a single fetch/decode/step loop —
// no special pairing required by the decoder.
//
// `branch_target` on the upper half is the *partial* target (so a
// diagnostic dump shows something useful). `swi_imm` on the lower
// half carries the shifted offset (imm11 << 1).
Instr ThumbDecoder::fmt19_long_branch(uint16_t hw, uint32_t pc) {
    Instr i; zero_instr(i, hw, pc);
    uint32_t b12_11 = bits(hw, 12, 11);  // 10 = prefix, 11 = BL lo, 01 = BLX lo
    uint32_t imm11 = bits(hw, 10, 0);
    i.branch_link = true;
    i.is_branch   = true;
    i.is_call     = true;
    if (b12_11 == 0b10) {
        // Shared upper half: LR = (PC+4) + sext(imm11)<<12.
        i.op = IrOp::BL_prefix;
        int32_t off = static_cast<int32_t>(imm11);
        if (off & 0x400) off |= 0xFFFFF800;
        i.branch_target = static_cast<uint32_t>(
            static_cast<int32_t>(pc + 4) + (off << 12));
    } else {
        // Lower half: BL (11111) stays THUMB; BLX (11101) switches to ARM.
        i.op = IrOp::BL_suffix;
        i.swi_imm = imm11 << 1;
        i.branch_exchange = (b12_11 == 0b01);  // BLX → exchange to ARM
    }
    return i;
}

Instr ThumbDecoder::decode(uint16_t hw, uint32_t pc) {
    // Top-3-bit fanout per ARM ARM A4.1 "THUMB instruction set encoding".
    uint32_t top3 = static_cast<uint32_t>(hw) >> 13;
    switch (top3) {
        case 0b000: {
            // 000xx ... — shift by imm OR add/sub
            if (bits(hw, 12, 11) == 0b11) return fmt2_add_sub(hw, pc);
            return fmt1_shift_imm(hw, pc);
        }
        case 0b001:
            return fmt3_alu_imm(hw, pc);
        case 0b010: {
            if (bits(hw, 12, 10) == 0b000) return fmt4_alu_reg(hw, pc);
            if (bits(hw, 12, 10) == 0b001) return fmt5_hi_reg(hw, pc);
            if (bits(hw, 12, 11) == 0b01)  return fmt6_pc_rel_load(hw, pc);
            // 0b0101xxx: after fmt 4/5/6 are taken, anything left with
            // bit 12 set is fmt 7 (LDR/STR reg-offset) or fmt 8
            // (sign-extended halfword/byte reg-offset). Bit 9
            // discriminates: 0 = fmt 7, 1 = fmt 8.
            if (bit(hw, 12)) {
                if (bit(hw, 9)) return fmt8_ldst_sext_reg(hw, pc);
                return fmt7_ldst_reg(hw, pc);
            }
            return undefined(hw, pc);
        }
        case 0b011:
            return fmt9_ldst_imm(hw, pc);
        case 0b100: {
            // 1000 = LDRH/STRH imm; 1001 = SP-rel LDR/STR.
            if (!bit(hw, 12)) return fmt10_ldst_halfword_imm(hw, pc);
            return fmt11_sp_rel(hw, pc);
        }
        case 0b101: {
            // 1010 = ADD Rd, PC|SP, #imm; 1011 = misc (push/pop, ADD/SUB SP).
            if (!bit(hw, 12)) return fmt12_load_address(hw, pc);

            // 1011 space. PUSH/POP encoding: 1011 L 10 R reglist
            //   bits 12..9 = 1010 → PUSH; bits 12..9 = 1110 → POP.
            // ADD/SUB SP, #imm: bits 15..8 = 1011 0000 (so bits 12..8 = 10000).
            uint32_t b12_9 = bits(hw, 12, 9);
            if (b12_9 == 0b1010 || b12_9 == 0b1110) {
                return fmt14_pushpop(hw, pc);
            }
            if (bits(hw, 12, 8) == 0b10000) {
                return fmt13_add_sub_sp(hw, pc);
            }
            return undefined(hw, pc);
        }
        case 0b110: {
            if (bits(hw, 12, 12) == 0b0) return fmt15_multi_ldst(hw, pc);
            // 1101 = conditional branch / SWI
            return fmt16_cond_branch(hw, pc);
        }
        case 0b111: {
            // bits 15..11 encoding inside top3=111:
            //   11100  → unconditional B   (fmt 18)
            //   11101  → BLX suffix (ARMv5; THUMB→ARM)  (fmt 19)
            //   11110  → BL/BLX upper half  (fmt 19)
            //   11111  → BL lower half      (fmt 19)
            uint32_t b12_11 = bits(hw, 12, 11);
            if (b12_11 == 0b00) return fmt18_uncond_branch(hw, pc);
            return fmt19_long_branch(hw, pc);    // 01 (BLX lo), 10 (hi), 11 (BL lo)
        }
    }
    return undefined(hw, pc);
}

}  // namespace armv4t
