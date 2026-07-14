// arm_codegen.cpp — see arm_codegen.h.

#include "arm_codegen.h"

#include <cstdio>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace armv4t {

namespace {

// ─────────────────────────────────────────────────────────────────────
// Tiny string helpers
// ─────────────────────────────────────────────────────────────────────

std::string fmt_hex32(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08Xu", v);
    return std::string(buf);
}

std::string label_for_addr(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "L_%08X", v);
    return std::string(buf);
}

uint64_t function_key(uint32_t addr, bool thumb) {
    return (static_cast<uint64_t>(addr) << 1u) | (thumb ? 1u : 0u);
}

// Read a guest register in operand position. R15 returns the
// PC-pipeline value as a literal (ARM: pc+8, THUMB: pc+4). Other
// registers read g_cpu.R[r].
std::string read_reg_expr(uint8_t r, const Instr& ins) {
    if (r == 15) {
        uint32_t pc_val = ins.pc + (ins.thumb ? 4u : 8u);
        return fmt_hex32(pc_val);
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "g_cpu.R[%u]", static_cast<unsigned>(r));
    return std::string(buf);
}

uint32_t stm_pc_store_value(const Instr& ins) {
    if (!ins.thumb) return ins.pc + 12u;
    return ((ins.pc + 4u) & ~2u) + 4u;
}

// True when this op writes PC and is NOT a branch — i.e. it incurs the
// +2 pipeline-refill surcharge in the interpreter's cycle model (branch
// refill is already folded into instr_cycle_base). Statically known
// from the destination register / reg-list, so the codegen folds it into
// the instruction's fixed cost. Mirrors interpreter.cpp's `wrote_pc &&
// !branch_op` test.
bool writes_pc_nonbranch(const Instr& ins) {
    switch (ins.op) {
        case IrOp::AND: case IrOp::EOR: case IrOp::SUB: case IrOp::RSB:
        case IrOp::ADD: case IrOp::ADC: case IrOp::SBC: case IrOp::RSC:
        case IrOp::ORR: case IrOp::MOV: case IrOp::BIC: case IrOp::MVN:
            // TST/TEQ/CMP/CMN write no destination, so they are excluded.
            return ins.rd == 15u;
        case IrOp::LDR:  case IrOp::LDRB: case IrOp::LDRH:
        case IrOp::LDRSB: case IrOp::LDRSH:
            return ins.rd == 15u;
        case IrOp::LDM:
            // Empty-list LDM loads PC at the 0x40-stride address; a
            // non-empty list writes PC only when R15 is in the list.
            // (STM never writes PC — it is a separate IrOp handled by
            // the default case below.)
            return ins.block.reg_list == 0u ||
                   (ins.block.reg_list & (1u << 15)) != 0u;
        default:
            return false;
    }
}

// ARM9 combine HAS_DATA (arm9_cycle_combine): true for every op that makes a
// data-bus (non-code) access — selects melonDS's AddCycles_CD/CDI vs C/CI.
bool arm9_has_data(IrOp op) {
    switch (op) {
        case IrOp::LDR:  case IrOp::LDRB: case IrOp::LDRH:
        case IrOp::LDRSB: case IrOp::LDRSH:
        case IrOp::STR:  case IrOp::STRB: case IrOp::STRH:
        case IrOp::LDM:  case IrOp::STM:
        case IrOp::LDRD: case IrOp::STRD:
        case IrOp::SWP:  case IrOp::SWPB:
            return true;
        default:
            return false;
    }
}

// ARM7 AddCycles_CDI (loads/read-modify-write) has one internal cycle and
// slightly different main-RAM overlap from AddCycles_CD (stores).
bool arm7_is_load(IrOp op) {
    switch (op) {
        case IrOp::LDR:  case IrOp::LDRB: case IrOp::LDRH:
        case IrOp::LDRSB: case IrOp::LDRSH:
        case IrOp::LDM: case IrOp::LDRD:
        case IrOp::SWP: case IrOp::SWPB:
            return true;
        default:
            return false;
    }
}

// Static ARM9 internal-cycle count (numI) for the combine — melonDS
// ARMInterpreter*.cpp: +1 for a register-specified shift, a FLAT 1 (S=0) or
// 3 (S=1) for the classic MUL family (NOT operand-dependent; the ARM7-only
// mul_wait_cycles path is untouched), +2 for MCR, +3 for MRC. Everything else
// (including the ARMv5
// signed xy-multiply family,
// which really is single-cycle on ARM9) is 0.
uint32_t arm9_internal_cost(const Instr& ins) {
    uint32_t n = 0u;
    if (ins.op2.kind == Op2::Kind::Shifted && ins.op2.shifted.by_register)
        n += 1u;
    switch (ins.op) {
        case IrOp::MUL: case IrOp::MLA:
        case IrOp::UMULL: case IrOp::UMLAL:
        case IrOp::SMULL: case IrOp::SMLAL:
            n += ins.set_flags ? 3u : 1u;
            break;
        case IrOp::MCR: n += 2u; break;
        case IrOp::MRC: n += 3u; break;
        default: break;
    }
    return n;
}

// ─────────────────────────────────────────────────────────────────────
// Condition wrapping
// ─────────────────────────────────────────────────────────────────────

bool cond_always(Cond c) { return c == Cond::AL; }
bool cond_never(Cond c)  { return c == Cond::NV; }

// Emit the opening of a condition guard. For AL emit nothing; for
// NV emit a do-nothing comment (the body is unreachable). For
// other conditions emit `if (arm_cond_passes(<N>)) {`.
void emit_cond_open(std::ostringstream& os, Cond c) {
    if (cond_always(c)) return;
    if (cond_never(c)) {
        os << "    /* cond NV — never executes */\n";
        return;
    }
    os << "    if (arm_cond_passes(0x" << std::hex
       << static_cast<unsigned>(c) << std::dec << "u)) {\n";
}

void emit_cond_close(std::ostringstream& os, Cond c) {
    if (cond_always(c) || cond_never(c)) return;
    os << "    }\n";
}

// Indentation under a condition wrapper. Use 8 spaces when inside
// the `if` block, 4 when outside.
const char* indent_for(Cond c) {
    return (cond_always(c) || cond_never(c)) ? "    " : "        ";
}

// ─────────────────────────────────────────────────────────────────────
// Op2 evaluation
// ─────────────────────────────────────────────────────────────────────
//
// Op2Code carries:
//   - `setup`: C statements that declare and compute the operand
//   - `value_expr`: a C expression (variable or literal) for op2.value
//   - `carry_expr`: a C expression for the shifter carry-out (0/1)
//                   — `cpsr_c()` if the encoding leaves carry unchanged.
//
// For S=1 logical ops the caller chains carry_expr into
// arm_set_nzc_logic. For arithmetic ops the shifter carry is
// irrelevant; carry comes from the arithmetic itself.

struct Op2Code {
    std::string setup;
    std::string value_expr;
    std::string carry_expr;
};

// Per-instruction "unique name" suffix so multiple decodes in one
// function body don't shadow. We use the PC; codegen guarantees a
// single instruction at a given PC.
std::string uniq_suffix(const Instr& ins) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "_%08X", ins.pc);
    return std::string(buf);
}

// The per-instruction cycle accumulator variable name. Declared in
// emit_instr; referenced by the body emitters (which add memory/multiply
// cost) and by the early-exit tick sites so each control-flow path ticks
// the instruction's cost exactly once.
std::string cyc_var_for(const Instr& ins) {
    return "_cyc" + uniq_suffix(ins);
}

// ARM9 cycle-combine accumulators (arm9_cycle_combine — see runtime_arm.h).
// _code holds this instruction's OWN numC (runtime_code_cycles(pc)),
// computed once, unconditionally (valid even on a runtime cond-fail, where
// melonDS still charges AddCycles_C() = numC for the skipped fetch). _data
// sums every runtime_mem_cycles() call this instruction makes (mirrors
// _cyc's own memory-cost additions, at the same 4 call sites); _int is the
// static internal-cycle count (arm9_internal_cost, below), assigned once
// inside the cond guard (0 on cond-fail, matching melonDS's class-C-only
// skipped-instruction cost). All three are declared in emit_instr alongside
// _cyc and are dead code on the ARM7 path (never referenced there). BL /
// BLX_reg / BLX_imm / BL_suffix zero _code (with _data/_int) right where
// they already zero _cyc, so the fall-through epilogue tick reached after
// the callee's C-return — which re-evaluates this same ternary — combines
// to 0 there too, instead of re-adding this instruction's own numC a
// second time (see arm9_tick_expr's doc comment below).
std::string code_var_for(const Instr& ins) {
    return "_code" + uniq_suffix(ins);
}
std::string data_var_for(const Instr& ins) {
    return "_data" + uniq_suffix(ins);
}
std::string int_var_for(const Instr& ins) {
    return "_int" + uniq_suffix(ins);
}

// Emit the ARM9-vs-ARM7 `runtime_tick` ternary argument. The ARM7 branch is
// ALWAYS `cyc_var` verbatim — the original flat expression, unchanged. The
// ARM9 branch calls arm9_cycle_combine with this instruction's OWN numC
// (`code_expr`), the runtime _data/_int accumulators, and HAS_DATA;
// `refill_target_expr`, when non-empty, is a C expression for a taken-
// branch/PC-write TARGET pc and adds melonDS's pipeline-refill term via
// arm9_refill_cycles(target) on top of the combine. `code_expr` is normally
// the `_code_<pc>`
// variable (see code_var_for's doc comment above for why a variable, not
// an inline runtime_code_cycles(pc) call, is required for correctness).
std::string arm9_tick_expr_ex(const std::string& code_expr,
                              const std::string& cyc_var,
                              const std::string& data_var,
                              const std::string& int_var, bool has_data,
                              bool arm7_load,
                              const std::string& refill_target_expr) {
    std::ostringstream s;
    s << "g_nds_active == NDS_ARM9 ? arm9_cycle_combine(" << code_expr << ", "
      << data_var << ", " << int_var << ", " << (has_data ? "1u" : "0u")
      << ")";
    if (!refill_target_expr.empty())
        s << " + arm9_refill_cycles(" << refill_target_expr << ")";
    s << " : arm7_cycle_combine(" << cyc_var << ", " << data_var << ", "
      << (arm7_load ? "1u" : "0u") << ", (" << int_var << " != 0u))";
    // The legacy ARM7 flat ALU cost already includes its fixed +2 PC-write
    // refill (writes_pc_nonbranch -> exec_cost += 2), so appending another one
    // would double-charge MOV/ADD/etc-to-PC. Loads are different:
    // arm7_cycle_combine reconstructs C/D/I from numC/numD and intentionally
    // discards the flat cost, so LDR/LDM-to-PC must append the real target
    // refill here just like ARM9. Direct B/BL/BX use branch_tick_expr.
    if (!refill_target_expr.empty() && has_data)
        s << " + arm7_refill_cycles(" << refill_target_expr << ")";
    return s.str();
}
std::string arm9_tick_expr(const Instr& ins, bool has_data,
                           const std::string& refill_target_expr = "") {
    return arm9_tick_expr_ex(code_var_for(ins), cyc_var_for(ins),
                             data_var_for(ins), int_var_for(ins), has_data,
                             arm7_is_load(ins.op),
                             refill_target_expr);
}

// Dedicated B/BL/BX/BLX timing. Both melonDS cores' branch handlers call
// JumpTo directly: a taken branch pays the target refill, not an additional
// class cost for its own prefetched instruction.
std::string branch_tick_expr(const std::string& refill_target_expr) {
    return "g_nds_active == NDS_ARM9 ? arm9_refill_cycles(" +
           refill_target_expr + ") : arm7_refill_cycles(" +
           refill_target_expr + ")";
}

Op2Code emit_op2(const Instr& ins, const char* indent) {
    Op2Code out;
    if (ins.op2.kind == Op2::Kind::Imm) {
        // Immediate. The decoder already pre-rotated imm_value;
        // imm_carry_out is 0/1, or 2 meaning "carry unchanged".
        out.value_expr = fmt_hex32(ins.op2.imm_value);
        if (ins.op2.imm_carry_out == 2u) {
            out.carry_expr = "cpsr_c()";
        } else {
            out.carry_expr = (ins.op2.imm_carry_out & 1u) ? "1u" : "0u";
        }
        return out;
    }

    // Shifted register. We always declare _rm and _op2/_co locals
    // even when the shift is a no-op, to keep the emit predictable.
    const auto& sr = ins.op2.shifted;
    std::string sfx = uniq_suffix(ins);
    std::string rm_var = "_rm" + sfx;
    std::string op2_var = "_op2" + sfx;
    std::string co_var = "_co" + sfx;
    out.value_expr = op2_var;
    out.carry_expr = co_var;

    std::ostringstream s;
    s << indent << "uint32_t " << rm_var << " = ";
    if (sr.by_register && sr.rm == 15 && !ins.thumb) {
        s << fmt_hex32(ins.pc + 12u);
    } else {
        s << read_reg_expr(sr.rm, ins);
    }
    s << ";\n";
    s << indent << "uint32_t " << op2_var << ";\n";
    s << indent << "uint32_t " << co_var << ";\n";

    if (sr.by_register) {
        // Count comes from the low byte of Rs at runtime. ARM ARM
        // register-shift rule: when Rs == R15, the count uses
        // read_reg(R15) + 4 (i.e., pc + 12 ARM). The decoder routes
        // through read_reg_expr which already returns pc+8; we add
        // the extra +4 here only when Rs==15.
        std::string count_var = "_cnt" + sfx;
        s << indent << "uint32_t " << count_var << " = (";
        if (sr.imm_or_rs == 15) {
            uint32_t base = ins.pc + (ins.thumb ? 4u : 8u);
            s << fmt_hex32(base + 4u);
        } else {
            s << "g_cpu.R[" << static_cast<unsigned>(sr.imm_or_rs) << "]";
        }
        s << ") & 0xFFu;\n";

        // Reg-form shifts at runtime: dispatch by count.
        switch (sr.type) {
            case ShiftType::LSL:
                s << indent << "if (" << count_var << " == 0)      { "
                  << op2_var << " = " << rm_var << "; "
                  << co_var << " = cpsr_c(); }\n";
                s << indent << "else if (" << count_var << " < 32) { "
                  << op2_var << " = " << rm_var << " << " << count_var << "; "
                  << co_var << " = (" << rm_var << " >> (32u - " << count_var << ")) & 1u; }\n";
                s << indent << "else if (" << count_var << " == 32){ "
                  << op2_var << " = 0u; "
                  << co_var << " = " << rm_var << " & 1u; }\n";
                s << indent << "else                                { "
                  << op2_var << " = 0u; " << co_var << " = 0u; }\n";
                break;
            case ShiftType::LSR:
                s << indent << "if (" << count_var << " == 0)      { "
                  << op2_var << " = " << rm_var << "; "
                  << co_var << " = cpsr_c(); }\n";
                s << indent << "else if (" << count_var << " < 32) { "
                  << op2_var << " = " << rm_var << " >> " << count_var << "; "
                  << co_var << " = (" << rm_var << " >> (" << count_var << " - 1u)) & 1u; }\n";
                s << indent << "else if (" << count_var << " == 32){ "
                  << op2_var << " = 0u; "
                  << co_var << " = (" << rm_var << " >> 31) & 1u; }\n";
                s << indent << "else                                { "
                  << op2_var << " = 0u; " << co_var << " = 0u; }\n";
                break;
            case ShiftType::ASR:
                s << indent << "if (" << count_var << " == 0)      { "
                  << op2_var << " = " << rm_var << "; "
                  << co_var << " = cpsr_c(); }\n";
                s << indent << "else if (" << count_var << " < 32) { "
                  << op2_var << " = (uint32_t)((int32_t)" << rm_var << " >> " << count_var << "); "
                  << co_var << " = (" << rm_var << " >> (" << count_var << " - 1u)) & 1u; }\n";
                s << indent << "else                                { "
                  << op2_var << " = (" << rm_var << " & 0x80000000u) ? 0xFFFFFFFFu : 0u; "
                  << co_var << " = (" << rm_var << " >> 31) & 1u; }\n";
                break;
            case ShiftType::ROR:
                s << indent << "if (" << count_var << " == 0)      { "
                  << op2_var << " = " << rm_var << "; "
                  << co_var << " = cpsr_c(); }\n";
                s << indent << "else { uint32_t _n = " << count_var << " & 31u; "
                  << "if (_n == 0) { " << op2_var << " = " << rm_var << "; "
                  << co_var << " = (" << rm_var << " >> 31) & 1u; } "
                  << "else { " << op2_var << " = (" << rm_var << " >> _n) | (" << rm_var << " << (32u - _n)); "
                  << co_var << " = (" << op2_var << " >> 31) & 1u; } }\n";
                break;
            case ShiftType::RRX:
                // RRX from a register-form shift shouldn't occur
                // — only imm-form ROR with count==0 produces RRX.
                // Emit a defensive abort.
                s << indent << "runtime_unimplemented_op(\"reg-shift RRX\", 0x"
                  << std::hex << ins.pc << std::dec << "u);\n";
                s << indent << op2_var << " = 0u; " << co_var << " = 0u;\n";
                break;
        }
    } else {
        // Immediate count. Statically dispatch on type + count.
        unsigned n = sr.imm_or_rs;
        switch (sr.type) {
            case ShiftType::LSL:
                if (n == 0) {
                    s << indent << op2_var << " = " << rm_var << ";\n";
                    s << indent << co_var << " = cpsr_c();\n";
                } else {
                    s << indent << op2_var << " = " << rm_var << " << " << n << ";\n";
                    s << indent << co_var << " = (" << rm_var << " >> " << (32 - n) << ") & 1u;\n";
                }
                break;
            case ShiftType::LSR:
                if (n == 0) {  // LSR #0 → LSR #32
                    s << indent << op2_var << " = 0u;\n";
                    s << indent << co_var << " = (" << rm_var << " >> 31) & 1u;\n";
                } else {
                    s << indent << op2_var << " = " << rm_var << " >> " << n << ";\n";
                    s << indent << co_var << " = (" << rm_var << " >> " << (n - 1) << ") & 1u;\n";
                }
                break;
            case ShiftType::ASR:
                if (n == 0) {  // ASR #0 → ASR #32
                    s << indent << op2_var << " = (" << rm_var << " & 0x80000000u) ? 0xFFFFFFFFu : 0u;\n";
                    s << indent << co_var << " = (" << rm_var << " >> 31) & 1u;\n";
                } else {
                    s << indent << op2_var << " = (uint32_t)((int32_t)" << rm_var << " >> " << n << ");\n";
                    s << indent << co_var << " = (" << rm_var << " >> " << (n - 1) << ") & 1u;\n";
                }
                break;
            case ShiftType::ROR:
                if (n == 0) {  // decoded as RRX, but guard anyway
                    s << indent << op2_var << " = (" << rm_var << " >> 1) | (cpsr_c() << 31);\n";
                    s << indent << co_var << " = " << rm_var << " & 1u;\n";
                } else {
                    s << indent << op2_var << " = (" << rm_var << " >> " << n
                      << ") | (" << rm_var << " << " << (32 - n) << ");\n";
                    s << indent << co_var << " = (" << op2_var << " >> 31) & 1u;\n";
                }
                break;
            case ShiftType::RRX:
                s << indent << op2_var << " = (" << rm_var << " >> 1) | (cpsr_c() << 31);\n";
                s << indent << co_var << " = " << rm_var << " & 1u;\n";
                break;
        }
    }

    out.setup = s.str();
    return out;
}

// Mode-privileged check matching is_priv_non_system in the interp.
const char* mode_is_priv_non_system_expr() {
    return "((g_cpu.cpsr & 0x1Fu) != 0x10u && (g_cpu.cpsr & 0x1Fu) != 0x1Fu)";
}

// ─────────────────────────────────────────────────────────────────────
// Branch helper
// ─────────────────────────────────────────────────────────────────────

// Emit a direct branch to `target`. If the target is a known
// recompiled function in the dispatch table, emit a direct call;
// otherwise route through runtime_dispatch. In both cases also
// update g_cpu.R[15] so any caller that reads PC after the branch
// (e.g. via stale stack values) sees the right value.
std::string emit_direct_branch(uint32_t target, uint32_t branch_pc,
                                bool is_link,
                                uint32_t link_value, bool thumb_link,
                                const CodegenCtx& ctx,
                                const char* indent) {
    std::ostringstream s;
    // The cycle/code/data/internal accumulators declared by emit_instr for
    // this branch's PC (emit_direct_branch only has the raw pc, not the
    // Instr, so rebuild the names the same way cyc_var_for/code_var_for/
    // data_var_for/int_var_for do).
    char cycbuf[24];
    std::snprintf(cycbuf, sizeof(cycbuf), "_cyc_%08X", branch_pc);
    const std::string cyc(cycbuf);
    char codebuf[24];
    std::snprintf(codebuf, sizeof(codebuf), "_code_%08X", branch_pc);
    const std::string codev(codebuf);
    char databuf[24];
    std::snprintf(databuf, sizeof(databuf), "_data_%08X", branch_pc);
    const std::string datav(databuf);
    char intbuf[24];
    std::snprintf(intbuf, sizeof(intbuf), "_int_%08X", branch_pc);
    const std::string intv(intbuf);
    // B/BL always have a compile-time-known target (no interworking), so the
    // ARM9 pipeline-refill term uses the static target literal directly.
    const std::string refill_target = fmt_hex32(target | (thumb_link ? 1u : 0u));
    // A link call that returns with any other PC was really a tail
    // transfer through the callee, so do not execute link fallthrough.
    if (is_link) {
        // LR = next_pc (with THUMB bit if the caller is THUMB).
        uint32_t lr = thumb_link ? (link_value | 1u) : link_value;
        s << indent << "g_cpu.R[14] = " << fmt_hex32(lr) << ";\n";
    }
    // Set CPSR.T based on whether the recompiled target body is
    // THUMB-emitted. We don't know that here; the dispatch
    // function's own preamble keeps cpsr.T set by the caller. For
    // ARM->ARM direct branches, T stays clear. For ARM->THUMB
    // direct branches (which don't exist on ARMv4T — only BX
    // switches modes), this would need updating.
    s << indent << "g_cpu.R[15] = " << fmt_hex32(target) << ";\n";

    if (!is_link &&
        target >= ctx.current_function_addr &&
        target < ctx.current_function_end_addr &&
        target < branch_pc) {
        s << indent << "runtime_trace_event(RUNTIME_TRACE_BRANCH, "
          << fmt_hex32(branch_pc) << ", " << fmt_hex32(target)
          << ", 0u, 0u);\n";
        s << indent << "runtime_tick("
          << branch_tick_expr(refill_target)
          << ");\n";
        s << indent << "if (runtime_unwinding()) return;\n";
        // Preemption point: a backward branch (loop top) is a dispatch
        // entry, so it is a SAFE place to yield to the scheduler — the
        // resume re-dispatches `target` cleanly. This is how a tight
        // guest spin (e.g. an IPCSYNC wait) is broken to let the other
        // core run, without ever resuming mid-function.
        s << indent << "if (runtime_slice_yield()) { g_cpu.R[15] = "
          << fmt_hex32(target) << "; return; }\n";
        s << indent << "goto " << label_for_addr(target) << ";\n";
        return s.str();
    }

    if (is_link && target == link_value) {
        // `bl` to the next instruction is a get-PC idiom. Guest
        // execution just continues with LR set; a host C call would
        // execute the suffix once as a callee and then fall through
        // to it again. It is still a taken branch architecturally, so charge
        // the refill here and clear the accumulators before the ordinary
        // fall-through epilogue.
        s << indent << "runtime_tick("
          << branch_tick_expr(refill_target)
          << ");\n";
        s << indent << "if (runtime_unwinding()) return;\n";
        s << indent << cyc << " = 0u; " << codev << " = 0u; " << datav
          << " = 0u; " << intv << " = 0u;\n";
        return s.str();
    }
    if (is_link) {
        s << indent << "runtime_call_push_return("
          << fmt_hex32(link_value & ~1u) << ");\n";
    }

    // Branch executes here: pump its fixed cost before transferring, so
    // the PPU/IRQ boundary lands between this branch and the target —
    // matching the interpreter (pump branch cost, then check IRQ at the
    // next boundary). For a link call (BL / BL_suffix) zero _cyc after,
    // so the fall-through epilogue tick (reached when the callee C-
    // returns) does not double-count.
    s << indent << "runtime_tick("
      << branch_tick_expr(refill_target)
      << ");\n";
    s << indent << "if (runtime_unwinding()) return;\n";
    if (is_link) {
        // Zero ALL THREE combine accumulators (not just _cyc) so the
        // fall-through epilogue tick reached after the callee's C-return
        // (this is a BL: control resumes here, see the is_link comment
        // below) evaluates arm9_cycle_combine(0, 0, 0, 0) = 0 for ARM9 too,
        // instead of re-deriving a fresh nonzero numC(own pc) — see
        // code_var_for's doc comment.
        s << indent << cyc << " = 0u; " << codev << " = 0u; " << datav
          << " = 0u; " << intv << " = 0u;\n";
    }

    const std::string* name = nullptr;
    if (ctx.names_by_key) {
        auto it = ctx.names_by_key->find(
            function_key(target, ctx.current_function_thumb));
        if (it != ctx.names_by_key->end()) name = &it->second;
    }
    if (name) {
        if (!is_link && target == ctx.current_function_addr) {
            // A tight `b .` loop must not become recursive host C.
            // Return to the dispatch loop with PC unchanged so the
            // runtime can observe/stall the guest loop normally.
        } else {
            s << indent << *name << "();\n";
        }
    } else {
        if (!is_link && target == ctx.current_function_addr) {
            // See known-name self-loop case above.
        } else {
            s << indent << "runtime_dispatch(" << fmt_hex32(target) << ");\n";
        }
    }
    // B is a tail-call: never return to this caller, so emit
    // `return;`. BL is a call: after the callee returns (its `bx lr`
    // sets PC=LR, then C-returns), control must resume in this
    // function's body at the next instruction. So DO NOT emit
    // `return;` for BL — let C control fall through to the next
    // decoded instruction.
    if (is_link) {
        // Slice preemption unwinds the host stack without a real guest
        // return — preserve the pushed return rather than cancelling it.
        s << indent << "if (runtime_unwinding()) return;\n";
        s << indent << "if (g_cpu.R[15] != " << fmt_hex32(link_value & ~1u)
          << ") { runtime_call_cancel_return("
          << fmt_hex32(link_value & ~1u) << "); return; }\n";
    } else {
        s << indent << "return;\n";
    }
    return s.str();
}

// ─────────────────────────────────────────────────────────────────────
// Memory access helpers
// ─────────────────────────────────────────────────────────────────────

// Emit the offset evaluation for an LDR/STR-family addressing mode.
// `offset_var` is the C identifier that should hold the offset.
void emit_mem_offset(std::ostringstream& s, const MemAddress& mem,
                     const Instr& ins, const std::string& offset_var,
                     const char* indent) {
    s << indent << "uint32_t " << offset_var << ";\n";
    if (!mem.by_register) {
        s << indent << offset_var << " = " << fmt_hex32(mem.imm_offset) << ";\n";
        return;
    }
    // Shifted-register offset. Address-mode shifts always use the
    // imm-count form (post/pre-indexed reg-form uses imm count).
    const auto& sr = mem.reg_offset;
    std::string rm_var = "_morm" + uniq_suffix(ins);
    s << indent << "uint32_t " << rm_var << " = "
      << read_reg_expr(sr.rm, ins) << ";\n";
    unsigned n = sr.imm_or_rs;
    switch (sr.type) {
        case ShiftType::LSL:
            if (n == 0) s << indent << offset_var << " = " << rm_var << ";\n";
            else s << indent << offset_var << " = " << rm_var << " << " << n << ";\n";
            break;
        case ShiftType::LSR:
            if (n == 0) s << indent << offset_var << " = 0u;\n";  // LSR #32
            else        s << indent << offset_var << " = " << rm_var << " >> " << n << ";\n";
            break;
        case ShiftType::ASR:
            if (n == 0)
                s << indent << offset_var
                  << " = (" << rm_var << " & 0x80000000u) ? 0xFFFFFFFFu : 0u;\n";
            else
                s << indent << offset_var
                  << " = (uint32_t)((int32_t)" << rm_var << " >> " << n << ");\n";
            break;
        case ShiftType::ROR:
            if (n == 0)
                s << indent << offset_var
                  << " = (" << rm_var << " >> 1) | (cpsr_c() << 31);\n";  // RRX
            else
                s << indent << offset_var
                  << " = (" << rm_var << " >> " << n << ") | ("
                  << rm_var << " << " << (32 - n) << ");\n";
            break;
        case ShiftType::RRX:
            s << indent << offset_var
              << " = (" << rm_var << " >> 1) | (cpsr_c() << 31);\n";
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────
// Per-op emitters
// ─────────────────────────────────────────────────────────────────────

bool emit_data_processing(std::ostringstream& body, const Instr& ins,
                          const char* indent) {
    Op2Code o2 = emit_op2(ins, indent);
    if (!o2.setup.empty()) body << o2.setup;

    bool is_test = (ins.op == IrOp::TST || ins.op == IrOp::TEQ ||
                    ins.op == IrOp::CMP || ins.op == IrOp::CMN);
    bool is_logical = (ins.op == IrOp::AND || ins.op == IrOp::EOR ||
                       ins.op == IrOp::ORR || ins.op == IrOp::BIC ||
                       ins.op == IrOp::MOV || ins.op == IrOp::MVN ||
                       ins.op == IrOp::TST || ins.op == IrOp::TEQ);

    std::string sfx = uniq_suffix(ins);
    std::string r_var = "_r" + sfx;
    std::string rn_var = "_rn" + sfx;

    // Rn is used by all ops except MOV/MVN.
    bool needs_rn = !(ins.op == IrOp::MOV || ins.op == IrOp::MVN);
    if (needs_rn) {
        // THUMB ADD/CMP Rd, PC, #imm aligns PC to 4-byte boundary
        // (also applies for the other THUMB DP forms that use PC).
        bool pc_align = (ins.thumb && ins.rn == 15 &&
                          ins.op2.kind == Op2::Kind::Imm);
        body << indent << "uint32_t " << rn_var << " = ";
        if (!ins.thumb && ins.rn == 15 &&
            ins.op2.kind == Op2::Kind::Shifted &&
            ins.op2.shifted.by_register) {
            body << fmt_hex32(ins.pc + 12u);
        } else {
            body << read_reg_expr(ins.rn, ins);
        }
        if (pc_align) body << " & ~3u";
        body << ";\n";
    }

    // Compute the result.
    body << indent << "uint32_t " << r_var << ";\n";
    switch (ins.op) {
        case IrOp::AND: case IrOp::TST:
            body << indent << r_var << " = " << rn_var << " & "
                 << o2.value_expr << ";\n";
            break;
        case IrOp::EOR: case IrOp::TEQ:
            body << indent << r_var << " = " << rn_var << " ^ "
                 << o2.value_expr << ";\n";
            break;
        case IrOp::ORR:
            body << indent << r_var << " = " << rn_var << " | "
                 << o2.value_expr << ";\n";
            break;
        case IrOp::BIC:
            body << indent << r_var << " = " << rn_var << " & ~("
                 << o2.value_expr << ");\n";
            break;
        case IrOp::MOV:
            body << indent << r_var << " = " << o2.value_expr << ";\n";
            break;
        case IrOp::MVN:
            body << indent << r_var << " = ~(" << o2.value_expr << ");\n";
            break;
        case IrOp::ADD: case IrOp::CMN:
            body << indent << r_var << " = " << rn_var << " + "
                 << o2.value_expr << ";\n";
            break;
        case IrOp::SUB: case IrOp::CMP:
            body << indent << r_var << " = " << rn_var << " - "
                 << o2.value_expr << ";\n";
            break;
        case IrOp::RSB:
            body << indent << r_var << " = " << o2.value_expr
                 << " - " << rn_var << ";\n";
            break;
        case IrOp::ADC:
            body << indent << r_var << " = " << rn_var << " + "
                 << o2.value_expr << " + cpsr_c();\n";
            break;
        case IrOp::SBC:
            body << indent << r_var << " = " << rn_var << " - "
                 << o2.value_expr << " - (1u - cpsr_c());\n";
            break;
        case IrOp::RSC:
            body << indent << r_var << " = " << o2.value_expr
                 << " - " << rn_var << " - (1u - cpsr_c());\n";
            break;
        default:
            return false;
    }

    // Set flags. Exception return: Rd=R15 + S=1 in priv non-system
    // mode skips flag-set and routes through runtime_exception_return.
    bool excpt_return = ins.set_flags && (ins.rd == 15) && !is_test;
    bool spsr_restore_test = ins.set_flags && (ins.rd == 15) && is_test;
    if (ins.set_flags) {
        if (excpt_return) {
            // Exception return: don't touch flags here; restore from SPSR.
            const std::string return_target =
                "(" + r_var + " | ((g_cpu.cpsr & CPSR_T_BIT) ? 1u : 0u))";
            const std::string exception_tick =
                "g_nds_active == NDS_ARM9 ? arm9_cycle_combine(" +
                code_var_for(ins) + ", " + data_var_for(ins) + ", " +
                int_var_for(ins) + ", 0u) + arm9_refill_cycles(" +
                return_target + ") : (1u + " + code_var_for(ins) +
                " + arm7_refill_cycles(" + return_target + "))";
            body << indent << "if (" << mode_is_priv_non_system_expr() << ") {\n"
                 << indent << "    runtime_exception_return(" << r_var << ");\n"
                 << indent << "    runtime_tick(" << exception_tick << ");\n"
                 << indent << "    return;\n"
                 << indent << "}\n";
        }
        if (is_logical) {
            // For S=1 logical: N/Z from result, C from shifter carry.
            body << indent << "arm_set_nzc_logic(" << r_var << ", "
                 << o2.carry_expr << ");\n";
        } else {
            // Arithmetic.
            switch (ins.op) {
                case IrOp::ADD: case IrOp::CMN:
                    body << indent << "arm_set_nzcv_add(" << rn_var << ", "
                         << o2.value_expr << ", " << r_var << ");\n";
                    break;
                case IrOp::ADC:
                    body << indent << "arm_set_nzcv_adc(" << rn_var << ", "
                         << o2.value_expr << ", cpsr_c(), " << r_var << ");\n";
                    break;
                case IrOp::SUB: case IrOp::CMP:
                    body << indent << "arm_set_nzcv_sub(" << rn_var << ", "
                         << o2.value_expr << ", " << r_var << ");\n";
                    break;
                case IrOp::SBC:
                    body << indent << "arm_set_nzcv_sbc(" << rn_var << ", "
                         << o2.value_expr << ", cpsr_c(), " << r_var << ");\n";
                    break;
                case IrOp::RSB:
                    body << indent << "arm_set_nzcv_sub(" << o2.value_expr
                         << ", " << rn_var << ", " << r_var << ");\n";
                    break;
                case IrOp::RSC:
                    body << indent << "arm_set_nzcv_sbc(" << o2.value_expr
                         << ", " << rn_var << ", cpsr_c(), " << r_var << ");\n";
                    break;
                default: break;
            }
        }
        if (spsr_restore_test) {
            body << indent << "if (" << mode_is_priv_non_system_expr() << ") {\n"
                 << indent << "    runtime_restore_cpsr_from_spsr();\n"
                 << indent << "}\n";
        }
    }

    // Writeback (non-test).
    if (!is_test) {
        if (ins.rd == 15) {
            // PC writeback: not an exception return (already handled
            // above). Detect the `mov pc, lr` return idiom (DP-MOV
            // with Op2 = plain R14, no shift) and emit a C-return
            // — that's the AAPCS return for THUMB-callable code and
            // for older ARM ABIs that used MOV instead of BX. Any
            // other PC write is a computed jump → dispatch.
            const bool is_lr_return =
                (ins.op == IrOp::MOV &&
                 ins.op2.kind == Op2::Kind::Shifted &&
                 ins.op2.shifted.rm == 14 &&
                 !ins.op2.shifted.by_register &&
                 ins.op2.shifted.imm_or_rs == 0);
            std::string pc_var = "_pc" + uniq_suffix(ins);
            body << indent << "uint32_t " << pc_var << " = " << r_var
                 << (ins.thumb ? " & ~1u" : " & ~3u") << ";\n";
            body << indent << "g_cpu.R[15] = " << pc_var << ";\n";
            // PC write completes the instruction; tick its cost (incl.
            // the +2 refill folded into _cyc for ARM7) before transferring.
            // ARM9: DP-to-PC never interworks (CPSR.T is unchanged), so
            // pc_var is already the correct-mode target for the refill.
            body << indent << "runtime_tick("
                 << arm9_tick_expr(ins, false,
                                   "(" + pc_var + (ins.thumb ? " | 1u)" : ")"))
                 << ");\n";
            body << indent << "if (runtime_unwinding()) return;\n";
            if (is_lr_return) {
                body << indent << "if (runtime_call_should_return("
                     << pc_var << ")) return;\n";
                body << indent << "runtime_dispatch(" << pc_var << ");\n";
                body << indent << "return;\n";
            } else {
                body << indent << "runtime_dispatch(" << pc_var << ");\n";
                body << indent << "return;\n";
            }
        } else {
            body << indent << "g_cpu.R[" << static_cast<unsigned>(ins.rd)
                 << "] = " << r_var << ";\n";
        }
    }
    return true;
}

bool emit_branch(std::ostringstream& body, const Instr& ins,
                 const CodegenCtx& ctx, const char* indent) {
    switch (ins.op) {
        case IrOp::B:
            body << emit_direct_branch(ins.branch_target, ins.pc, false, 0,
                                       ins.thumb, ctx, indent);
            return true;
        case IrOp::BL: {
            uint32_t link = ins.pc + (ins.thumb ? 2u : 4u);
            body << emit_direct_branch(ins.branch_target, ins.pc, true, link,
                                       ins.thumb, ctx, indent);
            return true;
        }
        case IrOp::BX: {
            std::string sfx = uniq_suffix(ins);
            std::string target_var = "_bxt" + sfx;
            body << indent << "uint32_t " << target_var << " = "
                 << read_reg_expr(ins.rm, ins) << ";\n";
            body << indent << "g_cpu.R[15] = " << target_var << " & ~1u;\n";
            body << indent << "if (" << target_var
                 << " & 1u) g_cpu.cpsr |= CPSR_T_BIT; else g_cpu.cpsr &= ~CPSR_T_BIT;\n";
            // BX always transfers; tick its cost before either the C-
            // return or the dispatch path (both exit the function). CPSR.T
            // for the ARM9 refill's target-numC is set AFTER this tick (by
            // interworking, below) — target_var&~1u is the correct-mode
            // target address regardless, but the combine's internal Thumb-
            // odd-half check may see the stale (pre-exchange) CPSR.T; see
            // the report's "hard site" note.
            body << indent << "runtime_tick("
                 << branch_tick_expr(target_var)
                 << ");\n";
            body << indent << "if (runtime_unwinding()) return;\n";
            if (ins.rm == 14 || ctx.force_bx_c_return) {
                // `bx lr` — the AAPCS function-return idiom. In the
                // direct-C-call dispatch model, BL emits a real C
                // call to the target; the target's `bx lr` is the
                // matching C return. No dispatch, but still perform
                // BX interworking before unwinding one host frame.
                //
                // Non-return BX (computed jump, trampoline, BX to a
                // non-caller via BL/BLX from a different source)
                // is handled by the dispatch path below.
                body << indent << "if (runtime_call_should_return(g_cpu.R[15])) return;\n";
                body << indent << "runtime_dispatch_with_exchange("
                     << target_var << ");\n";
                body << indent << "return;\n";
            } else {
                body << indent << "runtime_dispatch_with_exchange("
                     << target_var << ");\n";
                body << indent << "return;\n";
            }
            return true;
        }
        case IrOp::BLX_reg: {
            // BLX register (ARMv5): like BX, but links and the host C
            // stack models the call — control resumes here after the
            // callee's `bx lr`. Mode switches from the target's bit0.
            std::string sfx = uniq_suffix(ins);
            std::string target_var = "_blxt" + sfx;
            // ARM caller: LR=PC+4. THUMB register-form BLX: LR=(PC+2)|1.
            uint32_t link = ins.pc + (ins.thumb ? 2u : 4u);
            if (ins.thumb) link |= 1u;
            uint32_t return_pc = link & ~1u;
            body << indent << "uint32_t " << target_var << " = "
                 << read_reg_expr(ins.rm, ins) << ";\n";
            body << indent << "g_cpu.R[14] = " << fmt_hex32(link) << ";\n";
            body << indent << "g_cpu.R[15] = " << target_var << " & ~1u;\n";
            body << indent << "runtime_call_push_return("
                 << fmt_hex32(return_pc) << ");\n";
            body << indent << "if (" << target_var
                 << " & 1u) g_cpu.cpsr |= CPSR_T_BIT; else g_cpu.cpsr &= ~CPSR_T_BIT;\n";
            // CPSR.T staleness caveat: same as BX above (interworking
            // happens inside runtime_dispatch_with_exchange, after this
            // tick).
            body << indent << "runtime_tick("
                 << branch_tick_expr(target_var)
                 << ");\n";
            body << indent << "if (runtime_unwinding()) return;\n";
            // Zero ALL combine accumulators (not just _cyc): control resumes
            // here after the callee's C-return (BLX_reg is a CALL), and the
            // fall-through epilogue tick reached then must combine to 0 for
            // ARM9 too, not re-derive a fresh nonzero numC — see
            // code_var_for's doc comment.
            body << indent << cyc_var_for(ins) << " = 0u; " << code_var_for(ins)
                 << " = 0u; " << data_var_for(ins) << " = 0u; "
                 << int_var_for(ins) << " = 0u;\n";
            body << indent << "runtime_dispatch_with_exchange("
                 << target_var << ");\n";
            body << indent << "if (runtime_unwinding()) return;\n";
            body << indent << "if (g_cpu.R[15] != " << fmt_hex32(return_pc)
                 << ") { runtime_call_cancel_return(" << fmt_hex32(return_pc)
                 << "); return; }\n";
            return true;
        }
        case IrOp::BLX_imm: {
            // BLX immediate (ARMv5): unconditional ARM→THUMB call. The
            // decoder folded the H bit into branch_target; force the
            // THUMB bit on the dispatch so the exchange selects THUMB.
            uint32_t link = ins.pc + 4u;
            uint32_t tgt = ins.branch_target | 1u;
            body << indent << "g_cpu.R[14] = " << fmt_hex32(link) << ";\n";
            body << indent << "g_cpu.R[15] = "
                 << fmt_hex32(ins.branch_target) << ";\n";
            body << indent << "runtime_call_push_return("
                 << fmt_hex32(link) << ");\n";
            body << indent << "g_cpu.cpsr |= CPSR_T_BIT;\n";
            // BLX_imm's target is a compile-time constant, so the ARM9
            // refill can use the static literal directly. CPSR.T staleness
            // caveat: same as BX/BLX_reg (interworking happens inside
            // runtime_dispatch_with_exchange, after this tick).
            body << indent << "runtime_tick("
                 << branch_tick_expr(fmt_hex32(tgt))
                 << ");\n";
            body << indent << "if (runtime_unwinding()) return;\n";
            // Zero ALL combine accumulators — see the BLX_reg comment above.
            body << indent << cyc_var_for(ins) << " = 0u; " << code_var_for(ins)
                 << " = 0u; " << data_var_for(ins) << " = 0u; "
                 << int_var_for(ins) << " = 0u;\n";
            body << indent << "runtime_dispatch_with_exchange("
                 << fmt_hex32(tgt) << ");\n";
            body << indent << "if (runtime_unwinding()) return;\n";
            body << indent << "if (g_cpu.R[15] != " << fmt_hex32(link)
                 << ") { runtime_call_cancel_return(" << fmt_hex32(link)
                 << "); return; }\n";
            return true;
        }
        case IrOp::BL_prefix:
            // Upper half of THUMB BL pair: stash partial target into LR.
            body << indent << "g_cpu.R[14] = "
                 << fmt_hex32(ins.branch_target) << ";\n";
            return true;
        case IrOp::BL_suffix: {
            // Lower half: target = LR + (imm11 << 1).
            // New LR = (PC_lower + 2) | 1 so BX LR resumes in THUMB.
            // Like ARM BL, this is a CALL — control resumes in this
            // function after the callee's `bx lr` returns. No
            // trailing `return;` (see emit_direct_branch comment).
            //
            // branch_exchange = THUMB BLX (11101): the target is ARM,
            // word-aligned, and the dispatch switches to ARM state.
            const bool blx = ins.branch_exchange;
            std::string sfx = uniq_suffix(ins);
            std::string target_var = "_blt" + sfx;
            body << indent << "uint32_t " << target_var << " = "
                 << "(g_cpu.R[14] + " << fmt_hex32(ins.swi_imm) << ") & "
                 << (blx ? "~3u" : "~1u") << ";\n";
            uint32_t new_lr = (ins.pc + 2u) | 1u;
            body << indent << "g_cpu.R[14] = " << fmt_hex32(new_lr) << ";\n";
            body << indent << "g_cpu.R[15] = " << target_var << ";\n";
            body << indent << "runtime_call_push_return("
                 << fmt_hex32(new_lr & ~1u) << ");\n";
            if (blx)
                body << indent << "g_cpu.cpsr &= ~CPSR_T_BIT;\n";
            // Pump the call's cost before transferring; zero ALL combine
            // accumulators so the fall-through epilogue tick (after the
            // callee C-returns) does not double-count — for ARM7 that's just
            // _cyc; for ARM9 it's _cyc/_code/_data/_int together, so the
            // epilogue's combine call there evaluates to 0 too (see
            // code_var_for's doc comment). target_var is already masked to
            // the correct-mode address (~3u if this BL_suffix is a THUMB BLX
            // to ARM, ~1u if it stays THUMB); the blx (ARM-switch) case has
            // the same CPSR.T-staleness caveat as BX/BLX_reg/BLX_imm above
            // (interworking happens inside runtime_dispatch_with_exchange,
            // after this tick) — the plain-BL (stays THUMB) case has none.
            body << indent << "runtime_tick("
                 << branch_tick_expr(
                                   blx ? target_var : "(" + target_var + " | 1u)")
                 << ");\n";
            body << indent << "if (runtime_unwinding()) return;\n";
            body << indent << cyc_var_for(ins) << " = 0u; " << code_var_for(ins)
                 << " = 0u; " << data_var_for(ins) << " = 0u; "
                 << int_var_for(ins) << " = 0u;\n";
            // BLX switches to ARM (target is even → clears CPSR.T); plain
            // BL stays THUMB.
            body << indent
                 << (blx ? "runtime_dispatch_with_exchange(" : "runtime_dispatch(")
                 << target_var << ");\n";
            body << indent << "if (runtime_unwinding()) return;\n";
            body << indent << "if (g_cpu.R[15] != "
                 << fmt_hex32(new_lr & ~1u)
                 << ") { runtime_call_cancel_return("
                 << fmt_hex32(new_lr & ~1u) << "); return; }\n";
            return true;
        }
        default:
            return false;
    }
}

bool emit_memory(std::ostringstream& body, const Instr& ins,
                 const char* indent) {
    const auto& mem = ins.mem;
    std::string sfx = uniq_suffix(ins);
    std::string base_var = "_base" + sfx;
    std::string off_var = "_off" + sfx;
    std::string ea_var = "_ea" + sfx;
    std::string post_var = "_post" + sfx;

    // Base register read. THUMB PC-rel LDR aligns PC to 4 bytes.
    bool pc_align = (ins.thumb && mem.rn == 15);
    body << indent << "uint32_t " << base_var << " = "
         << read_reg_expr(mem.rn, ins);
    if (pc_align) body << " & ~3u";
    body << ";\n";

    emit_mem_offset(body, mem, ins, off_var, indent);

    body << indent << "uint32_t " << ea_var << " = "
         << (mem.pre_indexed
                 ? (mem.add ? (base_var + " + " + off_var)
                            : (base_var + " - " + off_var))
                 : base_var)
         << ";\n";
    body << indent << "uint32_t " << post_var << " = "
         << (mem.add ? (base_var + " + " + off_var)
                     : (base_var + " - " + off_var))
         << ";\n";

    // Region-aware data-access cost (single N access at the effective
    // address; width follows the op, like the interpreter's mem_cycles).
    unsigned access_w = 4u;
    switch (ins.op) {
        case IrOp::LDRB: case IrOp::STRB: case IrOp::LDRSB: access_w = 1u; break;
        case IrOp::LDRH: case IrOp::STRH: case IrOp::LDRSH: access_w = 2u; break;
        default: access_w = 4u; break;
    }
    body << indent << "{ uint32_t _m = runtime_mem_cycles(" << ea_var << ", "
         << access_w << "u, 0u); " << cyc_var_for(ins) << " += _m; "
         << data_var_for(ins) << " += _m; }\n";

    bool is_load = (ins.op == IrOp::LDR || ins.op == IrOp::LDRB ||
                    ins.op == IrOp::LDRH || ins.op == IrOp::LDRSB ||
                    ins.op == IrOp::LDRSH);

    if (is_load) {
        std::string val_var = "_v" + sfx;
        body << indent << "uint32_t " << val_var << ";\n";
        switch (ins.op) {
            case IrOp::LDR: {
                // ARMv4T misaligned word load rotates.
                body << indent << "{ uint32_t _w = bus_read_u32(" << ea_var
                     << " & ~3u); uint32_t _rot = (" << ea_var << " & 3u) * 8u; "
                     << val_var << " = (_rot == 0u) ? _w : ((_w >> _rot) | (_w << (32u - _rot))); }\n";
                break;
            }
            case IrOp::LDRB:
                body << indent << val_var << " = bus_read_u8(" << ea_var << ");\n";
                break;
            case IrOp::LDRH:
                body << indent << "{ uint32_t _h = bus_read_u16(" << ea_var
                     << " & ~1u); if (" << ea_var << " & 1u) " << val_var
                     << " = ((_h >> 8) | (_h << 24)); else " << val_var
                     << " = _h; }\n";
                break;
            case IrOp::LDRSB:
                body << indent << val_var << " = (uint32_t)(int32_t)(int8_t)bus_read_u8("
                     << ea_var << ");\n";
                break;
            case IrOp::LDRSH:
                body << indent << "if (" << ea_var << " & 1u) " << val_var
                     << " = (uint32_t)(int32_t)(int8_t)bus_read_u8(" << ea_var << ");\n";
                body << indent << "else " << val_var
                     << " = (uint32_t)(int32_t)(int16_t)bus_read_u16(" << ea_var << ");\n";
                break;
            default: return false;
        }

        // Writeback first (unless Rn == Rd — Rd wins on loads).
        if (mem.writeback) {
            body << indent << "if (" << static_cast<unsigned>(mem.rn) << "u != "
                 << static_cast<unsigned>(ins.rd) << "u) g_cpu.R["
                 << static_cast<unsigned>(mem.rn) << "] = "
                 << (mem.pre_indexed ? ea_var : post_var) << ";\n";
        } else if (!mem.pre_indexed) {
            // Post-indexed without explicit W is still writeback.
            body << indent << "if (" << static_cast<unsigned>(mem.rn) << "u != "
                 << static_cast<unsigned>(ins.rd) << "u) g_cpu.R["
                 << static_cast<unsigned>(mem.rn) << "] = " << post_var << ";\n";
        }

        // Store the loaded value into Rd. If Rd == PC, dispatch.
        if (ins.rd == 15) {
            body << indent << "g_cpu.R[15] = " << val_var << " & ~1u;\n";
            body << indent << "if (" << val_var
                 << " & 1u) g_cpu.cpsr |= CPSR_T_BIT; else g_cpu.cpsr &= ~CPSR_T_BIT;\n";
            // CPSR.T is already set to the target mode (just above), so the
            // ARM9 refill's numC(target) sees the correct mode — no
            // staleness caveat here (unlike the BX/BLX family).
            body << indent << "runtime_tick("
                 << arm9_tick_expr(ins, true, val_var)
                 << ");\n";
            body << indent << "if (runtime_unwinding()) return;\n";
            body << indent << "runtime_dispatch_with_exchange(" << val_var << ");\n";
            body << indent << "return;\n";
        } else {
            body << indent << "g_cpu.R[" << static_cast<unsigned>(ins.rd)
                 << "] = " << val_var << ";\n";
        }
    } else {
        // Store form. STR of PC stores pc + 12 on ARMv4T.
        std::string val_expr;
        if (ins.rd == 15) {
            val_expr = fmt_hex32(ins.pc + 12u);
        } else {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "g_cpu.R[%u]",
                          static_cast<unsigned>(ins.rd));
            val_expr = buf;
        }
        switch (ins.op) {
            case IrOp::STR:
                body << indent << "runtime_trace_event(RUNTIME_TRACE_MEM_WRITE, "
                     << fmt_hex32(ins.pc) << ", " << ea_var << " & ~3u, "
                     << val_expr << ", 4u);\n";
                body << indent << "bus_write_u32(" << ea_var << " & ~3u, "
                     << val_expr << ");\n";
                break;
            case IrOp::STRB:
                body << indent << "runtime_trace_event(RUNTIME_TRACE_MEM_WRITE, "
                     << fmt_hex32(ins.pc) << ", " << ea_var << ", (uint32_t)("
                     << val_expr << " & 0xFFu), 1u);\n";
                body << indent << "bus_write_u8(" << ea_var << ", (uint8_t)("
                     << val_expr << " & 0xFFu));\n";
                break;
            case IrOp::STRH:
                body << indent << "runtime_trace_event(RUNTIME_TRACE_MEM_WRITE, "
                     << fmt_hex32(ins.pc) << ", " << ea_var << " & ~1u, (uint32_t)("
                     << val_expr << " & 0xFFFFu), 2u);\n";
                body << indent << "bus_write_u16(" << ea_var << " & ~1u, (uint16_t)("
                     << val_expr << " & 0xFFFFu));\n";
                break;
            default: return false;
        }
        if (mem.writeback || !mem.pre_indexed) {
            body << indent << "g_cpu.R[" << static_cast<unsigned>(mem.rn)
                 << "] = " << (mem.pre_indexed ? ea_var : post_var) << ";\n";
        }
    }
    return true;
}

bool emit_block_transfer(std::ostringstream& body, const Instr& ins,
                          const char* indent) {
    const auto& blk = ins.block;
    if (blk.reg_list == 0) {
        std::string sfx = uniq_suffix(ins);
        std::string base_var = "_b" + sfx;
        std::string addr_var = "_a" + sfx;
        std::string fb_var = "_fb" + sfx;
        body << indent << "uint32_t " << base_var << " = g_cpu.R["
             << static_cast<unsigned>(blk.rn) << "];\n";
        if (blk.add) {
            body << indent << "uint32_t " << addr_var << " = " << base_var
                 << (blk.pre_indexed ? " + 4u" : "") << ";\n";
            body << indent << "uint32_t " << fb_var << " = " << base_var
                 << " + 0x40u;\n";
        } else {
            body << indent << "uint32_t " << addr_var << " = " << base_var
                 << (blk.pre_indexed ? " - 0x40u" : " - 0x3Cu") << ";\n";
            body << indent << "uint32_t " << fb_var << " = " << base_var
                 << " - 0x40u;\n";
        }
        // Empty-list LDM/STM performs a single N access at the
        // 0x40-stride address (interpreter parity).
        body << indent << "{ uint32_t _m = runtime_mem_cycles(" << addr_var
             << " & ~3u, 4u, 0u); " << cyc_var_for(ins) << " += _m; "
             << data_var_for(ins) << " += _m; }\n";
        if (blk.load) {
            std::string pcv = "_pc" + sfx;
            body << indent << "uint32_t " << pcv
                 << " = bus_read_u32(" << addr_var << " & ~3u);\n";
            if (blk.writeback) {
                body << indent << "g_cpu.R[" << static_cast<unsigned>(blk.rn)
                     << "] = " << fb_var << ";\n";
            }
            if (blk.s_bit) {
                body << indent << "if (" << mode_is_priv_non_system_expr()
                     << ") { runtime_exception_return(" << pcv
                     << " & ~1u); runtime_tick("
                     << arm9_tick_expr(ins, true,
                         "((" + pcv + " & ~1u) | ((g_cpu.cpsr & CPSR_T_BIT) ? 1u : 0u))")
                     << "); return; }\n";
            }
            body << indent << "g_cpu.R[15] = " << pcv << " & ~1u;\n";
            body << indent << "if (" << pcv
                 << " & 1u) g_cpu.cpsr |= CPSR_T_BIT; else g_cpu.cpsr &= ~CPSR_T_BIT;\n";
            // CPSR.T is already set to the target mode (just above) — no
            // staleness caveat here.
            body << indent << "runtime_tick("
                 << arm9_tick_expr(ins, true, pcv)
                 << ");\n";
            body << indent << "if (runtime_unwinding()) return;\n";
            body << indent << "runtime_dispatch_with_exchange(" << pcv << ");\n";
            body << indent << "return;\n";
        } else {
            body << indent << "runtime_trace_event(RUNTIME_TRACE_MEM_WRITE, "
                 << fmt_hex32(ins.pc) << ", " << addr_var << " & ~3u, "
                 << fmt_hex32(stm_pc_store_value(ins)) << ", 4u);\n";
            body << indent << "bus_write_u32(" << addr_var << " & ~3u, "
                 << fmt_hex32(stm_pc_store_value(ins)) << ");\n";
            if (blk.writeback) {
                body << indent << "g_cpu.R[" << static_cast<unsigned>(blk.rn)
                     << "] = " << fb_var << ";\n";
            }
        }
        return true;
    }
    int n = 0;
    for (int r = 0; r < 16; ++r) if (blk.reg_list & (1u << r)) ++n;

    std::string sfx = uniq_suffix(ins);
    std::string base_var = "_b" + sfx;
    std::string addr_var = "_a" + sfx;
    std::string fb_var = "_fb" + sfx;
    body << indent << "uint32_t " << base_var << " = g_cpu.R["
         << static_cast<unsigned>(blk.rn) << "];\n";

    // Start address per addressing mode.
    if (blk.add) {
        body << indent << "uint32_t " << addr_var << " = " << base_var
             << (blk.pre_indexed ? " + 4u" : "") << ";\n";
    } else {
        body << indent << "uint32_t " << addr_var << " = " << base_var
             << " - " << (blk.pre_indexed ? 4 * n : 4 * (n - 1)) << "u;\n";
    }
    body << indent << "uint32_t " << fb_var << " = "
         << (blk.add ? (base_var + " + " + std::to_string(4 * n) + "u")
                     : (base_var + " - " + std::to_string(4 * n) + "u"))
         << ";\n";

    bool pc_in_list = (blk.reg_list & (1u << 15)) != 0;

    // First access is non-sequential (N), each subsequent access is
    // sequential (S). Matches the interpreter's per-register cost loop,
    // which matters over slow regions (e.g. EWRAM: 6 N vs 3 S per word).
    bool first_access = true;
    std::string pc_var;

    // Iterate registers in ascending order.
    for (int r = 0; r < 16; ++r) {
        if (!(blk.reg_list & (1u << r))) continue;
        body << indent << "{ uint32_t _m = runtime_mem_cycles(" << addr_var
             << " & ~3u, 4u, " << (first_access ? "0u" : "1u") << "); "
             << cyc_var_for(ins) << " += _m; " << data_var_for(ins)
             << " += _m; }\n";
        first_access = false;
        if (blk.load) {
            if (r == 15) {
                std::string pcv = "_pc" + sfx;
                pc_var = pcv;
                body << indent << "uint32_t " << pcv
                     << " = bus_read_u32(" << addr_var << " & ~3u);\n";
                if (blk.s_bit) {
                    body << indent << "if (" << mode_is_priv_non_system_expr()
                         << ") { runtime_exception_return(" << pcv
                         << " & ~1u); runtime_tick("
                         << arm9_tick_expr(ins, true,
                             "((" + pcv + " & ~1u) | ((g_cpu.cpsr & CPSR_T_BIT) ? 1u : 0u))")
                         << "); return; }\n";
                }
                body << indent << "g_cpu.R[15] = " << pcv << " & ~1u;\n";
                body << indent << "if (" << pcv
                     << " & 1u) g_cpu.cpsr |= CPSR_T_BIT; else g_cpu.cpsr &= ~CPSR_T_BIT;\n";
            } else {
                if (blk.s_bit && !pc_in_list) {
                    body << indent << "runtime_write_user_reg(" << r
                         << "u, bus_read_u32(" << addr_var
                         << " & ~3u));\n";
                } else {
                    body << indent << "g_cpu.R[" << r << "] = bus_read_u32("
                         << addr_var << " & ~3u);\n";
                }
            }
        } else {
            std::string store_val;
            if (r == 15) {
                store_val = fmt_hex32(stm_pc_store_value(ins));
            } else {
                bool store_writeback_base =
                    blk.writeback && r == blk.rn &&
                    (blk.reg_list & ((1u << r) - 1u)) != 0;
                store_val = store_writeback_base
                    ? fb_var
                    : ((blk.s_bit ? "runtime_read_user_reg(" : "g_cpu.R[") +
                       std::to_string(r) + (blk.s_bit ? "u)" : "]"));
            }
            body << indent << "runtime_trace_event(RUNTIME_TRACE_MEM_WRITE, "
                 << fmt_hex32(ins.pc) << ", " << addr_var << " & ~3u, "
                 << store_val << ", 4u);\n";
            body << indent << "bus_write_u32(" << addr_var
                 << " & ~3u, " << store_val << ");\n";
        }
        body << indent << addr_var << " += 4u;\n";
    }

    bool base_in_list = (blk.reg_list & (1u << blk.rn)) != 0;
    if (blk.writeback && !(blk.load && base_in_list)) {
        body << indent << "g_cpu.R[" << static_cast<unsigned>(blk.rn)
             << "] = " << fb_var << ";\n";
    }

    if (blk.load && pc_in_list) {
        // LDM with PC in the list is the AAPCS "function return"
        // idiom when the base register is the stack pointer (R13):
        // a matching prior STMFD/PUSH stored LR onto the stack, so
        // popping into R15 is restoring the return address. Treat
        // this exactly like `bx lr` — C-return so the caller's body
        // continues at the instruction after the call.
        //
        // When the base is NOT SP, the popped PC is a computed
        // jump (state-machine dispatch, jump tables popped from
        // arbitrary memory). Those still need runtime_dispatch.
        // Every path here exits the function, so tick the accumulated
        // cost once before any of them. CPSR.T was already set to the
        // target mode inside the loop's r==15 iteration (r==15 is always
        // the last iteration — ascending order, and pc_in_list guarantees
        // it ran), so no staleness caveat here.
        body << indent << "runtime_tick("
             << arm9_tick_expr(ins, true, pc_var)
             << ");\n";
        body << indent << "if (runtime_unwinding()) return;\n";
        if (blk.rn == 13) {
            body << indent << "if (runtime_call_should_return(g_cpu.R[15])) return;\n";
            body << indent << "runtime_dispatch_with_exchange(" << pc_var << ");\n";
            body << indent << "return;\n";
        } else {
            body << indent << "runtime_dispatch_with_exchange(" << pc_var << ");\n";
            body << indent << "return;\n";
        }
    }
    return true;
}

bool emit_multiply(std::ostringstream& body, const Instr& ins,
                   const char* indent) {
    std::string sfx = uniq_suffix(ins);
    auto rd = static_cast<unsigned>(ins.rd);
    auto rn = static_cast<unsigned>(ins.rn);
    auto rm = static_cast<unsigned>(ins.rm);
    auto rs = static_cast<unsigned>(ins.rs);
    switch (ins.op) {
        case IrOp::MUL: {
            body << indent << cyc_var_for(ins) << " += runtime_mul_cycles(g_cpu.R["
                 << (ins.thumb ? rm : rs) << "], "
                 << (ins.thumb ? "0u" : "1u") << ", 0u);\n";
            std::string rv = "_r" + sfx;
            body << indent << "uint32_t " << rv << " = g_cpu.R[" << rm
                 << "] * g_cpu.R[" << rs << "];\n";
            body << indent << "g_cpu.R[" << rd << "] = " << rv << ";\n";
            if (ins.set_flags) body << indent << "arm_set_nz(" << rv << ");\n";
            return true;
        }
        case IrOp::MLA: {
            body << indent << cyc_var_for(ins)
                 << " += runtime_mul_cycles(g_cpu.R[" << rs << "], 1u, 1u);\n";
            std::string rv = "_r" + sfx;
            body << indent << "uint32_t " << rv << " = g_cpu.R[" << rm
                 << "] * g_cpu.R[" << rs << "] + g_cpu.R[" << rn << "];\n";
            body << indent << "g_cpu.R[" << rd << "] = " << rv << ";\n";
            if (ins.set_flags) body << indent << "arm_set_nz(" << rv << ");\n";
            return true;
        }
        case IrOp::UMULL: {
            body << indent << cyc_var_for(ins)
                 << " += runtime_mul_cycles(g_cpu.R[" << rs << "], 0u, 1u);\n";
            std::string pv = "_p" + sfx;
            body << indent << "uint64_t " << pv << " = (uint64_t)g_cpu.R["
                 << rm << "] * (uint64_t)g_cpu.R[" << rs << "];\n";
            body << indent << "g_cpu.R[" << rn << "] = (uint32_t)(" << pv << " & 0xFFFFFFFFu);\n";
            body << indent << "g_cpu.R[" << rd << "] = (uint32_t)(" << pv << " >> 32);\n";
            if (ins.set_flags) {
                body << indent << "{ uint32_t _c = g_cpu.cpsr & ~(CPSR_N_BIT|CPSR_Z_BIT); "
                     << "if (" << pv << " >> 63) _c |= CPSR_N_BIT; "
                     << "if (" << pv << " == 0) _c |= CPSR_Z_BIT; "
                     << "g_cpu.cpsr = _c; }\n";
            }
            return true;
        }
        case IrOp::UMLAL: {
            body << indent << cyc_var_for(ins)
                 << " += runtime_mul_cycles(g_cpu.R[" << rs << "], 0u, 2u);\n";
            std::string pv = "_p" + sfx;
            std::string av = "_acc" + sfx;
            std::string sv = "_sum" + sfx;
            body << indent << "uint64_t " << pv << " = (uint64_t)g_cpu.R["
                 << rm << "] * (uint64_t)g_cpu.R[" << rs << "];\n";
            body << indent << "uint64_t " << av << " = ((uint64_t)g_cpu.R["
                 << rd << "] << 32) | g_cpu.R[" << rn << "];\n";
            body << indent << "uint64_t " << sv << " = " << av << " + " << pv << ";\n";
            body << indent << "g_cpu.R[" << rn << "] = (uint32_t)(" << sv << " & 0xFFFFFFFFu);\n";
            body << indent << "g_cpu.R[" << rd << "] = (uint32_t)(" << sv << " >> 32);\n";
            if (ins.set_flags) {
                body << indent << "{ uint32_t _c = g_cpu.cpsr & ~(CPSR_N_BIT|CPSR_Z_BIT); "
                     << "if (" << sv << " >> 63) _c |= CPSR_N_BIT; "
                     << "if (" << sv << " == 0) _c |= CPSR_Z_BIT; "
                     << "g_cpu.cpsr = _c; }\n";
            }
            return true;
        }
        case IrOp::SMULL: {
            body << indent << cyc_var_for(ins)
                 << " += runtime_mul_cycles(g_cpu.R[" << rs << "], 1u, 1u);\n";
            std::string pv = "_p" + sfx;
            body << indent << "int64_t " << pv << " = (int64_t)(int32_t)g_cpu.R["
                 << rm << "] * (int64_t)(int32_t)g_cpu.R[" << rs << "];\n";
            body << indent << "g_cpu.R[" << rn << "] = (uint32_t)((uint64_t)" << pv << " & 0xFFFFFFFFu);\n";
            body << indent << "g_cpu.R[" << rd << "] = (uint32_t)((uint64_t)" << pv << " >> 32);\n";
            if (ins.set_flags) {
                body << indent << "{ uint32_t _c = g_cpu.cpsr & ~(CPSR_N_BIT|CPSR_Z_BIT); "
                     << "if ((uint64_t)" << pv << " >> 63) _c |= CPSR_N_BIT; "
                     << "if (" << pv << " == 0) _c |= CPSR_Z_BIT; "
                     << "g_cpu.cpsr = _c; }\n";
            }
            return true;
        }
        case IrOp::SMLAL: {
            body << indent << cyc_var_for(ins)
                 << " += runtime_mul_cycles(g_cpu.R[" << rs << "], 1u, 2u);\n";
            std::string pv = "_p" + sfx;
            std::string av = "_acc" + sfx;
            std::string sv = "_sum" + sfx;
            body << indent << "int64_t " << pv << " = (int64_t)(int32_t)g_cpu.R["
                 << rm << "] * (int64_t)(int32_t)g_cpu.R[" << rs << "];\n";
            body << indent << "uint64_t " << av << " = ((uint64_t)g_cpu.R["
                 << rd << "] << 32) | g_cpu.R[" << rn << "];\n";
            body << indent << "uint64_t " << sv << " = " << av << " + (uint64_t)" << pv << ";\n";
            body << indent << "g_cpu.R[" << rn << "] = (uint32_t)(" << sv << " & 0xFFFFFFFFu);\n";
            body << indent << "g_cpu.R[" << rd << "] = (uint32_t)(" << sv << " >> 32);\n";
            if (ins.set_flags) {
                body << indent << "{ uint32_t _c = g_cpu.cpsr & ~(CPSR_N_BIT|CPSR_Z_BIT); "
                     << "if (" << sv << " >> 63) _c |= CPSR_N_BIT; "
                     << "if (" << sv << " == 0) _c |= CPSR_Z_BIT; "
                     << "g_cpu.cpsr = _c; }\n";
            }
            return true;
        }
        default: return false;
    }
}

bool emit_swap(std::ostringstream& body, const Instr& ins,
               const char* indent) {
    std::string sfx = uniq_suffix(ins);
    if (ins.op == IrOp::SWP) {
        std::string av = "_a" + sfx;
        std::string ov = "_o" + sfx;
        body << indent << "uint32_t " << av << " = g_cpu.R["
             << static_cast<unsigned>(ins.rn) << "];\n";
        body << indent << "{ uint32_t _m = runtime_mem_cycles(" << av
             << " & ~3u, 4u, 0u); " << cyc_var_for(ins) << " += _m; "
             << data_var_for(ins) << " += _m; }\n";
        body << indent << "uint32_t " << ov
             << " = bus_read_u32(" << av << " & ~3u);\n";
        body << indent << "{ uint32_t _rot = (" << av << " & 3u) * 8u; "
             << "if (_rot) " << ov << " = (" << ov << " >> _rot) | ("
             << ov << " << (32u - _rot)); }\n";
        body << indent << "runtime_trace_event(RUNTIME_TRACE_MEM_WRITE, "
             << fmt_hex32(ins.pc) << ", " << av << " & ~3u, g_cpu.R["
             << static_cast<unsigned>(ins.rm) << "], 4u);\n";
        body << indent << "{ uint32_t _m = runtime_mem_cycles(" << av
             << " & ~3u, 4u, 0u); " << cyc_var_for(ins) << " += _m; "
             << data_var_for(ins) << " += _m; }\n";
        body << indent << "bus_write_u32(" << av << " & ~3u, g_cpu.R["
             << static_cast<unsigned>(ins.rm) << "]);\n";
        body << indent << "g_cpu.R[" << static_cast<unsigned>(ins.rd)
             << "] = " << ov << ";\n";
        return true;
    }
    if (ins.op == IrOp::SWPB) {
        std::string av = "_a" + sfx;
        std::string ov = "_o" + sfx;
        body << indent << "uint32_t " << av << " = g_cpu.R["
             << static_cast<unsigned>(ins.rn) << "];\n";
        body << indent << "{ uint32_t _m = runtime_mem_cycles(" << av
             << ", 1u, 0u); " << cyc_var_for(ins) << " += _m; "
             << data_var_for(ins) << " += _m; }\n";
        body << indent << "uint8_t " << ov << " = bus_read_u8(" << av << ");\n";
        body << indent << "runtime_trace_event(RUNTIME_TRACE_MEM_WRITE, "
             << fmt_hex32(ins.pc) << ", " << av << ", (uint32_t)(g_cpu.R["
             << static_cast<unsigned>(ins.rm) << "] & 0xFFu), 1u);\n";
        body << indent << "{ uint32_t _m = runtime_mem_cycles(" << av
             << ", 1u, 0u); " << cyc_var_for(ins) << " += _m; "
             << data_var_for(ins) << " += _m; }\n";
        body << indent << "bus_write_u8(" << av << ", (uint8_t)(g_cpu.R["
             << static_cast<unsigned>(ins.rm) << "] & 0xFFu));\n";
        body << indent << "g_cpu.R[" << static_cast<unsigned>(ins.rd)
             << "] = " << ov << ";\n";
        return true;
    }
    return false;
}

bool emit_psr(std::ostringstream& body, const Instr& ins,
              const char* indent) {
    if (ins.op == IrOp::MRS) {
        const char* fn = ins.psr.spsr ? "runtime_mrs_spsr" : "runtime_mrs_cpsr";
        body << indent << "g_cpu.R[" << static_cast<unsigned>(ins.rd)
             << "] = " << fn << "();\n";
        return true;
    }
    if (ins.op == IrOp::MSR) {
        std::string sfx = uniq_suffix(ins);
        std::string vv = "_msrv" + sfx;
        body << indent << "uint32_t " << vv << ";\n";
        if (ins.op2.kind == Op2::Kind::Imm) {
            body << indent << vv << " = " << fmt_hex32(ins.op2.imm_value) << ";\n";
        } else {
            body << indent << vv << " = g_cpu.R["
                 << static_cast<unsigned>(ins.op2.shifted.rm) << "];\n";
        }
        const char* fn = ins.psr.spsr ? "runtime_msr_spsr" : "runtime_msr_cpsr";
        body << indent << fn << "(" << vv << ", " << static_cast<unsigned>(ins.psr.mask) << "u);\n";
        return true;
    }
    return false;
}

bool emit_doubleword(std::ostringstream& body, const Instr& ins,
                     const char* indent) {
    // LDRD/STRD (ARMv5TE): transfer the register pair Rd, Rd+1 at the
    // effective address and EA+4. Reuses the LDR/STR addressing mode.
    const auto& mem = ins.mem;
    std::string sfx = uniq_suffix(ins);
    std::string base_var = "_base" + sfx;
    std::string off_var = "_off" + sfx;
    std::string ea_var = "_ea" + sfx;
    std::string post_var = "_post" + sfx;

    body << indent << "uint32_t " << base_var << " = "
         << read_reg_expr(mem.rn, ins) << ";\n";
    emit_mem_offset(body, mem, ins, off_var, indent);
    body << indent << "uint32_t " << ea_var << " = "
         << (mem.pre_indexed
                 ? (mem.add ? (base_var + " + " + off_var)
                            : (base_var + " - " + off_var))
                 : base_var)
         << ";\n";
    body << indent << "uint32_t " << post_var << " = "
         << (mem.add ? (base_var + " + " + off_var)
                     : (base_var + " - " + off_var))
         << ";\n";

    // Two word accesses: first non-sequential, second sequential.
    body << indent << "{ uint32_t _m = runtime_mem_cycles(" << ea_var
         << " & ~3u, 4u, 0u); " << cyc_var_for(ins) << " += _m; "
         << data_var_for(ins) << " += _m; }\n";
    body << indent << "{ uint32_t _m = runtime_mem_cycles((" << ea_var
         << " & ~3u) + 4u, 4u, 1u); " << cyc_var_for(ins) << " += _m; "
         << data_var_for(ins) << " += _m; }\n";

    unsigned rd = ins.rd;
    unsigned rd1 = (ins.rd + 1u) & 15u;
    if (ins.op == IrOp::LDRD) {
        body << indent << "g_cpu.R[" << rd << "] = bus_read_u32("
             << ea_var << " & ~3u);\n";
        body << indent << "g_cpu.R[" << rd1 << "] = bus_read_u32(("
             << ea_var << " & ~3u) + 4u);\n";
        if (mem.writeback || !mem.pre_indexed) {
            body << indent << "g_cpu.R[" << static_cast<unsigned>(mem.rn)
                 << "] = " << (mem.pre_indexed ? ea_var : post_var) << ";\n";
        }
    } else {  // STRD
        body << indent << "runtime_trace_event(RUNTIME_TRACE_MEM_WRITE, "
             << fmt_hex32(ins.pc) << ", " << ea_var << " & ~3u, g_cpu.R["
             << rd << "], 4u);\n";
        body << indent << "bus_write_u32(" << ea_var << " & ~3u, g_cpu.R["
             << rd << "]);\n";
        body << indent << "runtime_trace_event(RUNTIME_TRACE_MEM_WRITE, "
             << fmt_hex32(ins.pc) << ", (" << ea_var << " & ~3u) + 4u, g_cpu.R["
             << rd1 << "], 4u);\n";
        body << indent << "bus_write_u32((" << ea_var << " & ~3u) + 4u, g_cpu.R["
             << rd1 << "]);\n";
        if (mem.writeback || !mem.pre_indexed) {
            body << indent << "g_cpu.R[" << static_cast<unsigned>(mem.rn)
                 << "] = " << (mem.pre_indexed ? ea_var : post_var) << ";\n";
        }
    }
    return true;
}

bool emit_clz(std::ostringstream& body, const Instr& ins, const char* indent) {
    body << indent << "g_cpu.R[" << static_cast<unsigned>(ins.rd)
         << "] = runtime_clz(g_cpu.R[" << static_cast<unsigned>(ins.rm)
         << "]);\n";
    return true;
}

// Emit a 32-bit signed saturation of int64 expression `src` into a named
// int64 lvalue, raising CPSR.Q on clamp. Used by the QADD family.
void emit_signed_sat32(std::ostringstream& body, const std::string& var,
                       const char* indent) {
    body << indent << "if (" << var << " > 2147483647LL) { " << var
         << " = 2147483647LL; g_cpu.cpsr |= CPSR_Q_BIT; }\n";
    body << indent << "else if (" << var << " < -2147483648LL) { " << var
         << " = -2147483648LL; g_cpu.cpsr |= CPSR_Q_BIT; }\n";
}

bool emit_saturating(std::ostringstream& body, const Instr& ins,
                     const char* indent) {
    // QADD  Rd,Rm,Rn : Rd = SignedSat(Rm + Rn)
    // QSUB  Rd,Rm,Rn : Rd = SignedSat(Rm - Rn)
    // QDADD Rd,Rm,Rn : Rd = SignedSat(Rm + SignedSat(2*Rn))
    // QDSUB Rd,Rm,Rn : Rd = SignedSat(Rm - SignedSat(2*Rn))
    std::string sfx = uniq_suffix(ins);
    std::string Rm = "g_cpu.R[" + std::to_string(ins.rm) + "]";
    std::string Rn = "g_cpu.R[" + std::to_string(ins.rn) + "]";
    std::string s = "_qs" + sfx;
    const bool dbl = (ins.op == IrOp::QDADD || ins.op == IrOp::QDSUB);
    const bool sub = (ins.op == IrOp::QSUB || ins.op == IrOp::QDSUB);
    std::string rhs;
    if (dbl) {
        std::string d = "_qd" + sfx;
        body << indent << "int64_t " << d << " = (int64_t)(int32_t)" << Rn
             << " * 2;\n";
        emit_signed_sat32(body, d, indent);
        rhs = d;
    } else {
        rhs = "(int64_t)(int32_t)" + Rn;
    }
    body << indent << "int64_t " << s << " = (int64_t)(int32_t)" << Rm
         << (sub ? " - " : " + ") << rhs << ";\n";
    emit_signed_sat32(body, s, indent);
    body << indent << "g_cpu.R[" << static_cast<unsigned>(ins.rd)
         << "] = (uint32_t)(int32_t)" << s << ";\n";
    return true;
}

bool emit_signed_multiply(std::ostringstream& body, const Instr& ins,
                          const char* indent) {
    std::string sfx = uniq_suffix(ins);
    std::string Rm = "g_cpu.R[" + std::to_string(ins.rm) + "]";
    std::string Rs = "g_cpu.R[" + std::to_string(ins.rs) + "]";
    std::string Rn = "g_cpu.R[" + std::to_string(ins.rn) + "]";
    unsigned rd = ins.rd;
    // Sign-extended 16-bit half of a register (top or bottom).
    auto half = [](const std::string& r, bool top) -> std::string {
        return top ? ("(int32_t)(int16_t)(" + r + " >> 16)")
                   : ("(int32_t)(int16_t)" + r);
    };
    switch (ins.op) {
        case IrOp::SMULxy:
            body << indent << "g_cpu.R[" << rd << "] = (uint32_t)("
                 << half(Rm, ins.mul_x_top) << " * " << half(Rs, ins.mul_y_top)
                 << ");\n";
            return true;
        case IrOp::SMLAxy: {
            std::string s = "_sm" + sfx;
            body << indent << "int64_t " << s << " = (int64_t)("
                 << half(Rm, ins.mul_x_top) << " * " << half(Rs, ins.mul_y_top)
                 << ") + (int64_t)(int32_t)" << Rn << ";\n";
            body << indent << "g_cpu.R[" << rd << "] = (uint32_t)(int32_t)"
                 << s << ";\n";
            body << indent << "if (" << s << " != (int32_t)" << s
                 << ") g_cpu.cpsr |= CPSR_Q_BIT;\n";
            return true;
        }
        case IrOp::SMULWy:
            body << indent << "g_cpu.R[" << rd
                 << "] = (uint32_t)(int32_t)(((int64_t)(int32_t)" << Rm << " * "
                 << half(Rs, ins.mul_y_top) << ") >> 16);\n";
            return true;
        case IrOp::SMLAWy: {
            std::string s = "_sm" + sfx;
            body << indent << "int64_t " << s
                 << " = (((int64_t)(int32_t)" << Rm << " * "
                 << half(Rs, ins.mul_y_top) << ") >> 16) + (int64_t)(int32_t)"
                 << Rn << ";\n";
            body << indent << "g_cpu.R[" << rd << "] = (uint32_t)(int32_t)"
                 << s << ";\n";
            body << indent << "if (" << s << " != (int32_t)" << s
                 << ") g_cpu.cpsr |= CPSR_Q_BIT;\n";
            return true;
        }
        case IrOp::SMLALxy: {
            // RdHi:RdLo += Rm.x * Rs.y.  RdHi = ins.rd, RdLo = ins.rn.
            std::string p = "_sp" + sfx;
            std::string acc = "_sa" + sfx;
            body << indent << "int64_t " << p << " = (int64_t)("
                 << half(Rm, ins.mul_x_top) << " * " << half(Rs, ins.mul_y_top)
                 << ");\n";
            body << indent << "uint64_t " << acc << " = ((uint64_t)g_cpu.R["
                 << rd << "] << 32) | g_cpu.R[" << static_cast<unsigned>(ins.rn)
                 << "];\n";
            body << indent << acc << " += (uint64_t)" << p << ";\n";
            body << indent << "g_cpu.R[" << static_cast<unsigned>(ins.rn)
                 << "] = (uint32_t)(" << acc << " & 0xFFFFFFFFu);\n";
            body << indent << "g_cpu.R[" << rd << "] = (uint32_t)(" << acc
                 << " >> 32);\n";
            return true;
        }
        default:
            return false;
    }
}

bool emit_coprocessor(std::ostringstream& body, const Instr& ins,
                      const char* indent) {
    // ARM9 CP15 (and any future coprocessor) access. The register
    // selectors are statically known from the encoding; the value
    // transfer routes through the runtime, which owns the CP15 model
    // (MPU regions, TCM base/size, cache control). No HLE here — the
    // runtime drives the real ARM9 bus configuration from these writes.
    const auto& cp = ins.coproc;
    char abuf[80];
    std::snprintf(abuf, sizeof(abuf), "%uu, %uu, %uu, %uu, %uu",
                  static_cast<unsigned>(cp.cp_num),
                  static_cast<unsigned>(cp.op1),
                  static_cast<unsigned>(cp.crn),
                  static_cast<unsigned>(cp.crm),
                  static_cast<unsigned>(cp.op2));
    const std::string args(abuf);

    switch (ins.op) {
        case IrOp::MCR: {
            // MCR with Rd == R15 is unpredictable; stores pc+12 like the
            // other R15-as-source ARMv4T cases for determinism.
            std::string val = (ins.rd == 15)
                ? fmt_hex32(ins.pc + 12u)
                : ("g_cpu.R[" +
                   std::to_string(static_cast<unsigned>(ins.rd)) + "]");
            body << indent << "runtime_coproc_write(" << args << ", "
                 << val << ");\n";
            return true;
        }
        case IrOp::MRC: {
            std::string sfx = uniq_suffix(ins);
            std::string v = "_cpv" + sfx;
            body << indent << "uint32_t " << v
                 << " = runtime_coproc_read(" << args << ");\n";
            if (ins.rd == 15) {
                // MRC to R15 transfers the coprocessor value's top nibble
                // into CPSR's N/Z/C/V flags (ARM ARM A4.1.20). Not used by
                // CP15, but lowered for completeness.
                body << indent
                     << "g_cpu.cpsr = (g_cpu.cpsr & 0x0FFFFFFFu) | ("
                     << v << " & 0xF0000000u);\n";
            } else {
                body << indent << "g_cpu.R[" << static_cast<unsigned>(ins.rd)
                     << "] = " << v << ";\n";
            }
            return true;
        }
        case IrOp::CDP:
            body << indent << "runtime_coproc_cdp(" << args << ");\n";
            return true;
        default:
            return false;
    }
}

bool emit_swi(std::ostringstream& body, const Instr& ins,
              const char* indent) {
    body << indent << "g_cpu.R[15] = "
         << fmt_hex32(ins.pc + (ins.thumb ? 2u : 4u)) << ";\n";
    body << indent << "runtime_swi(" << fmt_hex32(ins.swi_imm) << ");\n";
    body << indent << "return;\n";
    return true;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────

std::string ArmCodegen::emit_instr(const Instr& ins, const CodegenCtx& ctx,
                                   bool* not_implemented) {
    if (not_implemented) *not_implemented = false;

    if (ins.is_undefined) {
        std::ostringstream s;
        s << "    /* undefined instruction at 0x" << std::hex << ins.pc
          << std::dec << " */\n";
        s << "    runtime_unimplemented_op(\"undefined\", "
          << fmt_hex32(ins.pc) << ");\n";
        s << "    return;\n";
        if (not_implemented) *not_implemented = false;  // explicit abort, not unhandled
        return s.str();
    }

    if (cond_never(ins.cond)) {
        // Never executes, but the instruction is still fetched: charge
        // the 1S fetch and advance PC, matching the interpreter's
        // condition-failed path (cycles_out = 1).
        std::ostringstream s;
        s << "    /* cond NV: never executes */\n";
        // Fingerprint at the current pc (pre-advance) so the armed insn trace
        // stays 1:1 with the interpreter, which fingerprints every fetched
        // instruction including condition-failed ones (MC-HP-002).
        s << "    g_cpu.R[15] = " << fmt_hex32(ins.pc) << ";\n";
        s << "    if (g_runtime_insn_trace) runtime_insn_fp();\n";
        s << "    g_cpu.R[15] = "
          << fmt_hex32(ins.pc + (ins.thumb ? 2u : 4u)) << ";\n";
        // No _cyc/_data/_int accumulators exist on this early-return path (a
        // statically-NV instruction never executes at all), so numC is
        // computed inline (no double-tick risk here — there is no callee
        // C-return / fall-through epilogue on this path, it always returns
        // immediately). ARM9 combines as class C (numI=0, has_data=0 ->
        // cost=numC) with a literal "1u" ARM7 fallback — the identical value
        // the old flat `runtime_tick(1u)` used for both CPUs, so ARM7 is
        // byte-for-byte unchanged.
        s << "    runtime_tick("
          << arm9_tick_expr_ex("runtime_code_cycles(" + fmt_hex32(ins.pc) + ")",
                               "1u", "0u", "0u", false, false, "")
          << ");\n";
        return s.str();
    }

    std::ostringstream os;
    // The generated bodies use static PC semantics for operand reads, so
    // keeping R15 at the current instruction PC here is only for the
    // exception/IRQ resume state read by runtime_irq/runtime_swi.
    os << "    g_cpu.R[15] = " << fmt_hex32(ins.pc) << ";\n";
    os << "    if (runtime_should_yield()) return;\n";
    // Per-instruction fingerprint (armed): record pre-execution state so the
    // recomp can be diffed against the interp oracle at identical cycle counts
    // (MC-HP-002). Placed AFTER the yield check so a yielded (not-executed)
    // instruction is not fingerprinted. Zero cost when disarmed.
    os << "    if (g_runtime_insn_trace) runtime_insn_fp();\n";

    // Per-instruction cycle accumulator. Starts at the cond-fail cost
    // (1S fetch); the cond-pass block raises it to the full execute cost,
    // and memory/multiply ops add their runtime-dependent component. A
    // single runtime_tick(_cyc) at the instruction boundary (epilogue
    // below) pumps the PPU/audio/timers and delivers any pending IRQ —
    // matching the interpreter oracle, which pumps the full cost AFTER
    // executing and checks IRQs at the next boundary.
    const std::string cyc_var = cyc_var_for(ins);
    const std::string code_var = code_var_for(ins);
    const std::string data_var = data_var_for(ins);
    const std::string int_var = int_var_for(ins);
    os << "    uint32_t " << cyc_var << " = 1u;\n";
    // ARM9 combine accumulators (arm9_cycle_combine — melonDS AddCycles_C/
    // CI/CD/CDI). Declared here (unconditionally, like _cyc) so they are
    // visible at the epilogue tick outside the cond guard below. _code is
    // this instruction's own numC, valid regardless of whether cond passes
    // (melonDS still fetches — and charges AddCycles_C() for — a condition-
    // failed instruction). _data/_int stay 0 unless cond passes (assigned/
    // accumulated only inside the guard), so a runtime cond-fail combines as
    // class C (numC only) — matching melonDS exactly. All three are dead
    // code on the ARM7 path.
    os << "    uint32_t " << code_var << " = runtime_code_cycles("
       << fmt_hex32(ins.pc) << ");\n";
    os << "    uint32_t " << data_var << " = 0u;\n";
    os << "    uint32_t " << int_var << " = 0u;\n";

    emit_cond_open(os, ins.cond);
    const char* indent = indent_for(ins.cond);

    // Static execute cost = base (fetch + internal cycles; branch refill
    // folded in) + register-shift surcharge + non-branch PC-write refill.
    // Mirrors interpreter.cpp's cost assembly. SWI ticks its own 3 cycles
    // inside runtime_swi (after masking IRQs), so it leaves _cyc at the
    // fetch baseline (used only on the cond-fail path).
    if (ins.op != IrOp::SWI) {
        uint32_t exec_cost = instr_cycle_base(ins.op);
        if (ins.op2.kind == Op2::Kind::Shifted &&
            ins.op2.shifted.by_register) {
            exec_cost += 1u;  // extra shifter I-cycle (register-specified shift)
        }
        if (writes_pc_nonbranch(ins)) {
            exec_cost += 2u;  // pipeline refill
        }
        os << indent << cyc_var << " = " << exec_cost << "u;\n";
        os << indent << int_var << " = " << arm9_internal_cost(ins) << "u;\n";
    }

    bool ok = false;

    switch (ins.op) {
        case IrOp::AND: case IrOp::EOR: case IrOp::SUB: case IrOp::RSB:
        case IrOp::ADD: case IrOp::ADC: case IrOp::SBC: case IrOp::RSC:
        case IrOp::TST: case IrOp::TEQ: case IrOp::CMP: case IrOp::CMN:
        case IrOp::ORR: case IrOp::MOV: case IrOp::BIC: case IrOp::MVN:
            ok = emit_data_processing(os, ins, indent);
            break;
        case IrOp::B: case IrOp::BL: case IrOp::BX:
        case IrOp::BLX_reg: case IrOp::BLX_imm:
        case IrOp::BL_prefix: case IrOp::BL_suffix:
            ok = emit_branch(os, ins, ctx, indent);
            break;
        case IrOp::LDR: case IrOp::STR:
        case IrOp::LDRB: case IrOp::STRB:
        case IrOp::LDRH: case IrOp::STRH:
        case IrOp::LDRSB: case IrOp::LDRSH:
            ok = emit_memory(os, ins, indent);
            break;
        case IrOp::LDRD: case IrOp::STRD:
            ok = emit_doubleword(os, ins, indent);
            break;
        case IrOp::CLZ:
            ok = emit_clz(os, ins, indent);
            break;
        case IrOp::QADD: case IrOp::QSUB:
        case IrOp::QDADD: case IrOp::QDSUB:
            ok = emit_saturating(os, ins, indent);
            break;
        case IrOp::SMLAxy: case IrOp::SMLAWy: case IrOp::SMULWy:
        case IrOp::SMLALxy: case IrOp::SMULxy:
            ok = emit_signed_multiply(os, ins, indent);
            break;
        case IrOp::PLD:
            os << indent << "/* pld (preload hint) — no-op */\n";
            ok = true;
            break;
        case IrOp::LDM: case IrOp::STM:
            ok = emit_block_transfer(os, ins, indent);
            break;
        case IrOp::MUL: case IrOp::MLA:
        case IrOp::UMULL: case IrOp::UMLAL:
        case IrOp::SMULL: case IrOp::SMLAL:
            ok = emit_multiply(os, ins, indent);
            break;
        case IrOp::SWP: case IrOp::SWPB:
            ok = emit_swap(os, ins, indent);
            break;
        case IrOp::MRS: case IrOp::MSR:
            ok = emit_psr(os, ins, indent);
            break;
        case IrOp::MCR: case IrOp::MRC: case IrOp::CDP:
            ok = emit_coprocessor(os, ins, indent);
            break;
        case IrOp::SWI:
            ok = emit_swi(os, ins, indent);
            break;
        default:
            ok = false;
            break;
    }

    if (!ok) {
        os << indent << "runtime_unimplemented_op(\""
           << ir_op_name(ins.op) << "\", " << fmt_hex32(ins.pc) << ");\n";
        os << indent << "return;\n";
        if (not_implemented) *not_implemented = true;
    }

    emit_cond_close(os, ins.cond);

    // Instruction boundary. Advance R15 to the next instruction so a
    // pending IRQ delivered by the tick saves the correct return address
    // (the stacked LR must match the interpreter, or IWRAM diverges),
    // then pump the accumulated cost. Branch / PC-write / SWI ops have
    // already set R15 and ticked _cyc before returning, so for them this
    // is dead code; it is the live path for fall-through ops and the
    // cond-fail case (where _cyc is the 1-cycle fetch baseline).
    os << "    g_cpu.R[15] = "
       << fmt_hex32(ins.pc + (ins.thumb ? 2u : 4u)) << ";\n";
    // Every PC-writing op (branch or non-branch) already ticked and returned
    // above, so this site never sees a taken branch / PC write — no refill
    // term. has_data reflects whether THIS op (if it in fact executed —
    // cond-fail leaves _data/_int at 0, see above) made a data-bus access.
    os << "    runtime_tick("
       << arm9_tick_expr(ins, arm9_has_data(ins.op)) << ");\n";
    // An IRQ handler may have reached the scheduler cap and unwound through
    // runtime_tick(). Do not let the interrupted generated body continue and
    // overwrite the handler's resumable PC.
    os << "    if (runtime_unwinding()) return;\n";
    return os.str();
}

CodegenResult ArmCodegen::emit_block(const std::vector<Instr>& block,
                                     const CodegenCtx& ctx) {
    CodegenResult r{};
    r.emitted_count = block.size();
    std::ostringstream os;
    for (const auto& ins : block) {
        bool ni = false;
        os << emit_instr(ins, ctx, &ni);
        if (ni) r.not_implemented = true;
    }
    r.text = os.str();
    return r;
}

}  // namespace armv4t
