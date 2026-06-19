#include "arm_ir.h"

#include <cstdio>
#include <cstring>

#include "condition_codes.h"

namespace armv4t {

const char* cond_text(Cond c) noexcept {
    switch (c) {
        case Cond::EQ: return "eq";
        case Cond::NE: return "ne";
        case Cond::CS: return "cs";
        case Cond::CC: return "cc";
        case Cond::MI: return "mi";
        case Cond::PL: return "pl";
        case Cond::VS: return "vs";
        case Cond::VC: return "vc";
        case Cond::HI: return "hi";
        case Cond::LS: return "ls";
        case Cond::GE: return "ge";
        case Cond::LT: return "lt";
        case Cond::GT: return "gt";
        case Cond::LE: return "le";
        case Cond::AL: return "";
        case Cond::NV: return "nv";
    }
    return "??";
}

const char* ir_op_name(IrOp op) noexcept {
    switch (op) {
        case IrOp::Undefined: return "UND";
        case IrOp::AND: return "and";
        case IrOp::EOR: return "eor";
        case IrOp::SUB: return "sub";
        case IrOp::RSB: return "rsb";
        case IrOp::ADD: return "add";
        case IrOp::ADC: return "adc";
        case IrOp::SBC: return "sbc";
        case IrOp::RSC: return "rsc";
        case IrOp::TST: return "tst";
        case IrOp::TEQ: return "teq";
        case IrOp::CMP: return "cmp";
        case IrOp::CMN: return "cmn";
        case IrOp::ORR: return "orr";
        case IrOp::MOV: return "mov";
        case IrOp::BIC: return "bic";
        case IrOp::MVN: return "mvn";
        case IrOp::B:   return "b";
        case IrOp::BL:  return "bl";
        case IrOp::BX:  return "bx";
        case IrOp::BLX_reg: return "blx";
        case IrOp::BLX_imm: return "blx";
        case IrOp::BL_prefix: return "bl.hi";
        case IrOp::BL_suffix: return "bl.lo";
        case IrOp::LDR: return "ldr";
        case IrOp::STR: return "str";
        case IrOp::LDRB: return "ldrb";
        case IrOp::STRB: return "strb";
        case IrOp::LDRH: return "ldrh";
        case IrOp::STRH: return "strh";
        case IrOp::LDRSB: return "ldrsb";
        case IrOp::LDRSH: return "ldrsh";
        case IrOp::LDRD: return "ldrd";
        case IrOp::STRD: return "strd";
        case IrOp::LDM: return "ldm";
        case IrOp::STM: return "stm";
        case IrOp::SWP: return "swp";
        case IrOp::SWPB: return "swpb";
        case IrOp::MUL: return "mul";
        case IrOp::MLA: return "mla";
        case IrOp::UMULL: return "umull";
        case IrOp::UMLAL: return "umlal";
        case IrOp::SMULL: return "smull";
        case IrOp::SMLAL: return "smlal";
        case IrOp::CLZ: return "clz";
        case IrOp::QADD: return "qadd";
        case IrOp::QSUB: return "qsub";
        case IrOp::QDADD: return "qdadd";
        case IrOp::QDSUB: return "qdsub";
        case IrOp::SMLAxy: return "smla";
        case IrOp::SMLAWy: return "smlaw";
        case IrOp::SMULWy: return "smulw";
        case IrOp::SMLALxy: return "smlalxy";
        case IrOp::SMULxy: return "smul";
        case IrOp::PLD: return "pld";
        case IrOp::SWI: return "swi";
        case IrOp::MCR: return "mcr";
        case IrOp::MRC: return "mrc";
        case IrOp::CDP: return "cdp";
        case IrOp::MRS: return "mrs";
        case IrOp::MSR: return "msr";
    }
    return "?";
}

uint32_t instr_cycle_base(IrOp op) noexcept {
    switch (op) {
        case IrOp::AND: case IrOp::EOR: case IrOp::SUB: case IrOp::RSB:
        case IrOp::ADD: case IrOp::ADC: case IrOp::SBC: case IrOp::RSC:
        case IrOp::TST: case IrOp::TEQ: case IrOp::CMP: case IrOp::CMN:
        case IrOp::ORR: case IrOp::MOV: case IrOp::BIC: case IrOp::MVN:
        case IrOp::MRS: case IrOp::MSR:
            return 1;  // 1S

        // Branches: 2S+1N pipeline refill folded in here.
        case IrOp::B: case IrOp::BL: case IrOp::BX: case IrOp::BLX_reg:
        case IrOp::BLX_imm: case IrOp::BL_suffix:
            return 3;  // 2S+1N
        case IrOp::BL_prefix:
            return 1;  // THUMB BL upper half only seeds LR; no PC write

        // Memory ops: fetch + internal/turnaround (per-access N/S cycles
        // are added at execute time via Bus::access_cycles). Matches the
        // interpreter's mGBA-derived model:
        //   LDR/LDM: prefetch + per-access waitstate + trailing I.
        //   STR/STM: prefetch + per-access waitstate (no extra trailing I).
        case IrOp::LDR:   case IrOp::LDRB:
        case IrOp::LDRH:  case IrOp::LDRSB: case IrOp::LDRSH:
            return 2;
        case IrOp::STR:   case IrOp::STRB:  case IrOp::STRH:
            return 1;
        case IrOp::LDM:
            return 2;
        case IrOp::STM:
            return 1;
        case IrOp::LDRD:
            return 2;
        case IrOp::STRD:
            return 1;
        case IrOp::SWP:  case IrOp::SWPB:
            return 3;  // 1S+1I + extra trailing cycle

        // MUL family: prefetch here; execute adds the operand-dependent
        // ARM_WAIT_*MUL cycles via mul_wait_cycles().
        case IrOp::MUL:
        case IrOp::MLA: case IrOp::UMULL: case IrOp::SMULL:
        case IrOp::UMLAL: case IrOp::SMLAL:
            return 1;

        case IrOp::SWI:
            return 3;  // 2S+1N — mode-change overhead is in the SWI entry

        // Coprocessor (ARM9 CP15) register transfer / data op. ARM9
        // timing differs from this ARM7TDMI-derived model and is the
        // subject of the Phase-2 CP15 work; charge the 1S fetch here.
        case IrOp::MCR: case IrOp::MRC: case IrOp::CDP:
            return 1;

        // ARMv5TE (ARM9-only) data-processing-class ops. ARM9 timing is
        // Phase-2 work; charge the 1S fetch baseline. PLD is a hint
        // modeled as a no-op.
        case IrOp::CLZ:
        case IrOp::QADD: case IrOp::QSUB: case IrOp::QDADD: case IrOp::QDSUB:
        case IrOp::SMLAxy: case IrOp::SMLAWy: case IrOp::SMULWy:
        case IrOp::SMLALxy: case IrOp::SMULxy:
        case IrOp::PLD:
            return 1;

        case IrOp::Undefined:
        default:
            return 1;
    }
}

uint32_t mul_wait_cycles(uint32_t rs_value, bool signed_variant,
                         uint32_t extra) noexcept {
    uint32_t wait = extra;
    if (signed_variant) {
        // Early-terminate on leading all-0s OR all-1s (the multiplier is
        // treated as signed).
        if ((rs_value & 0xFFFFFF00u) == 0xFFFFFF00u || !(rs_value & 0xFFFFFF00u)) {
            wait += 1;
        } else if ((rs_value & 0xFFFF0000u) == 0xFFFF0000u || !(rs_value & 0xFFFF0000u)) {
            wait += 2;
        } else if ((rs_value & 0xFF000000u) == 0xFF000000u || !(rs_value & 0xFF000000u)) {
            wait += 3;
        } else {
            wait += 4;
        }
    } else {
        // Unsigned long multiply: early-terminate on leading all-0s only.
        if (!(rs_value & 0xFFFFFF00u)) {
            wait += 1;
        } else if (!(rs_value & 0xFFFF0000u)) {
            wait += 2;
        } else if (!(rs_value & 0xFF000000u)) {
            wait += 3;
        } else {
            wait += 4;
        }
    }
    return wait;
}

namespace {

const char* shift_name(ShiftType t) {
    switch (t) {
        case ShiftType::LSL: return "lsl";
        case ShiftType::LSR: return "lsr";
        case ShiftType::ASR: return "asr";
        case ShiftType::ROR: return "ror";
        case ShiftType::RRX: return "rrx";
    }
    return "?";
}

void append_op2(std::string& out, const Op2& op2) {
    char buf[64];
    if (op2.kind == Op2::Kind::Imm) {
        std::snprintf(buf, sizeof(buf), "#0x%x", op2.imm_value);
        out += buf;
        return;
    }
    std::snprintf(buf, sizeof(buf), "r%u", op2.shifted.rm);
    out += buf;
    if (op2.shifted.type == ShiftType::LSL && !op2.shifted.by_register &&
        op2.shifted.imm_or_rs == 0) {
        return;  // lsl #0 elided
    }
    out += ",";
    out += shift_name(op2.shifted.type);
    if (op2.shifted.type == ShiftType::RRX) {
        return;
    }
    if (op2.shifted.by_register) {
        std::snprintf(buf, sizeof(buf), " r%u", op2.shifted.imm_or_rs);
    } else {
        std::snprintf(buf, sizeof(buf), " #%u", op2.shifted.imm_or_rs);
    }
    out += buf;
}

void append_reglist(std::string& out, uint16_t list) {
    out += "{";
    bool first = true;
    for (uint8_t i = 0; i < 16; ++i) {
        if (!(list & (1u << i))) continue;
        if (!first) out += ",";
        first = false;
        char buf[8];
        std::snprintf(buf, sizeof(buf), "r%u", i);
        out += buf;
    }
    out += "}";
}

}  // namespace

std::string format_ir(const Instr& i) {
    std::string out;
    char buf[96];

    // Header: pc, mode, cond, mnemonic
    std::snprintf(buf, sizeof(buf), "%08x %s %s%s%s ",
                  i.pc,
                  i.thumb ? "T" : "A",
                  ir_op_name(i.op),
                  cond_text(i.cond),
                  i.set_flags ? "s" : "");
    out += buf;

    if (i.is_undefined) {
        std::snprintf(buf, sizeof(buf), "raw=0x%08x", i.raw);
        out += buf;
        return out;
    }

    switch (i.op) {
        case IrOp::B:
        case IrOp::BL:
        case IrOp::BLX_imm:
        case IrOp::BL_prefix:
        case IrOp::BL_suffix:
            std::snprintf(buf, sizeof(buf), "0x%08x", i.branch_target);
            out += buf;
            break;

        case IrOp::BX:
        case IrOp::BLX_reg:
            std::snprintf(buf, sizeof(buf), "r%u", i.rm);
            out += buf;
            break;

        case IrOp::MOV:
        case IrOp::MVN:
            std::snprintf(buf, sizeof(buf), "r%u,", i.rd);
            out += buf;
            append_op2(out, i.op2);
            break;

        case IrOp::CMP:
        case IrOp::CMN:
        case IrOp::TST:
        case IrOp::TEQ:
            std::snprintf(buf, sizeof(buf), "r%u,", i.rn);
            out += buf;
            append_op2(out, i.op2);
            break;

        case IrOp::AND: case IrOp::EOR: case IrOp::SUB: case IrOp::RSB:
        case IrOp::ADD: case IrOp::ADC: case IrOp::SBC: case IrOp::RSC:
        case IrOp::ORR: case IrOp::BIC:
            std::snprintf(buf, sizeof(buf), "r%u,r%u,", i.rd, i.rn);
            out += buf;
            append_op2(out, i.op2);
            break;

        case IrOp::LDR: case IrOp::STR:
        case IrOp::LDRB: case IrOp::STRB:
        case IrOp::LDRH: case IrOp::STRH:
        case IrOp::LDRSB: case IrOp::LDRSH:
        case IrOp::LDRD: case IrOp::STRD:
            std::snprintf(buf, sizeof(buf), "r%u,[r%u", i.rd, i.mem.rn);
            out += buf;
            if (!i.mem.pre_indexed) out += "]";
            if (i.mem.by_register) {
                std::snprintf(buf, sizeof(buf), ",%sr%u",
                              i.mem.add ? "+" : "-",
                              i.mem.reg_offset.rm);
                out += buf;
            } else if (i.mem.imm_offset != 0) {
                std::snprintf(buf, sizeof(buf), ",#%s0x%x",
                              i.mem.add ? "" : "-", i.mem.imm_offset);
                out += buf;
            }
            if (i.mem.pre_indexed) {
                out += "]";
                if (i.mem.writeback) out += "!";
            } else if (i.mem.writeback) {
                // post-indexed always writes back; no explicit '!'
            }
            break;

        case IrOp::LDM: case IrOp::STM:
            std::snprintf(buf, sizeof(buf), "r%u%s%s,",
                          i.block.rn,
                          i.block.writeback ? "!" : "",
                          i.block.s_bit ? "^" : "");
            out += buf;
            append_reglist(out, i.block.reg_list);
            break;

        case IrOp::SWI:
            std::snprintf(buf, sizeof(buf), "#0x%x", i.swi_imm);
            out += buf;
            break;

        case IrOp::MCR:
        case IrOp::MRC:
            std::snprintf(buf, sizeof(buf), "p%u,%u,r%u,c%u,c%u,%u",
                          i.coproc.cp_num, i.coproc.op1, i.rd,
                          i.coproc.crn, i.coproc.crm, i.coproc.op2);
            out += buf;
            break;

        case IrOp::CDP:
            std::snprintf(buf, sizeof(buf), "p%u,%u,c%u,c%u,c%u,%u",
                          i.coproc.cp_num, i.coproc.op1, i.rd,
                          i.coproc.crn, i.coproc.crm, i.coproc.op2);
            out += buf;
            break;

        case IrOp::MUL:
            std::snprintf(buf, sizeof(buf), "r%u,r%u,r%u", i.rd, i.rm, i.rs);
            out += buf;
            break;

        case IrOp::MLA:
            std::snprintf(buf, sizeof(buf), "r%u,r%u,r%u,r%u",
                          i.rd, i.rm, i.rs, i.rn);
            out += buf;
            break;

        case IrOp::CLZ:
            std::snprintf(buf, sizeof(buf), "r%u,r%u", i.rd, i.rm);
            out += buf;
            break;

        case IrOp::QADD: case IrOp::QSUB:
        case IrOp::QDADD: case IrOp::QDSUB:
            std::snprintf(buf, sizeof(buf), "r%u,r%u,r%u", i.rd, i.rm, i.rn);
            out += buf;
            break;

        case IrOp::SMLAxy: case IrOp::SMLAWy: case IrOp::SMLALxy:
            std::snprintf(buf, sizeof(buf), "%s%s r%u,r%u,r%u,r%u",
                          i.mul_x_top ? "t" : "b", i.mul_y_top ? "t" : "b",
                          i.rd, i.rm, i.rs, i.rn);
            out += buf;
            break;

        case IrOp::SMULWy: case IrOp::SMULxy:
            std::snprintf(buf, sizeof(buf), "%s%s r%u,r%u,r%u",
                          i.mul_x_top ? "t" : "b", i.mul_y_top ? "t" : "b",
                          i.rd, i.rm, i.rs);
            out += buf;
            break;

        case IrOp::PLD:
            // hint, no operands rendered
            break;

        case IrOp::MRS: {
            std::snprintf(buf, sizeof(buf), "r%u,%s",
                          i.rd, i.psr.spsr ? "spsr" : "cpsr");
            out += buf;
            break;
        }
        case IrOp::MSR: {
            // <psr>_<mask> where mask letters mirror the assembler
            // convention: c=control, x=ext1, s=ext2, f=flags.
            char mask[5]{};
            int k = 0;
            if (i.psr.mask & 0x1) mask[k++] = 'c';
            if (i.psr.mask & 0x2) mask[k++] = 'x';
            if (i.psr.mask & 0x4) mask[k++] = 's';
            if (i.psr.mask & 0x8) mask[k++] = 'f';
            mask[k] = '\0';
            std::snprintf(buf, sizeof(buf), "%s_%s,",
                          i.psr.spsr ? "spsr" : "cpsr", mask);
            out += buf;
            // Source: either immediate or register (we re-encode as
            // Op2 in the decoder).
            if (i.op2.kind == Op2::Kind::Imm) {
                std::snprintf(buf, sizeof(buf), "#0x%x", i.op2.imm_value);
            } else {
                std::snprintf(buf, sizeof(buf), "r%u", i.op2.shifted.rm);
            }
            out += buf;
            break;
        }

        default:
            std::snprintf(buf, sizeof(buf), "raw=0x%08x", i.raw);
            out += buf;
            break;
    }
    return out;
}

}  // namespace armv4t
