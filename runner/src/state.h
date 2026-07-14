// state.h — runner-internal shared state.
//
// The recompiled banks call the C ABI in runtime_arm.h; this header is
// the C++-side shared state between the runtime, the bus, and CP15.

#pragma once

#include <cstdint>

#include "runtime_arm.h"

// Which core is currently executing (NdsCpu / g_nds_active — declared in
// runtime_arm.h, the ABI boundary generated banks also see, so there is
// one definition shared by C++ runtime code and C generated code). The
// bus view (TCM, per-CPU I/O) and the exception-vector base branch on
// this. Set by the scheduler; the first bring-up slice runs ARM9 only.

// Terminal-halt signal for the active CPU (dispatch miss / unlowered op):
// the emitted per-instruction prologue checks runtime_should_yield(), so
// setting this unwinds the host call stack back to the scheduler and the
// CPU is not resumed. (Cooperative slice preemption is separate — it fires
// only at backward branches via runtime_slice_yield(), where the resume PC
// is a dispatch entry.)
extern bool g_nds_terminal;
extern const char* g_nds_halt_reason;
// Opt-in coverage-discovery build mode: immutable BIOS misses may execute via
// the guest-byte interpreter while their branch targets are logged for static
// promotion. Normal/release execution keeps this false and misses stay fatal.
extern bool g_discover_static_misses;

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
uint32_t cp15_debug_mpu_region(unsigned index);
uint32_t cp15_debug_cache_cfg(unsigned index);

void cp15_reset();
// True if ARM9 code fetches from addr are I-cache-served (per-PU-region, C1 bit12).
bool cp15_code_cacheable(uint32_t addr);
bool cp15_data_cacheable(uint32_t addr);

// ── Dispatch + run control (implemented in runtime_arm.cpp) ─────────────
// Layout-compatible with the generated <bank>_dispatch.c table entries.
using DispatchEntry = NdsDispatchEntry;

extern "C" void nds_register_dispatch(int cpu, const DispatchEntry* t,
                                      unsigned len, uint32_t exc_base);
extern "C" void nds_set_cycle_cap(unsigned long long cap);
extern "C" void nds_reschedule_slice(unsigned long long system_deadline);
extern "C" void nds_halt(const char* reason);
// Begin a scheduler slice: clear yield/terminal and arm the cycle cap.
extern "C" void nds_slice_begin(unsigned long long cap);
extern "C" int nds_slice_over(void);
// Clear the slice-yield unwind flag (scheduler calls this before each
// dispatch so normal returns aren't mistaken for a preemption unwind).
extern "C" void nds_clear_unwinding(void);
// Tier-3 can be nested under a static BIOS IRQ dispatch. An exact-index stop
// must unwind those host frames without allowing their saved continuation PC
// to overwrite the interpreted state at which observation stopped.
void nds_preserve_unwind_state();
void nds_restore_unwind_state();

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
// ARM7-owned hardware bus masters (currently the SPU fetch/capture engines)
// access memory outside a guest instruction. These helpers select the ARM7
// memory view for the duration of the transfer and still feed the always-on
// bus ring, independent of which CPU the scheduler last loaded.
uint32_t bus_device_read32(int cpu, uint32_t addr);
void bus_device_write32(int cpu, uint32_t addr, uint32_t value);
// Tier-3 provenance. A writable mapping alone is not permission to interpret:
// its physical backing page must have been written by a guest CPU or hardware
// bus master since reset. Aliases share the same backing generation.
bool bus_addr_is_writable_ram(uint32_t addr);
bool bus_addr_has_write_provenance(uint32_t addr);
bool bus_range_has_write_provenance(uint32_t addr, uint32_t size);
uint32_t bus_exec_page_generation(uint32_t addr);
// Compare generated source bytes with the active CPU's live executable view
// without producing bus events or consuming guest cycles.
bool bus_live_bytes_equal(uint32_t addr, const uint8_t* expected,
                          uint32_t size);
struct BusExecProvenance {
    bool writable;
    bool written;
    uint32_t generation;
};
BusExecProvenance bus_debug_exec_provenance(int cpu, uint32_t addr);

struct BusWatchEvent {
    uint64_t seq;
    uint64_t cycles;
    uint64_t insn;
    uint8_t cpu;
    uint8_t write;
    uint8_t width;
    uint32_t pc;
    uint32_t addr;
    uint32_t value;
};
uint32_t bus_debug_watch_copy(BusWatchEvent* out, uint32_t max_entries);
