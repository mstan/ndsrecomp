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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "state.h"
#include "runtime_arm.h"
#include "io.h"
#include "wifi.h"
#include "vram.h"

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

// Tier-3 is permitted only for guest-materialized code. Track writes by
// physical backing page so mirrored virtual addresses and the two CPU views
// share one provenance generation. Reset/host image initialization does not
// mark pages; CPU stores and hardware masters converge on bus_write_* below.
constexpr uint32_t kExecPageShift = 12u;
constexpr uint32_t kExecPageSize = 1u << kExecPageShift;
std::vector<uint32_t> g_main_ram_generation;
std::vector<uint32_t> g_itcm_generation;
std::vector<uint32_t> g_dtcm_generation;
std::vector<uint32_t> g_shared_wram_generation;
std::vector<uint32_t> g_arm7_wram_generation;
std::vector<uint8_t> g_main_ram_written;
std::vector<uint8_t> g_itcm_written;
std::vector<uint8_t> g_dtcm_written;
std::vector<uint8_t> g_shared_wram_written;
std::vector<uint8_t> g_arm7_wram_written;

std::vector<uint32_t>* generation_for_ptr(const uint8_t* ptr,
                                          uint32_t* offset) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
    auto match = [&](const std::vector<uint8_t>& bytes,
                     std::vector<uint32_t>& generations) -> bool {
        const uintptr_t base = reinterpret_cast<uintptr_t>(bytes.data());
        if (p < base || p >= base + bytes.size()) return false;
        *offset = static_cast<uint32_t>(p - base);
        return true;
    };
    if (match(g_main_ram, g_main_ram_generation)) return &g_main_ram_generation;
    if (match(g_itcm, g_itcm_generation)) return &g_itcm_generation;
    if (match(g_dtcm, g_dtcm_generation)) return &g_dtcm_generation;
    if (match(g_shared_wram, g_shared_wram_generation)) return &g_shared_wram_generation;
    if (match(g_arm7_wram, g_arm7_wram_generation)) return &g_arm7_wram_generation;
    return nullptr;
}

std::vector<uint8_t>* written_for_ptr(const uint8_t* ptr, uint32_t* offset);

void note_ram_write(uint8_t* ptr, uint32_t width) {
    uint32_t offset = 0;
    std::vector<uint32_t>* generations = generation_for_ptr(ptr, &offset);
    if (!generations || width == 0u) return;
    const uint32_t first = offset >> kExecPageShift;
    const uint32_t last = (offset + width - 1u) >> kExecPageShift;
    for (uint32_t page = first; page <= last && page < generations->size(); ++page) {
        uint32_t& generation = (*generations)[page];
        if (++generation == 0u) generation = 1u;
    }
    runtime_note_code_write();
    uint32_t written_offset = 0u;
    if (std::vector<uint8_t>* written =
            written_for_ptr(ptr, &written_offset)) {
        const uint32_t end = std::min<uint32_t>(
            static_cast<uint32_t>(written->size()), written_offset + width);
        std::fill(written->begin() + written_offset,
                  written->begin() + end, uint8_t{1});
    }
}

uint32_t page_generation_for_ptr(const uint8_t* ptr) {
    uint32_t offset = 0;
    std::vector<uint32_t>* generations = generation_for_ptr(ptr, &offset);
    if (!generations) return 0u;
    const uint32_t page = offset >> kExecPageShift;
    return page < generations->size() ? (*generations)[page] : 0u;
}

std::vector<uint8_t>* written_for_ptr(const uint8_t* ptr, uint32_t* offset) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
    auto match = [&](const std::vector<uint8_t>& bytes,
                     std::vector<uint8_t>& written) -> bool {
        const uintptr_t base = reinterpret_cast<uintptr_t>(bytes.data());
        if (p < base || p >= base + bytes.size()) return false;
        *offset = static_cast<uint32_t>(p - base);
        return true;
    };
    if (match(g_main_ram, g_main_ram_written)) return &g_main_ram_written;
    if (match(g_itcm, g_itcm_written)) return &g_itcm_written;
    if (match(g_dtcm, g_dtcm_written)) return &g_dtcm_written;
    if (match(g_shared_wram, g_shared_wram_written)) return &g_shared_wram_written;
    if (match(g_arm7_wram, g_arm7_wram_written)) return &g_arm7_wram_written;
    return nullptr;
}

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
           // Firmware menu/settings job-control block. IPC callbacks and
           // editor state machines exchange command state here.
           watch_range(addr, width, 0x02356000u, 0x02356200u) ||
           watch_range(addr, width, 0x027FF800u, 0x027FF880u) ||
           // ARM9 firmware IRQ/callback stack.  Keeping this always-on makes
           // an interrupt accepted at the wrong instruction observable even
           // when the handler later reconverges in every architectural reg.
           watch_range(addr, width, 0x03003F00u, 0x03004000u) ||
           watch_range(addr, width, 0x0380F800u, 0x0380F840u) ||
           watch_range(addr, width, 0x04000130u, 0x04000138u) ||
           // Firmware settings performs a Wi-Fi MAC/RF self-test. Preserve
           // its complete MMIO tail so a wrong read can be traced to the
           // guest write that established the register value.
           watch_range(addr, width, 0x04800000u, 0x04810000u);
}

bool gba_slot_selected() {
    const bool arm7_owns = (nds_exmemcnt(0) & 0x0080u) != 0u;
    return (g_nds_active == NDS_ARM7) == arm7_owns;
}

uint16_t gba_open_bus_rom16(uint32_t addr) {
    static constexpr uint16_t decay[4] = {0xFE08u, 0x0000u, 0x0000u, 0xFFFFu};
    const int owner = (nds_exmemcnt(0) & 0x0080u) ? 1 : 0;
    const uint16_t ex = nds_exmemcnt(owner);
    return static_cast<uint16_t>(((addr >> 1u) & 0xFFFFu) |
                                 decay[(ex >> 2u) & 3u]);
}

bool gba_slot_read(uint32_t addr, uint32_t width, uint32_t* value) {
    if (addr < 0x08000000u || addr >= 0x0B000000u) return false;
    if (!gba_slot_selected()) {
        *value = 0u;
        return true;
    }
    if (addr < 0x0A000000u) {
        const uint32_t a = addr & ~1u;
        const uint16_t lo = gba_open_bus_rom16(a);
        if (width == 1u) *value = (lo >> ((addr & 1u) * 8u)) & 0xFFu;
        else if (width == 2u) *value = lo;
        else *value = uint32_t{lo} |
                      (uint32_t{gba_open_bus_rom16((addr & ~3u) + 2u)} << 16u);
    } else {
        // No GBA cartridge is inserted during firmware-menu boot.  melonDS's
        // empty-slot SRAM reads are pulled high.
        *value = width == 1u ? 0xFFu : width == 2u ? 0xFFFFu : 0xFFFFFFFFu;
    }
    return true;
}

bool gba_slot_write(uint32_t addr) {
    // Empty slot: selected writes have no effect; deselected writes are also
    // ignored.  Treat the range as mapped so it does not become a false
    // dispatch/unmapped diagnostic.
    return addr >= 0x08000000u && addr < 0x0B000000u;
}

void watch_push(uint8_t write, uint8_t width, uint32_t addr, uint32_t value) {
    if (!watch_addr(addr, width)) return;
    BusWatchEvent& e = g_watch[g_watch_w];
    e.seq = g_ring_seq;
    e.cpu = static_cast<uint8_t>(g_nds_active);
    e.cycles = g_runtime_cycles;
    const NdsEventCounts& counts = nds_event_counts();
    e.insn = g_nds_active == NDS_ARM9 ? counts.insn9 : counts.insn7;
    e.write = write;
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
    watch_push(write, width, addr, value);
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
        const uint32_t mode = nds_wramcnt() & 3u;
        if (arm9) {
            if (mode == 0) {
                const uint32_t o = addr & 0x7FFFu;
                if (o + len <= g_shared_wram.size()) return g_shared_wram.data() + o;
            } else if (mode == 1 || mode == 2) {
                const uint32_t base = mode == 1 ? 0x4000u : 0u;
                const uint32_t o = base + (addr & 0x3FFFu);
                if (o + len <= g_shared_wram.size()) return g_shared_wram.data() + o;
            }
        } else {
            if (mode == 0) {
                const uint32_t o = addr & 0xFFFFu;
                if (o + len <= g_arm7_wram.size()) return g_arm7_wram.data() + o;
            } else if (mode == 1 || mode == 2) {
                const uint32_t base = mode == 1 ? 0u : 0x4000u;
                const uint32_t o = base + (addr & 0x3FFFu);
                if (o + len <= g_shared_wram.size()) return g_shared_wram.data() + o;
            } else {
                const uint32_t o = addr & 0x7FFFu;
                if (o + len <= g_shared_wram.size()) return g_shared_wram.data() + o;
            }
        }
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
uint32_t g_last_data_addr[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};

void bus_init() {
    g_last_code_pc[0] = g_last_code_pc[1] = 0xFFFFFFFFu;
    g_last_data_addr[0] = g_last_data_addr[1] = 0xFFFFFFFFu;
    g_main_ram.assign(4u * 1024 * 1024, 0);
    g_itcm.assign(32u * 1024, 0);
    g_dtcm.assign(16u * 1024, 0);
    g_shared_wram.assign(32u * 1024, 0);
    g_arm7_wram.assign(64u * 1024, 0);
    g_arm9_bios.assign(4u * 1024, 0);
    g_arm7_bios.assign(16u * 1024, 0);
    g_main_ram_generation.assign(g_main_ram.size() / kExecPageSize, 0u);
    g_itcm_generation.assign(g_itcm.size() / kExecPageSize, 0u);
    g_dtcm_generation.assign(g_dtcm.size() / kExecPageSize, 0u);
    g_shared_wram_generation.assign(g_shared_wram.size() / kExecPageSize, 0u);
    g_arm7_wram_generation.assign(g_arm7_wram.size() / kExecPageSize, 0u);
    g_main_ram_written.assign(g_main_ram.size(), 0u);
    g_itcm_written.assign(g_itcm.size(), 0u);
    g_dtcm_written.assign(g_dtcm.size(), 0u);
    g_shared_wram_written.assign(g_shared_wram.size(), 0u);
    g_arm7_wram_written.assign(g_arm7_wram.size(), 0u);
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
    const uint8_t* ptr = nullptr;
    uint32_t len = 0;
    if (nds_video_get_region(name, &ptr, &len)) {
        *out = {ptr, len};
        return true;
    }
    return false;
}

// True for writable regions that can hold guest-copied executable code —
// the Tier-3 dirty-RAM interpreter runs from these. Branches on the active
// CPU for ARM9 ITCM (mirrored across its virtual span). Keeps the
// memory-map authority in the bus so Tier-3 can't drift from resolve().
bool bus_addr_is_writable_ram(uint32_t addr) {
    uint8_t* ptr = resolve(addr, 1u);
    uint32_t offset = 0;
    return ptr && generation_for_ptr(ptr, &offset) != nullptr;
}

bool bus_addr_has_write_provenance(uint32_t addr) {
    return bus_range_has_write_provenance(addr, 1u);
}

bool bus_range_has_write_provenance(uint32_t addr, uint32_t size) {
    if (size == 0u) return false;
    uint32_t offset = 0u;
    while (offset < size) {
        const uint32_t at = addr + offset;
        const uint32_t page_left =
            kExecPageSize - (at & (kExecPageSize - 1u));
        const uint32_t chunk = std::min(size - offset, page_left);
        uint8_t* live = resolve(at, chunk);
        uint32_t written_offset = 0u;
        std::vector<uint8_t>* written =
            live ? written_for_ptr(live, &written_offset) : nullptr;
        if (!written || written_offset + chunk > written->size() ||
            !std::all_of(written->begin() + written_offset,
                         written->begin() + written_offset + chunk,
                         [](uint8_t value) { return value != 0u; }))
            return false;
        offset += chunk;
    }
    return true;
}

uint32_t bus_exec_page_generation(uint32_t addr) {
    uint8_t* ptr = resolve(addr, 1u);
    return ptr ? page_generation_for_ptr(ptr) : 0u;
}

bool bus_live_bytes_equal(uint32_t addr, const uint8_t* expected,
                          uint32_t size) {
    if (!expected || size == 0u) return false;
    uint32_t offset = 0u;
    while (offset < size) {
        // Every writable executable backing is page-aligned and page-sized.
        // Staying within a 4 KiB virtual page also avoids crossing an ITCM or
        // RAM mirror boundary in a single resolve() call.
        const uint32_t at = addr + offset;
        const uint32_t page_left = kExecPageSize - (at & (kExecPageSize - 1u));
        const uint32_t chunk = std::min(size - offset, page_left);
        uint8_t* live = resolve(at, chunk);
        if (!live || std::memcmp(live, expected + offset, chunk) != 0)
            return false;
        offset += chunk;
    }
    return true;
}

BusExecProvenance bus_debug_exec_provenance(int cpu, uint32_t addr) {
    const NdsCpu old = g_nds_active;
    g_nds_active = cpu == 7 ? NDS_ARM7 : NDS_ARM9;
    BusExecProvenance result{};
    result.writable = bus_addr_is_writable_ram(addr);
    result.generation = bus_exec_page_generation(addr);
    result.written = result.generation != 0u;
    g_nds_active = old;
    return result;
}

uint8_t bus_debug_read8(int cpu, uint32_t addr) {
    NdsCpu old = g_nds_active;
    g_nds_active = (cpu == 7) ? NDS_ARM7 : NDS_ARM9;
    uint8_t v = 0;
    if (uint8_t* p = resolve(addr, 1)) v = *p;
    else if (nds_video_address(addr))
        v = static_cast<uint8_t>(nds_video_read(cpu == 7 ? 7 : 9, addr, 1));
    else if (nds_wifi_address(cpu, addr))
        v = static_cast<uint8_t>(nds_wifi_read(addr, 1, (nds_powercontrol7() & 2u) != 0u));
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
    else if (nds_video_address(addr)) v = nds_video_read(g_nds_active == NDS_ARM7 ? 7 : 9, addr, 4);
    else if (gba_slot_read(addr, 4, &v)) {}
    else if (nds_wifi_address(g_nds_active == NDS_ARM7 ? 7 : 9, addr))
        v = nds_wifi_read(addr, 4, (nds_powercontrol7() & 2u) != 0u);
    else if (is_io(addr)) v = io_read(addr, 4);
    else { unmapped(addr, false, 4, 0); v = 0; }
    ring_push(0, 4, addr, v);
    return v;
}

extern "C" uint16_t bus_read_u16(uint32_t addr) {
    uint16_t v;
    if (uint8_t* p = resolve(addr, 2)) { std::memcpy(&v, p, 2); }
    else { uint32_t x; if (nds_video_address(addr)) v = static_cast<uint16_t>(nds_video_read(g_nds_active == NDS_ARM7 ? 7 : 9, addr, 2));
    else if (gba_slot_read(addr, 2, &x)) v = static_cast<uint16_t>(x);
    else if (nds_wifi_address(g_nds_active == NDS_ARM7 ? 7 : 9, addr))
        v = static_cast<uint16_t>(nds_wifi_read(addr, 2, (nds_powercontrol7() & 2u) != 0u));
    else if (is_io(addr)) v = static_cast<uint16_t>(io_read(addr, 2));
    else { unmapped(addr, false, 2, 0); v = 0; } }
    ring_push(0, 2, addr, v);
    return v;
}

extern "C" uint8_t bus_read_u8(uint32_t addr) {
    uint8_t v;
    if (uint8_t* p = resolve(addr, 1)) { v = *p; }
    else { uint32_t x; if (nds_video_address(addr)) v = static_cast<uint8_t>(nds_video_read(g_nds_active == NDS_ARM7 ? 7 : 9, addr, 1));
    else if (gba_slot_read(addr, 1, &x)) v = static_cast<uint8_t>(x);
    else if (nds_wifi_address(g_nds_active == NDS_ARM7 ? 7 : 9, addr))
        v = static_cast<uint8_t>(nds_wifi_read(addr, 1, (nds_powercontrol7() & 2u) != 0u));
    else if (is_io(addr)) v = static_cast<uint8_t>(io_read(addr, 1));
    else { unmapped(addr, false, 1, 0); v = 0; } }
    ring_push(0, 1, addr, v);
    return v;
}

extern "C" void bus_write_u32(uint32_t addr, uint32_t val) {
    ring_push(1, 4, addr, val);
    if (uint8_t* p = resolve(addr, 4)) {
        std::memcpy(p, &val, 4);
        note_ram_write(p, 4u);
    }
    else if (nds_video_address(addr)) nds_video_write(g_nds_active == NDS_ARM7 ? 7 : 9, addr, val, 4);
    else if (gba_slot_write(addr)) {}
    else if (nds_wifi_address(g_nds_active == NDS_ARM7 ? 7 : 9, addr))
        nds_wifi_write(addr, val, 4, (nds_powercontrol7() & 2u) != 0u);
    else if (is_io(addr)) io_write(addr, val, 4);
    else unmapped(addr, true, 4, val);
}

extern "C" void bus_write_u16(uint32_t addr, uint16_t val) {
    ring_push(1, 2, addr, val);
    if (uint8_t* p = resolve(addr, 2)) {
        std::memcpy(p, &val, 2);
        note_ram_write(p, 2u);
    }
    else if (nds_video_address(addr)) nds_video_write(g_nds_active == NDS_ARM7 ? 7 : 9, addr, val, 2);
    else if (gba_slot_write(addr)) {}
    else if (nds_wifi_address(g_nds_active == NDS_ARM7 ? 7 : 9, addr))
        nds_wifi_write(addr, val, 2, (nds_powercontrol7() & 2u) != 0u);
    else if (is_io(addr)) io_write(addr, val, 2);
    else unmapped(addr, true, 2, val);
}

extern "C" void bus_write_u8(uint32_t addr, uint8_t val) {
    ring_push(1, 1, addr, val);
    if (uint8_t* p = resolve(addr, 1)) {
        *p = val;
        note_ram_write(p, 1u);
    }
    else if (nds_video_address(addr)) nds_video_write(g_nds_active == NDS_ARM7 ? 7 : 9, addr, val, 1);
    else if (gba_slot_write(addr)) {}
    else if (nds_wifi_address(g_nds_active == NDS_ARM7 ? 7 : 9, addr))
        nds_wifi_write(addr, val, 1, (nds_powercontrol7() & 2u) != 0u);
    else if (is_io(addr)) io_write(addr, val, 1);
    else unmapped(addr, true, 1, val);
}

uint32_t bus_device_read32(int cpu, uint32_t addr) {
    const NdsCpu old = g_nds_active;
    g_nds_active = cpu == 7 ? NDS_ARM7 : NDS_ARM9;
    const uint32_t value = bus_read_u32(addr);
    g_nds_active = old;
    return value;
}

void bus_device_write32(int cpu, uint32_t addr, uint32_t value) {
    const NdsCpu old = g_nds_active;
    g_nds_active = cpu == 7 ? NDS_ARM7 : NDS_ARM9;
    bus_write_u32(addr, value);
    g_nds_active = old;
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
struct Arm7Region { uint32_t n16, s16; uint8_t bus_width; };
inline Arm7Region arm7_region(uint32_t addr) {
    if (addr >= 0x02000000u && addr < 0x03000000u)
        return {8u, 1u, 16u};                              // main RAM, 16-bit bus
    if (addr >= 0x06000000u && addr < 0x07000000u)
        return {1u, 1u, 16u};                              // VRAM (ARM7 slot), 16-bit bus
    if (addr >= 0x04800000u && addr < 0x04810000u) {
        // Powered-off Wi-Fi is a fast 32-bit void region. Powered on, its two
        // 32 KiB halves are independent 16-bit buses configured by
        // WIFIWAITCNT (melonDS NDS::UpdateWifiTimings).
        if (!(nds_powercontrol7() & 0x0002u)) return {1u, 1u, 32u};
        static constexpr uint8_t n[4] = {10u, 8u, 6u, 18u};
        const uint16_t wait = nds_wifiwaitcnt();
        if (addr < 0x04808000u)
            return {n[wait & 3u], (wait & 0x0004u) ? 4u : 6u, 16u};
        return {n[(wait >> 3u) & 3u], (wait & 0x0020u) ? 4u : 10u, 16u};
    }
    if (addr >= 0x08000000u && addr < 0x0A000000u) {
        // GBA ROM timings apply only while ARM7 owns the slot (EXMEMCNT.7).
        const uint16_t ex = nds_exmemcnt(1);
        if (!(ex & 0x0080u)) return {1u, 1u, 32u};
        static constexpr uint8_t n[4] = {10u, 8u, 6u, 18u};
        return {n[(ex >> 2u) & 3u], (ex & 0x10u) ? 4u : 6u, 16u};
    }
    if (addr >= 0x0A000000u && addr < 0x0B000000u) {
        const uint16_t ex = nds_exmemcnt(1);
        if (!(ex & 0x0080u)) return {1u, 1u, 32u};
        static constexpr uint8_t n[4] = {10u, 8u, 6u, 18u};
        return {n[ex & 3u], n[ex & 3u], 8u};
    }
    return {1u, 1u, 32u};                                  // BIOS / WRAM / IO / void
}

inline uint32_t arm7_n32(const Arm7Region& r) {
    return r.bus_width == 32u ? r.n16
         : r.bus_width == 16u ? r.n16 + r.s16
                              : r.n16 + 3u * r.s16;
}
inline uint32_t arm7_s32(const Arm7Region& r) {
    return r.bus_width == 32u ? r.s16
         : r.bus_width == 16u ? 2u * r.s16
                              : 4u * r.s16;
}

enum class Arm9CodeTiming : uint8_t { Itcm, Cached, MainRam, Other };
struct Arm9CodeTimingCache {
    uint32_t page = 0xFFFFFFFFu;
    uint32_t cp15_generation = 0u;
    Arm9CodeTiming timing = Arm9CodeTiming::Other;
};
Arm9CodeTimingCache g_arm9_code_timing{};

Arm9CodeTiming arm9_code_timing(uint32_t addr) {
    const uint32_t page = addr & ~0xFFFu;
    if (g_arm9_code_timing.page == page &&
        g_arm9_code_timing.cp15_generation == g_cp15_timing_generation)
        return g_arm9_code_timing.timing;
    Arm9CodeTiming timing;
    if (g_cp15.itcm_enable && page < g_cp15.itcm_size)
        timing = Arm9CodeTiming::Itcm;
    else if (cp15_code_cacheable(page))
        timing = Arm9CodeTiming::Cached;
    else if (page >= 0x02000000u && page < 0x03000000u)
        timing = Arm9CodeTiming::MainRam;
    else
        timing = Arm9CodeTiming::Other;
    g_arm9_code_timing = {page, g_cp15_timing_generation, timing};
    return timing;
}

// ARM9 GBA-slot timings are selected dynamically by EXMEMCNT.  melonDS feeds
// these through SetARM9RegionTimings(), including the ARM946E-S three-cycle
// non-sequential CPU penalty, and then shifts bus timings into the ARM9's 2x
// clock domain in ARMv5::UpdateRegionTimings().  Keep this separate from the
// generic uncached fallback: the slot's waitstate fields can change at runtime
// and the unselected CPU sees a fast 32-bit void region instead.
inline uint32_t arm9_gba_slot_cycles(uint32_t addr, uint32_t width,
                                     bool sequential) {
    const bool arm9_owns = (nds_exmemcnt(0) & 0x0080u) == 0u;
    if (!arm9_owns) {
        // SetARM9RegionTimings(..., region=void, buswidth=32, N=1, S=1).
        return sequential && width >= 4u ? 2u : 8u;
    }

    static constexpr uint8_t n[4] = {10u, 8u, 6u, 18u};
    const uint16_t ex = nds_exmemcnt(0);
    if (addr < 0x0A000000u) {
        const uint32_t n16 = n[(ex >> 2u) & 3u];
        const uint32_t s16 = (ex & 0x0010u) ? 4u : 6u;
        if (sequential && width >= 4u)
            return (2u * s16) << 1u;              // S32, then ARM9 x2
        const uint32_t nonseq = width >= 4u ? n16 + s16 : n16;
        return (nonseq + 3u) << 1u;               // N32/N16 + CPU N, x2
    }

    // melonDS currently passes buswidth=8 to SetARM9RegionTimings(), whose
    // non-16-bit path models the SRAM window as a 32-bit bus: N32=N16 and
    // S32=S16.  Reproduce that effective model exactly.
    const uint32_t ram_n = n[ex & 3u];
    if (sequential && width >= 4u) return ram_n << 1u;
    return (ram_n + 3u) << 1u;
}

// Data-access memory timing (Commit C for the ARM9; Commit D for the ARM7).
// ARM9: returns the RAW region data cost (numD). The code/data overlap
// (melonDS: max(numC+numD-6, max(numC,numD))) is applied ONCE per
// instruction by arm9_cycle_combine, over the SUM of every call this
// instruction makes (e.g. one per register for LDM/STM) — never here, so
// this always returns the true per-access cost regardless of how many
// calls a single instruction makes or whether a given call is the
// sequential continuation of an LDM/STM. Cost is in the active CPU's
// cycle units (ARM9 = 2x system). Region costs are a first cut,
// calibrated against the oracle's cyc9-at-equal-retired-index.
//
// ARM7: region-aware per melonDS's ARM7MemTimings (single 8/16-bit accesses are
// always charged at the region's N — melonDS's DataRead8/16 always index the
// nonseq slot; only 32-bit LDM/STM continuations use `sequential` for S). For
// "fast" regions (BIOS/WRAM/IO) this reproduces melonDS's LDR/STR/LDM/STM cost
// EXACTLY against the recompiler's static ARMv4T base table (verified by hand:
// e.g. a fast-region LDR is base=2 + data=1 = 3, matching melonDS's
// AddCycles_CDI numC(1)+numD(1)+1 = 3). The one place the static base table
// ordinary instruction costs are additive. Taken branches instead use the
// target-region-dependent arm7_refill_cycles() at the actual transfer site,
// so no data-side calibration discount is needed.
extern "C" uint32_t runtime_mem_cycles(uint32_t addr, uint32_t width,
                                       uint32_t sequential) {
    g_last_data_addr[g_nds_active == NDS_ARM9 ? 0 : 1] = addr;
    if (g_nds_active != NDS_ARM9) {
        Arm7Region r = arm7_region(addr);
        uint32_t n32 = arm7_n32(r);
        uint32_t s32 = arm7_s32(r);
        // melonDS: single 8/16-bit transfers always cost N (DataRead8/16);
        // only 32-bit transfers (LDR/STR/LDM/STM) see the sequential S rate.
        return (width >= 4u) ? (sequential ? s32 : n32) : r.n16;
    }
    uint32_t data;
    if (g_cp15.itcm_enable && addr < g_cp15.itcm_size)
        data = 1u;                                          // ITCM
    else if (g_cp15.dtcm_enable && addr >= g_cp15.dtcm_base &&
             addr - g_cp15.dtcm_base < g_cp15.dtcm_size)
        data = 1u;                                          // DTCM
    else if (cp15_data_cacheable(addr))
        data = sequential ? 1u : 3u;                        // averaged D-cache
    else if (addr >= 0x08000000u && addr < 0x0B000000u)
        data = arm9_gba_slot_cycles(addr, width, sequential != 0u);
    else {
        const bool main_ram = addr >= 0x02000000u && addr < 0x03000000u;
        const bool pal_vram = addr >= 0x05000000u && addr < 0x07000000u;
        if (sequential)
            data = (main_ram || pal_vram) ? 4u : 2u;
        else if (width >= 4u)
            data = main_ram ? 18u : (pal_vram ? 10u : 8u);
        else
            data = main_ram ? 16u : 8u;
    }
    return data;                // raw numD in ARM9 clock units
}

// Code-FETCH memory timing (Commit B for the ARM9 — see docs/scheduler_design.md
// — Commit D for the ARM7). Charged per retired instruction on the active CPU,
// on TOP of the static per-instruction base baked into the recompiler's ARMv4T
// cost table (recompiler/armv4t/arm_ir.cpp instr_cycle_base), which already
// assumes a 1-cycle sequential code fetch ("1S") for every op.
//
// ARM9: returns the RAW numC (this instruction's own code-fetch cost, or a
// branch/PC-write TARGET's, when called from arm_codegen.cpp's pipeline-
// refill term) — melonDS: all code accesses are forced nonseq 32-bit
// (ARM.h:254), so numC is the region's N32 cost (post +3 ARM9 nonseq
// penalty on every region except main RAM, post x2 ARM9 clock shift), e.g.
// uncached BIOS fetch = 8 ARM9 cyc. arm9_cycle_combine (bottom of this
// file) is the ONLY place that folds numC together with numD/numI per
// melonDS's exact AddCycles_C/CI/CD/CDI — this function must never predict
// or absorb any part of that combine itself. Constants are a first cut,
// calibrated against the oracle's cyc9-at-equal-retired-index.
//
// ARM7: the static base's baked-in "1S" already matches melonDS's steady-state
// AddCycles_C exactly for every region that is genuinely 1-cycle-sequential —
// which is every ARM7 region EXCEPT main RAM / VRAM in ARM (32-bit) mode, where
// the 16-bit bus splits S32=S16+S16=2. So the ONLY correction needed here is
// that one region/width case (+1); everywhere else this returns 0 — adding a
// naive flat code-fetch cost on top of the ARM7's already-fast base would only
// overcharge further (measured starting point: native ~1.85 cyc/insn vs oracle
// ~1.63, i.e. ALREADY over, not under). Taken branches bypass this current-PC
// correction and charge the target region through arm7_refill_cycles().
extern "C" uint32_t runtime_code_cycles(uint32_t pc) {
    if (g_nds_active != NDS_ARM9) {
        g_last_code_pc[1] = pc;
        Arm7Region r = arm7_region(pc);
        if (r.bus_width == 32u) return 0u;                  // 32-bit-bus region: S32=S16=1
        const bool thumb = (g_cpu.cpsr & CPSR_T_BIT) != 0u;
        if (thumb) return 0u;                               // S16=1, matches the baked "1S"
        uint32_t s32 = r.s16 + r.s16;
        return (s32 > 1u) ? (s32 - 1u) : 0u;                 // ARM-mode S32 beyond the baked 1
    }
    // ARM9 code fetch is FORCED non-sequential 32-bit — melonDS: "all code
    // accesses are forced nonseq 32bit" (ARM.h:254); there is NO sequential-fetch
    // speedup on the ARM9 (RegionCodeCycles is pinned to the region's N32 cost).
    // Charge that N32 cost (post +3 non-seq penalty on every region except main
    // RAM, post x2 ARM9 clock shift) on EVERY fetch — the full RAW numC; the
    // combine (arm9_cycle_combine) is what folds this together with numD/numI,
    // not this function.
    // (Per-PU-region cacheability -> flat averaged 3/1 is a later refinement; the
    // firmware MPU/cache isn't set up yet during the early-boot IPC handshake.)
    g_last_code_pc[0] = pc;
    // Thumb "free second half" (melonDS numC=(R15&2)?0:CodeCycles): the ARM9
    // fetches 32 bits even in Thumb, so the odd-halfword instruction shares its
    // predecessor's fetch and costs ZERO. Halves the cost of Thumb loops (e.g.
    // the firmware's Thumb BIOS IRQ-wait spin during the IPC handshake). This is
    // a true raw-zero (not a baseline absorption) — kept as-is.
    const bool thumb = (g_cpu.cpsr & CPSR_T_BIT) != 0u;
    if (thumb && (pc & 2u)) return 0u;
    const Arm9CodeTiming timing = arm9_code_timing(pc);
    if (timing == Arm9CodeTiming::Itcm) return 1u;   // ITCM
    // I-cache-served region: melonDS degrades the fetch to a flat averaged cost
    // (kCodeCacheTiming=3 at a 32-byte line boundary, else 1; un-shifted). This
    // is what makes the firmware's BIOS spin loop cheap during the IPC handshake.
    if (timing == Arm9CodeTiming::Cached) {
        // Execute prefetches ahead of the instruction being retired.  The
        // CodeCycles consumed by this instruction were set by CodeRead32 at
        // current+4 in Thumb or current+8 in ARM state; cache-line timing is
        // keyed to that fetch address, not the architectural instruction PC.
        const uint32_t fetch_addr = pc + (thumb ? 4u : 8u);
        return (fetch_addr & 0x1Fu) ? 1u : 3u;
    }
    if (timing == Arm9CodeTiming::MainRam)           return 18u;  // main RAM N32=(8+1), x2
    return 8u;                                                    // WRAM/IO/OAM 32-bit: (1+3)x2
}

// ARM7 pipeline refill for a control transfer. `target` carries the
// destination instruction-set state in bit 0. JumpTo fetches N+S from the
// target region; ARM-mode fetches split on the 16-bit main-RAM/VRAM bus.
extern "C" uint32_t arm7_refill_cycles(uint32_t target) {
    const bool thumb = (target & 1u) != 0u;
    const Arm7Region r = arm7_region(target & ~1u);
    if (thumb) return r.n16 + r.s16;
    const uint32_t n32 = arm7_n32(r);
    const uint32_t s32 = arm7_s32(r);
    return n32 + s32;
}

// ARM9 pipeline refill cost for a control transfer. `target` carries the
// destination instruction-set state in bit 0 (set = Thumb), so this helper is
// independent of the caller's still-current CPSR.T. melonDS ARMv5::JumpTo
// fetches one 32-bit word for an even-half Thumb target and two words for ARM
// or an odd-half Thumb target.
extern "C" uint32_t arm9_refill_cycles(uint32_t target) {
    const bool thumb = (target & 1u) != 0u;
    const uint32_t addr = target & ~1u;
    const uint32_t words = (thumb && !(addr & 2u)) ? 1u : 2u;
    const Arm9CodeTiming timing = arm9_code_timing(addr);
    if (timing == Arm9CodeTiming::Itcm)
        return words;
    if (timing == Arm9CodeTiming::Cached) {
        uint32_t cycles = 3u;                              // branch fetch
        if (words == 2u) {
            const uint32_t second = addr + (thumb ? 2u : 4u);
            cycles += (second & 0x1Fu) ? 1u : 3u;
        }
        return cycles;
    }
    const uint32_t cc = timing == Arm9CodeTiming::MainRam ? 18u : 8u;
    return words * cc;
}

// ARM9 per-instruction cycle COMBINE — melonDS's exact AddCycles_C/CI/CD/CDI
// (ARM.h:266-303). See runtime_arm.h for the full contract; called from every
// codegen-emitted tick site's ARM9 branch (arm_codegen.cpp), never for ARM7.
// `has_data` selects CD/CDI (loads/stores: no internal cycle, code/data
// overlap) vs C/CI (everything else: internal cycles add flat, no overlap).
// int math for the -6 (only the has_data branch can go negative before the
// outer max), floored at 0 defensively though the max(numC,numD) term already
// guarantees a non-negative result whenever numC/numD themselves are.
extern "C" uint32_t arm9_cycle_combine(uint32_t numC, uint32_t numD,
                                       uint32_t numI, uint32_t has_data) {
    if (has_data) {
        int overlap = static_cast<int>(numC) + static_cast<int>(numD) - 6;
        int floor_v = static_cast<int>(numC) > static_cast<int>(numD)
                          ? static_cast<int>(numC) : static_cast<int>(numD);
        int m = overlap > floor_v ? overlap : floor_v;
        return m > 0 ? static_cast<uint32_t>(m) : 0u;
    }
    return numI ? (numC + numI) : numC;
}

// ARM7 memory instructions use melonDS ARMv4::AddCycles_CD/CDI.  The old
// flat `base + numD` expression is exact on fast same-bus regions, but it
// overcounts accesses between BIOS/WRAM code and the 16-bit main-RAM bus
// because those buses overlap.  Reconstruct the raw non-sequential code cost
// and apply the source model once over the instruction's accumulated numD.
extern "C" uint32_t arm7_cycle_combine(uint32_t flat_cycles, uint32_t numD,
                                        uint32_t is_load,
                                        uint32_t has_internal) {
    if (numD == 0u) {
        // The portable flat cost contains one baked sequential code cycle.
        // Replace that single cycle with melonDS's real S fetch for C-class
        // instructions, or its N fetch for CI-class instructions. CD/CDI
        // below already return the complete code+data cost and must not
        // receive a second correction from the retire observer.
        const uint32_t pc = g_last_code_pc[1];
        const Arm7Region code = arm7_region(pc);
        const bool thumb = (g_cpu.cpsr & CPSR_T_BIT) != 0u;
        const uint32_t seqC = thumb ? code.s16 : arm7_s32(code);
        if (!has_internal) return flat_cycles + seqC - 1u;
        const uint32_t nonseqC = thumb ? code.n16 : arm7_n32(code);
        return flat_cycles + nonseqC - 1u;
    }

    const uint32_t pc = g_last_code_pc[1];
    const uint32_t data_addr = g_last_data_addr[1];
    const Arm7Region code = arm7_region(pc);
    const bool thumb = (g_cpu.cpsr & CPSR_T_BIT) != 0u;
    uint32_t numC = thumb ? code.n16 : arm7_n32(code);
    const bool code_main = pc >= 0x02000000u && pc < 0x03000000u;
    const bool data_main = data_addr >= 0x02000000u && data_addr < 0x03000000u;

    if (data_main) {
        if (code_main) return numC + numD;
        if (is_load) ++numC;
        const uint32_t overlap = (numC + numD > 3u) ? numC + numD - 3u : 0u;
        return std::max(overlap, std::max(numC, numD));
    }
    if (code_main) {
        if (is_load) ++numD;
        const uint32_t overlap = (numC + numD > 3u) ? numC + numD - 3u : 0u;
        return std::max(overlap, std::max(numC, numD));
    }
    return numC + numD + (is_load ? 1u : 0u);
}
