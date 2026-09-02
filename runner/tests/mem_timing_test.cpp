// mem_timing_test.cpp — bit-exactness rail for the fused timed-access fast
// path (beads-yjp.70 phase 2 B).
//
// WHAT THIS PINS
// --------------
// Generated code calls runtime_mem_cycles(ea, width, seq) and then
// bus_read/write_*(ea) back to back for EVERY guest load and store. The
// timing half used to be an out-of-line call in runner/src/bus.cpp that
// re-decoded the region and walked all eight CP15 MPU protection regions
// per access. It is now an inline fast path in
// recompiler/armv4t/runtime_arm.h reading two pieces of published derived
// state: the g_memt_* TCM mirrors and a page-granular CP15 cacheability
// bitmap.
//
// Derived state is exactly where a timing model silently rots: a mirror
// that is not republished on some path that moves TCM, or a bitmap whose
// region fill is off by a page, changes guest cycle counts — which changes
// scheduler interleaving, which changes the game. So the ORIGINALS are kept
// in the tree under _reference names (runtime_mem_cycles_reference,
// cp15_{data,code}_cacheable_reference) and this test sweeps every region
// boundary against them:
//
//   * every documented DS map boundary and both TCM windows' edges, each
//     probed at -4..+4 bytes and at +/- one 4 KiB page;
//   * widths 1/2/4, sequential 0/1;
//   * both CPUs;
//   * a set of CP15 configurations covering MPU off / on, D-cache and
//     I-cache independently on/off, TCM off / on / moved / oversized, and a
//     deliberately SUB-PAGE MPU region — the one encoding the bitmap cannot
//     represent, where g_cp15_class_ready must go to 0 and every consumer
//     must fall back to the walk and STILL agree.
//
// Any disagreement is a guest-visible cycle-count change and fails here.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime_arm.h"
#include "state.h"

extern "C" uint32_t runtime_mem_cycles_slow(uint32_t addr, uint32_t width,
                                            uint32_t sequential);
extern "C" uint32_t runtime_mem_cycles_reference(uint32_t addr, uint32_t width,
                                                uint32_t sequential);

namespace {

int g_failures = 0;
long long g_checks = 0;

void fail(const std::string& what) {
    if (++g_failures <= 40) std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

const char* cpu_name(NdsCpu cpu) { return cpu == NDS_ARM9 ? "arm9" : "arm7"; }

// ── CP15 programming helpers (through the real coprocessor write path, so
// every rebuild/publish hook the runtime actually uses is exercised) ──
void cp15_control(uint32_t value) {
    runtime_coproc_write(15u, 0u, 1u, 0u, 0u, value);
}
void cp15_cacheable(uint32_t data_bits, uint32_t instr_bits) {
    runtime_coproc_write(15u, 0u, 2u, 0u, 0u, data_bits);
    runtime_coproc_write(15u, 0u, 2u, 0u, 1u, instr_bits);
}
// ARM946E-S c6: region size is 2^(n+1) bytes; bit 0 enables.
void cp15_region(uint32_t index, uint32_t base, uint32_t n, bool enable) {
    runtime_coproc_write(15u, 0u, 6u, index, 0u,
                         (base & 0xFFFFF000u) | (n << 1u) | (enable ? 1u : 0u));
}
// c9,c1: TCM size field is 512 << n.
void cp15_dtcm(uint32_t base, uint32_t n) {
    runtime_coproc_write(15u, 0u, 9u, 1u, 0u,
                         (base & 0xFFFFF000u) | (n << 1u));
}
void cp15_itcm(uint32_t n) {
    runtime_coproc_write(15u, 0u, 9u, 1u, 1u, n << 1u);
}

void clear_all_regions() {
    for (uint32_t i = 0; i < 8u; ++i) cp15_region(i, 0u, 11u, false);
}

struct Config {
    const char* name;
    void (*apply)();
    // Whether the page bitmap is EXPECTED to be usable. Checked so a future
    // change that silently disables it everywhere (making the sweep pass
    // vacuously by always taking the fallback) fails instead.
    bool expect_class_ready;
};

// Control-register bit helpers.
constexpr uint32_t kMpu = 1u << 0;
constexpr uint32_t kDCache = 1u << 2;
constexpr uint32_t kICache = 1u << 12;
constexpr uint32_t kHighVec = 1u << 13;
constexpr uint32_t kDtcmEn = 1u << 16;
constexpr uint32_t kItcmEn = 1u << 18;

void cfg_bare() {
    clear_all_regions();
    cp15_cacheable(0u, 0u);
    cp15_control(kHighVec);
}

// The shape a DS game actually programs: a background no-cache region over
// the whole space, cacheable main RAM on top, an uncacheable I/O hole, and
// both TCMs live.
void cfg_typical() {
    clear_all_regions();
    cp15_region(0, 0x00000000u, 31u, true);   // 4 GiB background
    cp15_region(1, 0x02000000u, 21u, true);   // 4 MiB main RAM
    cp15_region(2, 0x04000000u, 23u, true);   // 16 MiB I/O window
    cp15_region(3, 0x06000000u, 22u, true);   // 8 MiB VRAM
    cp15_region(4, 0x027C0000u, 13u, true);   // 16 KiB DTCM shadow
    cp15_cacheable(0b00001010u, 0b00001010u); // regions 1 and 3 cacheable
    cp15_itcm(6u);                            // 32 KiB ITCM at 0
    cp15_dtcm(0x027C0000u, 5u);               // 16 KiB DTCM
    cp15_control(kHighVec | kMpu | kDCache | kICache | kItcmEn | kDtcmEn);
}

void cfg_typical_no_dcache() {
    cfg_typical();
    cp15_control(kHighVec | kMpu | kICache | kItcmEn | kDtcmEn);
}

void cfg_typical_no_icache() {
    cfg_typical();
    cp15_control(kHighVec | kMpu | kDCache | kItcmEn | kDtcmEn);
}

// MPU off with both caches on: the walk short-circuits to the global bit,
// so EVERY address is cacheable.
void cfg_mpu_off_caches_on() {
    clear_all_regions();
    cp15_cacheable(0u, 0u);
    cp15_itcm(6u);
    cp15_dtcm(0x027C0000u, 5u);
    cp15_control(kHighVec | kDCache | kICache | kItcmEn | kDtcmEn);
}

// TCM moved and oversized: the ITCM virtual span swallows the bottom of
// main RAM, and DTCM sits in the GBA-slot window. Both are legal CP15
// programmings and both change which branch of the model an address takes.
void cfg_tcm_moved() {
    clear_all_regions();
    cp15_region(0, 0x00000000u, 31u, true);
    cp15_region(1, 0x02000000u, 21u, true);
    cp15_cacheable(0b00000010u, 0b00000010u);
    cp15_itcm(15u);                           // 16 MiB virtual ITCM span
    cp15_dtcm(0x0B000000u, 5u);
    cp15_control(kHighVec | kMpu | kDCache | kICache | kItcmEn | kDtcmEn);
}

// TCM configured but DISABLED in the control register: the model must not
// see either window (the mirrors carry the enable, not just the size).
void cfg_tcm_configured_but_off() {
    cfg_typical();
    cp15_control(kHighVec | kMpu | kDCache | kICache);
}

// A sub-page region (2^6 = 64 bytes). The ARM946E-S forbids it, but the
// register accepts it and a savestate can carry it, so the bitmap must
// refuse to represent it and every consumer must fall back to the walk.
void cfg_subpage_region() {
    clear_all_regions();
    cp15_region(0, 0x00000000u, 31u, true);
    cp15_region(1, 0x02000000u, 21u, true);
    cp15_region(2, 0x02001000u, 5u, true);    // 64 bytes
    cp15_cacheable(0b00000110u, 0b00000010u);
    cp15_itcm(6u);
    cp15_dtcm(0x027C0000u, 5u);
    cp15_control(kHighVec | kMpu | kDCache | kICache | kItcmEn | kDtcmEn);
}

const Config kConfigs[] = {
    {"bare (reset-like: MPU off, caches off, TCM off)", cfg_bare, true},
    {"typical game MPU + both caches + both TCM", cfg_typical, true},
    {"typical, D-cache off", cfg_typical_no_dcache, true},
    {"typical, I-cache off", cfg_typical_no_icache, true},
    {"MPU off, both caches on (global bit)", cfg_mpu_off_caches_on, true},
    {"ITCM span over main RAM, DTCM in GBA slot", cfg_tcm_moved, true},
    {"TCM programmed but disabled in c1", cfg_tcm_configured_but_off, true},
    {"sub-page MPU region (bitmap must refuse)", cfg_subpage_region, false},
};

// Every documented DS map boundary plus both TCM windows' edges. Probed at
// -4..+4 and at +/- one 4 KiB page so an off-by-one page in the bitmap fill
// or an off-by-one in a region compare cannot hide.
std::vector<uint32_t> probe_addresses() {
    static const uint32_t anchors[] = {
        0x00000000u,                                    // ITCM / vector base
        0x00001000u, 0x00004000u, 0x00008000u,          // ITCM backing edges
        0x01000000u, 0x02000000u,                       // main RAM start
        0x02001000u, 0x02400000u,                       // MPU sub-region edges
        0x027C0000u, 0x027C4000u,                       // DTCM window edges
        0x027E0000u, 0x027E0040u,                       // live-overlay watch
        0x02400000u, 0x03000000u,                       // main RAM end
        0x03004000u, 0x03008000u, 0x03800000u,          // WRAM splits
        0x03810000u, 0x04000000u,                       // WRAM end / I/O
        0x04000130u, 0x04800000u, 0x04808000u,          // I/O regs / Wi-Fi
        0x04810000u, 0x05000000u,                       // Wi-Fi end / palette
        0x05000400u, 0x06000000u,                       // palette end / VRAM
        0x06800000u, 0x07000000u,                       // VRAM end / OAM
        0x07000400u, 0x08000000u,                       // OAM end / GBA ROM
        0x09000000u, 0x0A000000u,                       // GBA ROM / SRAM
        0x0A010000u, 0x0B000000u,                       // GBA SRAM end
        0x0C000000u, 0x0FFFFFFFu, 0x10000000u,          // void
        0x80000000u, 0xFFFF0000u, 0xFFFFF000u,          // high vectors
    };
    std::vector<uint32_t> out;
    for (uint32_t anchor : anchors) {
        for (int32_t d = -4; d <= 4; ++d)
            out.push_back(anchor + static_cast<uint32_t>(d));
        out.push_back(anchor - 0x1000u);
        out.push_back(anchor + 0x1000u);
        out.push_back(anchor + 0xFFFu);
    }
    out.push_back(0xFFFFFFFFu);
    out.push_back(0xFFFFFFFEu);
    out.push_back(0xFFFFFFFCu);
    return out;
}

void sweep_one_config(const Config& config) {
    config.apply();
    if (config.expect_class_ready && !g_cp15_class_ready)
        fail(std::string("config '") + config.name +
             "': the page cacheability bitmap disabled itself; the sweep "
             "below would pass vacuously on the fallback");
    if (!config.expect_class_ready && g_cp15_class_ready)
        fail(std::string("config '") + config.name +
             "': the bitmap claims to represent a sub-page MPU region");

    const std::vector<uint32_t> addrs = probe_addresses();
    const NdsCpu saved = g_nds_active;
    const NdsCpu cpus[] = {NDS_ARM9, NDS_ARM7};
    const uint32_t widths[] = {1u, 2u, 4u};

    // Cacheability first: it is a strictly smaller claim than the cycle
    // count and localizes a bitmap bug without the timing model on top.
    for (uint32_t addr : addrs) {
        ++g_checks;
        if (cp15_data_cacheable(addr) != cp15_data_cacheable_reference(addr)) {
            char buf[192];
            std::snprintf(buf, sizeof buf,
                          "config '%s': cp15_data_cacheable(0x%08X) = %d, "
                          "reference walk = %d", config.name, addr,
                          static_cast<int>(cp15_data_cacheable(addr)),
                          static_cast<int>(cp15_data_cacheable_reference(addr)));
            fail(buf);
        }
        ++g_checks;
        if (cp15_code_cacheable(addr) != cp15_code_cacheable_reference(addr)) {
            char buf[192];
            std::snprintf(buf, sizeof buf,
                          "config '%s': cp15_code_cacheable(0x%08X) = %d, "
                          "reference walk = %d", config.name, addr,
                          static_cast<int>(cp15_code_cacheable(addr)),
                          static_cast<int>(cp15_code_cacheable_reference(addr)));
            fail(buf);
        }
    }

    for (NdsCpu cpu : cpus) {
        g_nds_active = cpu;
        for (uint32_t addr : addrs) {
            for (uint32_t width : widths) {
                for (uint32_t seq = 0; seq <= 1u; ++seq) {
                    // The inline fast path (this TU sees the header, so this
                    // call IS the fast path), the out-of-line fallback, and
                    // the retained original. All three must agree.
                    // The sentinel proves the INLINE path recorded the last
                    // data address; the fallback would otherwise mask it.
                    g_last_data_addr[0] = g_last_data_addr[1] = 0xDEADBEEFu;
                    const uint32_t fast = runtime_mem_cycles(addr, width, seq);
                    const uint32_t recorded =
                        g_last_data_addr[cpu == NDS_ARM9 ? 0 : 1];
                    const uint32_t slow =
                        runtime_mem_cycles_slow(addr, width, seq);
                    const uint32_t ref =
                        runtime_mem_cycles_reference(addr, width, seq);
                    ++g_checks;
                    if (fast != ref || slow != ref) {
                        char buf[256];
                        std::snprintf(buf, sizeof buf,
                            "config '%s': %s mem_cycles(0x%08X, w=%u, seq=%u) "
                            "inline=%u fallback=%u reference=%u",
                            config.name, cpu_name(cpu), addr, width, seq,
                            fast, slow, ref);
                        fail(buf);
                    }
                    // The last-data-address side effect is load-bearing for
                    // arm7_cycle_combine and must survive inlining.
                    ++g_checks;
                    if (recorded != addr) {
                        char buf[192];
                        std::snprintf(buf, sizeof buf,
                            "config '%s': %s mem_cycles(0x%08X) did not "
                            "record g_last_data_addr", config.name,
                            cpu_name(cpu), addr);
                        fail(buf);
                    }
                }
            }
        }
    }
    g_nds_active = saved;
}

// The bitmap is rebuilt from a snapshot of its inputs. Prove the snapshot
// cannot go stale: after a region write that CHANGES cacheability, the
// bitmap must move; after one that does not, it must still agree.
void check_input_tracking() {
    cfg_typical();
    const uint32_t probe = 0x06000000u;
    if (!cp15_data_cacheable(probe))
        fail("setup: expected 0x06000000 cacheable in the typical config");
    // Turn region 3 (VRAM) non-cacheable and nothing else.
    cp15_cacheable(0b00000010u, 0b00001010u);
    if (cp15_data_cacheable(probe) != cp15_data_cacheable_reference(probe))
        fail("a c2 cacheability write did not resync the page bitmap");
    if (cp15_data_cacheable(probe))
        fail("a c2 cacheability write left 0x06000000 cacheable");
    // Move region 3 off the probe entirely.
    cp15_region(3, 0x06000000u, 22u, false);
    if (cp15_data_cacheable(probe) != cp15_data_cacheable_reference(probe))
        fail("a c6 region-disable write did not resync the page bitmap");
    // Re-enable and re-mark cacheable; the answer must come back.
    cp15_region(3, 0x06000000u, 22u, true);
    cp15_cacheable(0b00001010u, 0b00001010u);
    if (cp15_data_cacheable(probe) != cp15_data_cacheable_reference(probe))
        fail("re-enabling a region did not resync the page bitmap");
    if (!cp15_data_cacheable(probe))
        fail("re-enabling region 3 did not restore 0x06000000 cacheability");
    // A CP15 write with no effect on cacheability must not desync anything.
    runtime_coproc_write(15u, 0u, 5u, 0u, 0u, 0x33333333u);   // access perms
    if (cp15_data_cacheable(probe) != cp15_data_cacheable_reference(probe))
        fail("an unrelated CP15 write desynced the page bitmap");
}

// The g_memt_* mirrors must equal the CP15 state they mirror after every
// path that can move TCM.
void check_tcm_mirrors(const char* when) {
    const uint32_t want_itcm = g_cp15.itcm_enable ? g_cp15.itcm_size : 0u;
    const uint32_t want_dtcm = g_cp15.dtcm_enable ? g_cp15.dtcm_size : 0u;
    if (g_memt_itcm_limit != want_itcm || g_memt_dtcm_base != g_cp15.dtcm_base ||
        g_memt_dtcm_size != want_dtcm) {
        char buf[256];
        std::snprintf(buf, sizeof buf,
            "%s: TCM timing mirrors stale (itcm %u vs %u, dtcm base %08X vs "
            "%08X, dtcm size %u vs %u)", when, g_memt_itcm_limit, want_itcm,
            g_memt_dtcm_base, g_cp15.dtcm_base, g_memt_dtcm_size, want_dtcm);
        fail(buf);
    }
}

}  // namespace

int main() {
    // No nds_io_reset(): io.cpp is stubbed in this link (see
    // savestate_test_stubs.cpp). The GBA-slot / Wi-Fi timings that read live
    // I/O registers therefore see the stubs' constant values -- which is
    // fine, because all three implementations under test read the SAME
    // stubs, and those addresses take the shared out-of-line path anyway.
    bus_init();
    cp15_reset();
    check_tcm_mirrors("after cp15_reset");

    for (const Config& config : kConfigs) {
        std::fprintf(stderr, "[sweep] %s\n", config.name);
        sweep_one_config(config);
        check_tcm_mirrors(config.name);
    }

    check_input_tracking();

    // cp15_reset must put the derived state back, not leave the last
    // config's bitmap behind.
    cfg_typical();
    cp15_reset();
    check_tcm_mirrors("after cp15_reset following a live config");
    for (uint32_t addr : probe_addresses()) {
        ++g_checks;
        if (cp15_data_cacheable(addr) != cp15_data_cacheable_reference(addr))
            fail("cp15_reset left a stale cacheability bitmap");
    }
    // And bus_init (which republishes the mirrors) must agree too.
    bus_init();
    check_tcm_mirrors("after bus_init");

    std::printf("mem_timing_test: %lld checks, %d failure(s)\n",
                g_checks, g_failures);
    if (g_failures) {
        std::printf("mem_timing_test: FAILED\n");
        return 1;
    }
    std::printf("mem_timing_test: OK\n");
    return 0;
}
