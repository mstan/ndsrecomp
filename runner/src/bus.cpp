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

inline void ring_push(uint8_t write, uint8_t width, uint32_t addr, uint32_t value) {
    BusEvent& e = g_ring[g_ring_w];
    e.seq = ++g_ring_seq;
    e.cpu = static_cast<uint8_t>(g_nds_active);
    e.write = write;
    e.width = width;
    e.addr = addr;
    e.value = value;
    g_ring_w = (g_ring_w + 1u) % kRingSize;
}

// Resolve an address to a backing pointer for the active CPU. Returns
// nullptr for unmapped / I-O addresses (handled separately). `len` is the
// access width for bounds checks.
uint8_t* resolve(uint32_t addr, uint32_t len) {
    const bool arm9 = (g_nds_active == NDS_ARM9);

    if (arm9) {
        // ITCM: mirrored across [0, 0x02000000) when enabled.
        if (g_cp15.itcm_enable && g_cp15.itcm_size &&
            addr < 0x02000000u && (addr & (0x02000000u - 1u)) < g_cp15.itcm_size) {
            uint32_t o = addr & (g_cp15.itcm_size - 1u);
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

void bus_init() {
    g_main_ram.assign(4u * 1024 * 1024, 0);
    g_itcm.assign(32u * 1024, 0);
    g_dtcm.assign(16u * 1024, 0);
    g_shared_wram.assign(32u * 1024, 0);
    g_arm7_wram.assign(64u * 1024, 0);
    g_arm9_bios.assign(4u * 1024, 0);
    g_arm7_bios.assign(16u * 1024, 0);
    g_ring_w = 0;
    g_ring_seq = 0;
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

// Region-aware access cost. Real DS waitstates land with the timing pass;
// for now charge a flat cost so the cycle clock advances monotonically.
extern "C" uint32_t runtime_mem_cycles(uint32_t addr, uint32_t width,
                                       uint32_t sequential) {
    (void)addr; (void)width; (void)sequential;
    return 1u;
}
