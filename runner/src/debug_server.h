#pragma once

#include <cstdint>
#include <functional>

// Registers the full power-on re-init (set by main, capturing the dumps) so the
// debug server can honour a `reset` command — the event-count bisector compares
// FRESH-from-reset at each N (repeated mid-slice event-breaks otherwise perturb
// the ARM9/ARM7 interleaving and accumulate false divergences).
void debug_set_reset_fn(std::function<void()> fn);

void debug_serve(uint16_t port);
