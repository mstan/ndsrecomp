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
NdsIoPeripheralSaveState default_peripheral_state() {
    NdsIoPeripheralSaveState state{};
    state.rtc_datetime[1] = 1u;
    state.rtc_datetime[2] = 1u;
    return state;
}
NdsIoPeripheralSaveState g_test_peripheral_state = default_peripheral_state();
NdsVramSaveState default_vram_state() {
    NdsVramSaveState state{};
    state.vram.resize(0xA4000u);
    state.written.resize(0xA4000u);
    state.exec_generation.resize((0xA4000u + 0xFFFu) / 0x1000u);
    state.palette.resize(0x800u);
    state.oam.resize(0x800u);
    state.texture_generation = 1u;
    return state;
}
NdsGpu2dSaveState default_gpu2d_state() {
    NdsGpu2dSaveState state{};
    state.framebuffers.resize(4u * 256u * 192u);
    state.unit[0].eva = state.unit[1].eva = 16u;
    return state;
}
NdsVramSaveState g_test_vram_state = default_vram_state();
NdsGpu2dSaveState g_test_gpu2d_state = default_gpu2d_state();
NdsGpu3dSaveState g_test_gpu3d_state{{0x47u, 0x50u, 0x33u, 0x44u}, 0u};
bool g_fail_next_io_import = false;
bool g_fail_next_peripheral_import_after_apply = false;
}

void savestate_test_reset_io_state() {
    g_test_io_state = NdsIoCoreSaveState{};
    g_test_peripheral_state = default_peripheral_state();
    g_test_vram_state = default_vram_state();
    g_test_gpu2d_state = default_gpu2d_state();
    g_test_gpu3d_state = {{0x47u, 0x50u, 0x33u, 0x44u}, 0u};
    g_fail_next_io_import = false;
    g_fail_next_peripheral_import_after_apply = false;
}

void savestate_test_fail_next_io_import() { g_fail_next_io_import = true; }
void savestate_test_fail_next_peripheral_import_after_apply() {
    g_fail_next_peripheral_import_after_apply = true;
}

void savestate_test_set_video_pattern(uint8_t seed) {
    g_test_vram_state = default_vram_state();
    for (size_t i = 0; i < g_test_vram_state.vram.size(); ++i) {
        g_test_vram_state.vram[i] = static_cast<uint8_t>(seed + i * 3u);
        g_test_vram_state.written[i] = static_cast<uint8_t>((i + seed) & 1u);
    }
    for (size_t i = 0; i < g_test_vram_state.exec_generation.size(); ++i)
        g_test_vram_state.exec_generation[i] = seed +
            static_cast<uint32_t>(i);
    for (size_t i = 0; i < g_test_vram_state.palette.size(); ++i) {
        g_test_vram_state.palette[i] = static_cast<uint8_t>(seed ^ i);
        g_test_vram_state.oam[i] = static_cast<uint8_t>(seed + i * 5u);
    }
    g_test_vram_state.vramcnt[0] = 0x80u;
    g_test_vram_state.vramcnt[2] = 0x82u;
    g_test_vram_state.texture_generation = 100u + seed;

    g_test_gpu2d_state = default_gpu2d_state();
    g_test_gpu2d_state.unit[0].dispcnt = 0x00010000u | seed;
    g_test_gpu2d_state.unit[0].refx_internal[0] = -1000 - seed;
    g_test_gpu2d_state.unit[0].capture = 0x80000000u | seed;
    g_test_gpu2d_state.unit[0].capture_latch = 1u;
    g_test_gpu2d_state.front = seed & 1u;
    g_test_gpu2d_state.frame_capture_active = 1u;
    for (size_t i = 0; i < g_test_gpu2d_state.framebuffers.size(); ++i)
        g_test_gpu2d_state.framebuffers[i] =
            (uint32_t{seed} << 24u) ^ static_cast<uint32_t>(i * 17u);

    g_test_gpu3d_state.device = {0x47u, 0x50u, 0x33u, 0x44u, seed,
                                 static_cast<uint8_t>(seed ^ 0xA5u)};
    g_test_gpu3d_state.arm9_timestamp = 0x1122334455660000ull + seed;
}

bool savestate_test_video_matches(uint8_t seed) {
    NdsVramSaveState expected_vram = default_vram_state();
    NdsGpu2dSaveState expected_gpu2d = default_gpu2d_state();
    NdsGpu3dSaveState expected_gpu3d{};
    const NdsVramSaveState saved_vram = g_test_vram_state;
    const NdsGpu2dSaveState saved_gpu2d = g_test_gpu2d_state;
    const NdsGpu3dSaveState saved_gpu3d = g_test_gpu3d_state;
    savestate_test_set_video_pattern(seed);
    expected_vram = g_test_vram_state;
    expected_gpu2d = g_test_gpu2d_state;
    expected_gpu3d = g_test_gpu3d_state;
    g_test_vram_state = saved_vram;
    g_test_gpu2d_state = saved_gpu2d;
    g_test_gpu3d_state = saved_gpu3d;
    return g_test_vram_state.vram == expected_vram.vram &&
        g_test_vram_state.written == expected_vram.written &&
        g_test_vram_state.exec_generation == expected_vram.exec_generation &&
        g_test_vram_state.palette == expected_vram.palette &&
        g_test_vram_state.oam == expected_vram.oam &&
        std::memcmp(g_test_vram_state.vramcnt, expected_vram.vramcnt, 9u) == 0 &&
        g_test_vram_state.texture_generation == expected_vram.texture_generation &&
        g_test_gpu2d_state.framebuffers == expected_gpu2d.framebuffers &&
        g_test_gpu2d_state.unit[0].dispcnt == expected_gpu2d.unit[0].dispcnt &&
        g_test_gpu2d_state.unit[0].refx_internal[0] ==
            expected_gpu2d.unit[0].refx_internal[0] &&
        g_test_gpu2d_state.unit[0].capture == expected_gpu2d.unit[0].capture &&
        g_test_gpu2d_state.unit[0].capture_latch == 1u &&
        g_test_gpu2d_state.front == expected_gpu2d.front &&
        g_test_gpu2d_state.frame_capture_active == 1u &&
        g_test_gpu3d_state.device == expected_gpu3d.device &&
        g_test_gpu3d_state.arm9_timestamp == expected_gpu3d.arm9_timestamp;
}

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

bool io_peripheral_savestate_export(NdsIoPeripheralSaveState* out) {
    if (!out) return false;
    *out = g_test_peripheral_state;
    return true;
}

bool io_peripheral_savestate_validate(const NdsIoPeripheralSaveState& in,
                                      std::string* error) {
    auto binary = [](uint8_t value) { return value <= 1u; };
    if (!binary(in.card_end_event) || in.card_irq_cpu > 1u ||
        in.card_command_mode > 2u || in.card_data_mode > 2u ||
        !binary(in.key1_available) || !binary(in.card_has_ir) ||
        !binary(in.auxspi_hold) || !binary(in.backup_dirty) ||
        !binary(in.backup_persistence_detached) ||
        !binary(in.firmware_dirty) ||
        !binary(in.firmware_persistence_detached) ||
        !binary(in.firmware_hold) || in.card_transfer_len > 0x4000u ||
        in.card_response.size() != in.card_transfer_len ||
        in.card_transfer_pos > in.card_transfer_len ||
        (in.card_transfer_pos & 3u) != 0u ||
        in.backup_type > 3u || in.backup_data.size() != in.backup_size ||
        in.rtc_inbit > 7u || in.rtc_outbit > 7u || in.rtc_outpos > 7u) {
        if (error) *error = "savestate IO peripheral state is invalid";
        return false;
    }
    const bool aux_busy = (in.auxspicnt & 0x0080u) != 0u;
    const bool spi_busy = (in.spicnt & 0x0080u) != 0u;
    if (aux_busy != (in.auxspi_deadline != UINT64_MAX) ||
        spi_busy != (in.spi_deadline != UINT64_MAX)) {
        if (error) *error = "savestate IO peripheral deadline is invalid";
        return false;
    }
    return true;
}

bool io_peripheral_savestate_import(const NdsIoPeripheralSaveState& in,
                                    std::string* error) {
    if (!io_peripheral_savestate_validate(in, error)) return false;
    g_test_peripheral_state = in;
    if (g_fail_next_peripheral_import_after_apply) {
        g_fail_next_peripheral_import_after_apply = false;
        if (error) *error = "injected peripheral apply failure";
        return false;
    }
    return true;
}

bool vram_savestate_export(NdsVramSaveState* out) {
    if (!out) return false;
    *out = g_test_vram_state;
    return true;
}

bool vram_savestate_validate(const NdsVramSaveState& in,
                             std::string* error) {
    if (in.vram.size() != 0xA4000u || in.written.size() != 0xA4000u ||
        in.exec_generation.size() != (0xA4000u + 0xFFFu) / 0x1000u ||
        in.palette.size() != 0x800u || in.oam.size() != 0x800u ||
        in.texture_generation == 0u) {
        if (error) *error = "savestate VRAM state is invalid";
        return false;
    }
    return true;
}

bool vram_savestate_import(const NdsVramSaveState& in,
                           std::string* error) {
    if (!vram_savestate_validate(in, error)) return false;
    g_test_vram_state = in;
    return true;
}

bool gpu2d_savestate_export(NdsGpu2dSaveState* out) {
    if (!out) return false;
    *out = g_test_gpu2d_state;
    return true;
}

bool gpu2d_savestate_validate(const NdsGpu2dSaveState& in,
                              std::string* error) {
    if (in.framebuffers.size() != 4u * 256u * 192u || in.front > 1u ||
        in.unit[0].capture_latch > 1u || in.unit[1].capture_latch > 1u) {
        if (error) *error = "savestate GPU2D state is invalid";
        return false;
    }
    return true;
}

bool gpu2d_savestate_import(const NdsGpu2dSaveState& in,
                            std::string* error) {
    if (!gpu2d_savestate_validate(in, error)) return false;
    g_test_gpu2d_state = in;
    return true;
}

bool gpu3d_savestate_export(NdsGpu3dSaveState* out, std::string*) {
    if (!out) return false;
    *out = g_test_gpu3d_state;
    return true;
}

bool gpu3d_savestate_validate(const NdsGpu3dSaveState& in,
                              std::string* error) {
    if (in.device.empty()) {
        if (error) *error = "savestate GPU3D state is invalid";
        return false;
    }
    return true;
}

bool gpu3d_savestate_import(const NdsGpu3dSaveState& in,
                            std::string* error) {
    if (!gpu3d_savestate_validate(in, error)) return false;
    g_test_gpu3d_state = in;
    return true;
}

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
