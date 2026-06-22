// state.h — runner-internal shared state.
//
// The recompiled banks call the C ABI in runtime_arm.h; this header is
// the C++-side shared state between the runtime, the bus, and CP15.

#pragma once

#include <cstdint>

// Which core is currently executing. The bus view (TCM, per-CPU I/O) and
// the exception-vector base branch on this. Set by the scheduler; the
// first bring-up slice runs ARM9 only.
enum NdsCpu { NDS_ARM9 = 0, NDS_ARM7 = 1 };
extern NdsCpu g_nds_active;

// Terminal-halt signal for the active CPU (dispatch miss / unlowered op):
// the emitted per-instruction prologue checks runtime_should_yield(), so
// setting this unwinds the host call stack back to the scheduler and the
// CPU is not resumed. (Cooperative slice preemption is separate — it fires
// only at backward branches via runtime_slice_yield(), where the resume PC
// is a dispatch entry.)
extern bool g_nds_terminal;
extern const char* g_nds_halt_reason;

// CP15 (ARM9 system-control) state the bus must honor — TCM placement
// changes what an address means, so the bus reads this directly.
struct Cp15State {
    uint32_t control;          // c1,c0,0 control register (raw)
    bool     high_vectors;     // control bit 13: exceptions at 0xFFFF0000
    bool     itcm_enable;      // control bit 18
    bool     dtcm_enable;      // control bit 16
    uint32_t itcm_size;        // bytes (base is always 0)
    uint32_t dtcm_base;        // region base, 4 KB-aligned
    uint32_t dtcm_size;        // bytes
};
extern Cp15State g_cp15;

void cp15_reset();

// ── Dispatch + run control (implemented in runtime_arm.cpp) ─────────────
// Layout-compatible with the generated <bank>_dispatch.c table entries.
struct DispatchEntry { uint32_t addr; uint8_t thumb; void (*fn)(void); };

extern "C" void nds_register_dispatch(int cpu, const DispatchEntry* t,
                                      unsigned len, uint32_t exc_base);
extern "C" void nds_set_cycle_cap(unsigned long long cap);
extern "C" void nds_halt(const char* reason);
// Begin a scheduler slice: clear yield/terminal and arm the cycle cap.
extern "C" void nds_slice_begin(unsigned long long cap);
// Clear the slice-yield unwind flag (scheduler calls this before each
// dispatch so normal returns aren't mistaken for a preemption unwind).
extern "C" void nds_clear_unwinding(void);

// Bus lifecycle / image loading (implemented in bus.cpp).
void bus_init();
void bus_load_arm9_bios(const uint8_t* p, uint32_t n);
void bus_load_arm7_bios(const uint8_t* p, uint32_t n);
void bus_dump_access_ring(uint32_t max_entries);

struct BusRegion {
    const uint8_t* ptr;
    uint32_t len;
};
bool bus_get_region(const char* name, BusRegion* out);
uint8_t bus_debug_read8(int cpu, uint32_t addr);
// True if `addr` maps to a writable region that can hold guest-copied
// executable code (main RAM, shared/ARM7 WRAM, ARM9 ITCM). Used by Tier-3
// to decide whether a PC is interpretable dirty RAM vs. a genuine gap.
bool bus_addr_is_exec_ram(uint32_t addr);

struct BusWatchEvent {
    uint64_t seq;
    uint8_t cpu;
    uint8_t width;
    uint32_t pc;
    uint32_t addr;
    uint32_t value;
};
uint32_t bus_debug_watch_copy(BusWatchEvent* out, uint32_t max_entries);
