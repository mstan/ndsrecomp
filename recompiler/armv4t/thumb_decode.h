// thumb_decode.h — THUMB (16-bit) instruction decoder.
//
// Decodes one 16-bit THUMB halfword at a given PC into the same Instr
// IR as the ARM decoder. The smoke test feeds both decoders into one
// printer, and golden output should look the same shape regardless of
// source mode (modulo the leading "T" vs "A" marker).
//
// Reference: ARM ARM ARMv4T A4 "The THUMB Instruction Set".

#pragma once

#include <cstdint>

#include "arm_ir.h"

namespace armv4t {

class ThumbDecoder {
public:
    // Decode one THUMB-state halfword. instr_pc is the address of `hw`
    // in guest memory; used for PC-relative computations.
    static Instr decode(uint16_t hw, uint32_t instr_pc);

private:
    // Format dispatchers (named to match the ARM ARM "Format N" labels).
    static Instr fmt1_shift_imm(uint16_t hw, uint32_t pc);
    static Instr fmt2_add_sub(uint16_t hw, uint32_t pc);
    static Instr fmt3_alu_imm(uint16_t hw, uint32_t pc);
    static Instr fmt4_alu_reg(uint16_t hw, uint32_t pc);
    static Instr fmt5_hi_reg(uint16_t hw, uint32_t pc);
    static Instr fmt6_pc_rel_load(uint16_t hw, uint32_t pc);
    static Instr fmt7_ldst_reg(uint16_t hw, uint32_t pc);
    static Instr fmt8_ldst_sext_reg(uint16_t hw, uint32_t pc);
    static Instr fmt9_ldst_imm(uint16_t hw, uint32_t pc);
    static Instr fmt10_ldst_halfword_imm(uint16_t hw, uint32_t pc);
    static Instr fmt11_sp_rel(uint16_t hw, uint32_t pc);
    static Instr fmt12_load_address(uint16_t hw, uint32_t pc);
    static Instr fmt13_add_sub_sp(uint16_t hw, uint32_t pc);
    static Instr fmt14_pushpop(uint16_t hw, uint32_t pc);
    static Instr fmt15_multi_ldst(uint16_t hw, uint32_t pc);
    static Instr fmt16_cond_branch(uint16_t hw, uint32_t pc);
    static Instr fmt17_swi(uint16_t hw, uint32_t pc);
    static Instr fmt18_uncond_branch(uint16_t hw, uint32_t pc);
    static Instr fmt19_long_branch(uint16_t hw, uint32_t pc);
    static Instr undefined(uint16_t hw, uint32_t pc);
};

}  // namespace armv4t
