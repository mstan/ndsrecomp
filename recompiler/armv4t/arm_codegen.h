// arm_codegen.h — IR → C code emission.
//
// Lowers each decoded `Instr` to a sequence of C statements that
// operate on the recomp ABI (g_cpu / bus_read_* / bus_write_* /
// arm_shift_* / arm_set_* / runtime_dispatch — see runtime_arm.h).
//
// The output is meant to be dropped directly into a `void fname(void)`
// body emitted by tools/gba_recompile/main.cpp. Each call to
// emit_instr returns a block of C source that:
//   - wraps in `if (arm_cond_passes(...))` for non-AL conditions,
//   - reads operands from g_cpu (with PC = pc+8 / pc+4 baked in
//     statically when R15 is read in operand position),
//   - writes results to g_cpu and (when the instruction writes PC)
//     emits a trailing `return;` so the runtime exec loop re-enters
//     runtime_dispatch with the new PC.
//
// PRINCIPLES.md "Interpreter is informative, never load-bearing":
// the interpreter is the semantic reference but is NEVER called from
// generated code. If emit_instr can't lower an op yet it returns
// `not_implemented = true` and the caller emits a
// `runtime_unimplemented_op(...)` abort — never an interpreter
// fallback.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "arm_ir.h"

namespace armv4t {

struct CodegenResult {
    std::string text;          // emitted C source for the block
    bool not_implemented;      // true if at least one Instr fell through
    std::size_t emitted_count;
};

// Context passed to per-instruction emission. The function-name map
// lets direct B/BL targets lower to a C function call when the
// target is known to be a recompiled function in the same dispatch
// table; unknown targets fall back to runtime_dispatch.
struct CodegenCtx {
    // Key is (addr << 1) | thumb_bit. Direct B/BL preserves the
    // current instruction-set state, so codegen can resolve the
    // correct same-mode callee even when ARM and THUMB entries share
    // the same numeric address.
    const std::unordered_map<uint64_t, std::string>* names_by_key = nullptr;
    uint32_t current_function_addr = 0xFFFFFFFFu;
    uint32_t current_function_end_addr = 0xFFFFFFFFu;
    bool current_function_thumb = false;
    bool force_bx_c_return = false;
};

class ArmCodegen {
public:
    // Emit C source for one decoded instruction. `not_implemented`
    // is set true if the IR shape is not yet lowered. The string
    // ends in a newline; callers may indent it as they please.
    static std::string emit_instr(const Instr& i, const CodegenCtx& ctx,
                                  bool* not_implemented);

    // Block-level helper: invoke emit_instr on every entry,
    // concatenate the result, and report whether any instruction
    // fell through.
    static CodegenResult emit_block(const std::vector<Instr>& block,
                                    const CodegenCtx& ctx);
};

}  // namespace armv4t
