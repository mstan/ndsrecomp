// cpu_state.h — ARM7TDMI register file.
//
// The ARM7TDMI is a single CPU that switches between ARM and THUMB
// instruction sets at runtime. We model that as a single CPUState with
// a CPSR.T bit indicating current mode.
//
// Banked registers:
//   - R0..R7    : never banked
//   - R8..R12   : banked between FIQ and the rest (FIQ has its own copy)
//   - R13 (SP)  : banked per mode (User/SVC/IRQ/FIQ/ABT/UND)
//   - R14 (LR)  : banked per mode
//   - R15 (PC)  : never banked
//   - SPSR      : banked per mode (no User/System SPSR)
//
// CPSR layout (bits 31..0):
//   N Z C V . . . . . . . . . . . . . . . . . . . . I F T M4 M3 M2 M1 M0
//
//   N (31)        : Negative
//   Z (30)        : Zero
//   C (29)        : Carry
//   V (28)        : Overflow
//   Q (27)        : sticky saturation (ARMv5TE only; ARM9)
//   I (7)         : IRQ disable
//   F (6)         : FIQ disable
//   T (5)         : THUMB state (set = THUMB)
//   M4..M0 (4..0) : Mode
//
// Reference: ARM Architecture Reference Manual, ARMv4T, A2.5.
//
// IMPORTANT: this header lives in the portable armv4t/ layer. It must not
// depend on anything from src/gba/ or src/runtime/. GBA-specific scheduler
// hooks live in src/runtime/ and are bound via function pointers, not by
// importing this struct.

#pragma once

#include <cstdint>

namespace armv4t {

enum class Mode : uint8_t {
    User       = 0x10,
    FIQ        = 0x11,
    IRQ        = 0x12,
    Supervisor = 0x13,
    Abort      = 0x17,
    Undefined  = 0x1B,
    System     = 0x1F,
};

// Indices into the banked register tables. The exact layout matches the
// "BankedSlot" enum so generated code can address them without knowing
// the mode encoding.
enum BankedSlot : uint8_t {
    Bank_User = 0,
    Bank_FIQ,
    Bank_IRQ,
    Bank_Supervisor,
    Bank_Abort,
    Bank_Undefined,
    Bank_Count,
};

struct CPSR {
    bool n : 1;
    bool z : 1;
    bool c : 1;
    bool v : 1;
    bool q : 1;  // sticky saturation (ARMv5TE; set by QADD/SMLAxy family)
    bool i : 1;  // IRQ disable
    bool f : 1;  // FIQ disable
    bool t : 1;  // THUMB state
    uint8_t mode : 5;
};

struct CPUState {
    // Active register window. R[0..15] is what currently-executing code
    // sees; the appropriate slots in banked_sp/banked_lr/r8_12_user/r8_12_fiq
    // are mirrored into this when modes change.
    uint32_t R[16];

    CPSR cpsr;

    // Per-mode banked storage. SPSR is only valid in the non-User modes;
    // SPSR for User/System is undefined.
    uint32_t banked_sp[Bank_Count];   // R13 per mode
    uint32_t banked_lr[Bank_Count];   // R14 per mode
    uint32_t banked_spsr[Bank_Count]; // SPSR per mode
    uint32_t r8_12_user[5];           // R8..R12 outside FIQ
    uint32_t r8_12_fiq[5];            // R8..R12 in FIQ

    // Indicators the recompiler/runtime read frequently. CPSR is canonical
    // but these duplicates make hot paths cheap.
    bool thumb;                        // == cpsr.t
};

}  // namespace armv4t
