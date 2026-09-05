// emu_profile.cpp -- see emu_profile.h.

#include "emu_profile.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nds_emu_detail {

NdsEmuProfile g_profile{};
bool     g_sampling = false;
uint8_t  g_cur = NDS_EMU_NONE;
uint64_t g_last = 0;

uint64_t g_bus_gate = 0;
uint64_t g_bus_modulus = 1009u;
// Seeded to the defaults, not to 0, so a build that somehow reaches a
// scheduler round before nds_emu_profile_init() still measures rather than
// silently accumulating nothing -- the same reasoning as dispatch_timing's
// g_ns_per_tick = 1.0 seed. The env overrides are applied by
// ensure_configured() at start-up.
uint64_t g_modulus = 31u;
uint64_t g_round_counter = 0;

namespace {

bool g_configured = false;

// NDS_PROFILE_EMU: unset = the default 1-in-31 round sampler; "off"/"0"
// disables the sampled buckets (the exact ones keep working); "exact"/"every"
// times every round; N >= 2 sets the modulus.
//
// Why 31 and not the 1009 used by the scheduler sampler and dispatch_timing:
// those two are gated per DISPATCH, millions of times a second, so 1009 is
// what makes them free. This gate fires per scheduler ROUND -- ~600k times a
// second at 60 fps, three orders of magnitude rarer than a dispatch -- and
// each sampled round costs ~20 rdtsc reads, so 1-in-31 lands around 0.2
// percent of emulation time. Meanwhile the estimator's variance is the whole
// problem being fixed: 1-in-1009 gave ~600 samples per 2 s interval against a
// heavy-tailed per-round cost and scattered +/-60 percent, once attributing
// 53 ms/frame of device time to a 35.8 ms/frame emu budget. 31 is prime, so
// like 1009 it cannot phase-lock to a power-of-two periodicity in round
// structure (the 64-cycle rendezvous grid, the 1024-cycle SPU deadline).
uint64_t parse_modulus() {
    const char* v = std::getenv("NDS_PROFILE_EMU");
    if (!v || !v[0]) return 31u;
    if (std::strcmp(v, "off") == 0 || std::strcmp(v, "0") == 0) return 0u;
    if (std::strcmp(v, "exact") == 0 || std::strcmp(v, "every") == 0)
        return 1u;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(v, &end, 10);
    if (end != v && *end == '\0' && parsed >= 2u)
        return static_cast<uint64_t>(parsed);
    return 31u;
}

// NDS_PROFILE_EMU_BUS, same grammar, for the independently-gated bus
// breakdown. Left at the 1009 house modulus: this gate IS per guest access.
uint64_t parse_bus_modulus() {
    const char* v = std::getenv("NDS_PROFILE_EMU_BUS");
    if (!v || !v[0]) return 1009u;
    if (std::strcmp(v, "off") == 0 || std::strcmp(v, "0") == 0) return 0u;
    if (std::strcmp(v, "exact") == 0 || std::strcmp(v, "every") == 0)
        return 1u;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(v, &end, 10);
    if (end != v && *end == '\0' && parsed >= 2u)
        return static_cast<uint64_t>(parsed);
    return 1009u;
}

const char* const kBucketNames[NDS_EMU_BUCKET_COUNT] = {
    "exec_arm9", "exec_arm7", "timers_arm9", "timers_arm7", "display",
    "spu", "wifi", "rtc", "sysev", "next_event", "ctx_switch",
    "overlay_poll", "sched_other",
    "geometry", "tier3_arm9", "tier3_arm7", "dma_arm9", "dma_arm7",
    "raster2d", "gpu3d_frame",
};

const char* const kBusNames[NDS_EMU_BUS_PATH_COUNT] = {
    "ram", "vram", "mmio", "other",
};

}  // namespace

void ensure_configured() {
    if (g_configured) return;
    g_configured = true;
    // The tick source and its overhead calibration are shared with
    // dispatch_timing; calibrating here as well would measure the same TSC
    // twice and could publish two different ns-per-tick scales for buckets
    // that are meant to be comparable.
    nds_dispatch_timing_init();
    g_modulus = parse_modulus();
    g_bus_modulus = parse_bus_modulus();
    g_bus_gate = 0;
}

}  // namespace nds_emu_detail

void nds_emu_profile_init() { nds_emu_detail::ensure_configured(); }

uint64_t nds_emu_modulus() {
    nds_emu_detail::ensure_configured();
    return nds_emu_detail::g_modulus;
}

const char* nds_emu_bucket_name(NdsEmuBucket bucket) {
    if (bucket >= NDS_EMU_BUCKET_COUNT) return "unknown";
    return nds_emu_detail::kBucketNames[bucket];
}

const char* nds_emu_bus_path_name(NdsEmuBusPath path) {
    if (path >= NDS_EMU_BUS_PATH_COUNT) return "unknown";
    return nds_emu_detail::kBusNames[path];
}

bool nds_emu_bucket_is_exact(NdsEmuBucket bucket) {
    if (bucket >= NDS_EMU_BUCKET_COUNT) return false;
    return ((nds_emu_detail::kExactMask >> bucket) & 1u) != 0u;
}

void nds_emu_profile(NdsEmuProfile* out) {
    if (!out) return;
    *out = nds_emu_detail::g_profile;
}

void nds_emu_profile_reset() {
    nds_emu_detail::g_profile = NdsEmuProfile{};
    nds_emu_detail::g_cur = NDS_EMU_NONE;
    nds_emu_detail::g_sampling = false;
}

// ── Reporting ───────────────────────────────────────────────────────────────

std::string nds_emu_profile_json() {
    nds_emu_detail::ensure_configured();
    NdsEmuProfile p{};
    nds_emu_profile(&p);
    const double ns_per_tick = nds_dispatch_timing_ns_per_tick();
    char buf[512];
    std::string out = "{";
    std::snprintf(buf, sizeof(buf),
                  "\"modulus\":%llu,\"bus_modulus\":%llu,"
                  "\"ns_per_tick\":%.6f,\"clock_overhead_ticks\":%llu,"
                  "\"rounds\":%llu,\"sampled_rounds\":%llu,"
                  "\"gxstall_rounds\":%llu,\"reads\":%llu,\"buckets\":{",
                  (unsigned long long)nds_emu_modulus(),
                  (unsigned long long)nds_emu_detail::g_bus_modulus,
                  ns_per_tick,
                  (unsigned long long)
                      nds_dispatch_timing_clock_overhead_ticks(),
                  (unsigned long long)p.rounds,
                  (unsigned long long)p.sampled_rounds,
                  (unsigned long long)p.gxstall_rounds,
                  (unsigned long long)p.reads);
    out += buf;
    for (int i = 0; i < NDS_EMU_BUCKET_COUNT; ++i) {
        if (i) out += ',';
        const NdsEmuBucket bucket = static_cast<NdsEmuBucket>(i);
        std::snprintf(buf, sizeof(buf),
                      "\"%s\":{\"ns\":%llu,\"entries\":%llu,\"exact\":%s}",
                      nds_emu_bucket_name(bucket),
                      (unsigned long long)(static_cast<double>(p.ticks[i]) *
                                           ns_per_tick),
                      (unsigned long long)p.entries[i],
                      nds_emu_bucket_is_exact(bucket) ? "true" : "false");
        out += buf;
    }
    std::snprintf(buf, sizeof(buf),
                  "},\"bus\":{\"events\":%llu,\"paths\":{",
                  (unsigned long long)p.bus_events);
    out += buf;
    for (int i = 0; i < NDS_EMU_BUS_PATH_COUNT; ++i) {
        if (i) out += ',';
        std::snprintf(buf, sizeof(buf),
                      "\"%s\":{\"ns\":%llu,\"samples\":%llu}",
                      nds_emu_bus_path_name(
                          static_cast<NdsEmuBusPath>(i)),
                      (unsigned long long)(
                          static_cast<double>(p.bus_ticks[i]) * ns_per_tick),
                      (unsigned long long)p.bus_samples[i]);
        out += buf;
    }
    out += "}}}";
    return out;
}
