#include "region_analysis.h"

#include <cstdio>
#include <vector>

using namespace armv4t;

namespace {

Instr instruction(IrOp op) {
    Instr instr{};
    instr.op = op;
    instr.cond = Cond::AL;
    return instr;
}

bool expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "%s\n", message);
    return false;
}

}  // namespace

int main() {
    int failures = 0;

    const std::vector<Instr> arithmetic = {
        instruction(IrOp::ADD), instruction(IrOp::MUL),
        instruction(IrOp::MLA),
    };
    const RegionEffects pure = summarize_region_effects(arithmetic);
    if (!expect(pure.instruction_count == 3u,
                "arithmetic instruction count was wrong")) ++failures;
    if (!expect(pure.local_state_candidate(),
                "pure arithmetic region was rejected")) ++failures;

    Instr load = instruction(IrOp::LDR);
    Instr store = instruction(IrOp::STR);
    Instr swap = instruction(IrOp::SWP);
    if (!expect(has_effect(classify_instr_effects(load),
                           InstrEffect::ReadMemory),
                "load did not report a memory read")) ++failures;
    if (!expect(has_effect(classify_instr_effects(store),
                           InstrEffect::WriteMemory),
                "store did not report a memory write")) ++failures;
    const InstrEffect swap_effects = classify_instr_effects(swap);
    if (!expect(has_effect(swap_effects, InstrEffect::ReadMemory) &&
                has_effect(swap_effects, InstrEffect::WriteMemory),
                "swap did not report both memory effects")) ++failures;

    if (!expect(has_effect(classify_instr_effects(instruction(IrOp::LDM)),
                           InstrEffect::ReadMemory),
                "LDM did not report a memory read")) ++failures;
    if (!expect(has_effect(classify_instr_effects(instruction(IrOp::STM)),
                           InstrEffect::WriteMemory),
                "STM did not report a memory write")) ++failures;

    Instr indirect = instruction(IrOp::BX);
    indirect.is_branch = true;
    indirect.is_indirect = true;
    const InstrEffect branch_effects = classify_instr_effects(indirect);
    if (!expect(has_effect(branch_effects, InstrEffect::ControlFlow) &&
                has_effect(branch_effects, InstrEffect::IndirectControl),
                "indirect branch effects were incomplete")) ++failures;

    const InstrEffect swi = classify_instr_effects(instruction(IrOp::SWI));
    if (!expect(has_effect(swi, InstrEffect::Exception) &&
                has_effect(swi, InstrEffect::ProcessorState),
                "SWI did not report its exception/state effects")) ++failures;

    Instr flag_write = instruction(IrOp::ADD);
    flag_write.set_flags = true;
    if (!expect(has_effect(classify_instr_effects(flag_write),
                           InstrEffect::ProcessorState),
                "flag-writing arithmetic did not report processor state"))
        ++failures;
    if (!expect(has_effect(classify_instr_effects(instruction(IrOp::MRS)),
                           InstrEffect::ProcessorState),
                "MRS did not report processor state")) ++failures;

    const RegionEffects memory_region = summarize_region_effects({load, store});
    if (!expect(!memory_region.local_state_candidate(),
                "memory region was accepted as local-state-only")) ++failures;

    return failures == 0 ? 0 : 1;
}
