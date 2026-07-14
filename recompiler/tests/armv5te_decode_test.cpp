// armv5te_decode_test.cpp — decode + codegen coverage for the ARMv5TE
// (ARM9) instruction extensions.
//
// The two DS BIOSes exercise the v4T core and (for the ARM9) MCR/MRC and
// BLX, but they do NOT use the DSP/saturating/CLZ/LDRD ops — those only
// show up in firmware/game code. This test gives those otherwise-
// unexercised encodings real coverage: each hand-picked encoding must
// (a) decode to the expected IrOp, and (b) lower through codegen without
// the not_implemented flag (i.e. it produces real C, not an abort).
//
// Encodings are taken from the ARM Architecture Reference Manual
// (ARMv5TE, DDI 0100E). See docs/references.md.

#include <cstdint>
#include <cstdio>

#include "arm_decode.h"
#include "arm_codegen.h"
#include "arm_ir.h"
#include "thumb_decode.h"

using namespace armv4t;

namespace {

int g_failures = 0;

const char* op_name(IrOp op) { return ir_op_name(op); }

// Decode `word` at pc=0x1000 and check it yields `expected` and lowers
// without not_implemented.
void check(uint32_t word, IrOp expected, const char* label) {
    Instr i = ArmDecoder::decode(word, 0x1000u);
    bool ok = true;
    if (i.op != expected) {
        std::printf("  FAIL %-22s 0x%08X decoded as %s, expected %s\n",
                    label, word, op_name(i.op), op_name(expected));
        ok = false;
    }
    if (i.is_undefined) {
        std::printf("  FAIL %-22s 0x%08X decoded UNDEFINED\n", label, word);
        ok = false;
    }
    CodegenCtx ctx;
    bool not_impl = false;
    std::string c = ArmCodegen::emit_instr(i, ctx, &not_impl);
    if (not_impl) {
        std::printf("  FAIL %-22s 0x%08X codegen not_implemented\n",
                    label, word);
        ok = false;
    }
    if (c.empty()) {
        std::printf("  FAIL %-22s 0x%08X codegen produced no text\n",
                    label, word);
        ok = false;
    }
    if (ok) std::printf("  ok   %-22s 0x%08X -> %s\n", label, word,
                        op_name(i.op));
    else ++g_failures;
}

// Extra structural assertions on selected fields.
void check_fields() {
    // CLZ r0, r1
    Instr clz = ArmDecoder::decode(0xE16F0F11u, 0x1000u);
    if (clz.rd != 0 || clz.rm != 1) {
        std::printf("  FAIL CLZ fields rd=%u rm=%u (want 0,1)\n",
                    clz.rd, clz.rm);
        ++g_failures;
    }
    // BLX (imm) must be unconditional even though the encoding's cond
    // field is NV, and must carry a computed target + link/exchange.
    Instr blx = ArmDecoder::decode(0xFA000000u, 0x1000u);
    if (blx.cond != Cond::AL || !blx.branch_link || !blx.branch_exchange) {
        std::printf("  FAIL BLX(imm) cond/link/exchange flags\n");
        ++g_failures;
    }
    // THUMB register-form BLX uses Format 5's H1 bit. It must not decode
    // as plain BX: the link address is (PC+2)|1 and is visible when the
    // callee saves LR.
    Instr tblx = ThumbDecoder::decode(0x4798u, 0x1000u);  // BLX r3
    if (tblx.op != IrOp::BLX_reg || tblx.rm != 3 || !tblx.branch_link ||
        !tblx.branch_exchange || !tblx.is_call) {
        std::printf("  FAIL THUMB BLX r3 decode/flags\n");
        ++g_failures;
    } else {
        CodegenCtx ctx;
        bool not_impl = false;
        std::string c = ArmCodegen::emit_instr(tblx, ctx, &not_impl);
        if (not_impl ||
            c.find("g_cpu.R[14] = 0x00001003u") == std::string::npos) {
            std::printf("  FAIL THUMB BLX r3 codegen link address\n");
            ++g_failures;
        }
    }
    // A taken branch tick may synchronously enter IRQ code which hands off
    // from Tier 3 back to a static bank and requests a host-stack unwind.
    // The branch must honor that unwind before executing its target. This is
    // the ARM9 BIOS WaitByLoop/VBlank regression (0xFFFF07CE -> 0xFFFF07CC).
    Instr loop = ThumbDecoder::decode(0xDCFDu, 0x1000u);  // BGT 0x0FFE
    CodegenCtx loop_ctx;
    loop_ctx.current_function_addr = 0x0F00u;
    loop_ctx.current_function_end_addr = 0x1100u;
    bool loop_ni = false;
    std::string loop_c = ArmCodegen::emit_instr(loop, loop_ctx, &loop_ni);
    size_t tick = loop_c.find("runtime_tick(");
    size_t unwind = loop_c.find("if (runtime_unwinding()) return;", tick);
    size_t yield = loop_c.find("runtime_slice_yield()", tick);
    if (loop_ni || tick == std::string::npos || unwind == std::string::npos ||
        yield == std::string::npos || !(tick < unwind && unwind < yield)) {
        std::printf("  FAIL taken branch does not honor IRQ/Tier3 unwind\n");
        ++g_failures;
    }
    // Thumb MOV pc,lr is a non-branch PC write. The ARM7 flat execution cost
    // already contains its fixed +2 refill; only ARM9 may append a dynamic
    // target-region refill. Doubling ARM7's term makes the BIOS return at
    // 0x2FA2 cost 5 cycles instead of melonDS's 3.
    Instr tmovpc = ThumbDecoder::decode(0x46F7u, 0x1000u);  // MOV pc,lr
    CodegenCtx tmovpc_ctx;
    bool tmovpc_ni = false;
    std::string tmovpc_c = ArmCodegen::emit_instr(tmovpc, tmovpc_ctx,
                                                  &tmovpc_ni);
    if (tmovpc_ni ||
        tmovpc_c.find("arm9_refill_cycles") == std::string::npos ||
        tmovpc_c.find("arm7_refill_cycles") != std::string::npos) {
        std::printf("  FAIL Thumb MOV pc,lr duplicates ARM7 refill\n");
        ++g_failures;
    }
    // LDR-to-PC takes the data-combine path, which reconstructs the ARM7
    // source/data cost and therefore needs the target refill appended.
    Instr ldrpc = ArmDecoder::decode(0xE510F004u, 0x1000u);  // LDR pc,[r0,#-4]
    CodegenCtx ldrpc_ctx;
    bool ldrpc_ni = false;
    std::string ldrpc_c = ArmCodegen::emit_instr(ldrpc, ldrpc_ctx, &ldrpc_ni);
    if (ldrpc_ni ||
        ldrpc_c.find("arm9_refill_cycles") == std::string::npos ||
        ldrpc_c.find("arm7_refill_cycles") == std::string::npos) {
        std::printf("  FAIL LDR pc omits ARM7 target refill\n");
        ++g_failures;
    }
    // SMLATB: x(bit5)=1 -> top half of Rm; y(bit6)=0 -> bottom of Rs.
    Instr smla = ArmDecoder::decode(0xE10000A0u, 0x1000u);
    if (smla.op != IrOp::SMLAxy || !smla.mul_x_top || smla.mul_y_top) {
        std::printf("  FAIL SMLATB op/x/y (op=%s x=%d y=%d)\n",
                    op_name(smla.op), smla.mul_x_top, smla.mul_y_top);
        ++g_failures;
    }
    // A real DP op in the same numeric neighborhood must NOT be grabbed
    // by the DSP interception: CMP r0,#0 (S=1) stays CMP.
    Instr cmp = ArmDecoder::decode(0xE3500000u, 0x1000u);
    if (cmp.op != IrOp::CMP) {
        std::printf("  FAIL CMP misdecoded as %s\n", op_name(cmp.op));
        ++g_failures;
    }
    // MSR cpsr_f, r0 (a real PSR transfer with S=0) must stay MSR, not
    // be stolen by the saturating/DSP detection.
    Instr msr = ArmDecoder::decode(0xE128F000u, 0x1000u);
    if (msr.op != IrOp::MSR) {
        std::printf("  FAIL MSR misdecoded as %s\n", op_name(msr.op));
        ++g_failures;
    }
}

}  // namespace

// Emit the lowered C for a set of encodings into a single synthetic
// translation unit, so the generated text for the otherwise-BIOS-absent
// ARMv5TE ops gets actually compiled (by the harness, after this runs).
void emit_to_file(const char* path) {
    const uint32_t words[] = {
        0xFA000000u, 0xE12FFF3Eu, 0xE16F0F11u,            // BLX imm/reg, CLZ
        0xE1021053u, 0xE1221053u, 0xE1421053u, 0xE1621053u,  // QADD/QSUB/QDADD/QDSUB
        0xE1000080u, 0xE10000A0u, 0xE1200080u, 0xE12000A0u,  // SMLA(BB/TB), SMLAWB, SMULWB
        0xE1400080u, 0xE1600080u,                          // SMLALBB, SMULBB
        0xE1C100D0u, 0xE1C100F0u, 0xF5D0F000u,             // LDRD, STRD, PLD
        0xEE010F10u, 0xEE110F10u, 0xEE070F90u,             // MCR, MRC, MCR(cache)
    };
    std::FILE* f = std::fopen(path, "wb");
    if (!f) { std::printf("  (could not open %s for emit)\n", path); return; }
    std::fprintf(f,
        "/* AUTO-GENERATED by armv5te_decode_test — compile-check only. */\n"
        "#include \"runtime_arm.h\"\n"
        "void armv5te_emit_check(void) {\n");
    CodegenCtx ctx;
    // Each instruction must decode at a DISTINCT pc: the codegen's local
    // names are suffixed by pc, so reusing one pc would collide in a
    // single C scope (mirrors how real banks place each instr at its
    // own address).
    uint32_t pc = 0x2000u;
    for (uint32_t w : words) {
        Instr i = ArmDecoder::decode(w, pc);
        bool ni = false;
        std::fprintf(f, "    /* 0x%08X %s */\n", w, ir_op_name(i.op));
        std::fputs(ArmCodegen::emit_instr(i, ctx, &ni).c_str(), f);
        pc += 4u;
    }
    std::fprintf(f, "}\n");
    std::fclose(f);
    std::printf("  emitted compile-check TU -> %s\n", path);
}

int main(int argc, char** argv) {
    std::printf("[armv5te_decode_test]\n");
    if (argc > 1) { emit_to_file(argv[1]); }

    check(0xFA000000u, IrOp::BLX_imm, "BLX imm (H=0)");
    check(0xFB000000u, IrOp::BLX_imm, "BLX imm (H=1)");
    check(0xE12FFF3Eu, IrOp::BLX_reg, "BLX r14");
    check(0xE16F0F11u, IrOp::CLZ,     "CLZ r0,r1");

    check(0xE1021053u, IrOp::QADD,    "QADD r1,r3,r2");
    check(0xE1221053u, IrOp::QSUB,    "QSUB r1,r3,r2");
    check(0xE1421053u, IrOp::QDADD,   "QDADD r1,r3,r2");
    check(0xE1621053u, IrOp::QDSUB,   "QDSUB r1,r3,r2");

    check(0xE1000080u, IrOp::SMLAxy,  "SMLABB");
    check(0xE10000A0u, IrOp::SMLAxy,  "SMLATB");
    check(0xE1200080u, IrOp::SMLAWy,  "SMLAWB");
    check(0xE12000A0u, IrOp::SMULWy,  "SMULWB");
    check(0xE1400080u, IrOp::SMLALxy, "SMLALBB");
    check(0xE1600080u, IrOp::SMULxy,  "SMULBB");

    check(0xE1C100D0u, IrOp::LDRD,    "LDRD r0,[r1]");
    check(0xE1C100F0u, IrOp::STRD,    "STRD r0,[r1]");

    check(0xF5D0F000u, IrOp::PLD,     "PLD [r0]");

    check(0xEE010F10u, IrOp::MCR,     "MCR p15 c1");
    check(0xEE110F10u, IrOp::MRC,     "MRC p15 c1");

    check_fields();

    if (g_failures == 0) {
        std::printf("[armv5te_decode_test] PASS\n");
        return 0;
    }
    std::printf("[armv5te_decode_test] FAIL (%d)\n", g_failures);
    return 1;
}
