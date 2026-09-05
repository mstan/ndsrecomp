// diagnostics.cpp -- see diagnostics.h.

#include "diagnostics.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>

#include "dispatch_stats.h"
#include "dispatch_timing.h"
#include "emu_profile.h"
#include "frontend.h"
#include "gpu2d.h"
#include "gpu3d.h"
#include "host_info.h"
#include "host_profile.h"
#include "live_overlay.h"
#include "pc_profile.h"
#if defined(NDS_HAVE_COMPUTE_RENDERER)
#include "melonds_compute/ComputeHost.h"
#endif
#include "scheduler.h"
#include "tier3.h"
#include "wifi_net.h"

namespace {

bool g_enabled = true;
std::string g_directory;
std::string g_run_stamp;
uint32_t g_interval_ms = 2000;
std::FILE* g_perf = nullptr;
uint64_t g_last_ticks = 0;
std::string g_rom_sha1;
std::string g_rom_name;
std::string g_build_id;
std::string g_framework_version;
std::string g_game_version;
std::string g_rom_game_code;
uint32_t g_rom_revision = 0;
uint64_t g_rom_size = 0;
NdsFrontendLiveStats g_prev_frontend{};
Tier3Stats g_prev_tier3{};
NdsDispatchStats g_prev_dispatch[2]{};
NdsDispatchTiming g_prev_timing[2]{};
NdsLocalMpStats g_prev_local_mp{};
// profile_totals shipped only CUMULATIVE run totals while every other block in
// the record ships a per-interval delta. Dividing a monotonically-growing run
// total by one interval's frame_delta is what made gpu2d/gpu3d/scheduler read
// as "flat" during the 2026-08-28 MPH dip analysis (beads-yjp.54). These
// snapshots let profile_totals_delta and emu_attrib be real deltas; the
// cumulative block stays for consumers that already parse it.
NdsSchedulerProfile g_prev_sched{};
NdsGpu2dProfile g_prev_gpu2d{};
NdsGpu3dProfile g_prev_gpu3d{};
NdsEmuProfile g_prev_emu{};
// Previous full PC-histogram snapshot per population per CPU (pc_profile.h).
// Two megabytes of BSS for the four, copied once per 2 s interval: the
// alternative -- keeping only the previous top-N -- cannot produce a correct
// delta, because a PC that was outside the previous top-N has no baseline and
// would be reported with its whole run total the first interval it becomes
// hot, which is exactly the "cumulative total read as an interval" defect
// profile_totals_delta exists to undo. Slots never move, so the delta is a
// per-slot subtraction.
NdsPcHotTable g_prev_pc[NDS_PC_HOT_KIND_COUNT][2];

std::string json_escape(const char* text) {
    std::string out;
    if (!text) return out;
    for (const unsigned char ch : std::string(text)) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return out;
}

// beads-yjp.53: the reject-cause breakdown, emitted in BOTH the session and
// the per-interval record so a bundle carries the absolute totals at launch
// and the deltas across the session. Always on, Release included -- the whole
// point is that a player's log explains where their shards went without
// anyone having to reproduce the session first.
//
// The names come from the runtime, not from a copy here, so appending a cause
// to the table in live_overlay.cpp needs no change in this file and no change
// in any ingest that reads the object as a name->count map.
std::string overlay_reject_json(const NdsLiveOverlaySummary& overlay) {
    std::string out;
    out.reserve(96u + static_cast<std::size_t>(overlay.reason_count) * 40u);
    out += ",\"rows_superseded\":";
    out += std::to_string(overlay.rows_superseded);
    out += ",\"rows_superseding\":";
    out += std::to_string(overlay.rows_superseding);
    // beads-yjp.62: compiled output held back because its target guest code
    // was not resident when it was preflighted, plus how much of it a Tier-3
    // entry proof has since woken and how much was given up on. A bundle
    // showing dormant_candidates climbing with dormant_activations at zero is
    // a live-overlay that is paying for compiles it never gets to use.
    out += ",\"dormant_candidates\":";
    out += std::to_string(overlay.dormant_candidates);
    out += ",\"dormant_activations\":";
    out += std::to_string(overlay.dormant_activations);
    out += ",\"dormant_parked\":";
    out += std::to_string(overlay.dormant_parked);
    out += ",\"dormant_requeues\":";
    out += std::to_string(overlay.dormant_requeues);
    out += ",\"reject_reasons\":{";
    for (uint32_t i = 0u; i < overlay.reason_count; ++i) {
        if (i) out += ',';
        out += '"';
        out += json_escape(overlay.reason_names ? overlay.reason_names[i]
                                                : "unnamed");
        out += "\":";
        out += std::to_string(overlay.reason_counts[i]);
    }
    out += '}';
    return out;
}

// Wall-clock stamps for every emitted record (beads: field bundles).
//
// WHY BOTH. The log's own time base is stats.now_ticks, a performance counter
// with an arbitrary origin, so nothing in a bundle could be aligned with
// anything outside it: a player saying "it hitched right after I saved" or a
// crash-dump timestamp or the live-overlay compiler's own log lines could only
// be matched by counting frame gaps and hoping. ts_ms is the machine-readable
// key (unix epoch milliseconds, monotonic across the session in practice and
// directly diffable); wall is the same instant in the form a human reads off a
// screen recording, so a report that says "17:42:03" lands on a record without
// anyone converting anything.
uint64_t unix_ms_now() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// Localtime, matching run_stamp()'s zone, so the filename and the records
// inside the file cannot disagree about which day it is.
std::string wall_clock_now() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[16];
    if (!std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm)) return "";
    return buf;
}

std::string run_stamp() {
    if (!g_run_stamp.empty()) return g_run_stamp;
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    g_run_stamp = buf;
    return g_run_stamp;
}

void ensure_directory() {
    if (g_directory.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(g_directory, error);
}

double ticks_to_ms(uint64_t ticks, uint64_t freq) {
    if (!freq) return 0.0;
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(freq);
}

double per_frame_ms(uint64_t ticks, uint64_t frames, uint64_t freq) {
    if (!frames) return 0.0;
    return ticks_to_ms(ticks, freq) / static_cast<double>(frames);
}

uint64_t sub_u64(uint64_t now, uint64_t before) {
    return now >= before ? now - before : 0u;
}

// Per-interval delta of the fields that profile_totals already ships as run
// totals. Same names, same order, so a consumer can read either block and a
// diff of the two is a self-check.
void write_profile_totals_delta(const NdsSchedulerProfile& sched,
                                const NdsGpu2dProfile& gpu2d,
                                const NdsGpu3dProfile& gpu3d) {
    std::fprintf(g_perf,
        "{\"scheduler_sampled_rounds\":%llu,\"scheduler_rounds\":%llu,"
        "\"scheduler_arm9_ns\":%llu,\"scheduler_arm7_ns\":%llu,"
        "\"scheduler_devices_ns\":%llu,\"scheduler_display_ns\":%llu,"
        "\"scheduler_spu_ns\":%llu,\"scheduler_wifi_ns\":%llu,"
        "\"scheduler_rtc_ns\":%llu,\"scheduler_sysev_ns\":%llu,"
        "\"scheduler_switch_ns\":%llu,\"scheduler_switches\":%llu,"
        "\"scheduler_crs_words\":%llu,\"scheduler_next_event_ns\":%llu,"
        "\"scheduler_sampled_round_ns\":%llu,\"gpu2d_render_ns\":%llu,"
        "\"gpu2d_fence_wait_ns\":%llu,\"gpu2d_threaded_lines\":%llu,"
        "\"gpu2d_inline_lines\":%llu,\"gpu2d_hd_frames\":%llu,"
        "\"gpu2d_hd_presented\":%llu,\"gpu3d_vcount215_ns\":%llu,"
        "\"gpu3d_vcount144_ns\":%llu,\"gpu3d_getline_ns\":%llu,"
        "\"gpu3d_compute_sync_ns\":%llu,\"gpu3d_compute_sync_calls\":%llu,"
        "\"gpu3d_compute_submit_ns\":%llu,"
        "\"gpu3d_compute_submit_calls\":%llu,"
        "\"gpu3d_compute_map_ns\":%llu,\"gpu3d_compute_map_calls\":%llu,"
        "\"gpu3d_compute_readback_ns\":%llu,"
        "\"gpu3d_compute_readback_calls\":%llu}",
        (unsigned long long)sub_u64(sched.sampled_rounds,
                                    g_prev_sched.sampled_rounds),
        (unsigned long long)sub_u64(sched.rounds, g_prev_sched.rounds),
        (unsigned long long)sub_u64(sched.arm9_ns, g_prev_sched.arm9_ns),
        (unsigned long long)sub_u64(sched.arm7_ns, g_prev_sched.arm7_ns),
        (unsigned long long)sub_u64(sched.devices_ns, g_prev_sched.devices_ns),
        (unsigned long long)sub_u64(sched.display_ns, g_prev_sched.display_ns),
        (unsigned long long)sub_u64(sched.spu_ns, g_prev_sched.spu_ns),
        (unsigned long long)sub_u64(sched.wifi_ns, g_prev_sched.wifi_ns),
        (unsigned long long)sub_u64(sched.rtc_ns, g_prev_sched.rtc_ns),
        (unsigned long long)sub_u64(sched.sysev_ns, g_prev_sched.sysev_ns),
        (unsigned long long)sub_u64(sched.switch_ns, g_prev_sched.switch_ns),
        (unsigned long long)sub_u64(sched.switches, g_prev_sched.switches),
        (unsigned long long)sub_u64(sched.crs_words, g_prev_sched.crs_words),
        (unsigned long long)sub_u64(sched.next_event_ns,
                                    g_prev_sched.next_event_ns),
        (unsigned long long)sub_u64(sched.sampled_round_ns,
                                    g_prev_sched.sampled_round_ns),
        (unsigned long long)sub_u64(gpu2d.render_ns, g_prev_gpu2d.render_ns),
        (unsigned long long)sub_u64(gpu2d.fence_wait_ns,
                                    g_prev_gpu2d.fence_wait_ns),
        (unsigned long long)sub_u64(gpu2d.threaded_lines,
                                    g_prev_gpu2d.threaded_lines),
        (unsigned long long)sub_u64(gpu2d.inline_lines,
                                    g_prev_gpu2d.inline_lines),
        (unsigned long long)sub_u64(gpu2d.hd_frames, g_prev_gpu2d.hd_frames),
        (unsigned long long)sub_u64(gpu2d.hd_presented,
                                    g_prev_gpu2d.hd_presented),
        (unsigned long long)sub_u64(gpu3d.vcount215_ns,
                                    g_prev_gpu3d.vcount215_ns),
        (unsigned long long)sub_u64(gpu3d.vcount144_ns,
                                    g_prev_gpu3d.vcount144_ns),
        (unsigned long long)sub_u64(gpu3d.getline_ns,
                                    g_prev_gpu3d.getline_ns),
        (unsigned long long)sub_u64(gpu3d.compute_sync_ns,
                                    g_prev_gpu3d.compute_sync_ns),
        (unsigned long long)sub_u64(gpu3d.compute_sync_calls,
                                    g_prev_gpu3d.compute_sync_calls),
        (unsigned long long)sub_u64(gpu3d.compute_submit_ns,
                                    g_prev_gpu3d.compute_submit_ns),
        (unsigned long long)sub_u64(gpu3d.compute_submit_calls,
                                    g_prev_gpu3d.compute_submit_calls),
        (unsigned long long)sub_u64(gpu3d.compute_map_ns,
                                    g_prev_gpu3d.compute_map_ns),
        (unsigned long long)sub_u64(gpu3d.compute_map_calls,
                                    g_prev_gpu3d.compute_map_calls),
        (unsigned long long)sub_u64(gpu3d.compute_readback_ns,
                                    g_prev_gpu3d.compute_readback_ns),
        (unsigned long long)sub_u64(gpu3d.compute_readback_calls,
                                    g_prev_gpu3d.compute_readback_calls));
}

// The emu-time partition for this interval (emu_profile.h). Every bucket is
// reported three ways so no consumer has to re-derive the estimator:
//
//   ns             the raw accumulated delta (sampled buckets: sampled rounds
//                  only, so NOT comparable across buckets on its own)
//   ms_per_frame   the estimate -- sampled buckets scaled by
//                  rounds/sampled_rounds, exact buckets as-is -- divided by
//                  the interval's presented frames, which is the unit
//                  ms_per_frame.emu is already in
//   entries        the population the ns was drawn from
//
// and the header carries the residual against the EXACT emu measurement, so
// "how much of emu time is still unexplained" is a number in the log rather
// than something a reader has to compute and get wrong.
void write_emu_attrib(const NdsEmuProfile& now, uint64_t frame_delta,
                      double emu_ms_per_frame) {
    const uint64_t rounds = sub_u64(now.rounds, g_prev_emu.rounds);
    const uint64_t sampled =
        sub_u64(now.sampled_rounds, g_prev_emu.sampled_rounds);
    // Unbiased extrapolation of the sampled buckets. Both denominators are in
    // the record too, so the scaling is auditable and not an assumption.
    const double scale = sampled ? static_cast<double>(rounds) /
                                       static_cast<double>(sampled)
                                 : 0.0;
    const double ns_per_tick = nds_dispatch_timing_ns_per_tick();
    const double frames = frame_delta ? static_cast<double>(frame_delta) : 1.0;
    double accounted_ms = 0.0;
    std::fprintf(g_perf,
        "{\"modulus\":%llu,\"rounds\":%llu,\"sampled_rounds\":%llu,"
        "\"scale\":%.4f,\"gxstall_rounds\":%llu,\"reads\":%llu,"
        "\"buckets\":{",
        (unsigned long long)nds_emu_modulus(),
        (unsigned long long)rounds,
        (unsigned long long)sampled,
        scale,
        (unsigned long long)sub_u64(now.gxstall_rounds,
                                    g_prev_emu.gxstall_rounds),
        (unsigned long long)sub_u64(now.reads, g_prev_emu.reads));
    for (int i = 0; i < NDS_EMU_BUCKET_COUNT; ++i) {
        const NdsEmuBucket bucket = static_cast<NdsEmuBucket>(i);
        const bool exact = nds_emu_bucket_is_exact(bucket);
        const uint64_t ticks = sub_u64(now.ticks[i], g_prev_emu.ticks[i]);
        const double ns = static_cast<double>(ticks) * ns_per_tick;
        const double ms = (ns * (exact ? 1.0 : scale)) / 1e6 / frames;
        accounted_ms += ms;
        std::fprintf(g_perf,
            "%s\"%s\":{\"ns\":%llu,\"ms_per_frame\":%.4f,"
            "\"entries\":%llu,\"exact\":%s}",
            i ? "," : "", nds_emu_bucket_name(bucket),
            (unsigned long long)ns, ms,
            (unsigned long long)sub_u64(now.entries[i], g_prev_emu.entries[i]),
            exact ? "true" : "false");
    }
    // Non-additive: a breakdown of exec_arm9/exec_arm7, never an addend. Kept
    // out of accounted_ms deliberately -- adding it would double-count the
    // guest-execution time it is a subdivision of.
    const uint64_t bus_events =
        sub_u64(now.bus_events, g_prev_emu.bus_events);
    uint64_t bus_samples_total = 0;
    for (int i = 0; i < NDS_EMU_BUS_PATH_COUNT; ++i)
        bus_samples_total +=
            sub_u64(now.bus_samples[i], g_prev_emu.bus_samples[i]);
    std::fprintf(g_perf, "},\"bus\":{\"events\":%llu,\"paths\":{",
                 (unsigned long long)bus_events);
    for (int i = 0; i < NDS_EMU_BUS_PATH_COUNT; ++i) {
        const uint64_t samples =
            sub_u64(now.bus_samples[i], g_prev_emu.bus_samples[i]);
        const uint64_t ticks =
            sub_u64(now.bus_ticks[i], g_prev_emu.bus_ticks[i]);
        const double ns = static_cast<double>(ticks) * ns_per_tick;
        // Whole-interval estimate: mean cost of this path times the share of
        // the exact event total the path's samples represent.
        const double est_ms = (bus_samples_total && samples)
            ? (ns / static_cast<double>(samples)) *
                  (static_cast<double>(samples) /
                   static_cast<double>(bus_samples_total)) *
                  static_cast<double>(bus_events) / 1e6 / frames
            : 0.0;
        std::fprintf(g_perf,
            "%s\"%s\":{\"ns\":%llu,\"samples\":%llu,"
            "\"est_ms_per_frame\":%.4f}",
            i ? "," : "",
            nds_emu_bus_path_name(static_cast<NdsEmuBusPath>(i)),
            (unsigned long long)ns, (unsigned long long)samples, est_ms);
    }
    const double residual_ms = emu_ms_per_frame - accounted_ms;
    const double residual_pct = emu_ms_per_frame > 0.0
        ? 100.0 * residual_ms / emu_ms_per_frame
        : 0.0;
    std::fprintf(g_perf,
        "}},\"accounted_ms_per_frame\":%.4f,\"emu_ms_per_frame\":%.4f,"
        "\"residual_ms_per_frame\":%.4f,\"residual_pct\":%.2f}",
        accounted_ms, emu_ms_per_frame, residual_ms, residual_pct);
}

void write_dispatch_delta(const NdsDispatchStats& now,
                          const NdsDispatchStats& before) {
    std::fprintf(g_perf,
        "{\"resume_dispatch\":%llu,\"dispatch_total\":%llu,"
        "\"dispatch_slice_yield\":%llu,\"dispatch_exchange\":%llu,"
        "\"literal_branch\":%llu,\"literal_call\":%llu,"
        "\"literal_fallthrough\":%llu,\"exception_dispatch\":%llu,"
        "\"cache_hit\":%llu,\"cache_hit_absent\":%llu,"
        "\"cache_slow_lookup\":%llu,\"crs_push\":%llu,"
        "\"crs_hit\":%llu,\"crs_miss\":%llu,\"crs_scan_iters\":%llu}",
        (unsigned long long)sub_u64(now.resume_dispatch, before.resume_dispatch),
        (unsigned long long)sub_u64(now.dispatch_total, before.dispatch_total),
        (unsigned long long)sub_u64(now.dispatch_slice_yield,
                                    before.dispatch_slice_yield),
        (unsigned long long)sub_u64(now.dispatch_exchange,
                                    before.dispatch_exchange),
        (unsigned long long)sub_u64(now.literal_branch, before.literal_branch),
        (unsigned long long)sub_u64(now.literal_call, before.literal_call),
        (unsigned long long)sub_u64(now.literal_fallthrough,
                                    before.literal_fallthrough),
        (unsigned long long)sub_u64(now.exception_dispatch,
                                    before.exception_dispatch),
        (unsigned long long)sub_u64(now.cache_hit, before.cache_hit),
        (unsigned long long)sub_u64(now.cache_hit_absent,
                                    before.cache_hit_absent),
        (unsigned long long)sub_u64(now.cache_slow_lookup,
                                    before.cache_slow_lookup),
        (unsigned long long)sub_u64(now.crs_push, before.crs_push),
        (unsigned long long)sub_u64(now.crs_hit, before.crs_hit),
        (unsigned long long)sub_u64(now.crs_miss, before.crs_miss),
        (unsigned long long)sub_u64(now.crs_scan_iters,
                                    before.crs_scan_iters));
}

// Per-class dispatch COST for this interval. Every bucket carries ns,
// the sample count that produced it, and the exact event count -- so a
// reader computes mean cost as ns/samples and whole-interval cost as
// (ns/samples)*events, and can verify the realised sampling ratio instead of
// assuming it. Emitting ns without its denominators is precisely the defect
// that made the scheduler buckets unusable in field analysis.
// cache_paths `events` here is the DISPATCHER-only lookup population, which
// is what cache_samples was drawn from. The all-consumers totals (including
// Tier 3's per-instruction poll) are the cache_hit / cache_hit_absent /
// cache_slow_lookup fields of dispatch_delta in the same record.
void write_timing_delta(const NdsDispatchTiming& now,
                        const NdsDispatchTiming& before) {
    std::fputs("{\"classes\":{", g_perf);
    for (int i = 0; i < NDS_DISPATCH_CLASS_COUNT; ++i) {
        if (i) std::fputc(',', g_perf);
        std::fprintf(g_perf,
                     "\"%s\":{\"ns\":%llu,\"samples\":%llu,\"events\":%llu}",
                     nds_dispatch_class_name(
                         static_cast<NdsDispatchClass>(i)),
                     (unsigned long long)sub_u64(now.class_ns[i],
                                                 before.class_ns[i]),
                     (unsigned long long)sub_u64(now.class_samples[i],
                                                 before.class_samples[i]),
                     (unsigned long long)sub_u64(now.class_events[i],
                                                 before.class_events[i]));
    }
    // Nested inside the class regions above -- a breakdown of where the
    // dispatch prologue spends its time, NOT an addend to class ns.
    std::fputs("},\"cache_paths\":{", g_perf);
    for (int i = 0; i < NDS_DISPATCH_CACHE_PATH_COUNT; ++i) {
        if (i) std::fputc(',', g_perf);
        std::fprintf(g_perf,
                     "\"%s\":{\"ns\":%llu,\"samples\":%llu,\"events\":%llu}",
                     nds_dispatch_cache_path_name(
                         static_cast<NdsDispatchCachePath>(i)),
                     (unsigned long long)sub_u64(now.cache_ns[i],
                                                 before.cache_ns[i]),
                     (unsigned long long)sub_u64(now.cache_samples[i],
                                                 before.cache_samples[i]),
                     (unsigned long long)sub_u64(now.cache_events[i],
                                                 before.cache_events[i]));
    }
    std::fputs("}}", g_perf);
}

// The hottest guest PCs of THIS interval, per CPU, for one population
// (pc_profile.h). Answers the question emu_attrib structurally cannot:
// exec_arm9 is the biggest bucket -- which guest code. Pairs rather than
// objects because eight of them appear in every record for each of two
// populations and the key names would be three times the payload; the order
// is [pc, count].
//
// BOTH populations ship, because they answer different questions and the
// difference is not derivable from either one alone:
//
//   pc_hot_delta  -- round-boundary PCs. Counts are out of
//                    emu_attrib.rounds/31, and in a normally-paced title they
//                    are dominated by the idle/halt loops, which is the
//                    measurement: the top entry's share IS the halt share.
//   pc_exec_delta -- dispatch-entry PCs, one sample per kNdsPcExecGate
//                    entries. Entry-frequency weighted, never time weighted;
//                    this is the list that ranks hot entry points.
//
// Top EIGHT each because a per-interval record is read by eye first: the full
// tables are available on demand over the debug server's pc_hot command
// ("kind":"park"|"exec"), and a bundle wanting more depth wants the shard/bank
// map, not more rows here.
void write_pc_delta(NdsPcHotKind kind) {
    static const char* const kNames[2] = {"arm9", "arm7"};
    std::fputc('{', g_perf);
    for (int cpu = 0; cpu < 2; ++cpu) {
        const NdsPcHotTable& now = nds_pc_profile_table(kind, cpu);
        NdsPcHotEntry top[8];
        const unsigned count =
            nds_pc_profile_top_delta(now, &g_prev_pc[kind][cpu], top, 8u);
        std::fprintf(g_perf, "%s\"%s\":[", cpu ? "," : "", kNames[cpu]);
        for (unsigned i = 0; i < count; ++i) {
            std::fprintf(g_perf, "%s[%u,%llu]", i ? "," : "",
                         top[i].pc, (unsigned long long)top[i].count);
        }
        std::fputc(']', g_perf);
    }
    std::fputc('}', g_perf);
}

// Re-baseline both populations for both CPUs. One helper, called from every
// place the other g_prev_* baselines are taken -- a population baselined in
// three of the four places would emit one wrong interval after a savestate
// load, and nothing would go red.
void snapshot_pc_baselines() {
    for (int kind = 0; kind < NDS_PC_HOT_KIND_COUNT; ++kind)
        for (int cpu = 0; cpu < 2; ++cpu)
            g_prev_pc[kind][cpu] =
                nds_pc_profile_table(static_cast<NdsPcHotKind>(kind), cpu);
}

void write_hist_delta(const uint64_t now[7], const uint64_t before[7]) {
    std::fputc('[', g_perf);
    for (int i = 0; i < 7; ++i) {
        if (i) std::fputc(',', g_perf);
        std::fprintf(g_perf, "%llu",
                     (unsigned long long)sub_u64(now[i], before[i]));
    }
    std::fputc(']', g_perf);
}

}  // namespace

void nds_diagnostics_configure(bool enabled, const char* directory,
                               uint32_t interval_ms) {
    g_enabled = enabled;
    g_directory = directory ? directory : "";
    g_interval_ms = std::max<uint32_t>(250u, interval_ms ? interval_ms : 2000u);
    g_run_stamp.clear();
    ensure_directory();
}

void nds_diagnostics_enable_profile_environment() {
    if (!g_enabled) return;
#ifdef _WIN32
    if (!std::getenv("NDS_PROFILE_GPU")) _putenv_s("NDS_PROFILE_GPU", "1");
    if (!std::getenv("NDS_PROFILE_SCHED"))
        _putenv_s("NDS_PROFILE_SCHED", "1009");
#else
    if (!std::getenv("NDS_PROFILE_GPU")) setenv("NDS_PROFILE_GPU", "1", 0);
    if (!std::getenv("NDS_PROFILE_SCHED"))
        setenv("NDS_PROFILE_SCHED", "1009", 0);
#endif
}

bool nds_diagnostics_enabled() { return g_enabled; }

std::string nds_diagnostics_directory() { return g_directory; }

std::string nds_diagnostics_run_stamp() { return run_stamp(); }

std::string nds_diagnostics_make_path(const char* filename) {
    if (!filename || !filename[0]) return {};
    if (g_directory.empty()) return filename;
    return (std::filesystem::path(g_directory) / filename).string();
}

std::string nds_diagnostics_run_base(const char* stem) {
    const std::string safe_stem = (stem && stem[0]) ? stem : "nds";
    return nds_diagnostics_make_path(
        (safe_stem + "-" + run_stamp()).c_str());
}

std::string nds_diagnostics_dispatch_miss_log_path() {
    return nds_diagnostics_make_path(
        ("dispatch_misses-" + run_stamp() + ".log").c_str());
}

void nds_diagnostics_set_identity(const char* rom_sha1, const char* rom_name,
                                  const char* build_id) {
    g_rom_sha1 = rom_sha1 ? rom_sha1 : "";
    g_rom_name = rom_name ? rom_name : "";
    g_build_id = build_id ? build_id : "";
}

void nds_diagnostics_set_versions(const char* framework_version,
                                  const char* game_version) {
    g_framework_version = framework_version ? framework_version : "";
    g_game_version = game_version ? game_version : "";
}

void nds_diagnostics_set_rom_header(const char* game_code, uint32_t revision,
                                    uint64_t rom_size) {
    g_rom_game_code = game_code ? game_code : "";
    g_rom_revision = revision;
    g_rom_size = rom_size;
}

void nds_diagnostics_start_performance_log(const NdsFrontendOptions& options) {
    if (!g_enabled || g_perf) return;
    ensure_directory();
    const std::string path = nds_diagnostics_make_path(
        ("performance-" + run_stamp() + ".jsonl").c_str());
    g_perf = std::fopen(path.c_str(), "wb");
    if (!g_perf) {
        std::fprintf(stderr, "[diagnostics] could not write %s\n",
                     path.c_str());
        return;
    }
    std::fprintf(stderr, "[diagnostics] writing %s\n", path.c_str());
    const NdsGpu3dRendererPolicy renderer_policy =
        nds_gpu3d_renderer_policy();
    const bool compute_built = nds_gpu3d_compute_renderer_built();
    const bool compute_preferred = nds_gpu3d_renderer_prefers_compute();
    const bool compute_required = nds_gpu3d_renderer_requires_compute();
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    const bool compute_active = nds_compute_host_active();
    const bool direct_present = nds_compute_host_has_visible_context();
#else
    const bool compute_active = false;
    const bool direct_present = false;
#endif
    const NdsHostInfo& host = nds_host_info();
    const std::string gpu_renderer = nds_host_gpu_renderer_string();
    NdsLiveOverlaySummary overlay{};
    live_overlay_summary(&overlay);
    const std::string session_wall = wall_clock_now();
    std::fprintf(g_perf,
        // ts_ms/wall on the session record as well as every perf record: the
        // session line is the anchor a bundle is read from, and without a
        // wall-clock origin the whole file is a timeline with no zero.
        "{\"kind\":\"session\",\"ts_ms\":%llu,\"wall\":\"%s\","
        "\"build_id\":\"%s\","
        "\"framework_version\":\"%s\",\"game_version\":\"%s\","
        "\"rom_sha1\":\"%s\","
        "\"rom_name\":\"%s\",\"rom_game_code\":\"%s\","
        "\"rom_revision\":%u,\"rom_size\":%llu,\"interval_ms\":%u,"
        "\"host\":{\"cpu_brand\":\"%s\",\"cpu_vendor\":\"%s\","
        "\"cpu_cores_physical\":%u,\"cpu_cores_logical\":%u,"
        "\"ram_bytes\":%llu,\"ram_gb\":%.2f,\"os_name\":\"%s\","
        "\"os_version\":\"%s\",\"arch\":\"%s\",\"gpu_renderer\":\"%s\"},"
        "\"dispatch_timing\":{\"modulus\":%llu,\"ns_per_tick\":%.6f,"
        "\"clock_overhead_ticks\":%llu},"
        "\"live_overlay\":{\"enabled\":%s,\"active\":%s,\"backend_tier\":%u,"
        "\"banks_loaded\":%llu,\"banks_rejected\":%llu,"
        "\"registered_banks\":%u,\"native_hits\":%llu,"
        "\"bank_rejects\":%llu,\"futile_runs\":%llu,"
        "\"auto_suppressed\":%s,"
        // Queue policy as it stands at session start: how much work the
        // persisted queue is carrying, and the cap/cooldown the next batch
        // will actually use. Without these a field bundle cannot tell a
        // converged install from one that never got a second batch.
        "\"pending_candidates\":%llu,\"batch_cap\":%u,"
        "\"cooldown_ms\":%u,\"persisted_backlog\":%s%s},"
        "\"renderer\":{\"policy\":\"%s\",\"effective\":\"%s\","
        "\"compute_built\":%s,\"compute_preferred\":%s,"
        "\"compute_required\":%s,\"direct_present\":%s},\"settings\":{"
        "\"frame_interpolation\":\"%s\","
        "\"screen_layout\":\"%s\",\"fullscreen\":\"%s\","
        "\"adaptive_screens\":\"%s\",\"adaptive_supported\":\"%s\","
        "\"adaptive_width_top\":%u,\"adaptive_width_bottom\":%u,"
        "\"internal_resolution\":%u,"
        "\"texture_upscale\":%u,\"supersampling\":%u,\"antialiasing\":%u,"
        "\"performance_governor\":\"%s\","
        "\"relative_mouse_touch\":%s,\"virtual_stylus\":%s,"
        "\"mph_prime_controls\":%s,"
        "\"mph_prime_unified_window_focus\":%s,"
        "\"network_enabled\":%s,\"wfc_enabled\":%s,"
        "\"local_wireless_enabled\":%s}}\n",
        (unsigned long long)unix_ms_now(),
        json_escape(session_wall.c_str()).c_str(),
        json_escape(g_build_id.c_str()).c_str(),
        json_escape(g_framework_version.c_str()).c_str(),
        json_escape(g_game_version.c_str()).c_str(),
        json_escape(g_rom_sha1.c_str()).c_str(),
        json_escape(g_rom_name.c_str()).c_str(),
        json_escape(g_rom_game_code.c_str()).c_str(),
        g_rom_revision, (unsigned long long)g_rom_size, g_interval_ms,
        json_escape(host.cpu_brand.c_str()).c_str(),
        json_escape(host.cpu_vendor.c_str()).c_str(),
        host.cpu_cores_physical, host.cpu_cores_logical,
        (unsigned long long)host.ram_bytes,
        static_cast<double>(host.ram_bytes) / (1024.0 * 1024.0 * 1024.0),
        json_escape(host.os_name.c_str()).c_str(),
        json_escape(host.os_version.c_str()).c_str(),
        json_escape(host.arch.c_str()).c_str(),
        json_escape(gpu_renderer.c_str()).c_str(),
        (unsigned long long)nds_dispatch_timing_modulus(),
        nds_dispatch_timing_ns_per_tick(),
        (unsigned long long)nds_dispatch_timing_clock_overhead_ticks(),
        overlay.enabled ? "true" : "false",
        overlay.active ? "true" : "false",
        overlay.backend_tier,
        (unsigned long long)overlay.banks_loaded,
        (unsigned long long)overlay.banks_rejected,
        overlay.registered_banks,
        (unsigned long long)overlay.native_hits,
        (unsigned long long)overlay.bank_rejects,
        (unsigned long long)overlay.futile_runs,
        overlay.auto_suppressed ? "true" : "false",
        (unsigned long long)overlay.pending_candidates,
        overlay.batch_cap, overlay.cooldown_ms,
        overlay.persisted_backlog ? "true" : "false",
        overlay_reject_json(overlay).c_str(),
        nds_gpu3d_renderer_policy_name(renderer_policy),
        compute_active ? "compute" : "soft",
        compute_built ? "true" : "false",
        compute_preferred ? "true" : "false",
        compute_required ? "true" : "false",
        direct_present ? "true" : "false",
        nds_frame_interpolation_name(options.frame_interpolation),
        nds_screen_layout_name(options.screen_layout),
        nds_fullscreen_mode_name(options.fullscreen),
        nds_adaptive_screens_name(options.adaptive_screens),
        nds_adaptive_screens_name(options.adaptive_supported),
        options.adaptive_max_width[0], options.adaptive_max_width[1],
        options.internal_resolution, options.texture_upscale,
        options.supersampling, options.antialiasing,
        nds_perf_governor_mode_name(options.perf_governor_mode),
        options.relative_mouse_touch ? "true" : "false",
        options.virtual_stylus.enabled ? "true" : "false",
        options.mph_prime_controls ? "true" : "false",
        options.mph_prime_unified_window_focus ? "true" : "false",
        options.network.enabled ? "true" : "false",
        options.network.wfc_enabled ? "true" : "false",
        options.local_wireless.enabled ? "true" : "false");
    std::fflush(g_perf);
    // Baselines for the first interval's pc_hot_delta and pc_exec_delta, taken
    // with every other prev-state snapshot below.
    snapshot_pc_baselines();
    g_last_ticks = 0;
    g_prev_frontend = {};
    g_prev_tier3 = tier3_stats();
    g_prev_dispatch[0] = g_nds_dispatch_stats[0];
    g_prev_dispatch[1] = g_nds_dispatch_stats[1];
    g_prev_timing[0] = g_nds_dispatch_timing[0];
    g_prev_timing[1] = g_nds_dispatch_timing[1];
    nds_wifi_local_mp_stats(&g_prev_local_mp);
    // Prime the profile_totals_delta / emu_attrib baselines from the same
    // instant as every other prev-state snapshot, so the first emitted
    // interval is a real delta and not the whole run so far.
    scheduler_profile(&g_prev_sched);
    nds_gpu2d_profile(&g_prev_gpu2d);
    nds_gpu3d_profile(&g_prev_gpu3d);
    nds_emu_profile(&g_prev_emu);
}

void nds_diagnostics_maybe_write_performance_sample(
    const NdsFrontendLiveStats& stats) {
    if (!g_enabled || !g_perf || !stats.active || !stats.freq ||
        !stats.now_ticks) {
        return;
    }
    if (g_last_ticks == 0) {
        g_last_ticks = stats.now_ticks;
        g_prev_frontend = stats;
        g_prev_tier3 = tier3_stats();
        g_prev_dispatch[0] = g_nds_dispatch_stats[0];
        g_prev_dispatch[1] = g_nds_dispatch_stats[1];
        g_prev_timing[0] = g_nds_dispatch_timing[0];
        g_prev_timing[1] = g_nds_dispatch_timing[1];
        nds_wifi_local_mp_stats(&g_prev_local_mp);
        scheduler_profile(&g_prev_sched);
        nds_gpu2d_profile(&g_prev_gpu2d);
        nds_gpu3d_profile(&g_prev_gpu3d);
        nds_emu_profile(&g_prev_emu);
        snapshot_pc_baselines();
        return;
    }
    if (g_last_ticks != 0) {
        const uint64_t elapsed_ticks = stats.now_ticks - g_last_ticks;
        const uint64_t threshold =
            (stats.freq * static_cast<uint64_t>(g_interval_ms)) / 1000u;
        if (elapsed_ticks < threshold) return;
    }

    const NdsFrontendLiveStats before = g_prev_frontend;
    const uint64_t frame_delta = sub_u64(stats.frames, before.frames);
    const uint64_t elapsed_ticks =
        g_last_ticks ? sub_u64(stats.now_ticks, g_last_ticks) : 0u;
    const double fps = elapsed_ticks
        ? static_cast<double>(frame_delta) * static_cast<double>(stats.freq) /
              static_cast<double>(elapsed_ticks)
        : 0.0;
    const Tier3Stats tier3 = tier3_stats();
    NdsLocalMpStats local_mp{};
    nds_wifi_local_mp_stats(&local_mp);
    NdsSchedulerDebugState sched_state{};
    scheduler_debug_state(&sched_state);
    NdsSchedulerProfile sched_profile{};
    scheduler_profile(&sched_profile);
    NdsGpu2dProfile gpu2d{};
    nds_gpu2d_profile(&gpu2d);
    NdsGpu3dProfile gpu3d{};
    nds_gpu3d_profile(&gpu3d);
    NdsEmuProfile emu{};
    nds_emu_profile(&emu);

    const std::string sample_wall = wall_clock_now();
    std::fprintf(g_perf,
        // Wall clock first, so a record can be located by eye or by a
        // timestamp from outside the log (a savestate event below, a crash
        // dump, a screen recording) without counting frame gaps.
        "{\"kind\":\"perf\",\"ts_ms\":%llu,\"wall\":\"%s\","
        "\"frames\":%llu,\"frame_delta\":%llu,"
        "\"fps\":%.3f,\"ms_per_frame\":{\"emu\":%.3f,\"present\":%.3f,"
        "\"adaptive\":%.3f,\"upload\":%.3f,\"draw\":%.3f,\"swap\":%.3f,"
        "\"drain\":%.3f},\"underruns_delta\":%llu,"
        "\"governor\":{\"stage\":%u,\"over_frames\":%u,"
        "\"under_frames\":%u,\"held\":%s,\"apply_failed\":%s,"
        "\"transitions\":%llu,\"compute_readback_ms\":%.3f},"
        "\"cycles\":{\"arm9\":%llu,\"arm7\":%llu},"
        "\"tier3_delta\":{\"arm9\":{\"entries\":%llu,"
        "\"instructions\":%llu,\"clean_ram_rejects\":%llu},"
        "\"arm7\":{\"entries\":%llu,\"instructions\":%llu,"
        "\"clean_ram_rejects\":%llu}},\"dispatch_delta\":{\"arm9\":",
        (unsigned long long)unix_ms_now(),
        json_escape(sample_wall.c_str()).c_str(),
        (unsigned long long)stats.frames,
        (unsigned long long)frame_delta,
        fps,
        per_frame_ms(sub_u64(stats.emu_ticks, before.emu_ticks),
                     frame_delta, stats.freq),
        per_frame_ms(sub_u64(stats.present_ticks, before.present_ticks),
                     frame_delta, stats.freq),
        per_frame_ms(sub_u64(stats.adaptive_ticks, before.adaptive_ticks),
                     frame_delta, stats.freq),
        per_frame_ms(sub_u64(stats.upload_ticks, before.upload_ticks),
                     frame_delta, stats.freq),
        per_frame_ms(sub_u64(stats.draw_ticks, before.draw_ticks),
                     frame_delta, stats.freq),
        per_frame_ms(sub_u64(stats.swap_ticks, before.swap_ticks),
                     frame_delta, stats.freq),
        per_frame_ms(sub_u64(stats.drain_ticks, before.drain_ticks),
                     frame_delta, stats.freq),
        (unsigned long long)sub_u64(stats.underruns, before.underruns),
        stats.perf_governor_stage,
        stats.perf_governor_over_frames,
        stats.perf_governor_under_frames,
        stats.perf_governor_held ? "true" : "false",
        stats.perf_governor_apply_failed ? "true" : "false",
        (unsigned long long)stats.perf_governor_transitions,
        // The always-on readback stall: the quantity stage 1 defers.
        frame_delta ? static_cast<double>(
                          sub_u64(gpu3d.compute_readback_ns,
                                  g_prev_gpu3d.compute_readback_ns)) /
                          1000000.0 / static_cast<double>(frame_delta)
                    : 0.0,
        (unsigned long long)sched_state.cycles[0],
        (unsigned long long)sched_state.cycles[1],
        (unsigned long long)sub_u64(tier3.entries[0],
                                    g_prev_tier3.entries[0]),
        (unsigned long long)sub_u64(tier3.instructions[0],
                                    g_prev_tier3.instructions[0]),
        (unsigned long long)sub_u64(tier3.clean_ram_rejects[0],
                                    g_prev_tier3.clean_ram_rejects[0]),
        (unsigned long long)sub_u64(tier3.entries[1],
                                    g_prev_tier3.entries[1]),
        (unsigned long long)sub_u64(tier3.instructions[1],
                                    g_prev_tier3.instructions[1]),
        (unsigned long long)sub_u64(tier3.clean_ram_rejects[1],
                                    g_prev_tier3.clean_ram_rejects[1]));
    write_dispatch_delta(g_nds_dispatch_stats[0], g_prev_dispatch[0]);
    std::fprintf(g_perf, ",\"arm7\":");
    write_dispatch_delta(g_nds_dispatch_stats[1], g_prev_dispatch[1]);
    std::fprintf(g_perf, "},\"dispatch_cost_delta\":{\"arm9\":");
    write_timing_delta(g_nds_dispatch_timing[0], g_prev_timing[0]);
    std::fprintf(g_perf, ",\"arm7\":");
    write_timing_delta(g_nds_dispatch_timing[1], g_prev_timing[1]);
    std::fprintf(g_perf,
        "},\"local_mp_delta\":{\"enabled\":%s,\"frames_sent\":%llu,"
        "\"frames_received\":%llu,\"recv_replies_calls\":%llu,"
        "\"recv_replies_timeouts\":%llu,\"recv_replies_wait_us\":%llu,"
        "\"recv_host_calls\":%llu,\"recv_host_timeouts\":%llu,"
        "\"recv_host_wait_us\":%llu,\"stale_reply_drops\":%llu,"
        "\"reply_latency_ms\":",
        local_mp.enabled ? "true" : "false",
        (unsigned long long)sub_u64(local_mp.frames_sent,
                                    g_prev_local_mp.frames_sent),
        (unsigned long long)sub_u64(local_mp.frames_received,
                                    g_prev_local_mp.frames_received),
        (unsigned long long)sub_u64(local_mp.recv_replies_calls,
                                    g_prev_local_mp.recv_replies_calls),
        (unsigned long long)sub_u64(local_mp.recv_replies_timeouts,
                                    g_prev_local_mp.recv_replies_timeouts),
        (unsigned long long)sub_u64(local_mp.recv_replies_wait_us,
                                    g_prev_local_mp.recv_replies_wait_us),
        (unsigned long long)sub_u64(local_mp.recv_host_calls,
                                    g_prev_local_mp.recv_host_calls),
        (unsigned long long)sub_u64(local_mp.recv_host_timeouts,
                                    g_prev_local_mp.recv_host_timeouts),
        (unsigned long long)sub_u64(local_mp.recv_host_wait_us,
                                    g_prev_local_mp.recv_host_wait_us),
        (unsigned long long)sub_u64(local_mp.stale_reply_drops,
                                    g_prev_local_mp.stale_reply_drops));
    write_hist_delta(local_mp.reply_latency_ms,
                     g_prev_local_mp.reply_latency_ms);
    std::fprintf(g_perf, ",\"turnaround_ms\":");
    write_hist_delta(local_mp.turnaround_ms, g_prev_local_mp.turnaround_ms);
    NdsLiveOverlaySummary overlay{};
    live_overlay_summary(&overlay);
    std::fprintf(g_perf,
        "},\"profile_totals\":{\"scheduler_sampled_rounds\":%llu,"
        // scheduler_rounds is the EXACT total; sampled_rounds is how many of
        // them were timed. Both are required to scale the sampled ns to the
        // whole interval. Emitting only sampled_rounds is what forced field
        // analysis to guess the ratio and derive a 106.6-percent-of-wall
        // scheduler total.
        "\"scheduler_rounds\":%llu,"
        "\"scheduler_arm9_ns\":%llu,\"scheduler_arm7_ns\":%llu,"
        "\"scheduler_devices_ns\":%llu,"
        // Device split: all present in NdsSchedulerProfile since it was
        // written, never serialized until now. Without them "devices_ns"
        // is a single opaque number that cannot indict display vs SPU vs
        // Wi-Fi. switch_ns shares the sampled rounds; switches and crs_words
        // are exact whole-run counters.
        "\"scheduler_display_ns\":%llu,\"scheduler_spu_ns\":%llu,"
        "\"scheduler_wifi_ns\":%llu,\"scheduler_rtc_ns\":%llu,"
        "\"scheduler_sysev_ns\":%llu,\"scheduler_switch_ns\":%llu,"
        "\"scheduler_switches\":%llu,\"scheduler_crs_words\":%llu,"
        "\"scheduler_next_event_ns\":%llu,"
        "\"scheduler_sampled_round_ns\":%llu,"
        "\"gpu2d_render_ns\":%llu,"
        "\"gpu2d_hd_frames\":%llu,\"gpu2d_hd_presented\":%llu,"
        "\"gpu3d_compute_sync_ns\":%llu,"
        "\"gpu3d_compute_sync_calls\":%llu,"
        // The submit/map split separates GPU-side work queued from the
        // readback stall, which the single sync bucket conflates.
        "\"gpu3d_compute_submit_ns\":%llu,"
        "\"gpu3d_compute_submit_calls\":%llu,"
        "\"gpu3d_compute_map_ns\":%llu,"
        "\"gpu3d_compute_map_calls\":%llu},"
        // NOTE: everything in profile_totals above is a CUMULATIVE run total.
        // profile_totals_delta and emu_attrib, emitted after live_overlay, are
        // the per-interval numbers -- use those to compare against
        // ms_per_frame, which is itself per-interval.
        "\"live_overlay\":{\"active\":%s,\"backend_tier\":%u,"
        "\"banks_loaded\":%llu,\"banks_rejected\":%llu,"
        "\"registered_banks\":%u,\"native_hits\":%llu,"
        "\"bank_rejects\":%llu,\"tier3_arm9\":%llu,\"tier3_arm7\":%llu,"
        // Per-interval so the drain rate is directly measurable from a
        // bundle: pending_candidates falling across intervals IS the queue
        // converging, and batch_cap/cooldown_ms show the policy that did it.
        "\"pending_candidates\":%llu,\"batch_cap\":%u,\"cooldown_ms\":%u,"
        // busy says a compiler child was running when this interval was
        // sampled. Splitting a session's intervals on it is the whole
        // frame-time-theft check, self-contained in one log.
        "\"busy\":%s,\"runs_started\":%llu%s}",
        (unsigned long long)sched_profile.sampled_rounds,
        (unsigned long long)sched_profile.rounds,
        (unsigned long long)sched_profile.arm9_ns,
        (unsigned long long)sched_profile.arm7_ns,
        (unsigned long long)sched_profile.devices_ns,
        (unsigned long long)sched_profile.display_ns,
        (unsigned long long)sched_profile.spu_ns,
        (unsigned long long)sched_profile.wifi_ns,
        (unsigned long long)sched_profile.rtc_ns,
        (unsigned long long)sched_profile.sysev_ns,
        (unsigned long long)sched_profile.switch_ns,
        (unsigned long long)sched_profile.switches,
        (unsigned long long)sched_profile.crs_words,
        (unsigned long long)sched_profile.next_event_ns,
        (unsigned long long)sched_profile.sampled_round_ns,
        (unsigned long long)gpu2d.render_ns,
        (unsigned long long)gpu2d.hd_frames,
        (unsigned long long)gpu2d.hd_presented,
        (unsigned long long)gpu3d.compute_sync_ns,
        (unsigned long long)gpu3d.compute_sync_calls,
        (unsigned long long)gpu3d.compute_submit_ns,
        (unsigned long long)gpu3d.compute_submit_calls,
        (unsigned long long)gpu3d.compute_map_ns,
        (unsigned long long)gpu3d.compute_map_calls,
        overlay.active ? "true" : "false",
        overlay.backend_tier,
        (unsigned long long)overlay.banks_loaded,
        (unsigned long long)overlay.banks_rejected,
        overlay.registered_banks,
        (unsigned long long)overlay.native_hits,
        (unsigned long long)overlay.bank_rejects,
        (unsigned long long)overlay.tier3[0],
        (unsigned long long)overlay.tier3[1],
        (unsigned long long)overlay.pending_candidates,
        overlay.batch_cap, overlay.cooldown_ms,
        overlay.busy ? "true" : "false",
        (unsigned long long)overlay.runs_started,
        overlay_reject_json(overlay).c_str());
    std::fprintf(g_perf, ",\"profile_totals_delta\":");
    write_profile_totals_delta(sched_profile, gpu2d, gpu3d);
    std::fprintf(g_perf, ",\"emu_attrib\":");
    write_emu_attrib(emu, frame_delta,
                     per_frame_ms(sub_u64(stats.emu_ticks, before.emu_ticks),
                                  frame_delta, stats.freq));
    // Emitted right after emu_attrib because it is read with it: emu_attrib
    // says which bucket, the two pc blocks say which guest code inside it --
    // pc_hot_delta how idle each core was, pc_exec_delta which entry points
    // carried the dispatches. Both, always: reading either alone is how the
    // first cut of this block spent a field session reporting the idle loop as
    // "the hottest code in the game".
    std::fprintf(g_perf, ",\"pc_hot_delta\":");
    write_pc_delta(NDS_PC_HOT_PARK);
    std::fprintf(g_perf, ",\"pc_exec_delta\":");
    write_pc_delta(NDS_PC_HOT_EXEC);
    // Register-address keys with bit 31 = write, not PCs; see NDS_PC_HOT_MMIO.
    std::fprintf(g_perf, ",\"mmio_hot_delta\":");
    write_pc_delta(NDS_PC_HOT_MMIO);
    std::fprintf(g_perf, "}\n");
    std::fflush(g_perf);

    g_last_ticks = stats.now_ticks;
    g_prev_frontend = stats;
    g_prev_tier3 = tier3;
    g_prev_dispatch[0] = g_nds_dispatch_stats[0];
    g_prev_dispatch[1] = g_nds_dispatch_stats[1];
    g_prev_timing[0] = g_nds_dispatch_timing[0];
    g_prev_timing[1] = g_nds_dispatch_timing[1];
    g_prev_local_mp = local_mp;
    g_prev_sched = sched_profile;
    g_prev_gpu2d = gpu2d;
    g_prev_gpu3d = gpu3d;
    g_prev_emu = emu;
    snapshot_pc_baselines();
}

void nds_diagnostics_note_savestate(const char* action, unsigned slot,
                                    bool ok) {
    // No-op with the log closed (diagnostics disabled, or a shortcut pressed
    // after shutdown). A savestate must never depend on diagnostics being on.
    if (!g_perf) return;
    // WHY THIS IS IN THE PERF LOG AT ALL. A savestate load is the single
    // largest discontinuity a session can contain: it resets every host-history
    // accumulator (nds_savestate_reset_host_history), re-primes the interval
    // baselines, and hands the machine a different workload from one frame to
    // the next. Reading a bundle without knowing where those instants were
    // means attributing a load's re-priming to a "dip", and locating them by
    // frame-gap forensics is exactly the guesswork field diagnostics exist to
    // remove. A save is cheaper but no less confusing: it writes a file
    // mid-frame and shows up as one slow interval with no other cause.
    //
    // Emitted as its own {"kind":"event"} line rather than a field on the next
    // perf record: the events are asynchronous to the 2 s cadence, several can
    // land inside one interval, and a consumer that only knows "perf" records
    // skips an unknown kind without breaking.
    std::fprintf(g_perf,
        "{\"kind\":\"event\",\"event\":\"savestate_%s\",\"slot\":%u,"
        "\"ok\":%s,\"ts_ms\":%llu,\"wall\":\"%s\"}\n",
        json_escape(action ? action : "unknown").c_str(), slot,
        ok ? "true" : "false", (unsigned long long)unix_ms_now(),
        json_escape(wall_clock_now().c_str()).c_str());
    // Flushed like every other record: a bundle is usually collected after a
    // crash or a hang, and the events worth locating are the ones nearest the
    // end of the file.
    std::fflush(g_perf);
}

void nds_diagnostics_note_perf_governor_transition(uint8_t from_stage,
                                                   uint8_t to_stage,
                                                   const char* reason) {
    if (!g_perf) return;
    std::fprintf(g_perf,
        "{\"kind\":\"event\",\"event\":\"performance_governor_transition\","
        "\"from_stage\":%u,\"to_stage\":%u,\"reason\":\"%s\","
        "\"ts_ms\":%llu,\"wall\":\"%s\"}\n",
        from_stage, to_stage,
        json_escape(reason ? reason : "").c_str(),
        (unsigned long long)unix_ms_now(),
        json_escape(wall_clock_now().c_str()).c_str());
    std::fflush(g_perf);
}

void nds_diagnostics_write_perf_governor_history() {
    if (!g_perf) return;
    // The ring is always-on, so this record exists even when every individual
    // transition event above was written before the log was opened (or when a
    // load reset the interval baselines). It is the retroactive query, not a
    // second live feed.
    std::array<NdsPerfGovernorTransition, kNdsPerfGovernorHistoryCapacity>
        entries{};
    const uint32_t count = nds_perf_governor_history(
        entries.data(), kNdsPerfGovernorHistoryCapacity);
    std::fprintf(g_perf,
        "{\"kind\":\"governor_history\",\"ts_ms\":%llu,\"wall\":\"%s\","
        "\"total\":%llu,\"capacity\":%u,\"transitions\":[",
        (unsigned long long)unix_ms_now(),
        json_escape(wall_clock_now().c_str()).c_str(),
        (unsigned long long)nds_perf_governor_history_total(),
        kNdsPerfGovernorHistoryCapacity);
    for (uint32_t i = 0; i < count; ++i) {
        const NdsPerfGovernorTransition& e = entries[i];
        std::fprintf(g_perf,
            "%s{\"frame\":%llu,\"ts_ms\":%llu,\"from_stage\":%u,"
            "\"to_stage\":%u,\"reason\":\"%s\",\"held\":%s,"
            "\"apply_failed\":%s}",
            i ? "," : "",
            (unsigned long long)e.frame_index,
            (unsigned long long)e.ts_ms,
            e.from_stage, e.to_stage,
            json_escape(nds_perf_governor_reason_name(e.reason)).c_str(),
            e.stage2_held ? "true" : "false",
            e.apply_failed ? "true" : "false");
    }
    std::fprintf(g_perf, "]}\n");
    std::fflush(g_perf);
}

void nds_diagnostics_write_hostprof_bundle() {
    if (!g_perf) return;
    // The host CPU sampler's whole resident ring plus the module map, as a
    // sidecar next to the perf log. It is a separate FILE rather than a JSONL
    // record because it is 36 MB of fixed-width binary samples -- inlining that
    // as JSON would make the perf log unreadable and cost an order of magnitude
    // in size. The record written here is the pointer to it, so a bundle is
    // still self-describing.
    //
    // The module BASES only mean anything for this run (ASLR), which is exactly
    // why they must be captured with the samples and not reconstructed later.
    const std::string path =
        nds_diagnostics_run_base("hostprof") + ".ndshp";
    char error[256] = {};
    std::string extra;
    const bool ok = nds_hostprof_write_bundle(path.c_str(), error,
                                              sizeof(error), &extra);
    std::fprintf(g_perf,
        "{\"kind\":\"hostprof\",\"ts_ms\":%llu,\"wall\":\"%s\",\"ok\":%s,"
        "\"path\":\"%s\",\"error\":\"%s\"%s,\"status\":%s,\"top\":%s}\n",
        (unsigned long long)unix_ms_now(),
        json_escape(wall_clock_now().c_str()).c_str(),
        ok ? "true" : "false",
        json_escape(path.c_str()).c_str(),
        json_escape(error).c_str(),
        ok ? extra.c_str() : "",
        nds_hostprof_status_json().c_str(),
        // A top-40 inline as well: the binary dump is the complete answer but
        // needs the offline symbolizer, and a reader skimming the bundle should
        // still see the shape of the host profile without running a tool.
        nds_hostprof_top_json(0.0, 40).c_str());
    std::fflush(g_perf);
    if (ok)
        std::fprintf(stderr, "[diagnostics] wrote %s\n", path.c_str());
}

void nds_diagnostics_stop_performance_log() {
    if (!g_perf) return;
    // Last records in the bundle: the whole always-on governor transition ring
    // and the whole always-on host CPU sample ring, so a field report carries
    // both histories even if the run never wrote an interval sample.
    nds_diagnostics_write_perf_governor_history();
    nds_diagnostics_write_hostprof_bundle();
    std::fclose(g_perf);
    g_perf = nullptr;
}

void nds_diagnostics_reset_performance_history() {
    g_last_ticks = 0;
    g_prev_frontend = {};
    g_prev_tier3 = {};
    g_prev_dispatch[0] = {};
    g_prev_dispatch[1] = {};
    g_prev_timing[0] = {};
    g_prev_timing[1] = {};
    g_prev_local_mp = {};
    g_prev_sched = {};
    g_prev_gpu2d = {};
    g_prev_gpu3d = {};
    g_prev_emu = {};
    // RE-PRIMED, not zeroed, unlike every baseline above it. Those mirror
    // accumulators that nds_savestate_reset_host_history() has just reset to
    // zero, so a zero baseline is the correct one. The PC histogram is never
    // reset (pc_profile.h: no reset, ever), so zeroing its baseline would make
    // the next interval report the whole run's counts as if they were one
    // interval's -- the same "cumulative read as a delta" defect that made the
    // 2026-08-28 profile_totals unusable.
    snapshot_pc_baselines();
}
