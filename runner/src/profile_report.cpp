// profile_report.cpp — see profile_report.h.

#include "profile_report.h"

#include "dispatch_stats.h"
#include "gpu2d.h"
#include "gpu3d.h"
#include "scheduler.h"

void nds_profile_report(std::FILE* out) {
    for (int cpu = 0; cpu < 2; ++cpu) {
        const NdsDispatchStats& d = g_nds_dispatch_stats[cpu];
        if (!d.dispatch_total && !d.crs_push) continue;
        std::fprintf(out,
            "  Dispatch %s: total=%llu (resume=%llu yield=%llu exchange=%llu) "
            "literal branch/call/fallthrough=%llu/%llu/%llu exception=%llu; "
            "cache hit/absent/slow=%llu/%llu/%llu; calls=%llu returns "
            "hit/miss=%llu/%llu scan=%.2f/return\n",
            cpu == 0 ? "arm9" : "arm7",
            (unsigned long long)d.dispatch_total,
            (unsigned long long)d.resume_dispatch,
            (unsigned long long)d.dispatch_slice_yield,
            (unsigned long long)d.dispatch_exchange,
            (unsigned long long)d.literal_branch,
            (unsigned long long)d.literal_call,
            (unsigned long long)d.literal_fallthrough,
            (unsigned long long)d.exception_dispatch,
            (unsigned long long)d.cache_hit,
            (unsigned long long)d.cache_hit_absent,
            (unsigned long long)d.cache_slow_lookup,
            (unsigned long long)d.crs_push,
            (unsigned long long)d.crs_hit,
            (unsigned long long)d.crs_miss,
            static_cast<double>(d.crs_scan_iters) /
                static_cast<double>((d.crs_hit + d.crs_miss)
                                        ? (d.crs_hit + d.crs_miss) : 1));
    }
    NdsGpu2dProfile gpu_profile{};
    nds_gpu2d_profile(&gpu_profile);
    // Threaded-render accounting is maintained on every run, not only under
    // NDS_PROFILE_GPU: fence frequency is the whole performance question.
    if (gpu_profile.threaded_lines || gpu_profile.inline_lines) {
        std::fprintf(out,
            "  GPU2D threading: threaded=%llu inline=%llu helped=%llu "
            "captures_applied=%llu fence_wait=%.3f ms\n",
            (unsigned long long)gpu_profile.threaded_lines,
            (unsigned long long)gpu_profile.inline_lines,
            (unsigned long long)gpu_profile.fence_helped_lines,
            (unsigned long long)gpu_profile.staged_captures,
            static_cast<double>(gpu_profile.fence_wait_ns) / 1.0e6);
        std::fprintf(out, "  GPU2D fences:");
        for (uint32_t i = 0; i < NDS_GPU2D_FENCE_CAUSE_COUNT; ++i) {
            if (!gpu_profile.fence_drains[i]) continue;
            std::fprintf(out, " %s=%llu/%llu",
                         nds_gpu2d_fence_cause_name(i),
                         (unsigned long long)gpu_profile.fence_drains[i],
                         (unsigned long long)gpu_profile.fenced_lines[i]);
        }
        std::fprintf(out, "   (drains/lines)\n");
    }
    if (gpu_profile.scanlines) {
        std::fprintf(out,
            "  GPU2D profile: %.3f seconds (A %.3f, B %.3f, OBJ %.3f) "
            "across %llu scanlines; direct %llu frames / %.3f seconds\n",
            static_cast<double>(gpu_profile.render_ns) / 1.0e9,
            static_cast<double>(gpu_profile.engine_ns[0]) / 1.0e9,
            static_cast<double>(gpu_profile.engine_ns[1]) / 1.0e9,
            static_cast<double>(gpu_profile.obj_ns) / 1.0e9,
            static_cast<unsigned long long>(gpu_profile.scanlines),
            static_cast<unsigned long long>(gpu_profile.direct_frames),
            static_cast<double>(gpu_profile.direct_overlay_ns) / 1.0e9);
        std::fprintf(out,
            "  GPU2D lines: A text[0..4]=%llu/%llu/%llu/%llu/%llu "
            "no-effect=%llu; B=%llu/%llu/%llu/%llu/%llu no-effect=%llu\n",
            (unsigned long long)gpu_profile.text_lines[0][0],
            (unsigned long long)gpu_profile.text_lines[0][1],
            (unsigned long long)gpu_profile.text_lines[0][2],
            (unsigned long long)gpu_profile.text_lines[0][3],
            (unsigned long long)gpu_profile.text_lines[0][4],
            (unsigned long long)gpu_profile.no_effect_lines[0],
            (unsigned long long)gpu_profile.text_lines[1][0],
            (unsigned long long)gpu_profile.text_lines[1][1],
            (unsigned long long)gpu_profile.text_lines[1][2],
            (unsigned long long)gpu_profile.text_lines[1][3],
            (unsigned long long)gpu_profile.text_lines[1][4],
            (unsigned long long)gpu_profile.no_effect_lines[1]);
        uint64_t classified_frames = 0;
        for (uint32_t index = 0;
             index < NDS_GPU2D_DIRECT_CLASS_COUNT; ++index)
            classified_frames += gpu_profile.direct_class_frames[index];
        if (classified_frames) {
            std::fprintf(out,
                "  GPU2D direct classes: %llu transitions",
                (unsigned long long)gpu_profile.direct_class_transitions);
            for (uint32_t index = 0;
                 index < NDS_GPU2D_DIRECT_CLASS_COUNT; ++index) {
                if (!gpu_profile.direct_class_frames[index]) continue;
                std::fprintf(out, "; %s=%llu/%.3f s A",
                    nds_gpu2d_direct_class_name(index),
                    (unsigned long long)
                        gpu_profile.direct_class_frames[index],
                    static_cast<double>(
                        gpu_profile.direct_class_engine_a_ns[index]) /
                        1.0e9);
            }
            std::fputc('\n', out);
        }
    }
    NdsGpu3dProfile gpu3d_profile{};
    nds_gpu3d_profile(&gpu3d_profile);
    if (gpu3d_profile.vcount215_calls || gpu3d_profile.getline_calls ||
        gpu3d_profile.vcount144_calls || gpu3d_profile.compute_sync_calls) {
        std::fprintf(out,
            "  GPU3D profile: submit/render %.3f s (%llu calls), "
            "GetLine %.3f s (%llu calls), sync144 %.3f s (%llu calls), "
            "compute readback %.3f s (%llu calls; submit %.3f, map %.3f)\n",
            static_cast<double>(gpu3d_profile.vcount215_ns) / 1.0e9,
            (unsigned long long)gpu3d_profile.vcount215_calls,
            static_cast<double>(gpu3d_profile.getline_ns) / 1.0e9,
            (unsigned long long)gpu3d_profile.getline_calls,
            static_cast<double>(gpu3d_profile.vcount144_ns) / 1.0e9,
            (unsigned long long)gpu3d_profile.vcount144_calls,
            static_cast<double>(gpu3d_profile.compute_sync_ns) / 1.0e9,
            (unsigned long long)gpu3d_profile.compute_sync_calls,
            static_cast<double>(gpu3d_profile.compute_submit_ns) / 1.0e9,
            static_cast<double>(gpu3d_profile.compute_map_ns) / 1.0e9);
    }
    NdsSchedulerProfile sched{};
    scheduler_profile(&sched);
    if (sched.sampled_rounds != 0) {
        const double scale = 1.0e-6 / sched.sampled_rounds;
        std::fprintf(out,
            "  Scheduler profile: %.3f ms/1000 rounds "
            "(next %.3f, ARM9 %.3f, ARM7 %.3f, devices %.3f; %llu samples)\n",
            sched.sampled_round_ns * scale * 1000.0,
            sched.next_event_ns * scale * 1000.0,
            sched.arm9_ns * scale * 1000.0,
            sched.arm7_ns * scale * 1000.0,
            sched.devices_ns * scale * 1000.0,
            static_cast<unsigned long long>(sched.sampled_rounds));
        std::fprintf(out,
            "  Scheduler sub: switch %.3f ms/1000 rounds; devices split "
            "display %.3f spu %.3f wifi %.3f rtc %.3f sysev %.3f\n",
            sched.switch_ns * scale * 1000.0,
            sched.display_ns * scale * 1000.0,
            sched.spu_ns * scale * 1000.0,
            sched.wifi_ns * scale * 1000.0,
            sched.rtc_ns * scale * 1000.0,
            sched.sysev_ns * scale * 1000.0);
        std::fprintf(out,
            "  Scheduler counters: switches=%llu crs_words=%llu "
            "(%.2f switches/round, %.1f words/switch)\n",
            static_cast<unsigned long long>(sched.switches),
            static_cast<unsigned long long>(sched.crs_words),
            static_cast<double>(sched.switches) /
                static_cast<double>(sched.rounds ? sched.rounds : 1),
            static_cast<double>(sched.crs_words) /
                static_cast<double>(sched.switches ? sched.switches : 1));
    }
}
