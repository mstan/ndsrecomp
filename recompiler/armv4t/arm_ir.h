// arm_ir.h — Normalized intermediate representation for ARM and THUMB.
//
// Goal: capture the *semantics* of an ARM7TDMI instruction in a form
// that's identical whether the source was ARM (32-bit) or THUMB
// (16-bit). Codegen and the smoke-test printer both consume this.
//
// We do NOT try to be a generic compiler IR. This is purpose-built for
// translating ARMv4T to C, and structured so an interpreter can run it
// directly during decoder bring-up — that's what the test harness uses
// before the full codegen is online.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "condition_codes.h"

namespace armv4t {

// ─────────────────────────────────────────────────────────────────────
// Operands
// ─────────────────────────────────────────────────────────────────────

enum class ShiftType : uint8_t {
    LSL = 0,  // logical shift left
    LSR = 1,  // logical shift right
    ASR = 2,  // arithmetic shift right
    ROR = 3,  // rotate right
    RRX = 4,  // rotate right with extend (ROR by 0, special)
};

// A shifter operand: register Rm shifted by either an immediate count or
// the low byte of register Rs. The shifter's carry-out feeds CPSR.C when
// the S bit is set on data-processing instructions.
struct ShiftedRegister {
    uint8_t   rm;
    ShiftType type;
    bool      by_register;       // true: count = Rs[7:0]; false: count = imm
    uint8_t   imm_or_rs;         // immediate count (0..31), or Rs index
};

// Operand2 for data-processing instructions: either an immediate (8-bit
// rotated by an even amount), or a shifted register.
struct Op2 {
    enum class Kind : uint8_t { Imm, Shifted };
    Kind kind;
    // Imm form: imm_value already rotated; original encoding is preserved
    // separately so codegen can decide whether to emit the raw form.
    uint32_t imm_value;
    uint32_t imm_carry_out;  // shifter carry result for imm form (0 or 1, or 2 = unchanged)
    ShiftedRegister shifted;
};

// ─────────────────────────────────────────────────────────────────────
// Operations
// ─────────────────────────────────────────────────────────────────────

enum class IrOp : uint16_t {
    Undefined = 0,

    // Data processing (DP) — same op for ARM and THUMB encodings; the
    // decoder normalizes to this set.
    AND, EOR, SUB, RSB, ADD, ADC, SBC, RSC,
    TST, TEQ, CMP, CMN,
    ORR, MOV, BIC, MVN,

    // Branches
    B,       // branch
    BL,      // branch with link (ARM 32-bit form)
    BX,      // branch and exchange (interworking)
    BLX_reg, // branch link exchange via register (ARMv5+; reserved on ARMv4T but trapped)
    BLX_imm, // branch link exchange immediate (ARMv5; ARM→THUMB, unconditional)
    BL_prefix, // THUMB BL upper half:  LR = (PC + 4) + sext(imm11) << 12
    BL_suffix, // THUMB BL lower half:  target = LR + imm11 << 1; LR = (PC+2)|1; PC = target

    // Loads / stores
    LDR, STR, LDRB, STRB, LDRH, STRH, LDRSB, LDRSH,
    LDRD, STRD,  // doubleword (ARMv5TE) — transfer Rd and Rd+1

    // Block transfer
    LDM, STM,

    // Swap
    SWP, SWPB,

    // Multiplies
    MUL, MLA, UMULL, UMLAL, SMULL, SMLAL,

    // ARMv5TE enhancements (ARM9). CLZ; saturating add/sub (Q flag);
    // the signed 16x16 / 32x16 multiply-accumulate family (SMLA<x><y>,
    // SMLAW<y>/SMULW<y>, SMLAL<x><y>, SMUL<x><y>); PLD preload hint.
    CLZ,
    QADD, QSUB, QDADD, QDSUB,
    SMLAxy, SMLAWy, SMULWy, SMLALxy, SMULxy,
    PLD,

    // Coprocessor / supervisor
    SWI,

    // Coprocessor register transfer + data op (ARMv5TE; CP15 on the
    // ARM9). MCR: ARM reg → coprocessor. MRC: coprocessor → ARM reg.
    // CDP: coprocessor-internal op with no ARM-register transfer.
    MCR, MRC, CDP,

    // PSR transfers
    MRS, MSR,
};

const char* ir_op_name(IrOp op) noexcept;

// ── ARM7TDMI per-instruction cycle accounting (single source of truth) ──
//
// Used by the IR interpreter (the timing oracle), the recompiler codegen,
// and the runtime tick helpers — so the recompiled cycle stream advances
// the PPU / audio / timers exactly like the interpreter does, byte-for-byte.
//
// `instr_cycle_base` is the FIXED part of an op's cost: the 1S fetch plus
// any internal (I) cycles, with the branch pipeline-refill folded in. The
// memory-access (N/S) cycles and operand-dependent multiply cycles are NOT
// included here — they depend on the runtime target region / operand and
// are added at execute time via `Bus::access_cycles` and `mul_wait_cycles`.
// Two further fixed surcharges (statically known at codegen) are layered on
// by the caller, matching the interpreter:
//   + 1  when Op2 is a register-shifted operand (extra shifter I-cycle)
//   + 2  when a NON-branch op writes PC (pipeline refill)
uint32_t instr_cycle_base(IrOp op) noexcept;

// ARM7TDMI multiply m-cycle count for the multiplier operand `rs_value`.
// `extra` is the accumulate/long adjustment (0 for MUL; 1 for MLA, UMULL,
// SMULL; 2 for UMLAL, SMLAL). `signed_variant` selects the leading
// 0s-or-1s early-termination rule used by the signed multiplies and the
// short MUL/MLA forms (true), versus the leading-0s-only rule of the
// unsigned long multiplies (false).
uint32_t mul_wait_cycles(uint32_t rs_value, bool signed_variant,
                         uint32_t extra) noexcept;

// Addressing form for LDR/STR family. The 'P', 'U', 'W', 'B' bits in the
// ARM encoding all funnel through these flags. THUMB load/store encodings
// also normalize into the same shape.
struct MemAddress {
    uint8_t  rn;
    bool     pre_indexed;       // P bit
    bool     add;               // U bit (offset is added, not subtracted)
    bool     writeback;         // W bit
    bool     byte_access;       // B bit (for LDR/STR only)
    bool     by_register;       // offset is a register or an immediate
    uint32_t imm_offset;        // when by_register == false
    ShiftedRegister reg_offset; // when by_register == true
};

// PSR transfer operands (MRS / MSR).
//   spsr:   false → CPSR, true → SPSR_<current mode>
//   mask:   4-bit field mask (bits 19..16 of the MSR encoding); each
//           bit enables one PSR byte:
//             bit 0 → control byte (bits 7..0)   I, F, T, mode
//             bit 1 → ext1 byte    (bits 15..8)  reserved on ARMv4T
//             bit 2 → ext2 byte    (bits 23..16) reserved on ARMv4T
//             bit 3 → flags byte   (bits 31..24) N, Z, C, V (and Q on later archs)
//           For MRS, `mask` is unused (whole PSR is read).
struct PsrTransfer {
    uint8_t mask;
    bool    spsr;
};

// Coprocessor register transfer (MCR / MRC) and data operation (CDP).
// On the DS the only coprocessor is the ARM9's CP15 (system control:
// MPU regions, ITCM/DTCM base+size, cache control). The ARM-side
// register for MCR/MRC is carried in Instr::rd; for CDP, Instr::rd
// holds CRd. Field meanings follow the ARM ARM MCR/MRC/CDP encodings:
//   op1 — opcode_1: 3 bits (bits 23..21) for MCR/MRC, 4 bits
//         (bits 23..20) for CDP
//   op2 — opcode_2 (bits 7..5)
//   crn — CRn / first coprocessor register (bits 19..16)
//   crm — CRm / second coprocessor register (bits 3..0)
struct CoprocTransfer {
    uint8_t cp_num;   // coprocessor number (15 = CP15)
    uint8_t op1;
    uint8_t op2;
    uint8_t crn;
    uint8_t crm;
    bool    load;     // MRC (coproc → ARM reg) when true; MCR when false
};

// Block-transfer addressing flags
struct BlockAddr {
    uint8_t rn;
    bool    pre_indexed;     // P
    bool    add;             // U
    bool    s_bit;           // user-bank or SPSR transfer
    bool    writeback;       // W
    bool    load;            // L (LDM if true, STM if false)
    uint16_t reg_list;       // 16-bit register bitmap (R0..R15)
};

// ─────────────────────────────────────────────────────────────────────
// Instruction
// ─────────────────────────────────────────────────────────────────────

struct Instr {
    // Source bookkeeping
    uint32_t pc;            // address of this instruction in guest memory
    bool     thumb;         // source was THUMB
    uint32_t raw;           // raw machine word (32-bit ARM or 16-bit THUMB
                            // zero-extended); for 32-bit THUMB pairs we
                            // would re-encode if/when ARMv5T support comes.

    // Decoded semantics
    Cond     cond;
    IrOp     op;
    bool     set_flags;     // S bit on data-processing, etc.

    // Most instructions use a small set of register slots; this is the
    // union by index, validated against the op.
    uint8_t  rd;
    uint8_t  rn;
    uint8_t  rs;
    uint8_t  rm;

    // Data-processing operand 2.
    Op2      op2;

    // Memory addressing for LDR/STR family.
    MemAddress mem;

    // Block transfer.
    BlockAddr  block;

    // Branch target (absolute guest address). For BX / BLX_reg the target
    // is in rm at runtime; this field carries 0 for those.
    uint32_t branch_target;
    bool     branch_link;       // BL/BLX
    bool     branch_exchange;   // BX/BLX (sets CPSR.T from target bit 0)

    // SWI number (24-bit on ARM, 8-bit on THUMB; we widen to 32).
    uint32_t swi_imm;

    // Half-register selectors for the ARMv5TE signed-multiply family
    // (SMLA/SMLAW/SMLAL/SMUL xy). mul_x_top selects the top (true) or
    // bottom (false) 16 bits of Rm; mul_y_top selects the half of Rs.
    bool mul_x_top;
    bool mul_y_top;

    // PSR transfer operands (used by MRS / MSR).
    PsrTransfer psr;

    // Coprocessor operands (used by MCR / MRC / CDP).
    CoprocTransfer coproc;

    // Classification flags — populated by the decoder, mostly hints
    // for the block discoverer and the codegen.
    bool is_branch;
    bool is_call;
    bool is_return;             // BX LR, LDM with R15 in list, etc.
    bool is_indirect;           // computed control flow
    bool is_pc_writing;         // any instruction that writes PC
    bool is_undefined;
};

// Render an Instr to a single-line text string for diff-tested smoke
// tests. The format is deliberately simple and stable.
std::string format_ir(const Instr& i);

}  // namespace armv4t
