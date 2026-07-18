#pragma once

#include <cstdint>

#include "state.h"

// Runner-private view of the ARM9 code-region timing latched by bus.cpp.
// Generated banks must not depend on this state; it exists only so the exact
// combined ARM9 prologue can consume the already-latched cost without adding
// a second common-path host call.
struct Arm9CodeTimingSnapshot {
    uint32_t normal = 8u;
    uint32_t line_start = 8u;
    uint32_t valid = 0u;
};

extern Arm9CodeTimingSnapshot g_arm9_code_timing_snapshot;
extern uint32_t g_last_code_pc[2];

inline bool arm9_arm_code_cycles_fast(uint32_t pc, uint32_t* cycles) {
    // CodeRead32 observes live ITCM placement even while the surrounding
    // memory-timing region remains latched until the next control transfer.
    if (g_cp15.itcm_enable && pc < g_cp15.itcm_size) {
        g_last_code_pc[0] = pc;
        *cycles = 1u;
        return true;
    }
    if (!g_arm9_code_timing_snapshot.valid) return false;

    g_last_code_pc[0] = pc;
    const uint32_t fetch_addr = pc + 8u;
    *cycles = (fetch_addr & 0x1Fu)
        ? g_arm9_code_timing_snapshot.normal
        : g_arm9_code_timing_snapshot.line_start;
    return true;
}
