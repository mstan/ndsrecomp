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
