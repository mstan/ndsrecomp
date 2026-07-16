// profile_report.cpp — see profile_report.h.

#include "profile_report.h"

#include "gpu2d.h"
#include "scheduler.h"

void nds_profile_report(std::FILE* out) {
    NdsGpu2dProfile gpu_profile{};
    nds_gpu2d_profile(&gpu_profile);
    if (gpu_profile.scanlines) {
        std::fprintf(out,
            "  GPU2D profile: %.3f seconds (A %.3f, B %.3f, OBJ %.3f) "
            "across %llu scanlines\n",
            static_cast<double>(gpu_profile.render_ns) / 1.0e9,
            static_cast<double>(gpu_profile.engine_ns[0]) / 1.0e9,
            static_cast<double>(gpu_profile.engine_ns[1]) / 1.0e9,
            static_cast<double>(gpu_profile.obj_ns) / 1.0e9,
            static_cast<unsigned long long>(gpu_profile.scanlines));
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
