#include "region_analysis.h"

namespace armv4t {

InstrEffect classify_instr_effects(const Instr& instr) noexcept {
    InstrEffect effects = InstrEffect::None;

    switch (instr.op) {
        case IrOp::LDR: case IrOp::LDRB: case IrOp::LDRH:
        case IrOp::LDRSB: case IrOp::LDRSH: case IrOp::LDRD:
            effects |= InstrEffect::ReadMemory;
            break;
        case IrOp::STR: case IrOp::STRB: case IrOp::STRH: case IrOp::STRD:
            effects |= InstrEffect::WriteMemory;
            break;
        case IrOp::LDM:
            effects |= InstrEffect::ReadMemory;
            break;
        case IrOp::STM:
            effects |= InstrEffect::WriteMemory;
            break;
        case IrOp::SWP: case IrOp::SWPB:
            effects |= InstrEffect::ReadMemory;
            effects |= InstrEffect::WriteMemory;
            break;
        case IrOp::B: case IrOp::BL: case IrOp::BX:
        case IrOp::BLX_reg: case IrOp::BLX_imm:
        case IrOp::BL_prefix: case IrOp::BL_suffix:
            effects |= InstrEffect::ControlFlow;
            break;
        case IrOp::SWI:
            effects |= InstrEffect::ControlFlow;
            effects |= InstrEffect::ProcessorState;
            effects |= InstrEffect::Exception;
            break;
        case IrOp::MCR: case IrOp::MRC: case IrOp::CDP:
        case IrOp::MRS: case IrOp::MSR:
        case IrOp::QADD: case IrOp::QSUB:
        case IrOp::QDADD: case IrOp::QDSUB:
        case IrOp::SMLAxy: case IrOp::SMLAWy:
            effects |= InstrEffect::ProcessorState;
            break;
        case IrOp::Undefined:
            effects |= InstrEffect::Exception;
            effects |= InstrEffect::Undefined;
            break;
        default:
            break;
    }

    if (instr.is_pc_writing || instr.is_branch)
        effects |= InstrEffect::ControlFlow;
    if (instr.is_indirect)
        effects |= InstrEffect::IndirectControl;
    if (instr.set_flags)
        effects |= InstrEffect::ProcessorState;
    if (instr.is_undefined) {
        effects |= InstrEffect::Exception;
        effects |= InstrEffect::Undefined;
    }
    return effects;
}

RegionEffects summarize_region_effects(
        const std::vector<Instr>& instructions) noexcept {
    RegionEffects summary;
    summary.instruction_count = instructions.size();
    for (const Instr& instr : instructions)
        summary.effects |= classify_instr_effects(instr);
    return summary;
}

}  // namespace armv4t
