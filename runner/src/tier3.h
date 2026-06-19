// tier3.h — Tier-3 dirty-RAM interpreter entry.
//
// Runs the guest's OWN bytes (firmware boot loader, menu code copied into
// RAM at runtime) through the reference ARM/THUMB interpreter, for PCs that
// have no static Tier-1 bank. See docs/dispatch_architecture.md and
// PRINCIPLES.md "the one exception". Never an HLE model — only the real
// copied bytes.

#pragma once

#include <cstdint>

// Interpret from `pc` (already in g_cpu) until the PC reaches a Tier-1 bank
// entry, a SWI/IRQ vector takes over, or the slice cap is reached. State is
// synced g_cpu→interp on entry and interp→g_cpu on exit.
void tier3_run(uint32_t pc);
