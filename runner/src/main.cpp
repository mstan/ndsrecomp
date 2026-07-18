// main.cpp — DS runner driver.
//
// Loads + SHA-1-verifies the three dumps (CLAUDE.md: refuse to start
// otherwise), maps both BIOSes, resets both cores, and interleaves them on
// the scheduler until ARM9 reaches the cycle budget or both terminally
// halt. Reports where each core got — the execution-driven signal for the
// next pieces (SPI/firmware boot for ARM7, IPC handshake between them).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "state.h"
#include "scheduler.h"
#include "runtime_arm.h"
#include "io.h"
#include "debug_server.h"
#include "frontend.h"
#include "gpu2d.h"
#include "gpu3d.h"
#include "profile_report.h"
#include "sha1.h"
#if defined(NDS_HAVE_COMPUTE_RENDERER)
#include "melonds_compute/ComputeHost.h"
#endif

// Generated per-CPU dispatch tables (C linkage).
extern "C" const DispatchEntry g_dispatch_arm9_bios[];
extern "C" const unsigned g_dispatch_arm9_bios_len;
extern "C" const DispatchEntry g_dispatch_arm7_bios[];
extern "C" const unsigned g_dispatch_arm7_bios_len;
#ifdef NDS_HAVE_SM64DS_BANKS
extern "C" const DispatchEntry g_dispatch_sm64ds_arm9[];
extern "C" const unsigned g_dispatch_sm64ds_arm9_len;
extern "C" const DispatchEntry g_dispatch_sm64ds_arm7[];
extern "C" const unsigned g_dispatch_sm64ds_arm7_len;
extern "C" const DispatchEntry g_dispatch_sm64ds_arm7_ram[];
extern "C" const unsigned g_dispatch_sm64ds_arm7_ram_len;
extern "C" const DispatchEntry g_dispatch_sm64ds_arm9_ram[];
extern "C" const unsigned g_dispatch_sm64ds_arm9_ram_len;
#ifdef NDS_HAVE_SM64DS_ARM9_GAMEPLAY_RAM_BANKS
extern "C" const DispatchEntry g_dispatch_sm64ds_arm9_ram_gameplay[];
extern "C" const unsigned g_dispatch_sm64ds_arm9_ram_gameplay_len;
#endif
#endif
#ifndef NDS_BOOTSTRAP_FIRMWARE
extern "C" const DispatchEntry g_dispatch_fw_arm9_early[];
extern "C" const unsigned g_dispatch_fw_arm9_early_len;
extern "C" const DispatchEntry g_dispatch_fw_arm9_menu[];
extern "C" const unsigned g_dispatch_fw_arm9_menu_len;
extern "C" const DispatchEntry g_dispatch_fw_arm7_early[];
extern "C" const unsigned g_dispatch_fw_arm7_early_len;
extern "C" const DispatchEntry g_dispatch_fw_arm7_intermediate[];
extern "C" const unsigned g_dispatch_fw_arm7_intermediate_len;
extern "C" const DispatchEntry g_dispatch_fw_arm7_shared_ready[];
extern "C" const unsigned g_dispatch_fw_arm7_shared_ready_len;
extern "C" const DispatchEntry g_dispatch_fw_arm7_irq_ready[];
extern "C" const unsigned g_dispatch_fw_arm7_irq_ready_len;
extern "C" const DispatchEntry g_dispatch_fw_arm7_menu[];
extern "C" const unsigned g_dispatch_fw_arm7_menu_len;
#ifdef NDS_HAVE_FW_EXTENDED_BANKS
extern "C" const DispatchEntry g_dispatch_fw_arm9_calibration_save[];
extern "C" const unsigned g_dispatch_fw_arm9_calibration_save_len;
extern "C" const DispatchEntry g_dispatch_fw_arm9_profile_save[];
extern "C" const unsigned g_dispatch_fw_arm9_profile_save_len;
extern "C" const DispatchEntry g_dispatch_fw_arm9_system_options_save[];
extern "C" const unsigned g_dispatch_fw_arm9_system_options_save_len;
extern "C" const DispatchEntry g_dispatch_fw_arm9_date_alarm_save[];
extern "C" const unsigned g_dispatch_fw_arm9_date_alarm_save_len;
extern "C" const DispatchEntry g_dispatch_fw_arm9_main_menu_controls[];
extern "C" const unsigned g_dispatch_fw_arm9_main_menu_controls_len;
extern "C" const DispatchEntry g_dispatch_fw_arm9_download_play_shutdown[];
extern "C" const unsigned g_dispatch_fw_arm9_download_play_shutdown_len;
extern "C" const DispatchEntry g_dispatch_fw_arm9_pictochat_room_a[];
extern "C" const unsigned g_dispatch_fw_arm9_pictochat_room_a_len;
extern "C" const DispatchEntry g_dispatch_fw_arm9_shutdown[];
extern "C" const unsigned g_dispatch_fw_arm9_shutdown_len;
extern "C" const DispatchEntry g_dispatch_fw_arm7_calibration_save[];
extern "C" const unsigned g_dispatch_fw_arm7_calibration_save_len;
extern "C" const DispatchEntry g_dispatch_fw_arm7_profile_save[];
extern "C" const unsigned g_dispatch_fw_arm7_profile_save_len;
extern "C" const DispatchEntry g_dispatch_fw_arm7_system_options_save[];
extern "C" const unsigned g_dispatch_fw_arm7_system_options_save_len;
extern "C" const DispatchEntry g_dispatch_fw_arm7_shutdown[];
extern "C" const unsigned g_dispatch_fw_arm7_shutdown_len;
extern "C" const DispatchEntry g_dispatch_fw_arm7_date_alarm_save[];
extern "C" const unsigned g_dispatch_fw_arm7_date_alarm_save_len;
extern "C" const DispatchEntry g_dispatch_fw_arm7_main_menu_controls[];
extern "C" const unsigned g_dispatch_fw_arm7_main_menu_controls_len;
extern "C" const DispatchEntry g_dispatch_fw_arm7_download_play_shutdown[];
extern "C" const unsigned g_dispatch_fw_arm7_download_play_shutdown_len;
extern "C" const DispatchEntry g_dispatch_fw_arm7_pictochat_room_a[];
extern "C" const unsigned g_dispatch_fw_arm7_pictochat_room_a_len;
#endif
#endif

namespace {

std::vector<uint8_t> read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

bool verify(const std::vector<uint8_t>& data, const char* want,
            const char* label) {
    if (data.empty()) { std::fprintf(stderr, "[load] %s: missing/empty\n", label); return false; }
    std::string got = gba::sha1(data.data(), data.size()).hex();
    if (got != want) {
        std::fprintf(stderr, "[load] %s: SHA-1 mismatch\n  got  %s\n  want %s\n",
                     label, got.c_str(), want);
        return false;
    }
    std::fprintf(stderr, "[load] %s: %zu bytes, SHA-1 ok\n", label, data.size());
    return true;
}

uint16_t firmware_crc16(const uint8_t* data, size_t len, uint32_t start) {
    static constexpr uint16_t kPoly[8] = {
        0xC0C1u, 0xC181u, 0xC301u, 0xC601u,
        0xCC01u, 0xD801u, 0xF001u, 0xA001u,
    };
    for (size_t i = 0; i < len; ++i) {
        start ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            if (start & 1u)
                start = (start >> 1u) ^ (uint32_t{kPoly[bit]} << (7u - bit));
            else
                start >>= 1u;
        }
    }
    return static_cast<uint16_t>(start);
}

// melonDS exposes host screen pixels through a deterministic, ideal DS touch
// calibration. It applies this to both redundant user-settings blocks before
// the firmware CPU reads them. Mirror that in our private in-memory image so
// an input coordinate means the same thing on both sides; never alter the dump
// on disk (the SHA-1 above always verifies the original bytes).
bool normalize_touch_calibration(std::vector<uint8_t>& fw) {
    if (fw.size() < 0x22u) return false;
    const size_t user = size_t{fw[0x20]} << 3u |
                        size_t{fw[0x21]} << 11u;
    if (user + 0x200u > fw.size()) return false;

    auto put16 = [&](size_t off, uint16_t value) {
        fw[off] = static_cast<uint8_t>(value);
        fw[off + 1u] = static_cast<uint8_t>(value >> 8u);
    };
    for (unsigned copy = 0; copy < 2; ++copy) {
        const size_t base = user + size_t{copy} * 0x100u;
        put16(base + 0x58u, 0u);          // ADC1 X
        put16(base + 0x5Au, 0u);          // ADC1 Y
        fw[base + 0x5Cu] = 0u;            // pixel1 X
        fw[base + 0x5Du] = 0u;            // pixel1 Y
        put16(base + 0x5Eu, 255u << 4u);  // ADC2 X
        put16(base + 0x60u, 191u << 4u);  // ADC2 Y
        fw[base + 0x62u] = 255u;          // pixel2 X
        fw[base + 0x63u] = 191u;          // pixel2 Y
        put16(base + 0x72u,
              firmware_crc16(fw.data() + base, 0x70u, 0xFFFFu));
    }
    return true;
}

void dump_cpu(const char* name, const ArmCpuState& c, uint64_t cycles) {
    std::fprintf(stderr, "  %s: PC=%08X CPSR=%08X SP=%08X LR=%08X "
                 "R0=%08X R12=%08X  cycles=%llu\n",
                 name, c.R[15], c.cpsr, c.R[13], c.R[14], c.R[0], c.R[12],
                 (unsigned long long)cycles);
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir = "bios";
    std::string rom_path;
    uint64_t budget = 4000000ull;
    bool serve = false;
    bool interactive = false;
    bool discover_static_misses = false;
    uint16_t port = 19842;
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--serve") {
            serve = true;
        } else if (a == "--interactive") {
            interactive = true;
        } else if (a == "--discover-static-misses") {
            discover_static_misses = true;
        } else if (a == "--rtc-host") {
            // Start the guest RTC at host local time on every boot. Opt-in:
            // the oracle gates compare RTC state, so parity runs keep the
            // deterministic power-on clock.
            g_nds_rtc_host = true;
        } else if (a == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (a == "--rom" && i + 1 < argc) {
            rom_path = argv[++i];
        } else if (a == "--help" || a == "-h") {
            std::fprintf(stderr,
                "usage: %s [bios-dir] [cycle-budget] [--rom game.nds] "
                "[--serve|--interactive] [--port 19842] "
                "[--discover-static-misses] [--rtc-host]\n",
                argv[0]);
            return 0;
        } else if (positional == 0) {
            dir = a;
            ++positional;
        } else if (positional == 1) {
            budget = std::strtoull(a.c_str(), nullptr, 0);
            ++positional;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return 2;
        }
    }

    g_discover_static_misses = discover_static_misses;

    bool compute_requested = false;
    if (const char* renderer = std::getenv("NDS_3D_RENDERER")) {
        if (std::strcmp(renderer, "soft") != 0 &&
            std::strcmp(renderer, "compute") != 0) {
            std::fprintf(stderr,
                         "invalid NDS_3D_RENDERER (expected soft or compute)\n");
            return 2;
        }
        compute_requested = std::strcmp(renderer, "compute") == 0;
        if (compute_requested && !nds_gpu3d_compute_renderer_built()) {
            std::fprintf(stderr,
                "NDS_3D_RENDERER=compute requested but this runner was "
                "built without NDS_ENABLE_COMPUTE_RENDERER\n");
            return 2;
        }
    }

    // Interactive play benefits from overlapping the upstream soft
    // rasterizer with guest execution. Keep serve/non-frontend runs on the
    // established single-threaded path by default so parity gates are
    // unchanged; NDS_3D_THREADED=0/1 provides same-binary A/B and a forced-on
    // parity proof.
    bool gpu3d_threaded = interactive;
    if (const char* value = std::getenv("NDS_3D_THREADED")) {
        if (value[0] == '0' && value[1] == '\0') {
            gpu3d_threaded = false;
        } else if (value[0] == '1' && value[1] == '\0') {
            gpu3d_threaded = true;
        } else {
            std::fprintf(stderr,
                         "invalid NDS_3D_THREADED value (expected 0 or 1)\n");
            return 2;
        }
    }
    std::fprintf(stderr, "[gpu3d] threaded soft renderer: %s\n",
                 gpu3d_threaded ? "on" : "off");

    auto a9 = read_file(dir + "/biosnds9.rom");
    auto a7 = read_file(dir + "/biosnds7.rom");
    auto fw = read_file(dir + "/firmware.bin");
    auto rom = rom_path.empty() ? std::vector<uint8_t>{} : read_file(rom_path);
    bool ok = verify(a9, "bfaac75f101c135e32e2aaf541de6b1be4c8c62d", "arm9 bios")
            & verify(a7, "24f67bdea115a2c847c8813a262502ee1607b7df", "arm7 bios")
            & verify(fw, "ae22de59fbf3f35ccfbeacaeba6fa87ac5e7b14b", "firmware");
    if (!ok) { std::fprintf(stderr, "refusing to start: dump verification failed\n"); return 1; }
    if (!rom_path.empty()) {
        if (rom.size() < 0x200u) {
            std::fprintf(stderr, "refusing to start: cartridge image is missing or truncated\n");
            return 1;
        }
        const std::string rom_sha1 = gba::sha1(rom.data(), rom.size()).hex();
        std::fprintf(stderr, "[load] cartridge: %zu bytes, SHA-1 %s\n",
                     rom.size(), rom_sha1.c_str());
    }
    if (!normalize_touch_calibration(fw)) {
        std::fprintf(stderr, "refusing to start: malformed firmware user-settings layout\n");
        return 1;
    }

    // Full power-on init, reusable so the debug server can honour `reset`
    // (the bisector compares fresh-from-reset at each event count).
    auto boot = [&]() {
        // A debug reset may arrive after a threaded frame was started. Join
        // the worker before GPU3D::Reset clears its render buffers, then
        // restore the selected host policy once initialization is complete.
        nds_gpu3d_set_threaded(false);
        bus_init();
        bus_load_arm9_bios(a9.data(), (uint32_t)a9.size());
        bus_load_arm7_bios(a7.data(), (uint32_t)a7.size());
        cp15_reset();
        nds_io_reset();
        nds_io_load_firmware(fw.data(), (uint32_t)fw.size());
        if (!rom.empty() && !nds_io_load_cartridge(
                rom.data(), static_cast<uint32_t>(rom.size()),
                a7.data(), static_cast<uint32_t>(a7.size()))) {
            std::fprintf(stderr, "refusing to boot: cartridge initialization failed\n");
            std::exit(1);
        }
        runtime_init(nullptr);
        runtime_trace_reset();

        nds_register_dispatch(NDS_ARM9, g_dispatch_arm9_bios,
                              g_dispatch_arm9_bios_len, 0xFFFF0000u);
        nds_register_dispatch(NDS_ARM7, g_dispatch_arm7_bios,
                              g_dispatch_arm7_bios_len, 0x00000000u);
#ifndef NDS_BOOTSTRAP_FIRMWARE
        nds_register_dispatch(NDS_ARM9, g_dispatch_fw_arm9_early,
                              g_dispatch_fw_arm9_early_len, 0xFFFF0000u);
        nds_register_dispatch(NDS_ARM9, g_dispatch_fw_arm9_menu,
                              g_dispatch_fw_arm9_menu_len, 0xFFFF0000u);
#ifdef NDS_HAVE_FW_EXTENDED_BANKS
        nds_register_dispatch(NDS_ARM9, g_dispatch_fw_arm9_calibration_save,
                              g_dispatch_fw_arm9_calibration_save_len,
                              0xFFFF0000u);
        nds_register_dispatch(NDS_ARM9, g_dispatch_fw_arm9_profile_save,
                              g_dispatch_fw_arm9_profile_save_len,
                              0xFFFF0000u);
        nds_register_dispatch(NDS_ARM9, g_dispatch_fw_arm9_system_options_save,
                              g_dispatch_fw_arm9_system_options_save_len,
                              0xFFFF0000u);
        nds_register_dispatch(NDS_ARM9, g_dispatch_fw_arm9_date_alarm_save,
                              g_dispatch_fw_arm9_date_alarm_save_len,
                              0xFFFF0000u);
        nds_register_dispatch(NDS_ARM9, g_dispatch_fw_arm9_main_menu_controls,
                              g_dispatch_fw_arm9_main_menu_controls_len,
                              0xFFFF0000u);
        nds_register_dispatch(NDS_ARM9,
                              g_dispatch_fw_arm9_download_play_shutdown,
                              g_dispatch_fw_arm9_download_play_shutdown_len,
                              0xFFFF0000u);
        nds_register_dispatch(NDS_ARM9, g_dispatch_fw_arm9_pictochat_room_a,
                              g_dispatch_fw_arm9_pictochat_room_a_len,
                              0xFFFF0000u);
        nds_register_dispatch(NDS_ARM9, g_dispatch_fw_arm9_shutdown,
                              g_dispatch_fw_arm9_shutdown_len,
                              0xFFFF0000u);
#endif
        nds_register_dispatch(NDS_ARM7, g_dispatch_fw_arm7_early,
                              g_dispatch_fw_arm7_early_len, 0x00000000u);
        nds_register_dispatch(NDS_ARM7, g_dispatch_fw_arm7_intermediate,
                              g_dispatch_fw_arm7_intermediate_len, 0x00000000u);
        nds_register_dispatch(NDS_ARM7, g_dispatch_fw_arm7_shared_ready,
                              g_dispatch_fw_arm7_shared_ready_len, 0x00000000u);
        nds_register_dispatch(NDS_ARM7, g_dispatch_fw_arm7_irq_ready,
                              g_dispatch_fw_arm7_irq_ready_len, 0x00000000u);
        nds_register_dispatch(NDS_ARM7, g_dispatch_fw_arm7_menu,
                              g_dispatch_fw_arm7_menu_len, 0x00000000u);
#ifdef NDS_HAVE_FW_EXTENDED_BANKS
        nds_register_dispatch(NDS_ARM7, g_dispatch_fw_arm7_calibration_save,
                              g_dispatch_fw_arm7_calibration_save_len,
                              0x00000000u);
        nds_register_dispatch(NDS_ARM7, g_dispatch_fw_arm7_profile_save,
                              g_dispatch_fw_arm7_profile_save_len,
                              0x00000000u);
        nds_register_dispatch(NDS_ARM7, g_dispatch_fw_arm7_system_options_save,
                              g_dispatch_fw_arm7_system_options_save_len,
                              0x00000000u);
        nds_register_dispatch(NDS_ARM7, g_dispatch_fw_arm7_shutdown,
                              g_dispatch_fw_arm7_shutdown_len,
                              0x00000000u);
        nds_register_dispatch(NDS_ARM7, g_dispatch_fw_arm7_date_alarm_save,
                              g_dispatch_fw_arm7_date_alarm_save_len,
                              0x00000000u);
        nds_register_dispatch(NDS_ARM7, g_dispatch_fw_arm7_main_menu_controls,
                              g_dispatch_fw_arm7_main_menu_controls_len,
                              0x00000000u);
        nds_register_dispatch(NDS_ARM7,
                              g_dispatch_fw_arm7_download_play_shutdown,
                              g_dispatch_fw_arm7_download_play_shutdown_len,
                              0x00000000u);
        nds_register_dispatch(NDS_ARM7, g_dispatch_fw_arm7_pictochat_room_a,
                              g_dispatch_fw_arm7_pictochat_room_a_len,
                              0x00000000u);
#endif
#endif
#ifdef NDS_HAVE_SM64DS_BANKS
        nds_register_dispatch(NDS_ARM9, g_dispatch_sm64ds_arm9,
                              g_dispatch_sm64ds_arm9_len, 0xFFFF0000u);
        nds_register_dispatch(NDS_ARM7, g_dispatch_sm64ds_arm7,
                              g_dispatch_sm64ds_arm7_len, 0x00000000u);
#ifdef NDS_HAVE_SM64DS_RAM_BANKS
        // Content-validated runtime-RAM bank (relocated sound engine +
        // services); registered after the ROM-derived closure so the
        // immutable payload rows win for their own address range.
        nds_register_dispatch(NDS_ARM7, g_dispatch_sm64ds_arm7_ram,
                              g_dispatch_sm64ds_arm7_ram_len, 0x00000000u);
#endif
#ifdef NDS_HAVE_SM64DS_ARM9_RAM_BANKS
        // Content-validated ARM9 runtime-RAM bank (ITCM-resident code +
        // overlays loaded over/past the static image); registered after
        // the ROM-derived closure so the closure's rows win where the
        // live bytes still match the static image.
        nds_register_dispatch(NDS_ARM9, g_dispatch_sm64ds_arm9_ram,
                              g_dispatch_sm64ds_arm9_ram_len, 0xFFFF0000u);
#endif
#ifdef NDS_HAVE_SM64DS_ARM9_GAMEPLAY_RAM_BANKS
        // A later gameplay capture carries different overlay generations at
        // many of the same virtual addresses. Keep it in a separate
        // content-validated bank: boot/title bytes above win when present,
        // then this generation becomes eligible after the guest swaps them.
        nds_register_dispatch(NDS_ARM9,
                              g_dispatch_sm64ds_arm9_ram_gameplay,
                              g_dispatch_sm64ds_arm9_ram_gameplay_len,
                              0xFFFF0000u);
#endif
#endif

        // Reset both cores: SVC mode, IRQ+FIQ masked, ARM state, reset vector.
        const uint32_t reset_cpsr = 0x13u | CPSR_I_BIT | CPSR_F_BIT;
        scheduler_init();
        scheduler_reset_cpu(0, 0xFFFF0000u, reset_cpsr);  // ARM9
        scheduler_reset_cpu(1, 0x00000000u, reset_cpsr);  // ARM7
        nds_gpu3d_set_threaded(gpu3d_threaded);
    };
    boot();

#if defined(NDS_HAVE_COMPUTE_RENDERER)
    // Interactive mode creates the hidden context only after its visible SDL
    // allocations succeed. Headless/serve modes own it here so forced
    // compute selection is observable by the parity and perf harnesses.
    if (!interactive && !nds_compute_host_start()) return 1;
#endif

    if (interactive) {
        // The per-access deep-trace payloads (bus ring, mem_r/mem_w events,
        // per-insn register images) default OFF in play mode for real-time
        // headroom — the B3 inline bus fast path engages while they are off.
        // The play-mode TCP surface below can re-arm them on demand
        // (`deep_trace` command); event counters always advance.
        runtime_set_deep_trace(0);
        // Play-mode debug surface (sibling-recomp model): an I/O thread owns
        // the socket, commands execute on the frontend thread between frames
        // via debug_pump(). Execution stays frontend-owned (run_to_* are
        // rejected); queries, rings, and touch/keys injection are live.
        debug_set_reset_fn(boot);
        debug_pump_start(port);
        std::fprintf(stderr, "[run] interactive SDL mode from reset\n");
        const int rc = nds_run_interactive_frontend();
        debug_pump_stop();
        return rc;
    }

    if (serve) {
        // Optional: NDS_DEEP_TRACE=0 drops the per-access payloads (bus
        // ring, mem_r/mem_w events, per-insn register images) in serve
        // mode too — the bus fast path then engages exactly as in the
        // interactive frontend. Used to prove fast-path execution
        // equivalence under the G3 byte-lock and for honest serve-mode
        // perf A/B (deep trace otherwise masks bank/bus wins).
        if (const char* dt = std::getenv("NDS_DEEP_TRACE"))
            if (dt[0] == '0' && dt[1] == '\0') runtime_set_deep_trace(0);
        std::fprintf(stderr, "[run] debug server mode from reset\n");
        debug_set_reset_fn(boot);
        debug_serve(port);
#if defined(NDS_HAVE_COMPUTE_RENDERER)
        const bool compute_failed = nds_gpu3d_compute_runtime_failed();
        nds_compute_host_stop();
#else
        const bool compute_failed = false;
#endif
        return compute_failed ? 1 : 0;
    }

    std::fprintf(stderr, "[run] dual-CPU from reset, ARM9 budget=%llu cycles\n",
                 (unsigned long long)budget);
    SchedResult r = scheduler_run(budget);

    std::fprintf(stderr, "\n== result (%llu rounds) ==\n",
                 (unsigned long long)r.rounds);
    dump_cpu("ARM9", scheduler_cpu_state(0), r.cycles[0]);
    std::fprintf(stderr, "        %s\n",
                 r.halted[0] ? r.reason[0] : "running (reached budget / idle)");
    dump_cpu("ARM7", scheduler_cpu_state(1), r.cycles[1]);
    std::fprintf(stderr, "        %s\n",
                 r.halted[1] ? r.reason[1] : "running (reached budget / idle)");
    std::fprintf(stderr, "  CP15: control=%08X DTCM(en=%d base=%08X sz=%u)\n",
                 g_cp15.control, g_cp15.dtcm_enable, g_cp15.dtcm_base,
                 g_cp15.dtcm_size);
    nds_dump_irq();
    nds_profile_report(stderr);
    std::fprintf(stderr, "\n== recent execution trace (last-scheduled CPU, tail) ==\n");
    runtime_trace_dump_recent(24);
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    const bool compute_failed = nds_gpu3d_compute_runtime_failed();
    nds_compute_host_stop();
#else
    const bool compute_failed = false;
#endif
    return compute_failed ? 1 : 0;
}
