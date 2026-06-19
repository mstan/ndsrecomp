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

}  // namespace

void cp15_reset() {
    g_cp15 = {};
    // ARM9 powers up with the high exception base (the DS ARM9 BIOS lives
    // at 0xFFFF0000); the BIOS reasserts this via the control register.
    g_cp15.high_vectors = true;
    g_cp15.control = (1u << 13);
}

extern "C" void runtime_coproc_write(uint32_t cp_num, uint32_t op1,
                                     uint32_t crn, uint32_t crm,
                                     uint32_t op2, uint32_t value) {
    if (cp_num != 15) {
        std::fprintf(stderr, "[cp15] write to unexpected coproc p%u "
                     "(c%u,c%u,%u) = 0x%08X\n", cp_num, crn, crm, op2, value);
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
            } else if (crm == 1 && op2 == 1) {   // ITCM size (base = 0)
                g_cp15.itcm_size = tcm_bytes(value);
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
