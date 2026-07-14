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

#include "state.h"
#include "runtime_arm.h"
#include "io.h"

Cp15State g_cp15 = {};

namespace {

// MPU protection-region registers (c6,c0..c7) and assorted cache/access
// registers we accept and store but do not yet act on. Behavior that the
// bus needs (control + TCM) is mirrored into g_cp15.
uint32_t g_mpu_region[8] = {};
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

bool mpu_region_contains(uint32_t reg, uint32_t addr) {
    if (!(reg & 1u)) return false;
    // ARM946E-S MPU encoding: region size is 2^(N+1) bytes (minimum
    // architectural N=11 => 4 KiB), unlike the c9 TCM size encoding's
    // 512<<N formula.  Use 64-bit math so the 4 GiB encoding remains
    // representable, and align the programmed base down to the region size.
    const uint32_t n = (reg >> 1u) & 0x1Fu;
    const uint64_t size = uint64_t{1} << (n + 1u);
    const uint64_t base = uint64_t{reg & 0xFFFFF000u} & ~(size - 1u);
    const uint64_t a = addr;
    return a >= base && a < base + size;
}

}  // namespace

void cp15_reset() {
    g_cp15 = {};
    // ARM9 powers up with the high exception base (the DS ARM9 BIOS lives
    // at 0xFFFF0000); the BIOS reasserts this via the control register.
    g_cp15.high_vectors = true;
    g_cp15.control = (1u << 13);
}

// True if code fetches from `addr` are served by the ARM9 instruction cache:
// the I-cache is enabled (C1 bit12) AND `addr` falls in an MPU protection region
// whose instruction-cacheable bit (c2,c0,1) is set. Highest-numbered enabled
// region wins (ARM946E-S priority). Mirrors melonDS's per-PU-region cacheability
// (pu & 0x40), which the code-fetch timing degrades to a flat averaged cost.
bool cp15_code_cacheable(uint32_t addr) {
    if (!(g_cp15.control & (1u << 12))) return false;      // I-cache disabled
    if (!(g_cp15.control & 1u)) return true;               // MPU disabled: global cache bit
    const uint32_t icache_bits = g_cache_cfg[1];           // c2,c0,1 = instr cacheable
    for (int i = 7; i >= 0; --i) {
        uint32_t r = g_mpu_region[i];
        if (mpu_region_contains(r, addr))
            return (icache_bits >> i) & 1u;
    }
    return false;
}

bool cp15_data_cacheable(uint32_t addr) {
    if (!(g_cp15.control & (1u << 2))) return false;       // D-cache disabled
    if (!(g_cp15.control & 1u)) return true;               // MPU disabled: global cache bit
    const uint32_t dcache_bits = g_cache_cfg[0];           // c2,c0,0 = data cacheable
    for (int i = 7; i >= 0; --i) {
        uint32_t r = g_mpu_region[i];
        if (mpu_region_contains(r, addr))
            return (dcache_bits >> i) & 1u;
    }
    return false;
}

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
    switch (crn) {
        case 1:  // control register (c1,c0,0)
            if (crm == 0 && op2 == 0) apply_control(value);
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
            g_mpu_region[crm & 7] = value;
            break;
        case 7:  // cache/write-buffer ops, wait-for-interrupt — no state
            break;
        case 9:  // TCM region registers (c9,c1,x)
            if (crm == 1 && op2 == 0) {          // DTCM base/size
                g_cp15.dtcm_base = value & 0xFFFFF000u;
                g_cp15.dtcm_size = tcm_bytes(value);
                std::fprintf(stderr, "[cp15] DTCM region: base=%08X vsize=%u\n",
                             g_cp15.dtcm_base, g_cp15.dtcm_size);
            } else if (crm == 1 && op2 == 1) {   // ITCM size (base = 0)
                g_cp15.itcm_size = tcm_bytes(value);
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
