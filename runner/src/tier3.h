// tier3.h — Tier-3 dirty-RAM interpreter entry.
//
// Runs the guest's OWN bytes (firmware boot loader, menu code copied into
// RAM at runtime) through the reference ARM/THUMB interpreter, for PCs that
// have no static Tier-1 bank. See docs/dispatch_architecture.md and
// PRINCIPLES.md "the one exception". Never an HLE model — only the real
// copied bytes.

#pragma once

#include <cstdint>

struct Tier3TraceEvent {
    uint64_t seq;
    uint8_t cpu;
    uint8_t thumb;
    uint8_t phase;   // 0 entry, 1 before step, 2 after step, 3 exit
    uint8_t result;  // armv4t::Interpreter::Result for phase 2, else 0
    uint32_t pc;
    uint32_t raw;
    uint32_t next_pc;
    uint32_t cpsr;
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;
    uint32_t sp;
    uint32_t lr;
    uint64_t cycles;
};

// Interpret from `pc` (already in g_cpu) until the PC reaches a Tier-1 bank
// entry, a SWI/IRQ vector takes over, or the slice cap is reached. State is
// synced g_cpu→interp on entry and interp→g_cpu on exit.
void tier3_run(uint32_t pc);

uint32_t tier3_debug_trace_copy(Tier3TraceEvent* out, uint32_t max_entries);
