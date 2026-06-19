// arm_decode.cpp — ARM-state decoder.
//
// Scaffolding pass. Covers the encoding shapes we need for the bring-up
// smoke test: data processing (imm + shifted reg), B/BL, BX, LDR/STR
// immediate, LDM/STM, MUL, SWI, MRS/MSR. Other encodings fall through
// to `Undefined` and will be filled in as the test corpus grows.

#include "arm_decode.h"

#include <cstring>

namespace armv4t {

namespace {

constexpr uint32_t bits(uint32_t w, int hi, int lo) {
    return (w >> lo) & ((1u << (hi - lo + 1)) - 1u);
}

constexpr bool bit(uint32_t w, int b) {
    return ((w >> b) & 1u) != 0;
}

Cond cond_from(uint32_t w) {
    return static_cast<Cond>(bits(w, 31, 28));
}

void zero_instr(Instr& i, uint32_t word, uint32_t pc) {
    std::memset(&i, 0, sizeof(i));
    i.pc = pc;
    i.thumb = false;
    i.raw = word;
    i.cond = cond_from(word);
}

// 8-bit immediate rotated by 2 * rot4. Returns the value and the carry
// produced by the rotation (1 if rotated > 0 and bit 31 of the result
// set; 0 if rotated > 0 and bit 31 clear; 2 if rotation was 0, meaning
// the shifter carry is unchanged from CPSR.C — the codegen must read
// the current CPSR).
struct ImmExpand {
    uint32_t value;
    uint32_t carry;
};
ImmExpand expand_imm(uint32_t encoded_imm12) {
    uint32_t imm8 = encoded_imm12 & 0xFFu;
    uint32_t rot  = (encoded_imm12 >> 8) & 0xFu;
    if (rot == 0) {
        return {imm8, 2u};  // 2 = unchanged
    }
    uint32_t r = (imm8 >> (rot * 2)) | (imm8 << (32 - rot * 2));
    return {r, (r >> 31) & 1u};
}

}  // namespace

Op2 ArmDecoder::decode_op2(uint32_t word) {
    Op2 op2{};
    if (bit(word, 25)) {
        // Immediate form
        auto exp = expand_imm(bits(word, 11, 0));
        op2.kind          = Op2::Kind::Imm;
        op2.imm_value     = exp.value;
        op2.imm_carry_out = exp.carry;
        return op2;
    }
    // Register form
    op2.kind = Op2::Kind::Shifted;
    op2.shifted.rm   = static_cast<uint8_t>(bits(word, 3, 0));
    op2.shifted.type = static_cast<ShiftType>(bits(word, 6, 5));
    if (bit(word, 4)) {
        // Shift by register (Rs[7:0])
        op2.shifted.by_register = true;
        op2.shifted.imm_or_rs   = static_cast<uint8_t>(bits(word, 11, 8));
    } else {
        // Shift by immediate
        op2.shifted.by_register = false;
        uint8_t cnt = static_cast<uint8_t>(bits(word, 11, 7));
        // Special-case: LSR/ASR with imm 0 mean "shift by 32".
        // ROR with imm 0 means RRX. Carry handling for these lives in the
        // interpreter / codegen, not here, but we annotate by mapping ROR
        // #0 → RRX so consumers don't have to special-case it.
        if (op2.shifted.type == ShiftType::ROR && cnt == 0) {
            op2.shifted.type = ShiftType::RRX;
        }
        op2.shifted.imm_or_rs = cnt;
    }
    return op2;
}

Instr ArmDecoder::decode_data_processing(uint32_t word, uint32_t pc) {
    Instr i; zero_instr(i, word, pc);

    static constexpr IrOp dp_ops[16] = {
        IrOp::AND, IrOp::EOR, IrOp::SUB, IrOp::RSB,
        IrOp::ADD, IrOp::ADC, IrOp::SBC, IrOp::RSC,
        IrOp::TST, IrOp::TEQ, IrOp::CMP, IrOp::CMN,
        IrOp::ORR, IrOp::MOV, IrOp::BIC, IrOp::MVN,
    };
    uint32_t opcode4 = bits(word, 24, 21);
    i.op = dp_ops[opcode4];
    i.set_flags = bit(word, 20);
    i.rn = static_cast<uint8_t>(bits(word, 19, 16));
    i.rd = static_cast<uint8_t>(bits(word, 15, 12));
    i.op2 = decode_op2(word);

    if (i.rd == 15 &&
        (i.op == IrOp::MOV || i.op == IrOp::MVN || i.op == IrOp::ADD ||
         i.op == IrOp::SUB || i.op == IrOp::AND || i.op == IrOp::EOR ||
         i.op == IrOp::ORR || i.op == IrOp::BIC || i.op == IrOp::ADC ||
         i.op == IrOp::SBC || i.op == IrOp::RSB || i.op == IrOp::RSC)) {
        i.is_pc_writing = true;
        i.is_branch = true;
        i.is_indirect = true;
    }
    return i;
}

Instr ArmDecoder::decode_branch(uint32_t word, uint32_t pc) {
    Instr i; zero_instr(i, word, pc);
    bool link = bit(word, 24);
    int32_t imm24 = static_cast<int32_t>(bits(word, 23, 0));
    // Sign-extend 24-bit and shift left by 2.
    if (imm24 & 0x00800000) imm24 |= 0xFF000000;
    int32_t offset = imm24 << 2;
    i.op = link ? IrOp::BL : IrOp::B;
    i.branch_link    = link;
    i.branch_target  = static_cast<uint32_t>(static_cast<int32_t>(pc + 8) + offset);
    i.is_branch      = true;
    i.is_call        = link;
    return i;
}

Instr ArmDecoder::decode_branch_exchange(uint32_t word, uint32_t pc) {
    Instr i; zero_instr(i, word, pc);
    i.op = IrOp::BX;
    i.rm = static_cast<uint8_t>(bits(word, 3, 0));
    i.branch_exchange = true;
    i.is_branch = true;
    i.is_indirect = true;
    if (i.rm == 14) i.is_return = true;  // BX LR
    return i;
}

Instr ArmDecoder::decode_single_data_transfer(uint32_t word, uint32_t pc) {
    Instr i; zero_instr(i, word, pc);
    bool load = bit(word, 20);
    bool byte = bit(word, 22);
    if (load) {
        i.op = byte ? IrOp::LDRB : IrOp::LDR;
    } else {
        i.op = byte ? IrOp::STRB : IrOp::STR;
    }
    i.mem.rn          = static_cast<uint8_t>(bits(word, 19, 16));
    i.rd              = static_cast<uint8_t>(bits(word, 15, 12));
    i.mem.pre_indexed = bit(word, 24);
    i.mem.add         = bit(word, 23);
    i.mem.writeback   = bit(word, 21) || !i.mem.pre_indexed;
    i.mem.byte_access = byte;
    i.mem.by_register = bit(word, 25);
    if (i.mem.by_register) {
        i.mem.reg_offset.rm   = static_cast<uint8_t>(bits(word, 3, 0));
        i.mem.reg_offset.type = static_cast<ShiftType>(bits(word, 6, 5));
        i.mem.reg_offset.by_register = false;
        i.mem.reg_offset.imm_or_rs   = static_cast<uint8_t>(bits(word, 11, 7));
        if (i.mem.reg_offset.type == ShiftType::ROR &&
            i.mem.reg_offset.imm_or_rs == 0) {
            i.mem.reg_offset.type = ShiftType::RRX;
        }
    } else {
        i.mem.imm_offset = bits(word, 11, 0);
    }
    if (load && i.rd == 15) {
        i.is_pc_writing = true;
        i.is_branch = true;
        i.is_indirect = true;
    }
    return i;
}

Instr ArmDecoder::decode_halfword_transfer(uint32_t word, uint32_t pc) {
    Instr i; zero_instr(i, word, pc);
    bool load = bit(word, 20);
    uint32_t sh = bits(word, 6, 5);  // S/H bits
    if (load) {
        switch (sh) {
            case 0b01: i.op = IrOp::LDRH;  break;
            case 0b10: i.op = IrOp::LDRSB; break;
            case 0b11: i.op = IrOp::LDRSH; break;
            default:   return decode_undefined(word, pc);
        }
    } else {
        // L=0 in the extra-load/store space. sh=01 is STRH; sh=10/11 are
        // the ARMv5TE doubleword transfers LDRD/STRD (despite L=0).
        switch (sh) {
            case 0b01: i.op = IrOp::STRH; break;
            case 0b10: i.op = IrOp::LDRD; break;
            case 0b11: i.op = IrOp::STRD; break;
            default:   return decode_undefined(word, pc);
        }
    }
    i.mem.rn          = static_cast<uint8_t>(bits(word, 19, 16));
    i.rd              = static_cast<uint8_t>(bits(word, 15, 12));
    i.mem.pre_indexed = bit(word, 24);
    i.mem.add         = bit(word, 23);
    i.mem.writeback   = bit(word, 21) || !i.mem.pre_indexed;
    bool imm_offset   = bit(word, 22);
    i.mem.by_register = !imm_offset;
    if (imm_offset) {
        i.mem.imm_offset = (bits(word, 11, 8) << 4) | bits(word, 3, 0);
    } else {
        i.mem.reg_offset.rm = static_cast<uint8_t>(bits(word, 3, 0));
        i.mem.reg_offset.type = ShiftType::LSL;
        i.mem.reg_offset.imm_or_rs = 0;
        i.mem.reg_offset.by_register = false;
    }
    return i;
}

Instr ArmDecoder::decode_block_data_transfer(uint32_t word, uint32_t pc) {
    Instr i; zero_instr(i, word, pc);
    bool load = bit(word, 20);
    i.op = load ? IrOp::LDM : IrOp::STM;
    i.block.rn          = static_cast<uint8_t>(bits(word, 19, 16));
    i.block.pre_indexed = bit(word, 24);
    i.block.add         = bit(word, 23);
    i.block.s_bit       = bit(word, 22);
    i.block.writeback   = bit(word, 21);
    i.block.load        = load;
    i.block.reg_list    = static_cast<uint16_t>(bits(word, 15, 0));
    if (load && ((i.block.reg_list & (1u << 15)) ||
                 i.block.reg_list == 0)) {
        i.is_pc_writing = true;
        i.is_branch = true;
        i.is_indirect = true;
        i.is_return = true;  // common epilogue: ldm sp!,{...,pc}
    }
    return i;
}

Instr ArmDecoder::decode_swap(uint32_t word, uint32_t pc) {
    Instr i; zero_instr(i, word, pc);
    i.op  = bit(word, 22) ? IrOp::SWPB : IrOp::SWP;
    i.rn  = static_cast<uint8_t>(bits(word, 19, 16));
    i.rd  = static_cast<uint8_t>(bits(word, 15, 12));
    i.rm  = static_cast<uint8_t>(bits(word, 3, 0));
    return i;
}

Instr ArmDecoder::decode_multiply(uint32_t word, uint32_t pc) {
    Instr i; zero_instr(i, word, pc);
    bool acc = bit(word, 21);
    i.op = acc ? IrOp::MLA : IrOp::MUL;
    i.set_flags = bit(word, 20);
    i.rd = static_cast<uint8_t>(bits(word, 19, 16));
    i.rn = static_cast<uint8_t>(bits(word, 15, 12));  // accumulator
    i.rs = static_cast<uint8_t>(bits(word, 11, 8));
    i.rm = static_cast<uint8_t>(bits(word, 3, 0));
    return i;
}

Instr ArmDecoder::decode_multiply_long(uint32_t word, uint32_t pc) {
    Instr i; zero_instr(i, word, pc);
    bool sign = bit(word, 22);
    bool acc  = bit(word, 21);
    i.op = sign ? (acc ? IrOp::SMLAL : IrOp::SMULL)
                : (acc ? IrOp::UMLAL : IrOp::UMULL);
    i.set_flags = bit(word, 20);
    // RdHi / RdLo / Rs / Rm
    i.rd = static_cast<uint8_t>(bits(word, 19, 16));   // RdHi
    i.rn = static_cast<uint8_t>(bits(word, 15, 12));   // RdLo
    i.rs = static_cast<uint8_t>(bits(word, 11, 8));
    i.rm = static_cast<uint8_t>(bits(word, 3, 0));
    return i;
}

Instr ArmDecoder::decode_psr_transfer(uint32_t word, uint32_t pc) {
    Instr i; zero_instr(i, word, pc);
    bool to_psr = bit(word, 21);   // 0 = MRS (PSR → Rd); 1 = MSR (Rm/imm → PSR)
    bool spsr   = bit(word, 22);   // 0 = CPSR, 1 = SPSR_<mode>
    i.psr.spsr = spsr;
    if (!to_psr) {
        // MRS Rd, <psr>:  cond 0001 0R00 1111 Rd 0000 0000 0000
        i.op = IrOp::MRS;
        i.rd = static_cast<uint8_t>(bits(word, 15, 12));
        i.psr.mask = 0;  // unused for MRS
        return i;
    }
    // MSR <psr>_<fields>, Rm   (reg form)  cond 0001 0R10 mask 1111 0000 0000 Rm
    // MSR <psr>_<fields>, #imm (imm form)  cond 0011 0R10 mask 1111 rot  imm8
    i.op = IrOp::MSR;
    i.psr.mask = static_cast<uint8_t>(bits(word, 19, 16));
    if (bit(word, 25)) {
        // Immediate form. decode_op2 already handles the rot/imm8
        // expansion correctly because the encoding shape matches DP
        // immediate.
        i.op2 = decode_op2(word);
    } else {
        // Register form: Rm in low 4 bits, no shift.
        i.op2.kind = Op2::Kind::Shifted;
        i.op2.shifted.rm = static_cast<uint8_t>(bits(word, 3, 0));
        i.op2.shifted.type = ShiftType::LSL;
        i.op2.shifted.by_register = false;
        i.op2.shifted.imm_or_rs = 0;
    }
    return i;
}

Instr ArmDecoder::decode_software_interrupt(uint32_t word, uint32_t pc) {
    Instr i; zero_instr(i, word, pc);
    i.op = IrOp::SWI;
    i.swi_imm = bits(word, 23, 0);
    return i;
}

Instr ArmDecoder::decode_blx_immediate(uint32_t word, uint32_t pc) {
    // BLX (immediate), ARMv5: 1111 101H imm24. Unconditional; the
    // ARMv4T "never" condition slot is repurposed. Always switches to
    // THUMB. target = (PC+8) + sext(imm24)<<2 + (H<<1); LR = PC+4.
    Instr i; zero_instr(i, word, pc);
    int32_t imm24 = static_cast<int32_t>(bits(word, 23, 0));
    if (imm24 & 0x00800000) imm24 |= 0xFF000000;
    uint32_t h = bit(word, 24) ? 2u : 0u;
    i.op = IrOp::BLX_imm;
    i.cond = Cond::AL;  // unconditional — overrides the NV from the encoding
    i.branch_link = true;
    i.branch_exchange = true;
    i.branch_target =
        static_cast<uint32_t>(static_cast<int32_t>(pc + 8) + (imm24 << 2)) + h;
    i.is_branch = true;
    i.is_call = true;
    i.is_pc_writing = true;
    return i;
}

Instr ArmDecoder::decode_blx_register(uint32_t word, uint32_t pc) {
    // BLX (register), ARMv5: cond 0001 0010 SBO SBO SBO 0011 Rm.
    // Like BX but links (LR = PC+4) and switches mode from Rm bit0.
    Instr i; zero_instr(i, word, pc);
    i.op = IrOp::BLX_reg;
    i.rm = static_cast<uint8_t>(bits(word, 3, 0));
    i.branch_link = true;
    i.branch_exchange = true;
    i.is_branch = true;
    i.is_call = true;
    i.is_indirect = true;
    i.is_pc_writing = true;
    return i;
}

Instr ArmDecoder::decode_clz(uint32_t word, uint32_t pc) {
    // CLZ, ARMv5: cond 0001 0110 SBO Rd SBO 0001 Rm.
    Instr i; zero_instr(i, word, pc);
    i.op = IrOp::CLZ;
    i.rd = static_cast<uint8_t>(bits(word, 15, 12));
    i.rm = static_cast<uint8_t>(bits(word, 3, 0));
    return i;
}

Instr ArmDecoder::decode_saturating(uint32_t word, uint32_t pc) {
    // Saturating add/sub, ARMv5TE: cond 0001 0op0 0 Rn Rd 0000 0101 Rm.
    // Rd = SignedSat(Rm {+,-} [2*]Rn); sets CPSR.Q on saturation.
    Instr i; zero_instr(i, word, pc);
    switch (bits(word, 22, 21)) {
        case 0b00: i.op = IrOp::QADD;  break;
        case 0b01: i.op = IrOp::QSUB;  break;
        case 0b10: i.op = IrOp::QDADD; break;
        case 0b11: i.op = IrOp::QDSUB; break;
    }
    i.rn = static_cast<uint8_t>(bits(word, 19, 16));
    i.rd = static_cast<uint8_t>(bits(word, 15, 12));
    i.rm = static_cast<uint8_t>(bits(word, 3, 0));
    return i;
}

Instr ArmDecoder::decode_signed_multiply(uint32_t word, uint32_t pc) {
    // Signed multiply (ARMv5TE DSP), bit7=1 bit4=0 in the misc space:
    //   bits 22..21 = 00 → SMLA<x><y>:  Rd = (Rm.x * Rs.y) + Rn   (Q)
    //              = 01 → SMLAW<y> (bit5=0): Rd = (Rm * Rs.y)>>16 + Rn (Q)
    //                     SMULW<y> (bit5=1): Rd = (Rm * Rs.y)>>16
    //              = 10 → SMLAL<x><y>: RdHi:RdLo += Rm.x * Rs.y
    //              = 11 → SMUL<x><y>:  Rd = Rm.x * Rs.y
    // x = bit5 (Rm half), y = bit6 (Rs half); 1 = top 16 bits.
    Instr i; zero_instr(i, word, pc);
    i.mul_x_top = bit(word, 5);
    i.mul_y_top = bit(word, 6);
    i.rd = static_cast<uint8_t>(bits(word, 19, 16));  // Rd / RdHi
    i.rn = static_cast<uint8_t>(bits(word, 15, 12));  // Rn (accum) / RdLo
    i.rs = static_cast<uint8_t>(bits(word, 11, 8));
    i.rm = static_cast<uint8_t>(bits(word, 3, 0));
    switch (bits(word, 22, 21)) {
        case 0b00: i.op = IrOp::SMLAxy; break;
        case 0b01: i.op = bit(word, 5) ? IrOp::SMULWy : IrOp::SMLAWy; break;
        case 0b10: i.op = IrOp::SMLALxy; break;
        case 0b11: i.op = IrOp::SMULxy; break;
    }
    return i;
}

Instr ArmDecoder::decode_preload(uint32_t word, uint32_t pc) {
    // PLD (preload hint), ARMv5TE. Architecturally a memory hint with no
    // register/flag side effects — modeled as a no-op. Unconditional.
    Instr i; zero_instr(i, word, pc);
    i.op = IrOp::PLD;
    i.cond = Cond::AL;
    return i;
}

Instr ArmDecoder::decode_coprocessor(uint32_t word, uint32_t pc) {
    // cond 1110 ...  Bit 4 selects the form:
    //   bit4 = 1 → MCR/MRC (coprocessor register transfer)
    //   bit4 = 0 → CDP     (coprocessor data operation)
    Instr i; zero_instr(i, word, pc);
    i.coproc.cp_num = static_cast<uint8_t>(bits(word, 11, 8));
    i.coproc.crn    = static_cast<uint8_t>(bits(word, 19, 16));
    i.coproc.crm    = static_cast<uint8_t>(bits(word, 3, 0));
    i.coproc.op2    = static_cast<uint8_t>(bits(word, 7, 5));
    if (bit(word, 4)) {
        // MCR/MRC: cond 1110 op1(3) L CRn Rd cp_num op2 1 CRm
        bool load = bit(word, 20);  // L: 1 = MRC, 0 = MCR
        i.op           = load ? IrOp::MRC : IrOp::MCR;
        i.coproc.load  = load;
        i.coproc.op1   = static_cast<uint8_t>(bits(word, 23, 21));
        i.rd           = static_cast<uint8_t>(bits(word, 15, 12));  // ARM reg
    } else {
        // CDP: cond 1110 op1(4) CRn CRd cp_num op2 0 CRm
        i.op           = IrOp::CDP;
        i.coproc.load  = false;
        i.coproc.op1   = static_cast<uint8_t>(bits(word, 23, 20));
        i.rd           = static_cast<uint8_t>(bits(word, 15, 12));  // CRd
    }
    return i;
}

Instr ArmDecoder::decode_undefined(uint32_t word, uint32_t pc) {
    Instr i; zero_instr(i, word, pc);
    i.op = IrOp::Undefined;
    i.is_undefined = true;
    return i;
}

Instr ArmDecoder::decode(uint32_t word, uint32_t pc) {
    // ARMv5 repurposes the ARMv4T "never" (NV) condition for a few
    // unconditional instructions. Handle the ones the ARM9 uses; any
    // other NV-space encoding falls through and is nop'd by the cond-NV
    // path in codegen.
    if (cond_from(word) == Cond::NV) {
        // BLX (immediate): 1111 101H imm24
        if (bits(word, 27, 25) == 0b101) {
            return decode_blx_immediate(word, pc);
        }
        // PLD (preload hint): 1111 01Ix x101 Rn 1111 ...  → no-op
        if (bits(word, 27, 26) == 0b01 && bit(word, 24) && bit(word, 22) &&
            !bit(word, 21) && bit(word, 20) && bits(word, 15, 12) == 0b1111) {
            return decode_preload(word, pc);
        }
    }
    // Branch and Exchange: cond 0001 0010 1111 1111 1111 0001 Rn
    if ((word & 0x0FFFFFF0u) == 0x012FFF10u) {
        return decode_branch_exchange(word, pc);
    }
    // BLX (register), ARMv5: cond 0001 0010 1111 1111 1111 0011 Rm
    if ((word & 0x0FFFFFF0u) == 0x012FFF30u) {
        return decode_blx_register(word, pc);
    }
    // CLZ, ARMv5: cond 0001 0110 1111 Rd 1111 0001 Rm
    if ((word & 0x0FFF0FF0u) == 0x016F0F10u) {
        return decode_clz(word, pc);
    }
    // Branch / Branch with link: cond 101L imm24
    if (bits(word, 27, 25) == 0b101) {
        return decode_branch(word, pc);
    }
    // Software interrupt: cond 1111 imm24
    if (bits(word, 27, 24) == 0b1111) {
        return decode_software_interrupt(word, pc);
    }
    // Coprocessor register transfer / data op: cond 1110 ...
    // (MCR/MRC when bit4=1, CDP when bit4=0). On the DS this is the
    // ARM9's CP15 system-control coprocessor. LDC/STC (cond 110x)
    // target no DS coprocessor, so they fall through to Undefined and
    // surface loudly if ever encountered, rather than being decoded.
    if (bits(word, 27, 24) == 0b1110) {
        return decode_coprocessor(word, pc);
    }
    // Block data transfer: cond 100 P U S W L Rn reglist
    if (bits(word, 27, 25) == 0b100) {
        return decode_block_data_transfer(word, pc);
    }
    // Single data transfer: cond 01 I P U B W L Rn Rd offset
    if (bits(word, 27, 26) == 0b01) {
        // ARMv4T reserves "cond 011x xxxx xxxx xxxx xxxx xxx1 xxxx" as
        // the architecturally-undefined instruction (ARM ARM A3.13.2).
        // Single data transfer with register-shifted register offset
        // (I=1) requires bit 4 = 0; bit 4 = 1 makes this the UDF
        // encoding, which must trap.
        if (bit(word, 25) && bit(word, 4)) {
            return decode_undefined(word, pc);
        }
        return decode_single_data_transfer(word, pc);
    }

    // The 0b00xxxxxx space holds data-processing, multiply, multiply-long,
    // halfword transfer, swap, PSR transfer.
    if (bits(word, 27, 26) == 0b00) {
        // Multiply / Multiply-long / SWP / Halfword transfer all live in
        // the 0b000xxxxx region with low bits 1001 (mul/swap) or 1xx1
        // (halfword). Disambiguate before falling through to DP.
        if (!bit(word, 25)) {
            // Register-form region
            uint32_t low8 = bits(word, 7, 4);
            if (low8 == 0b1001) {
                // Multiply / multiply-long / swap share the 1001 marker
                if (bits(word, 27, 23) == 0b00000) {
                    return decode_multiply(word, pc);          // MUL/MLA
                }
                if (bits(word, 27, 23) == 0b00001) {
                    return decode_multiply_long(word, pc);     // [SU]MULL/[SU]MLAL
                }
                if (bits(word, 27, 23) == 0b00010 && bits(word, 21, 20) == 0b00) {
                    return decode_swap(word, pc);              // SWP/SWPB
                }
            }
            // ARMv5TE control/DSP extension space: cond 00010 xx 0 …
            // (bits 27..23 = 00010, S bit clear). Saturating add/sub
            // (low nibble 0101) and the signed multiply family (bit7=1,
            // bit4=0). Must be caught before the PSR/DP fallthrough,
            // which would otherwise silently mis-decode them as DP ops.
            if (bits(word, 27, 23) == 0b00010 && !bit(word, 20)) {
                if (low8 == 0b0101) {
                    return decode_saturating(word, pc);
                }
                if (bit(word, 7) && !bit(word, 4)) {
                    return decode_signed_multiply(word, pc);
                }
            }
            if (bit(word, 7) && bit(word, 4)) {
                // Halfword + signed byte transfer family
                return decode_halfword_transfer(word, pc);
            }
        }
        // PSR transfer: cond 00 I 10 P 0 1111 Rd ...  (S=22 distinguishes
        // CPSR vs SPSR). Detect by the dead "00x10x00" bit pattern across
        // bits 24..20 where the data-processing opcode would be TST/TEQ/
        // CMP/CMN with S=0 — illegal for DP, used by MRS/MSR.
        uint32_t opcode4 = bits(word, 24, 21);
        bool s_bit = bit(word, 20);
        if (!s_bit && (opcode4 == 0b1000 || opcode4 == 0b1010)) {
            return decode_psr_transfer(word, pc);
        }
        if (!s_bit && (opcode4 == 0b1001 || opcode4 == 0b1011)) {
            return decode_psr_transfer(word, pc);
        }
        return decode_data_processing(word, pc);
    }

    return decode_undefined(word, pc);
}

}  // namespace armv4t
