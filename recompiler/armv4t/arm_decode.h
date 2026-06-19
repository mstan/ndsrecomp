// arm_decode.h — ARM (32-bit) instruction decoder.
//
// Decodes one 32-bit ARM instruction word at a given PC into our
// normalized IR. The decoder is reference-implementation style:
// readable, with explicit shape checks, not a packed table.
// Performance optimization comes later.
//
// Coverage target: every ARMv4T ARM-state encoding the GBA actually
// uses. Encodings reserved on ARMv4T (notably ARMv5T BLX-imm and the
// always-NV space) produce `IrOp::Undefined` with `is_undefined=true`.
//
// Reference: ARM ARM ARMv4T A3 "The ARM Instruction Set".

#pragma once

#include <cstdint>

#include "arm_ir.h"

namespace armv4t {

class ArmDecoder {
public:
    // Decode one ARM-state instruction. instr_pc is the address of `word`
    // in guest memory; it's used for branch-target computation.
    static Instr decode(uint32_t word, uint32_t instr_pc);

private:
    static Instr decode_data_processing(uint32_t word, uint32_t pc);
    static Instr decode_psr_transfer(uint32_t word, uint32_t pc);
    static Instr decode_branch(uint32_t word, uint32_t pc);
    static Instr decode_branch_exchange(uint32_t word, uint32_t pc);
    static Instr decode_blx_immediate(uint32_t word, uint32_t pc);
    static Instr decode_blx_register(uint32_t word, uint32_t pc);
    static Instr decode_clz(uint32_t word, uint32_t pc);
    static Instr decode_saturating(uint32_t word, uint32_t pc);
    static Instr decode_signed_multiply(uint32_t word, uint32_t pc);
    static Instr decode_preload(uint32_t word, uint32_t pc);
    static Instr decode_single_data_transfer(uint32_t word, uint32_t pc);
    static Instr decode_halfword_transfer(uint32_t word, uint32_t pc);
    static Instr decode_block_data_transfer(uint32_t word, uint32_t pc);
    static Instr decode_swap(uint32_t word, uint32_t pc);
    static Instr decode_multiply(uint32_t word, uint32_t pc);
    static Instr decode_multiply_long(uint32_t word, uint32_t pc);
    static Instr decode_software_interrupt(uint32_t word, uint32_t pc);
    static Instr decode_coprocessor(uint32_t word, uint32_t pc);
    static Instr decode_undefined(uint32_t word, uint32_t pc);

    // Helpers
    static Op2 decode_op2(uint32_t word);
};

}  // namespace armv4t
