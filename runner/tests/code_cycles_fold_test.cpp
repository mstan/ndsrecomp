// code_cycles_fold_test.cpp — the equivalence rail for beads-yjp.70 phase 2A.
//
// Generated code no longer calls runtime_code_cycles(pc) per instruction; it
// evaluates the inline nds_code_numc(pc, NDS_ARM9_CODE_K(pc, thumb)) instead
// (recompiler/armv4t/runtime_arm.h). That is only admissible if the two agree
// EXACTLY — for every ARM9 code-timing class, both instruction-set states,
// every pc parity relative to the 32-byte I-cache line, the ITCM window, and
// the ARM7 side (where the inline form must fall back to the real call, whose
// g_last_code_pc[1] store is read by arm7_cycle_combine).
//
// The test drives the REAL runner bus/cp15 units, so it pins the actual
// runtime, not a model of it. For each class it also checks the run-sum
// property the fold rests on: the sum of the inline per-instruction costs over
// a straight-line run equals the sum of the out-of-line ones.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "runtime_arm.h"
#include "state.h"

namespace {

int g_failures = 0;

void fail(const char* what, uint32_t pc, bool thumb, const char* cls,
          uint32_t got, uint32_t want) {
    std::fprintf(stderr,
                 "FAIL %s: class=%s pc=0x%08X %s got=%u want=%u\n", what, cls,
                 pc, thumb ? "thumb" : "arm", got, want);
    ++g_failures;
}

void expect(bool cond, const char* msg) {
    if (cond) return;
    std::fprintf(stderr, "FAIL %s\n", msg);
    ++g_failures;
}

// Mirror of what a CP15 write does: change the state, then bump the timing
// generation (runtime_coproc_write does this for crn 1/2/6/9).
void bump_generation() {
    if (++g_cp15_timing_generation == 0u) g_cp15_timing_generation = 1u;
}

void set_thumb(bool thumb) {
    if (thumb) g_cpu.cpsr |= CPSR_T_BIT;
    else       g_cpu.cpsr &= ~CPSR_T_BIT;
}

// The four classes, each expressed purely through CP15 state + a latch
// address, so the class is established the way the guest establishes it.
struct ClassCase {
    const char* name;
    uint32_t base;          // code window base
    bool icache;            // C1 bit 12 (I-cache enable), MPU disabled
    bool itcm;              // C1 bit 18 + ITCM size
    uint32_t expect_numc;   // expected non-boundary numC (0 = don't check)
};

const ClassCase kCases[] = {
    {"Itcm",    0x00000000u, false, true,  1u},
    {"Cached",  0x02100000u, true,  false, 0u},   // 1 or 3 by line boundary
    {"MainRam", 0x02100000u, false, false, 18u},
    {"Other",   0x037F8000u, false, false, 8u},
};

void apply_case(const ClassCase& c) {
    g_cp15 = {};
    g_cp15.high_vectors = true;
    // MPU off (bit 0 clear) so cp15_code_cacheable falls back to the global
    // I-cache-enable bit, and no region table has to be programmed.
    g_cp15.control = (1u << 13) | (c.icache ? (1u << 12) : 0u) |
                     (c.itcm ? (1u << 18) : 0u);
    g_cp15.itcm_enable = c.itcm;
    g_cp15.itcm_size = c.itcm ? 0x8000u : 0u;
    bump_generation();
}

void run_class(const ClassCase& c, bool thumb) {
    apply_case(c);
    set_thumb(thumb);
    g_nds_active = NDS_ARM9;

    // Latch the region class the way a taken branch does. This also
    // publishes the inline fast path's three words.
    arm9_refill_cycles(c.base | (thumb ? 1u : 0u));
    expect(g_arm9_code_pub_gen == g_cp15_timing_generation,
           "publication is not live after a refill latch");

    const uint32_t step = thumb ? 2u : 4u;
    uint64_t sum_inline = 0, sum_ref = 0;
    bool saw_one = false, saw_three = false;
    // Two full 32-byte lines plus a little, so every (pc & 0x1F) phase and
    // both Thumb halfword parities appear.
    for (uint32_t i = 0; i < 40u; ++i) {
        const uint32_t pc = c.base + i * step;
        // Fast path must be live for this comparison to mean anything.
        expect(g_arm9_code_pub_gen == g_cp15_timing_generation,
               "publication went stale mid-run");
        const uint32_t k = thumb ? NDS_ARM9_CODE_K(pc, 1)
                                 : NDS_ARM9_CODE_K(pc, 0);
        const uint32_t got = nds_code_numc(pc, k);
        expect(g_last_code_pc[0] == pc,
               "inline path did not record g_last_code_pc[0]");
        const uint32_t want = runtime_code_cycles(pc);
        if (got != want) fail("numc", pc, thumb, c.name, got, want);
        sum_inline += got;
        sum_ref += want;
        if (thumb && (pc & 2u)) {
            if (got != 0u) fail("thumb-odd-not-free", pc, thumb, c.name, got, 0u);
            continue;
        }
        if (c.expect_numc && got != c.expect_numc)
            fail("class-constant", pc, thumb, c.name, got, c.expect_numc);
        if (!c.expect_numc) {           // Cached: 3 at a line boundary, else 1
            if (got == 1u) saw_one = true;
            if (got == 3u) saw_three = true;
        }
    }
    if (sum_inline != sum_ref) {
        std::fprintf(stderr,
                     "FAIL run-sum: class=%s %s inline=%llu ref=%llu\n",
                     c.name, thumb ? "thumb" : "arm",
                     (unsigned long long)sum_inline,
                     (unsigned long long)sum_ref);
        ++g_failures;
    }
    if (!c.expect_numc)
        expect(saw_one && saw_three,
               "cached corpus did not cover both line-boundary phases");
}

// A CP15 write bumps the generation; the inline path must then fall back to
// the out-of-line function (which republishes) and never serve a stale class.
void run_invalidation() {
    apply_case(kCases[2]);                       // MainRam
    set_thumb(false);
    g_nds_active = NDS_ARM9;
    arm9_refill_cycles(0x02100000u);
    expect(nds_code_numc(0x02100000u, NDS_ARM9_CODE_K(0x02100000u, 0)) == 18u,
           "main-RAM class not published");

    // Now enable the I-cache the way a CP15 control write does. The LATCHED
    // class must not change (melonDS snapshots it at JumpTo), but the
    // publication must be invalidated so the inline path re-derives.
    g_cp15.control |= (1u << 12);
    bump_generation();
    expect(g_arm9_code_pub_gen != g_cp15_timing_generation,
           "generation bump did not invalidate the publication");
    const uint32_t after =
        nds_code_numc(0x02100004u, NDS_ARM9_CODE_K(0x02100004u, 0));
    expect(after == 18u, "latched class was not preserved across a CP15 write");
    expect(g_arm9_code_pub_gen == g_cp15_timing_generation,
           "fallback did not republish");

    // ITCM enable takes effect without a branch (CodeRead32 checks the live
    // mapping), and the publication must reflect it after the fallback.
    g_cp15.itcm_enable = true;
    g_cp15.itcm_size = 0x8000u;
    bump_generation();
    expect(nds_code_numc(0x00000010u, NDS_ARM9_CODE_K(0x00000010u, 0)) ==
               runtime_code_cycles(0x00000010u),
           "live ITCM window not honoured by the inline path");
    expect(nds_code_numc(0x00000010u, NDS_ARM9_CODE_K(0x00000010u, 0)) == 1u,
           "ITCM fetch is not 1 cycle");
    // An explicit invalidation (savestate import) must also force the call.
    arm9_code_fast_invalidate();
    expect(g_arm9_code_pub_gen == 0u, "invalidate did not clear the publication");
}

// ARM7: the inline form must be the unchanged call, including the
// g_last_code_pc[1] store arm7_cycle_combine reads.
void run_arm7() {
    apply_case(kCases[2]);
    g_nds_active = NDS_ARM7;
    for (bool thumb : {false, true}) {
        set_thumb(thumb);
        for (uint32_t pc : {0x02100000u, 0x02100002u, 0x037F8000u, 0x00000100u,
                            0x06000000u}) {
            g_last_code_pc[1] = 0xDEADBEEFu;
            const uint32_t k = thumb ? NDS_ARM9_CODE_K(pc, 1)
                                     : NDS_ARM9_CODE_K(pc, 0);
            const uint32_t got = nds_code_numc(pc, k);
            expect(g_last_code_pc[1] == pc,
                   "ARM7 path did not record g_last_code_pc[1]");
            g_last_code_pc[1] = 0xDEADBEEFu;
            const uint32_t want = runtime_code_cycles(pc);
            if (got != want) fail("arm7-numc", pc, thumb, "Arm7", got, want);
        }
    }
    g_nds_active = NDS_ARM9;
}

// arm_cond_passes_i (inline) must equal arm_cond_passes (exported) for every
// condition code and every N/Z/C/V combination.
void run_cond() {
    for (uint32_t flags = 0; flags < 16u; ++flags) {
        g_cpu.cpsr = ((flags & 8u) ? CPSR_N_BIT : 0u) |
                     ((flags & 4u) ? CPSR_Z_BIT : 0u) |
                     ((flags & 2u) ? CPSR_C_BIT : 0u) |
                     ((flags & 1u) ? CPSR_V_BIT : 0u);
        for (unsigned cond = 0; cond < 16u; ++cond) {
            const int inl = arm_cond_passes_i(cond);
            const int out = arm_cond_passes(cond);
            if ((inl != 0) != (out != 0)) {
                std::fprintf(stderr,
                             "FAIL cond: flags=%X cond=%X inline=%d out=%d\n",
                             flags, cond, inl, out);
                ++g_failures;
            }
        }
    }
    g_cpu.cpsr = 0u;
}

}  // namespace

int main() {
    bus_init();
    for (const ClassCase& c : kCases) {
        run_class(c, false);
        run_class(c, true);
    }
    run_invalidation();
    run_arm7();
    run_cond();
    if (g_failures) {
        std::fprintf(stderr, "code_cycles_fold_test: %d failure(s)\n",
                     g_failures);
        return 1;
    }
    std::printf("code_cycles_fold_test: OK\n");
    return 0;
}
