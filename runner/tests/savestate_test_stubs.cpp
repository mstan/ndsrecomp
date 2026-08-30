#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "coverage_manifest.h"
#include "diagnostics.h"
#include "io.h"
#include "live_overlay.h"
#include "runtime_arm.h"
#include "savestate.h"
#include "vram.h"

namespace {

[[noreturn]] void unexpected_device_call(const char* name) {
    std::fprintf(stderr, "unexpected device call in savestate_test: %s\n", name);
    std::abort();
}

}  // namespace

uint64_t g_coverage_write_epoch = 0;
CoverageExecCache g_coverage_exec_cache[2]{};
extern "C" uint64_t g_insn_count[2]{};
bool g_nds_insn_stop = false;

void coverage_capture_exec_page(int, uint32_t, uint32_t) {}
void coverage_note_generation_entry(int, uint32_t, bool, uint8_t, uint32_t) {}

void live_overlay_runtime_reset() {}
void live_overlay_note_cached_hit(uint32_t) {}
void live_overlay_note_transfer(int, uint32_t, uint32_t, uint32_t, uint32_t,
                                uint32_t) {}
void live_overlay_note_lookup(int, uint32_t, uint32_t, uint32_t, uint32_t,
                              const NdsDispatchEntry*,
                              const NdsDispatchEntry*, uint32_t,
                              const char*) {}
uint32_t live_overlay_candidate_serial(int, const NdsDispatchEntry*) {
    return 0;
}
void live_overlay_note_write(int, uint32_t, uint32_t, uint32_t, uint32_t,
                             uint32_t) {}
void live_overlay_poll() {}
bool live_overlay_note_tier3_entry(int, uint32_t) { return false; }
bool live_overlay_dormant_covers(int, uint32_t) { return false; }

bool nds_diagnostics_enabled() { return false; }
std::string nds_diagnostics_dispatch_miss_log_path() { return {}; }

uint32_t nds_io_read(uint32_t, uint32_t) {
    unexpected_device_call("nds_io_read");
}
void nds_io_write(uint32_t, uint32_t, uint32_t) {
    unexpected_device_call("nds_io_write");
}
uint64_t nds_next_system_event_time() { return UINT64_MAX; }
uint64_t nds_next_timer_overflow_time() { return UINT64_MAX; }
void nds_run_system_events(uint64_t) {}
void nds_tick_timers(int, unsigned long long) {}
void nds_tick_display(unsigned long long) {}
void nds_tick_rtc(unsigned long long) {}
bool nds_event_break_hit() { return false; }
void nds_note_insn_retired(int) {}
void nds_insn_hook_recompute() {}
void nds_note_irq_accept(int, uint32_t) {}
uint16_t nds_exmemcnt(int) { return 0; }
uint16_t nds_powercontrol7() { return 0; }
uint16_t nds_powercontrol9() { return 0; }
uint16_t nds_wifiwaitcnt() { return 0; }
uint8_t nds_wramcnt() { return 0; }
bool nds_powered_off() { return false; }
uint32_t nds_irq_pending(int) { return 0; }
uint32_t g_nds_irq_pending_cache[2]{};
bool nds_cpu_halted(int) { return false; }
void nds_cpu_enter_halt(int) {}
void nds_cpu_wake(int) {}
bool nds_halt_wake_pending(int) { return false; }
unsigned long long nds_halt_entry_cycle(int) { return 0; }
bool nds_dma_cpu_stalled(int) { return false; }
unsigned long long nds_dma_entry_cycle(int) { return 0; }
void nds_dma_run(int, unsigned long long) {}
bool nds_gxfifo_stalled() { return false; }
namespace {
NdsIoCoreSaveState g_test_io_state{};
bool g_fail_next_io_import = false;
}

void savestate_test_fail_next_io_import() { g_fail_next_io_import = true; }

bool io_savestate_export(NdsIoCoreSaveState* out) {
    if (!out) return false;
    *out = g_test_io_state;
    return true;
}

bool io_savestate_validate(const NdsIoCoreSaveState& in,
                           std::string* error) {
    auto binary = [](uint8_t value) { return value <= 1u; };
    if (!binary(in.next_vcount_valid) || !binary(in.in_vblank) ||
        !binary(in.gxfifo_stall) || !binary(in.pm_hold) ||
        !binary(in.powered_off) || in.vcount > 262u ||
        in.tsc_datapos < 0 || in.tsc_datapos > 2 ||
        (in.powercontrol7 & ~0x0003u) != 0u || in.pm_index >= 8u) {
        if (error) *error = "savestate IO core scalar is invalid";
        return false;
    }
    for (int cpu = 0; cpu < 2; ++cpu) {
        if (!binary(in.vcount_match[cpu]) || !binary(in.cpu_halted[cpu]) ||
            in.fifo_count[cpu] > 16u || in.fifo_head[cpu] >= 16u ||
            (in.fifocnt[cpu] & ~0xC404u) != 0u)
            return false;
        for (int ch = 0; ch < 4; ++ch) {
            const auto& dma = in.dma[cpu][ch];
            const bool valid_mode = cpu == 0
                ? dma.start_mode <= 7u
                : dma.start_mode == 0u ||
                    (dma.start_mode >= 0x10u && dma.start_mode <= 0x13u);
            if (!binary(dma.running) || !binary(dma.in_progress) ||
                !binary(dma.burst_start) || dma.src_inc < -1 ||
                dma.src_inc > 1 || dma.dst_inc < -1 || dma.dst_inc > 1 ||
                !valid_mode || in.timer[cpu][ch].accum >= 1024u)
                return false;
        }
    }
    return true;
}

bool io_savestate_import(const NdsIoCoreSaveState& in, std::string* error) {
    if (!io_savestate_validate(in, error)) return false;
    if (g_fail_next_io_import) {
        g_fail_next_io_import = false;
        if (error) *error = "injected IO apply failure";
        return false;
    }
    g_test_io_state = in;
    return true;
}

void nds_tick_spu(uint64_t) {}
uint64_t nds_wifi_next_event_time() { return UINT64_MAX; }
void nds_wifi_run_events(uint64_t) {}
bool nds_wifi_address(int, uint32_t) { return false; }
uint32_t nds_wifi_read(uint32_t, uint32_t, bool) {
    unexpected_device_call("nds_wifi_read");
}
void nds_wifi_write(uint32_t, uint32_t, uint32_t, bool) {
    unexpected_device_call("nds_wifi_write");
}

bool nds_video_address(uint32_t) { return false; }
bool nds_video_get_region(const char*, const uint8_t**, uint32_t*) {
    return false;
}
uint32_t nds_video_read(int, uint32_t, uint32_t) {
    unexpected_device_call("nds_video_read");
}
void nds_video_write(int, uint32_t, uint32_t, uint32_t) {
    unexpected_device_call("nds_video_write");
}
bool nds_vram_exec_writable(int, uint32_t) { return false; }
bool nds_vram_range_has_write_provenance(int, uint32_t, uint32_t) {
    return false;
}
uint32_t nds_vram_exec_page_generation(int, uint32_t) { return 0; }
bool nds_vram_live_bytes_equal(int, uint32_t, const uint8_t*, uint32_t) {
    return false;
}

int32_t nds_gpu3d_cycles_to_run() { return 0; }
void nds_gpu3d_run(unsigned long long) {}

void tier3_reset() {}
void tier3_note_clean_ram_reject() {}
void tier3_run(uint32_t) {
    unexpected_device_call("tier3_run");
}
