// bus.cpp — DS memory bus (first bring-up slice: ARM9-centric).
//
// Implements bus_read/write_{u8,u16,u32} (the C ABI the recompiled banks
// call) over the DS memory map. Per-CPU views branch on g_nds_active.
// ARM9 honors CP15-placed ITCM (at 0) and DTCM (at a configurable base),
// which is why g_cp15 is consulted here.
//
// Memory map per GBATEK ("DS Memory Map"). I/O registers are stubbed for
// now (return 0, logged) — they get real models as the boot demands them
// (IPC, SPI, IRQ, POWCNT…). An always-on access ring records every bus
// touch so a probe can query the window of interest retroactively
// (OBSERVABILITY rule — never arm-then-capture).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "state.h"
#include "runtime_arm.h"
#include "io.h"

namespace {

// Backing stores. Sized to the architectural maxima; mirroring is applied
// at access time.
std::vector<uint8_t> g_main_ram;     // 4 MB shared, mirrored every 4 MB
std::vector<uint8_t> g_itcm;         // 32 KB max (ARM9)
std::vector<uint8_t> g_dtcm;         // 16 KB max (ARM9)
std::vector<uint8_t> g_shared_wram;  // 32 KB shared WRAM
std::vector<uint8_t> g_arm7_wram;    // 64 KB ARM7-only WRAM
std::vector<uint8_t> g_arm9_bios;    // 4 KB
std::vector<uint8_t> g_arm7_bios;    // 16 KB

// Always-on bus-access ring.
struct BusEvent {
    uint64_t seq;
    uint8_t  cpu;     // 0 ARM9 / 1 ARM7
    uint8_t  write;   // 0 read / 1 write
    uint8_t  width;   // 1 / 2 / 4
    uint32_t addr;
    uint32_t value;
};
constexpr uint32_t kRingSize = 8192;
BusEvent g_ring[kRingSize];
uint32_t g_ring_w = 0;
uint64_t g_ring_seq = 0;

constexpr uint32_t kWatchSize = 512;
BusWatchEvent g_watch[kWatchSize];
uint32_t g_watch_w = 0;
uint32_t g_watch_count = 0;

bool watch_range(uint32_t addr, uint32_t width, uint32_t lo, uint32_t hi) {
    uint32_t end = addr + width;
    return addr < hi && end > lo;
}

bool watch_addr(uint32_t addr, uint32_t width) {
    return watch_range(addr, width, 0x021F0000u, 0x021F0040u) ||
           watch_range(addr, width, 0x027FF800u, 0x027FF880u) ||
           watch_range(addr, width, 0x0380F800u, 0x0380F840u);
}

void watch_push(uint8_t width, uint32_t addr, uint32_t value) {
    if (!watch_addr(addr, width)) return;
    BusWatchEvent& e = g_watch[g_watch_w];
    e.seq = g_ring_seq;
    e.cpu = static_cast<uint8_t>(g_nds_active);
    e.width = width;
    e.pc = g_cpu.R[15];
    e.addr = addr;
    e.value = value;
    g_watch_w = (g_watch_w + 1u) % kWatchSize;
    if (g_watch_count < kWatchSize) ++g_watch_count;
}

inline void ring_push(uint8_t write, uint8_t width, uint32_t addr, uint32_t value) {
    BusEvent& e = g_ring[g_ring_w];
    e.seq = ++g_ring_seq;
    e.cpu = static_cast<uint8_t>(g_nds_active);
    e.write = write;
    e.width = width;
    e.addr = addr;
    e.value = value;
    g_ring_w = (g_ring_w + 1u) % kRingSize;
    if (write) watch_push(width, addr, value);
}

// Resolve an address to a backing pointer for the active CPU. Returns
// nullptr for unmapped / I-O addresses (handled separately). `len` is the
// access width for bounds checks.
uint8_t* resolve(uint32_t addr, uint32_t len) {
    const bool arm9 = (g_nds_active == NDS_ARM9);

    if (arm9) {
        // ITCM: responds across its virtual region [0, itcm_size) (the DS
        // firmware programs a 32 MB span), with the 32 KB physical backing
        // MIRRORED every 32 KB within that span. itcm_size is the virtual
        // span; the mirror modulus is the physical size (g_itcm.size()).
        if (g_cp15.itcm_enable && g_cp15.itcm_size &&
            addr < 0x02000000u && addr < g_cp15.itcm_size) {
            uint32_t o = addr & static_cast<uint32_t>(g_itcm.size() - 1u);
            if (o + len <= g_itcm.size()) return g_itcm.data() + o;
        }
        // DTCM: at its configured base.
        if (g_cp15.dtcm_enable && g_cp15.dtcm_size &&
            addr >= g_cp15.dtcm_base &&
            addr - g_cp15.dtcm_base < g_cp15.dtcm_size) {
            uint32_t o = addr - g_cp15.dtcm_base;
            if (o + len <= g_dtcm.size()) return g_dtcm.data() + o;
        }
    }

    // Main RAM: 0x02000000-0x02FFFFFF, mirrored every 4 MB.
    if (addr >= 0x02000000u && addr < 0x03000000u) {
        uint32_t o = addr & 0x003FFFFFu;
        if (o + len <= g_main_ram.size()) return g_main_ram.data() + o;
    }
    // Shared WRAM (0x03000000 region) — simplified: serve the 32 KB block
    // mirrored. (WRAMCNT split modeling comes with the ARM7 slice.)
    if (addr >= 0x03000000u && addr < 0x03800000u) {
        uint32_t o = addr & 0x00007FFFu;
        if (o + len <= g_shared_wram.size()) return g_shared_wram.data() + o;
    }
    // ARM7-only WRAM: 64 KB at 0x03800000, mirrored up to 0x03FFFFFF
    // (the ARM7 IRQ-vector / stack area at 0x03FFFFxx maps here).
    if (addr >= 0x03800000u && addr < 0x04000000u) {
        uint32_t o = addr & 0x0000FFFFu;
        if (o + len <= g_arm7_wram.size()) return g_arm7_wram.data() + o;
    }
    // ARM9 BIOS (high) / ARM7 BIOS (low).
    if (arm9 && addr >= 0xFFFF0000u) {
        uint32_t o = addr & 0x00000FFFu;
        if (o + len <= g_arm9_bios.size()) return g_arm9_bios.data() + o;
    }
    if (!arm9 && addr < 0x00004000u) {
        uint32_t o = addr & 0x00003FFFu;
        if (o + len <= g_arm7_bios.size()) return g_arm7_bios.data() + o;
    }
    return nullptr;
}

// I/O space (0x04000000-0x04FFFFFF) routes to the register model (io.cpp).
bool is_io(uint32_t addr) { return (addr >= 0x04000000u && addr < 0x05000000u); }

uint32_t io_read(uint32_t addr, uint32_t width) { return nds_io_read(addr, width); }
void io_write(uint32_t addr, uint32_t value, uint32_t width) {
    nds_io_write(addr, value, width);
}

void unmapped(uint32_t addr, bool write, uint32_t width, uint32_t value) {
    static int warned = 0;
    if (warned < 64) {
        std::fprintf(stderr, "[bus] ARM%c unmapped %s 0x%08X w=%u%s\n",
                     g_nds_active == NDS_ARM9 ? '9' : '7',
                     write ? "write" : "read", addr, width,
                     write ? "" : " (→0)");
        ++warned;
    }
    (void)value;
}

}  // namespace

// Per-CPU last code-fetch PC, for the sequential/non-sequential distinction in
// runtime_code_cycles (a sequential fetch is far cheaper than a branch/refill).
uint32_t g_last_code_pc[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};

void bus_init() {
    g_last_code_pc[0] = g_last_code_pc[1] = 0xFFFFFFFFu;
    g_main_ram.assign(4u * 1024 * 1024, 0);
    g_itcm.assign(32u * 1024, 0);
    g_dtcm.assign(16u * 1024, 0);
    g_shared_wram.assign(32u * 1024, 0);
    g_arm7_wram.assign(64u * 1024, 0);
    g_arm9_bios.assign(4u * 1024, 0);
    g_arm7_bios.assign(16u * 1024, 0);
    g_ring_w = 0;
    g_ring_seq = 0;
    g_watch_w = 0;
    g_watch_count = 0;
}

void bus_load_arm9_bios(const uint8_t* p, uint32_t n) {
    if (n > g_arm9_bios.size()) n = static_cast<uint32_t>(g_arm9_bios.size());
    std::memcpy(g_arm9_bios.data(), p, n);
}

void bus_load_arm7_bios(const uint8_t* p, uint32_t n) {
    if (n > g_arm7_bios.size()) n = static_cast<uint32_t>(g_arm7_bios.size());
    std::memcpy(g_arm7_bios.data(), p, n);
}

void bus_dump_access_ring(uint32_t max_entries) {
    uint32_t count = (g_ring_seq < kRingSize) ? static_cast<uint32_t>(g_ring_seq)
                                              : kRingSize;
    if (max_entries < count) count = max_entries;
    std::fprintf(stderr, "[bus] last %u access(es):\n", count);
    uint32_t start = (g_ring_w + kRingSize - count) % kRingSize;
    for (uint32_t i = 0; i < count; ++i) {
        const BusEvent& e = g_ring[(start + i) % kRingSize];
        std::fprintf(stderr, "  #%llu ARM%c %s%u 0x%08X = 0x%08X\n",
                     static_cast<unsigned long long>(e.seq),
                     e.cpu == 0 ? '9' : '7', e.write ? "W" : "R",
                     e.width, e.addr, e.value);
    }
}

bool bus_get_region(const char* name, BusRegion* out) {
    if (!name || !out) return false;
    if (std::strcmp(name, "mainram") == 0) {
        *out = {g_main_ram.data(), static_cast<uint32_t>(g_main_ram.size())};
        return true;
    }
    if (std::strcmp(name, "wram7") == 0) {
        *out = {g_arm7_wram.data(), static_cast<uint32_t>(g_arm7_wram.size())};
        return true;
    }
    if (std::strcmp(name, "wramshared") == 0) {
        *out = {g_shared_wram.data(), static_cast<uint32_t>(g_shared_wram.size())};
        return true;
    }
    if (std::strcmp(name, "itcm") == 0) {
        *out = {g_itcm.data(), static_cast<uint32_t>(g_itcm.size())};
        return true;
    }
    if (std::strcmp(name, "dtcm") == 0) {
        *out = {g_dtcm.data(), static_cast<uint32_t>(g_dtcm.size())};
        return true;
    }
    return false;
}

// True for writable regions that can hold guest-copied executable code —
// the Tier-3 dirty-RAM interpreter runs from these. Branches on the active
// CPU for ARM9 ITCM (mirrored across its virtual span). Keeps the
// memory-map authority in the bus so Tier-3 can't drift from resolve().
bool bus_addr_is_exec_ram(uint32_t addr) {
    if (g_nds_active == NDS_ARM9 && g_cp15.itcm_enable && g_cp15.itcm_size &&
        addr < 0x02000000u && addr < g_cp15.itcm_size)
        return true;                              // ITCM virtual span
    if (addr >= 0x02000000u && addr < 0x04000000u)
        return true;                              // main RAM + WRAM (+DTCM)
    return false;
}

uint8_t bus_debug_read8(int cpu, uint32_t addr) {
    NdsCpu old = g_nds_active;
    g_nds_active = (cpu == 7) ? NDS_ARM7 : NDS_ARM9;
    uint8_t v = 0;
    if (uint8_t* p = resolve(addr, 1)) v = *p;
    else if (is_io(addr)) v = static_cast<uint8_t>(io_read(addr, 1));
    g_nds_active = old;
    return v;
}

uint32_t bus_debug_watch_copy(BusWatchEvent* out, uint32_t max_entries) {
    if (!out || max_entries == 0) return 0;
    uint32_t count = g_watch_count;
    if (count > max_entries) count = max_entries;
    uint32_t start = (g_watch_w + kWatchSize - count) % kWatchSize;
    for (uint32_t i = 0; i < count; ++i)
        out[i] = g_watch[(start + i) % kWatchSize];
    return count;
}

// ── C ABI: the generated banks call these ──────────────────────────────

extern "C" uint32_t bus_read_u32(uint32_t addr) {
    uint32_t v;
    if (uint8_t* p = resolve(addr, 4)) { std::memcpy(&v, p, 4); }
    else if (is_io(addr)) v = io_read(addr, 4);
    else { unmapped(addr, false, 4, 0); v = 0; }
    ring_push(0, 4, addr, v);
    return v;
}

extern "C" uint16_t bus_read_u16(uint32_t addr) {
    uint16_t v;
    if (uint8_t* p = resolve(addr, 2)) { std::memcpy(&v, p, 2); }
    else if (is_io(addr)) v = static_cast<uint16_t>(io_read(addr, 2));
    else { unmapped(addr, false, 2, 0); v = 0; }
    ring_push(0, 2, addr, v);
    return v;
}

extern "C" uint8_t bus_read_u8(uint32_t addr) {
    uint8_t v;
    if (uint8_t* p = resolve(addr, 1)) { v = *p; }
    else if (is_io(addr)) v = static_cast<uint8_t>(io_read(addr, 1));
    else { unmapped(addr, false, 1, 0); v = 0; }
    ring_push(0, 1, addr, v);
    return v;
}

extern "C" void bus_write_u32(uint32_t addr, uint32_t val) {
    ring_push(1, 4, addr, val);
    if (uint8_t* p = resolve(addr, 4)) std::memcpy(p, &val, 4);
    else if (is_io(addr)) io_write(addr, val, 4);
    else unmapped(addr, true, 4, val);
}

extern "C" void bus_write_u16(uint32_t addr, uint16_t val) {
    ring_push(1, 2, addr, val);
    if (uint8_t* p = resolve(addr, 2)) std::memcpy(p, &val, 2);
    else if (is_io(addr)) io_write(addr, val, 2);
    else unmapped(addr, true, 2, val);
}

extern "C" void bus_write_u8(uint32_t addr, uint8_t val) {
    ring_push(1, 1, addr, val);
    if (uint8_t* p = resolve(addr, 1)) *p = val;
    else if (is_io(addr)) io_write(addr, val, 1);
    else unmapped(addr, true, 1, val);
}

// ARM7 (ARMv4T) region N/S bus timings, ported from melonDS's effective model
// (NDS::InitTimings / NDS::SetARM7RegionTimings — third_party/melonDS/src/NDS.cpp).
// Unlike the uncached ARM9, the ARM7 has NO +3 nonseq CPU penalty and (mostly)
// no 32-bit-bus splitting: BIOS, shared+ARM7 WRAM and I/O are true 32-bit-bus
// regions with N=S=1 ("fast", matches the recompiler's static ARMv4T cost table,
// which was tuned against a uniform-speed GBA-style bus). Only main RAM (16-bit
// bus, N16=8/S16=1) and VRAM (16-bit bus, N16=S16=1 but N32/S32 split) diverge
// from "fast". `n16`/`s16` are the raw bus timings; `bus16` selects the 16-bit
// N32/S32-splitting rule (melonDS: N32=N16+S16, S32=S16+S16) vs the 32-bit
// rule (N32=N16, S32=S16).
struct Arm7Region { uint32_t n16, s16; bool bus16; };
inline Arm7Region arm7_region(uint32_t addr) {
    if (addr >= 0x02000000u && addr < 0x03000000u)
        return {8u, 1u, true};                              // main RAM, 16-bit bus
    if (addr >= 0x06000000u && addr < 0x07000000u)
        return {1u, 1u, true};                               // VRAM (ARM7 slot), 16-bit bus
    return {1u, 1u, false};                                  // BIOS / WRAM / IO / void: 32-bit bus
}

// Data-access memory timing (Commit C for the ARM9; Commit D for the ARM7).
// ARM9: returns the MARGINAL data cost after the code/data overlap: melonDS
// combines them as max(code+data-6, max(code,data)), so the first ~6 data cycles
// hide behind the code fetch already charged by runtime_code_cycles. Cost is in
// the active CPU's cycle units (ARM9 = 2x system). Region costs are a first cut,
// calibrated against the oracle's cyc9-at-equal-retired-index.
//
// ARM7: region-aware per melonDS's ARM7MemTimings (single 8/16-bit accesses are
// always charged at the region's N — melonDS's DataRead8/16 always index the
// nonseq slot; only 32-bit LDM/STM continuations use `sequential` for S). For
// "fast" regions (BIOS/WRAM/IO) this reproduces melonDS's LDR/STR/LDM/STM cost
// EXACTLY against the recompiler's static ARMv4T base table (verified by hand:
// e.g. a fast-region LDR is base=2 + data=1 = 3, matching melonDS's
// AddCycles_CDI numC(1)+numD(1)+1 = 3). The one place the static base table
// CANNOT be matched is taken branches: melonDS charges a taken branch exactly
// JumpTo's N+S of the TARGET region (2 in a fast region), but the recompiler's
// static base is a flat, region-blind 3 ("2S+1N") for every branch — baked in
// at codegen time (recompiler/armv4t/arm_ir.cpp instr_cycle_base), not
// reachable from this bus-only hook (only bus.cpp is in scope for Commit D; see
// docs/scheduler_design.md). runtime_code_cycles is called with the CURRENT
// instruction's pc, before it's known whether/where it branches, so charging
// the refill there would double-count against the branch's own static
// "2S+1N" — see runtime_code_cycles below. kArm7DataDiscount is the resulting,
// empirically-calibrated compensation: since branches are common (spin/poll
// loops dominate early boot) and always overcosted by +1 in fast regions, and
// this hook is the only ADDITIVE lever available (can't subtract from the
// static base), a small uniform discount on the data-access cost nets the
// windowed cyc7/insn average back to the oracle. See the calibration table in
// the Commit-D changelist.
constexpr uint32_t kArm7DataDiscount = 1u;
extern "C" uint32_t runtime_mem_cycles(uint32_t addr, uint32_t width,
                                       uint32_t sequential) {
    if (g_nds_active != NDS_ARM9) {
        Arm7Region r = arm7_region(addr);
        uint32_t n32 = r.bus16 ? (r.n16 + r.s16) : r.n16;
        uint32_t s32 = r.bus16 ? (r.s16 + r.s16) : r.s16;
        // melonDS: single 8/16-bit transfers always cost N (DataRead8/16);
        // only 32-bit transfers (LDR/STR/LDM/STM) see the sequential S rate.
        uint32_t cost = (width >= 4u) ? (sequential ? s32 : n32) : r.n16;
        return (cost > kArm7DataDiscount) ? (cost - kArm7DataDiscount) : 0u;
    }
    uint32_t data;
    if (g_cp15.itcm_enable && addr < g_cp15.itcm_size)
        data = 2u;                                          // ITCM
    else if (g_cp15.dtcm_enable && addr >= g_cp15.dtcm_base &&
             addr - g_cp15.dtcm_base < g_cp15.dtcm_size)
        data = 2u;                                          // DTCM
    else if (addr >= 0x02000000u && addr < 0x03000000u)
        data = (width >= 4) ? 18u : 10u;                    // main RAM 16-bit bus
    else if (addr >= 0x03000000u && addr < 0x04000000u)
        data = 4u;                                          // shared / ARM7 WRAM 32-bit
    else
        data = 4u;                                          // I/O, VRAM, palette, ...
    // Overlap applies ONCE per instruction: the first data access hides ~6
    // cycles behind the code fetch. Subsequent (sequential) words of an LDM/STM
    // have no fetch to hide behind, so they cost the full data rate — otherwise
    // multi-word transfers (the early-boot memory clears) are undercharged.
    if (sequential) return data;
    return data > 6u ? data - 6u : 1u;
}

// Code-FETCH memory timing (Commit B for the ARM9 — see docs/scheduler_design.md
// — Commit D for the ARM7). Charged per retired instruction on the active CPU,
// on TOP of the static per-instruction base baked into the recompiler's ARMv4T
// cost table (recompiler/armv4t/arm_ir.cpp instr_cycle_base), which already
// assumes a 1-cycle sequential code fetch ("1S") for every op.
//
// ARM9: the naive ~1 cyc/insn undercount (measured -75% vs melonDS) is almost
// entirely the missing code fetch: uncached BIOS fetch is 8 ARM9 cyc ((32-bit
// nonseq base 1 + ARM9 nonseq penalty 3) x2). Uncached ARM9 has no sequential-
// fetch speedup (it is cache-line based), so we charge the region's fetch cost
// every instruction; the CP15 I-cache, once enabled, averages it down.
// Constants are a first cut, calibrated against the oracle's cyc9-at-equal-
// retired-index.
//
// ARM7: the static base's baked-in "1S" already matches melonDS's steady-state
// AddCycles_C exactly for every region that is genuinely 1-cycle-sequential —
// which is every ARM7 region EXCEPT main RAM / VRAM in ARM (32-bit) mode, where
// the 16-bit bus splits S32=S16+S16=2. So the ONLY correction needed here is
// that one region/width case (+1); everywhere else this returns 0 — adding a
// naive flat code-fetch cost on top of the ARM7's already-fast base would only
// overcharge further (measured starting point: native ~1.85 cyc/insn vs oracle
// ~1.63, i.e. ALREADY over, not under). Taken branches are also mis-costed
// (melonDS charges exactly the TARGET region's JumpTo N+S, e.g. 2 in a fast
// region, vs the static base's flat region-blind 3) but that correction is NOT
// applied here: this hook only sees the CURRENT pc, before the branch is taken
// and its target is known, so there is no way to attribute the target-region
// refill without double-counting the static base's own flat guess. See the
// data-side kArm7DataDiscount (above) for how that residual is compensated.
extern "C" uint32_t runtime_code_cycles(uint32_t pc) {
    if (g_nds_active != NDS_ARM9) {
        Arm7Region r = arm7_region(pc);
        if (!r.bus16) return 0u;                            // 32-bit-bus region: S32=S16=1
        const bool thumb = (g_cpu.cpsr & CPSR_T_BIT) != 0u;
        if (thumb) return 0u;                               // S16=1, matches the baked "1S"
        uint32_t s32 = r.s16 + r.s16;
        return (s32 > 1u) ? (s32 - 1u) : 0u;                 // ARM-mode S32 beyond the baked 1
    }
    // ARM9 code fetch is FORCED non-sequential 32-bit — melonDS: "all code
    // accesses are forced nonseq 32bit" (ARM.h:254); there is NO sequential-fetch
    // speedup on the ARM9 (RegionCodeCycles is pinned to the region's N32 cost).
    // Charge that N32 cost (post +3 non-seq penalty on every region except main
    // RAM, post x2 ARM9 clock shift) on EVERY fetch. The static instr_cycle_base
    // already bakes one sequential fetch cycle, so subtract 1 to absorb it.
    // (Per-PU-region cacheability -> flat averaged 3/1 is a later refinement; the
    // firmware MPU/cache isn't set up yet during the early-boot IPC handshake.)
    g_last_code_pc[0] = pc;
    if (g_cp15.itcm_enable && pc < g_cp15.itcm_size) return 0u;   // ITCM = 1, baked
    uint32_t n;
    if (pc >= 0xFFFF0000u)                            n = 8u;     // BIOS 32-bit: (1+3)x2
    else if (pc >= 0x02000000u && pc < 0x03000000u)  n = 18u;    // main RAM 16-bit: (8+1)x2
    else                                             n = 8u;     // WRAM/IO/OAM 32-bit: (1+3)x2
    return n > 1u ? n - 1u : 0u;                                  // absorb the baked 1S
}
