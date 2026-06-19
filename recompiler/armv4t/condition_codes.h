// condition_codes.h — ARMv4T condition codes.
//
// CPSR holds the N, Z, C, V flags in bits 31..28. Every ARM instruction
// carries a 4-bit condition field in bits 31..28 of the instruction word
// that gates execution against these flags. THUMB conditional branches
// (Bcc) use the same encoding in their cond field.
//
// Reference: ARM Architecture Reference Manual, ARMv4T, table A3-1.

#pragma once

#include <cstdint>

namespace armv4t {

enum class Cond : uint8_t {
    EQ = 0x0,  // Z set                  : equal
    NE = 0x1,  // Z clear                : not equal
    CS = 0x2,  // C set                  : carry set / unsigned ≥ (alias HS)
    CC = 0x3,  // C clear                : carry clear / unsigned < (alias LO)
    MI = 0x4,  // N set                  : minus / negative
    PL = 0x5,  // N clear                : plus / positive or zero
    VS = 0x6,  // V set                  : overflow
    VC = 0x7,  // V clear                : no overflow
    HI = 0x8,  // C set and Z clear      : unsigned >
    LS = 0x9,  // C clear or Z set       : unsigned ≤
    GE = 0xA,  // N == V                 : signed ≥
    LT = 0xB,  // N != V                 : signed <
    GT = 0xC,  // Z clear and N == V     : signed >
    LE = 0xD,  // Z set or N != V        : signed ≤
    AL = 0xE,  // always
    NV = 0xF,  // (never; reserved on ARMv4T — undefined / used for
               //  unconditional space on ARMv5+)
};

// Stable short text, useful for printing IR. Empty string for AL.
const char* cond_text(Cond c) noexcept;

}  // namespace armv4t
