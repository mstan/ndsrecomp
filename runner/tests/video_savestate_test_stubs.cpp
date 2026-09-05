#include <cstdint>

#include "net/net_ring.h"

extern "C" unsigned long long g_runtime_cycles = 0;

uint16_t nds_powercontrol9() { return 0xFFFFu; }
void nds_raise_irq(int, uint32_t) {}
void nds_clear_irq(int, uint32_t) {}
void nds_dma_trigger(int, unsigned) {}
void nds_gxfifo_set_stall(bool) {}
void scheduler_terminal_halt_all(const char*) {}
bool nds_title_patches_mph_adaptive_centered_native() { return false; }

uint64_t net_ring_push(NdsNetEventKind, uint8_t, uint16_t, uint32_t,
                       const uint8_t*, const uint8_t*, uint32_t, uint32_t,
                       uint16_t, uint16_t, uint16_t, uint32_t) {
    return 0;
}
