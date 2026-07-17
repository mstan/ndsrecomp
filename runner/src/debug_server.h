#pragma once

#include <cstdint>
#include <functional>

// Registers the full power-on re-init (set by main, capturing the dumps) so the
// debug server can honour a `reset` command — the event-count bisector compares
// FRESH-from-reset at each N (repeated mid-slice event-breaks otherwise perturb
// the ARM9/ARM7 interleaving and accumulate false divergences).
void debug_set_reset_fn(std::function<void()> fn);

// Headless serve mode: blocking accept loop; commands drive execution
// (run_to_event etc.). Used by the oracle probes and gates.
void debug_serve(uint16_t port);

// Play-mode surface (psxrecomp model): a dedicated I/O thread owns the
// socket; a command still EXECUTES on the frontend/emu thread at the
// debug_pump() safe point between frames, so no emulator state needs
// locking. Execution-driving commands (run_to_pc/run_to_event/run_cycles/
// run_rounds) are rejected in this mode — the frontend owns execution;
// query the always-on rings instead. Everything else (queries, rings,
// touch/keys injection, frontend_stats/profile) is bounded and available.
bool debug_pump_start(uint16_t port);
void debug_pump();
void debug_pump_stop();
