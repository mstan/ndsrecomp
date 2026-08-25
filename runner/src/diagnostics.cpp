// diagnostics.cpp -- see diagnostics.h.

#include "diagnostics.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>

#include "dispatch_stats.h"
#include "frontend.h"
#include "gpu2d.h"
#include "gpu3d.h"
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
NdsFrontendLiveStats g_prev_frontend{};
Tier3Stats g_prev_tier3{};
NdsDispatchStats g_prev_dispatch[2]{};
NdsLocalMpStats g_prev_local_mp{};

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
    std::fprintf(g_perf,
        "{\"kind\":\"session\",\"build_id\":\"%s\",\"rom_sha1\":\"%s\","
        "\"rom_name\":\"%s\",\"interval_ms\":%u,\"settings\":{"
        "\"screen_layout\":\"%s\",\"fullscreen\":\"%s\","
        "\"adaptive_screens\":\"%s\",\"internal_resolution\":%u,"
        "\"texture_upscale\":%u,\"supersampling\":%u,\"antialiasing\":%u,"
        "\"relative_mouse_touch\":%s,\"mph_prime_controls\":%s,"
        "\"mph_prime_unified_window_focus\":%s,"
        "\"network_enabled\":%s,\"wfc_enabled\":%s,"
        "\"local_wireless_enabled\":%s}}\n",
        json_escape(g_build_id.c_str()).c_str(),
        json_escape(g_rom_sha1.c_str()).c_str(),
        json_escape(g_rom_name.c_str()).c_str(), g_interval_ms,
        nds_screen_layout_name(options.screen_layout),
        nds_fullscreen_mode_name(options.fullscreen),
        nds_adaptive_screens_name(options.adaptive_screens),
        options.internal_resolution, options.texture_upscale,
        options.supersampling, options.antialiasing,
        options.relative_mouse_touch ? "true" : "false",
        options.mph_prime_controls ? "true" : "false",
        options.mph_prime_unified_window_focus ? "true" : "false",
        options.network.enabled ? "true" : "false",
        options.network.wfc_enabled ? "true" : "false",
        options.local_wireless.enabled ? "true" : "false");
    std::fflush(g_perf);
    g_last_ticks = 0;
    g_prev_frontend = {};
    g_prev_tier3 = tier3_stats();
    g_prev_dispatch[0] = g_nds_dispatch_stats[0];
    g_prev_dispatch[1] = g_nds_dispatch_stats[1];
    nds_wifi_local_mp_stats(&g_prev_local_mp);
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
        nds_wifi_local_mp_stats(&g_prev_local_mp);
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

    std::fprintf(g_perf,
        "{\"kind\":\"perf\",\"frames\":%llu,\"frame_delta\":%llu,"
        "\"fps\":%.3f,\"ms_per_frame\":{\"emu\":%.3f,\"present\":%.3f,"
        "\"adaptive\":%.3f,\"upload\":%.3f,\"draw\":%.3f,\"swap\":%.3f,"
        "\"drain\":%.3f},\"underruns_delta\":%llu,"
        "\"cycles\":{\"arm9\":%llu,\"arm7\":%llu},"
        "\"tier3_delta\":{\"arm9\":{\"entries\":%llu,"
        "\"instructions\":%llu,\"clean_ram_rejects\":%llu},"
        "\"arm7\":{\"entries\":%llu,\"instructions\":%llu,"
        "\"clean_ram_rejects\":%llu}},\"dispatch_delta\":{\"arm9\":",
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
    std::fprintf(g_perf,
        "},\"profile_totals\":{\"scheduler_sampled_rounds\":%llu,"
        "\"scheduler_arm9_ns\":%llu,\"scheduler_arm7_ns\":%llu,"
        "\"scheduler_devices_ns\":%llu,\"gpu2d_render_ns\":%llu,"
        "\"gpu2d_hd_frames\":%llu,\"gpu2d_hd_presented\":%llu,"
        "\"gpu3d_compute_sync_ns\":%llu,"
        "\"gpu3d_compute_sync_calls\":%llu}}\n",
        (unsigned long long)sched_profile.sampled_rounds,
        (unsigned long long)sched_profile.arm9_ns,
        (unsigned long long)sched_profile.arm7_ns,
        (unsigned long long)sched_profile.devices_ns,
        (unsigned long long)gpu2d.render_ns,
        (unsigned long long)gpu2d.hd_frames,
        (unsigned long long)gpu2d.hd_presented,
        (unsigned long long)gpu3d.compute_sync_ns,
        (unsigned long long)gpu3d.compute_sync_calls);
    std::fflush(g_perf);

    g_last_ticks = stats.now_ticks;
    g_prev_frontend = stats;
    g_prev_tier3 = tier3;
    g_prev_dispatch[0] = g_nds_dispatch_stats[0];
    g_prev_dispatch[1] = g_nds_dispatch_stats[1];
    g_prev_local_mp = local_mp;
}

void nds_diagnostics_stop_performance_log() {
    if (!g_perf) return;
    std::fclose(g_perf);
    g_perf = nullptr;
}
