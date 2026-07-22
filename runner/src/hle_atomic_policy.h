#pragma once

#include <cstdint>

inline bool nds_hle_atomic_policy_allows(
        uint32_t blockers, uint32_t deferred_cycles,
        uint64_t current_cycles, uint64_t cycle_cap, uint32_t max_cycles) {
    if (blockers != 0u || deferred_cycles != 0u) return false;
    if (cycle_cap == 0u) return true;
    if (current_cycles >= cycle_cap) return false;
    return static_cast<uint64_t>(max_cycles) <= cycle_cap - current_cycles;
}
