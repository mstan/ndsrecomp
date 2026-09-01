// cp15.cpp — ARM9 CP15 (system control coprocessor) model.
//
// Backs runtime_coproc_{write,read,cdp}. On the DS the only coprocessor
// is the ARM9's CP15: MPU regions, ITCM/DTCM placement, cache control,
// and the control register (which selects high exception vectors). CP15
// state is load-bearing for the bus — TCM placement changes what an
// address means — so the relevant fields are published into g_cp15.
//
// Register map + reset behavior per GBATEK ("ARM9 CP15 System Control
// Coprocessor") and the ARM946E-S TRM; implemented clean-room from those
// docs (see docs/references.md).

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "state.h"
#include "savestate.h"
#include "runtime_arm.h"
#include "io.h"

Cp15State g_cp15 = {};
uint32_t g_cp15_timing_generation = 1u;

namespace {

// MPU protection-region registers (c6,c0..c7) and assorted cache/access
// registers we accept and store but do not yet act on. Behavior that the
// bus needs (control + TCM) is mirrored into g_cp15.
uint32_t g_mpu_region[8] = {};
uint64_t g_mpu_region_base[8] = {};
uint64_t g_mpu_region_end[8] = {};
uint32_t g_cache_cfg[8]  = {};   // c2 (cachability) / c3 (bufferability)
uint32_t g_access_perm[8] = {};  // c5 (access permissions)

// TCM size field → bytes. ARM946E-S: region size = 512 bytes << N, where
// N = bits [5:1] of the c9,c1 register (valid N: 3..23 → 4KB..4GB).
uint32_t tcm_bytes(uint32_t reg) {
    uint32_t n = (reg >> 1) & 0x1Fu;
    return 512u << n;
}

void apply_control(uint32_t v) {
    g_cp15.control      = v;
    g_cp15.high_vectors = (v & (1u << 13)) != 0;
    g_cp15.dtcm_enable  = (v & (1u << 16)) != 0;
    g_cp15.itcm_enable  = (v & (1u << 18)) != 0;
}

void set_mpu_region(uint32_t index, uint32_t reg) {
    g_mpu_region[index] = reg;
    if (!(reg & 1u)) {
        g_mpu_region_base[index] = 0u;
        g_mpu_region_end[index] = 0u;
        return;
    }
    // ARM946E-S MPU encoding: region size is 2^(N+1) bytes (minimum
    // architectural N=11 => 4 KiB), unlike the c9 TCM size encoding's
    // 512<<N formula.  Use 64-bit math so the 4 GiB encoding remains
    // representable, and align the programmed base down to the region size.
    const uint32_t n = (reg >> 1u) & 0x1Fu;
    const uint64_t size = uint64_t{1} << (n + 1u);
    const uint64_t base = uint64_t{reg & 0xFFFFF000u} & ~(size - 1u);
    g_mpu_region_base[index] = base;
    g_mpu_region_end[index] = base + size;
}

bool mpu_region_contains(uint32_t index, uint32_t addr) {
    const uint64_t end = g_mpu_region_end[index];
    const uint64_t a = addr;
    return end != 0u && a >= g_mpu_region_base[index] && a < end;
}

// ── Page-granular cacheability class bitmaps (beads-yjp.70 phase 2 B) ──
// cp15_data_cacheable() used to walk all eight MPU regions on EVERY
// non-TCM ARM9 data access (runtime_mem_cycles calls it once per guest
// load/store): 1.0% + 0.5% of emu-thread self time in the Kanden host
// sampler, plus the out-of-line call itself.
//
// The walk's answer is constant over a 4 KiB page whenever every ENABLED
// MPU region is at least 4 KiB and 4 KiB-aligned — which the ARM946E-S
// architecturally requires (minimum region size N=11 => 2^12 bytes) and
// which the region base is aligned to by set_mpu_region().  So the whole
// answer is precomputed into a 1-bit-per-page bitmap (1 MiB of 4 KiB
// pages = 128 KiB per bitmap) and rebuilt only when an input actually
// changes.  The inline data-timing fast path in runtime_arm.h then does
// one load + shift + test instead of a call and a region walk.
//
// g_cp15_class_ready is the honesty rail: this model permits (and the
// savestate importer can restore) sub-page region encodings that the
// bitmap CANNOT represent, and when one is present the flag stays 0 and
// every consumer falls back to the reference walk below.  The bitmaps are
// pure derived state — never saved, always rebuilt from the registers.
constexpr size_t kCp15ClassPages = size_t{1} << 20;   // 4 GiB / 4 KiB
constexpr size_t kCp15ClassBytes = kCp15ClassPages / 8u;

inline void class_bit_set(uint8_t* bm, uint64_t page, bool value) {
    const uint8_t mask = static_cast<uint8_t>(1u << (page & 7u));
    if (value) bm[page >> 3u] |= mask;
    else       bm[page >> 3u] = static_cast<uint8_t>(bm[page >> 3u] & ~mask);
}

// Fill the half-open page range [first, end) with `value`.
void class_bitmap_fill(uint8_t* bm, uint64_t first, uint64_t end, bool value) {
    for (; first < end && (first & 7u) != 0u; ++first)
        class_bit_set(bm, first, value);
    const uint64_t tail = end & ~uint64_t{7};
    if (tail > first) {
        std::memset(bm + (first >> 3u), value ? 0xFF : 0x00,
                    static_cast<size_t>((tail - first) >> 3u));
        first = tail;
    }
    for (; first < end; ++first) class_bit_set(bm, first, value);
}

// The exact inputs the walk reads. Rebuilds are skipped when none moved,
// so a burst of CP15 writes that does not change cacheability (cache
// maintenance ops, re-writing the same region register, control-register
// writes that only touch other bits) costs nothing.
struct Cp15ClassInputs {
    uint32_t control;      // masked to the bits the walk reads
    uint32_t dcache_bits;
    uint32_t icache_bits;
    uint32_t region[8];
};
constexpr uint32_t kCp15ClassControlMask = 1u | (1u << 2) | (1u << 12);
Cp15ClassInputs g_class_inputs{};
bool g_class_inputs_valid = false;

void cp15_rebuild_class_bitmaps();

}  // namespace

extern "C" {
// Bit (page & 7) of byte [page >> 3], page = addr >> 12. Exported as DATA
// so generated banks and live shards read the runner's storage directly.
uint8_t g_cp15_dcache_page[kCp15ClassBytes] = {};
uint8_t g_cp15_icache_page[kCp15ClassBytes] = {};
uint32_t g_cp15_class_ready = 0u;
}

namespace {

void cp15_rebuild_class_bitmaps() {
    const uint32_t control = g_cp15.control;
    const bool dcache_on = (control & (1u << 2)) != 0u;
    const bool icache_on = (control & (1u << 12)) != 0u;
    const bool mpu_on = (control & 1u) != 0u;
    bool exact = true;
    if (!mpu_on) {
        // MPU disabled: the walk returns the global cache-enable bit for
        // every address.
        std::memset(g_cp15_dcache_page, dcache_on ? 0xFF : 0x00,
                    kCp15ClassBytes);
        std::memset(g_cp15_icache_page, icache_on ? 0xFF : 0x00,
                    kCp15ClassBytes);
    } else {
        // No enabled region containing the address => not cacheable.
        std::memset(g_cp15_dcache_page, 0x00, kCp15ClassBytes);
        std::memset(g_cp15_icache_page, 0x00, kCp15ClassBytes);
        const uint32_t dbits = g_cache_cfg[0];
        const uint32_t ibits = g_cache_cfg[1];
        for (uint32_t i = 0; i < 8u; ++i) {
            const uint64_t base = g_mpu_region_base[i];
            const uint64_t end = g_mpu_region_end[i];
            if (end == 0u) continue;              // region disabled
            if ((base & 0xFFFu) != 0u || (end & 0xFFFu) != 0u) {
                // Sub-page region: not representable page-granularly.
                exact = false;
                break;
            }
            // Ascending index so the HIGHEST-numbered enabled region wins,
            // exactly as the reference walk's downward scan does.
            class_bitmap_fill(g_cp15_dcache_page, base >> 12u, end >> 12u,
                              dcache_on && ((dbits >> i) & 1u) != 0u);
            class_bitmap_fill(g_cp15_icache_page, base >> 12u, end >> 12u,
                              icache_on && ((ibits >> i) & 1u) != 0u);
        }
    }
    g_cp15_class_ready = exact ? 1u : 0u;
}

void cp15_class_sync() {
    Cp15ClassInputs in{};
    in.control = g_cp15.control & kCp15ClassControlMask;
    in.dcache_bits = g_cache_cfg[0];
    in.icache_bits = g_cache_cfg[1];
    for (uint32_t i = 0; i < 8u; ++i) in.region[i] = g_mpu_region[i];
    if (g_class_inputs_valid &&
        std::memcmp(&in, &g_class_inputs, sizeof in) == 0)
        return;
    g_class_inputs = in;
    g_class_inputs_valid = true;
    cp15_rebuild_class_bitmaps();
}

inline bool class_page_bit(const uint8_t* bm, uint32_t addr) {
    const uint32_t page = addr >> 12u;
    return ((bm[page >> 3u] >> (page & 7u)) & 1u) != 0u;
}

}  // namespace

void cp15_reset() {
    g_cp15 = {};
    // ARM9 powers up with the high exception base (the DS ARM9 BIOS lives
    // at 0xFFFF0000); the BIOS reasserts this via the control register.
    g_cp15.high_vectors = true;
    g_cp15.control = (1u << 13);
    if (++g_cp15_timing_generation == 0u) g_cp15_timing_generation = 1u;
    g_class_inputs_valid = false;
    cp15_class_sync();
    bus_fast_refresh();
}

// True if code fetches from `addr` are served by the ARM9 instruction cache:
// the I-cache is enabled (C1 bit12) AND `addr` falls in an MPU protection region
// whose instruction-cacheable bit (c2,c0,1) is set. Highest-numbered enabled
// region wins (ARM946E-S priority). Mirrors melonDS's per-PU-region cacheability
// (pu & 0x40), which the code-fetch timing degrades to a flat averaged cost.
// The ORIGINAL per-access MPU region walks, retained verbatim under a
// _reference name. They are (a) the fallback whenever the page bitmaps
// cannot exactly represent the programmed regions and (b) the oracle
// runner/tests/mem_timing_test.cpp sweeps the bitmap against.
bool cp15_code_cacheable_reference(uint32_t addr) {
    if (!(g_cp15.control & (1u << 12))) return false;      // I-cache disabled
    if (!(g_cp15.control & 1u)) return true;               // MPU disabled: global cache bit
    const uint32_t icache_bits = g_cache_cfg[1];           // c2,c0,1 = instr cacheable
    for (int i = 7; i >= 0; --i) {
        if (mpu_region_contains(static_cast<uint32_t>(i), addr))
            return (icache_bits >> i) & 1u;
    }
    return false;
}

bool cp15_data_cacheable_reference(uint32_t addr) {
    if (!(g_cp15.control & (1u << 2))) return false;       // D-cache disabled
    if (!(g_cp15.control & 1u)) return true;               // MPU disabled: global cache bit
    const uint32_t dcache_bits = g_cache_cfg[0];           // c2,c0,0 = data cacheable
    for (int i = 7; i >= 0; --i) {
        if (mpu_region_contains(static_cast<uint32_t>(i), addr))
            return (dcache_bits >> i) & 1u;
    }
    return false;
}

bool cp15_code_cacheable(uint32_t addr) {
    if (g_cp15_class_ready) return class_page_bit(g_cp15_icache_page, addr);
    return cp15_code_cacheable_reference(addr);
}

bool cp15_data_cacheable(uint32_t addr) {
    if (g_cp15_class_ready) return class_page_bit(g_cp15_dcache_page, addr);
    return cp15_data_cacheable_reference(addr);
}

void cp15_class_rebuild_for_test() { cp15_class_sync(); }

uint32_t cp15_debug_mpu_region(unsigned index) {
    return index < 8u ? g_mpu_region[index] : 0u;
}
uint32_t cp15_debug_cache_cfg(unsigned index) {
    return index < 8u ? g_cache_cfg[index] : 0u;
}

extern "C" void runtime_coproc_write(uint32_t cp_num, uint32_t op1,
                                     uint32_t crn, uint32_t crm,
                                     uint32_t op2, uint32_t value) {
    if (cp_num != 15) {
        std::fprintf(stderr, "[cp15] write to unexpected coproc p%u "
                     "(c%u,c%u,%u) = 0x%08X\n", cp_num, crn, crm, op2, value);
        return;
    }
    // ARM946E-S WFI encodings used by Nintendo's BIOS. melonDS maps both
    // CP15 IDs 0x704 and 0x782 to Halt(1).
    if (crn == 7u && ((crm == 0u && op2 == 4u) ||
                      (crm == 8u && op2 == 2u))) {
        nds_cpu_enter_halt(0);
        return;
    }
    if (crn == 1u || crn == 2u || crn == 6u || crn == 9u)
        if (++g_cp15_timing_generation == 0u)
            g_cp15_timing_generation = 1u;
    switch (crn) {
        case 1:  // control register (c1,c0,0)
            if (crm == 0 && op2 == 0) {
                apply_control(value);
                bus_fast_refresh();   // ITCM/DTCM enable bits live here
            }
            break;
        case 2:  // cachability bits
            if (crm == 0) g_cache_cfg[op2 & 7] = value;
            break;
        case 3:  // write-bufferability
            g_cache_cfg[4 + (op2 & 3)] = value;
            break;
        case 5:  // access permissions
            g_access_perm[op2 & 7] = value;
            break;
        case 6:  // protection-region base/size (c6,c0..c7,0)
            set_mpu_region(crm & 7u, value);
            break;
        case 7:  // cache/write-buffer ops, wait-for-interrupt — no state
            break;
        case 9:  // TCM region registers (c9,c1,x)
            if (crm == 1 && op2 == 0) {          // DTCM base/size
                g_cp15.dtcm_base = value & 0xFFFFF000u;
                g_cp15.dtcm_size = tcm_bytes(value);
                bus_fast_refresh();
                std::fprintf(stderr, "[cp15] DTCM region: base=%08X vsize=%u\n",
                             g_cp15.dtcm_base, g_cp15.dtcm_size);
            } else if (crm == 1 && op2 == 1) {   // ITCM size (base = 0)
                g_cp15.itcm_size = tcm_bytes(value);
                bus_fast_refresh();
                std::fprintf(stderr, "[cp15] ITCM region: base=0 vsize=%u\n",
                             g_cp15.itcm_size);
            }
            break;
        default:
            // Unhandled register — log once so a real dependency surfaces.
            std::fprintf(stderr, "[cp15] write c%u,c%u,%u = 0x%08X "
                         "(unmodeled)\n", crn, crm, op2, value);
            break;
    }
    // Control (c1), cacheability (c2) and the protection regions (c6) are
    // the only inputs to the page-granular cacheability bitmaps; the sync
    // is a no-op unless one actually moved.
    if (crn == 1u || crn == 2u || crn == 6u) cp15_class_sync();
}

extern "C" uint32_t runtime_coproc_read(uint32_t cp_num, uint32_t op1,
                                        uint32_t crn, uint32_t crm,
                                        uint32_t op2) {
    if (cp_num != 15) return 0;
    switch (crn) {
        case 0:  // ID registers — report ARM946E-S main ID (GBATEK).
            return 0x41059461u;
        case 1:
            return g_cp15.control;
        case 2:
            return g_cache_cfg[op2 & 7];
        case 3:
            return g_cache_cfg[4 + (op2 & 3)];
        case 5:
            return g_access_perm[op2 & 7];
        case 6:
            return g_mpu_region[crm & 7];
        case 9:
            if (crm == 1 && op2 == 0)
                return g_cp15.dtcm_base | ((g_cp15.dtcm_size ? 1u : 0u));
            return 0;
        default:
            return 0;
    }
}

extern "C" void runtime_coproc_cdp(uint32_t cp_num, uint32_t op1,
                                   uint32_t crn, uint32_t crm, uint32_t op2) {
    std::fprintf(stderr, "[cp15] CDP p%u (c%u,c%u,%u) — no-op\n",
                 cp_num, crn, crm, op2);
}

void cp15_savestate_export(NdsCp15SaveState* out) {
    if (!out) return;
    out->visible = g_cp15;
    out->timing_generation = g_cp15_timing_generation;
    for (uint32_t i = 0; i < 8u; ++i) {
        out->mpu_region[i] = g_mpu_region[i];
        out->cache_cfg[i] = g_cache_cfg[i];
        out->access_perm[i] = g_access_perm[i];
    }
}

bool cp15_savestate_import(const NdsCp15SaveState& in, std::string* error) {
    (void)error;
    g_cp15 = in.visible;
    g_cp15_timing_generation = in.timing_generation
        ? in.timing_generation
        : 1u;
    for (uint32_t i = 0; i < 8u; ++i) {
        g_mpu_region[i] = in.mpu_region[i];
        g_cache_cfg[i] = in.cache_cfg[i];
        g_access_perm[i] = in.access_perm[i];
        set_mpu_region(i, g_mpu_region[i]);
    }
    g_class_inputs_valid = false;
    cp15_class_sync();
    bus_fast_refresh();
    return true;
}
