#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "arm_ir.h"

namespace armv4t {

// Conservative effects used to form optimizer-facing regions. This is
// intentionally independent of codegen: an effect-free classification only
// means the instruction is eligible for local-state optimization. Timing,
// IRQ, debug, and scheduler guards are still required before executing a
// multi-instruction region atomically.
enum class InstrEffect : uint32_t {
    None            = 0,
    ReadMemory      = 1u << 0,
    WriteMemory     = 1u << 1,
    ControlFlow     = 1u << 2,
    IndirectControl = 1u << 3,
    ProcessorState  = 1u << 4,
    Exception       = 1u << 5,
    Undefined       = 1u << 6,
};

constexpr InstrEffect operator|(InstrEffect lhs, InstrEffect rhs) noexcept {
    return static_cast<InstrEffect>(static_cast<uint32_t>(lhs) |
                                    static_cast<uint32_t>(rhs));
}

constexpr InstrEffect& operator|=(InstrEffect& lhs, InstrEffect rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

constexpr bool has_effect(InstrEffect effects, InstrEffect wanted) noexcept {
    return (static_cast<uint32_t>(effects) &
            static_cast<uint32_t>(wanted)) != 0u;
}

InstrEffect classify_instr_effects(const Instr& instr) noexcept;

struct RegionEffects {
    InstrEffect effects = InstrEffect::None;
    std::size_t instruction_count = 0;

    bool local_state_candidate() const noexcept {
        constexpr uint32_t disallowed =
            static_cast<uint32_t>(InstrEffect::ReadMemory) |
            static_cast<uint32_t>(InstrEffect::WriteMemory) |
            static_cast<uint32_t>(InstrEffect::ControlFlow) |
            static_cast<uint32_t>(InstrEffect::IndirectControl) |
            static_cast<uint32_t>(InstrEffect::ProcessorState) |
            static_cast<uint32_t>(InstrEffect::Exception) |
            static_cast<uint32_t>(InstrEffect::Undefined);
        return instruction_count != 0u &&
               (static_cast<uint32_t>(effects) & disallowed) == 0u;
    }
};

RegionEffects summarize_region_effects(const std::vector<Instr>& instructions)
    noexcept;

}  // namespace armv4t
