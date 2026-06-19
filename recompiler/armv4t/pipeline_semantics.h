// pipeline_semantics.h — PC-visible pipeline rules.
//
// On ARM7TDMI the visible value of PC depends on the current instruction
// set. Code that *reads* PC sees current_pc + offset_pc, where offset_pc
// is:
//
//   ARM   : +8  (two instructions ahead, 4 bytes each)
//   THUMB : +4  (two instructions ahead, 2 bytes each)
//
// This rule applies to:
//   - PC as an operand of any data-processing instruction
//   - PC-relative loads
//   - Branch target computation
//   - Anywhere R15 appears in a register list (LDM/STM)
//
// The instruction's *own* PC (its address in memory) is what we use for
// fetching, decoding, and computing the instruction window for an
// always-on debug ring.
//
// Reference: ARM ARM ARMv4T A2.4 "Program counter" and A2.4.3 "Reading
// the program counter".

#pragma once

#include <cstdint>

namespace armv4t {

constexpr uint32_t kArmPcOffset   = 8;
constexpr uint32_t kThumbPcOffset = 4;

constexpr uint32_t arm_pc_value(uint32_t instr_pc) noexcept {
    return instr_pc + kArmPcOffset;
}

constexpr uint32_t thumb_pc_value(uint32_t instr_pc) noexcept {
    return instr_pc + kThumbPcOffset;
}

// Instruction sizes
constexpr uint32_t kArmInsnBytes   = 4;
constexpr uint32_t kThumbInsnBytes = 2;

}  // namespace armv4t
