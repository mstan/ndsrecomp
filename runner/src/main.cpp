// main.cpp — DS runner driver.
//
// Loads + SHA-1-verifies the three dumps (CLAUDE.md: refuse to start
// otherwise), maps both BIOSes, resets both cores, and interleaves them on
// the scheduler until ARM9 reaches the cycle budget or both terminally
// halt. Reports where each core got — the execution-driven signal for the
// next pieces (SPI/firmware boot for ARM7, IPC handshake between them).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "state.h"
#include "scheduler.h"
#include "direct_boot.h"
#include "generated_firmware.h"
#include "firmware_state.h"
#include "firmware_user_settings.h"
#include "freebios_images.h"

#include <array>
#include <random>
#include "runtime_arm.h"
#include "io.h"
#include "debug_server.h"
#include "diagnostics.h"
#include "frontend.h"
#include "gpu2d.h"
#include "gpu3d.h"
#include "live_overlay.h"
#include "melonds_compute/TextureUpscale.h"
#include "net/net_ring.h"
#include "net/net_capture.h"
#include "net/wfc_provider.h"
#include "wifi_net.h"
#include "profile_report.h"
#include "sha1.h"
#include "title_banks.h"
#include "coverage_manifest.h"
#include "title_patches.h"
#if defined(NDS_HAVE_COMPUTE_RENDERER)
#include "melonds_compute/ComputeHost.h"
#endif

// Generated per-CPU dispatch tables (C linkage).
extern "C" const DispatchEntry g_dispatch_arm9_bios[];
extern "C" const unsigned g_dispatch_arm9_bios_len;
extern "C" const DispatchEntry g_dispatch_arm7_bios[];
extern "C" const unsigned g_dispatch_arm7_bios_len;
extern "C" const DispatchEntry g_dispatch_freebios_arm9[];
extern "C" const unsigned g_dispatch_freebios_arm9_len;
extern "C" const DispatchEntry g_dispatch_freebios_arm7[];
extern "C" const unsigned g_dispatch_freebios_arm7_len;
#ifdef NDS_HAVE_SM64DS_BANKS
extern "C" const DispatchEntry g_dispatch_sm64ds_arm9[];
extern "C" const unsigned g_dispatch_sm64ds_arm9_len;
#if defined(NDS_PROFILE_HLE_HEAT)
extern "C" const NdsHleProfileDescriptor* const
    g_hle_profile_sm64ds_arm9[];
extern "C" const unsigned g_hle_profile_sm64ds_arm9_len;
#endif
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

// Per-install identity for generated firmware (beads-yjp.1.11): a MAC that
// is generated once, persisted next to the BIOS dumps (the "console"
// directory), and reused on every boot. melonDS ships one shared
// DEFAULT_MAC for every single-instance install, so all such consoles look
// identical to Wiimmfi; we copy its multi-instance perturbation MECHANISM
// (nds_apply_instance_mac, firmware_user_settings.cpp) but never the shared
// default.
bool parse_identity_mac(const std::string& text, std::array<uint8_t, 6>* out) {
    unsigned b[6];
    char extra = 0;
    if (std::sscanf(text.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x%c",
                    &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &extra) != 6)
        return false;
    if (b[0] & 0x01u) return false;  // multicast/broadcast is never a station
    for (int i = 0; i < 6; ++i) (*out)[i] = static_cast<uint8_t>(b[i]);
    return true;
}

bool load_or_create_identity_mac(const std::string& path,
                                 std::array<uint8_t, 6>* out) {
    {
        std::ifstream f(path, std::ios::binary);
        if (f) {
            std::array<uint8_t, 6> mac{};
            f.read(reinterpret_cast<char*>(mac.data()), 6);
            if (f.gcount() == 6 && !(mac[0] & 0x01u)) {
                *out = mac;
                std::fprintf(stderr,
                    "[identity] persisted MAC %02X:%02X:%02X:%02X:%02X:%02X "
                    "from %s\n", mac[0], mac[1], mac[2], mac[3], mac[4],
                    mac[5], path.c_str());
                return true;
            }
            std::fprintf(stderr,
                "refusing to start: %s exists but is not a valid identity\n",
                path.c_str());
            return false;
        }
    }
    std::array<uint8_t, 6> mac = {0x00, 0x09, 0xBF, 0, 0, 0};
    std::random_device rng;
    for (int i = 3; i < 6; ++i) mac[i] = static_cast<uint8_t>(rng());
    {
        // A pure no-dump setup may not have a bios folder at all yet; the
        // identity file is what brings it into existence.
        std::error_code error;
        std::filesystem::create_directories(
            std::filesystem::path(path).parent_path(), error);
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f || !f.write(reinterpret_cast<const char*>(mac.data()), 6)) {
        std::fprintf(stderr,
            "refusing to start: could not persist identity to %s\n",
            path.c_str());
        return false;
    }
    std::fprintf(stderr,
        "[identity] generated new MAC %02X:%02X:%02X:%02X:%02X:%02X, "
        "persisted to %s\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        path.c_str());
    *out = mac;
    return true;
}

// Concrete direct-boot machine (beads-yjp.15 increment 1). Guest memory
// writes go through the device bus path — never raw RAM pokes — so the
// copied ARM9/ARM7 binaries acquire write provenance (Tier-3 and static
// bank validation both require it). Note the cpu convention translation:
// nds_direct_boot speaks 0=ARM9/1=ARM7, bus_device_write* speaks 9/7.
struct RunnerDirectBootMachine final : NdsDirectBootMachine {
    void write16(int cpu, uint32_t addr, uint16_t value) override {
        bus_device_write16(cpu == 1 ? 7 : 9, addr, value);
    }
    void write32(int cpu, uint32_t addr, uint32_t value) override {
        bus_device_write32(cpu == 1 ? 7 : 9, addr, value);
    }
    void cp15_write(uint32_t crn, uint32_t crm, uint32_t op2,
                    uint32_t value) override {
        runtime_coproc_write(15, 0, crn, crm, op2, value);
    }
    void set_cpu_boot(int cpu, uint32_t entry, uint32_t sp, uint32_t sp_irq,
                      uint32_t sp_svc) override {
        scheduler_set_cpu_boot(cpu, entry, sp, sp_irq, sp_svc);
    }
    void set_wramcnt(uint8_t value) override { nds_io_set_wramcnt(value); }
    void set_arm7_bios_prot(uint32_t value) override {
        nds_io_set_arm7_bios_prot(value);
    }
    void set_post_boot_latches() override {
        nds_io_apply_direct_boot_latches();
    }
    void copy_logo_into_arm9_bios(const uint8_t* logo,
                                  uint32_t size) override {
        bus_patch_arm9_bios(0x20, logo, size);
    }
};

bool is_ipv4_loopback(uint32_t ipv4_host_order) {
    return (ipv4_host_order & 0xFF000000u) == 0x7F000000u;
}

void dump_cpu(const char* name, const ArmCpuState& c, uint64_t cycles) {
    std::fprintf(stderr, "  %s: PC=%08X CPSR=%08X SP=%08X LR=%08X "
                 "R0=%08X R12=%08X  cycles=%llu\n",
                 name, c.R[15], c.cpsr, c.R[13], c.R[14], c.R[0], c.R[12],
                 (unsigned long long)cycles);
}

// Wiimmfi M8: end-of-run summary for a --network-backend replay run. A
// no-op (prints nothing) when the resolved backend isn't Replay, matching
// the existing net_ring_dump block's "print only if this feature was
// active" convention. PASS/FAIL is unambiguous by design: `mismatch=true`
// means NetReplay::SendPacket found the first byte-for-byte divergence
// against the recorded expectation (see net_replay.h) -- never a silent
// "0 records replayed, looks fine."
void dump_replay_status() {
    NdsNetReplayStatus st{};
    if (!nds_wifi_replay_status(&st)) return;
    std::fprintf(stderr, "\n== network replay result (Wiimmfi M8) ==\n");
    std::fprintf(stderr,
        "  TX matched: %llu/%llu   RX delivered: %llu/%llu\n",
        (unsigned long long)st.tx_matched, (unsigned long long)st.tx_total,
        (unsigned long long)st.rx_delivered, (unsigned long long)st.rx_total);
    if (st.mismatch) {
        std::fprintf(stderr,
            "  status: FAIL -- first divergence at TX frame #%llu "
            "(guest_cycle=%llu arm9_pc=0x%08X arm7_pc=0x%08X): %s\n",
            (unsigned long long)st.mismatch_tx_frame_index,
            (unsigned long long)st.mismatch_guest_cycle, st.mismatch_arm9_pc,
            st.mismatch_arm7_pc, st.mismatch_reason.c_str());
    } else {
        std::fprintf(stderr, "  status: PASS -- no divergence detected\n");
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Wiimmfi: Winsock (Windows only) MUST be initialized before ANY
    // Winsock API call anywhere in this process -- including WSAPoll
    // inside Net_Slirp::PollHostSockets(), reached from the host
    // networking worker thread that boot() (below) starts via
    // nds_wifi3d_attach(). Previously, WSAStartup was called only inside
    // debug_server.cpp's debug_serve()/debug_pump_start() -- both invoked
    // AFTER boot() in every mode that calls them at all -- so there was a
    // real window (present on the end-user launcher's own --interactive
    // path) where the worker thread could call into Winsock before
    // WSAStartup had succeeded anywhere in the process (undefined
    // behavior per Microsoft's documented WSAStartup contract). Worse, a
    // plain run (neither --serve nor --interactive, e.g. every batch/
    // scenario/regression-gate invocation) never called WSAStartup at
    // all, for the process's entire lifetime. This call, unconditionally
    // first in main() before any argument parsing or boot() work, fixes
    // every mode at once. See wifi_net.h/.cpp for the implementation --
    // it lives there (a translation unit that already transitively pulls
    // in winsock2.h via Net_Slirp.h -> libslirp.h) rather than pulling
    // windows.h/winsock2.h into main.cpp directly. debug_server.cpp's own
    // WSAStartup/WSACleanup pair is UNCHANGED and stays exactly as it
    // was: WSAStartup is reference-counted by design, so that pre-existing
    // balanced pair is harmless on top of this one, and keeping it means
    // debug_serve()/debug_pump_start() remain independently safe to call
    // (e.g. from a future standalone test) without depending on main()
    // having run first. A no-op returning true on every non-Windows build.
    if (!nds_net_platform_init()) {
        std::fprintf(stderr,
                     "refusing to start: Winsock initialization failed\n");
        return 1;
    }
    std::atexit(nds_net_platform_shutdown);
    std::atexit(live_overlay_shutdown);

    std::string dir = "bios";
    std::string rom_path;
    std::string config_path = "game.toml";
    std::string cli_screen_layout;
    std::string cli_fullscreen;
    std::string cli_adaptive_screens;
    std::string cli_widescreen_width;
    std::string cli_supersampling;
    std::string cli_internal_resolution;
    std::string cli_texture_upscale;
    std::string cli_antialiasing;
    std::string cli_frame_interpolation;
    std::string cli_relative_mouse_touch;
    std::string cli_tab_turbo;
    std::string cli_relative_mouse_sensitivity;
    std::string cli_relative_mouse_invert_y;
    std::string cli_relative_mouse_fire_key;
    std::string cli_mph_prime_controls;
    std::string cli_mph_prime_unified_window_focus;
    std::string cli_mph_virtual_stylus_sensitivity;
    std::string cli_mph_pad_aim_sensitivity;
    std::string cli_startup_mode;
    std::string cli_boot_mode;
    std::string cli_identity_mac;
    std::string cli_player_name;
    bool cli_generated_firmware = false;
    bool cli_freebios = false;
    std::string cli_instance_index;
    std::string cli_save_path;
    std::string cli_diagnostics;
    std::string cli_diagnostics_dir;
    std::string cli_diagnostics_interval_ms;
    // beads-yjp.28: where the Tier-3 coverage manifest lands. Empty means
    // "derive it" (next to the save, else next to the ROM). Written on every
    // exit path, with no flag required, so a player's ordinary session yields
    // an ingestible file.
    std::string cli_coverage_manifest;
    bool coverage_manifest_disabled = false;
    std::string cli_firmware_path;
    std::string cli_firmware_state_path;
    std::string cli_net_ring_filter;
    std::string cli_network_enabled;
    std::string cli_network_backend;
    std::string cli_pcap_adapter;
    std::string cli_wfc_enabled;
    std::string cli_wfc_provider;
    std::string cli_local_wireless_enabled;
    std::string cli_local_wireless_base_port;
    // Wiimmfi M8: capture/replay at the Ethernet backend boundary.
    std::string cli_net_capture_out;
    std::string cli_net_capture_in;
    bool cli_net_capture_raw = false;
    bool cli_net_capture_no_pcap = false;
    std::string cli_net_capture_scenario;
    bool cli_live_overlay_enable = false;
    bool cli_live_overlay_auto = false;
    bool cli_live_overlay_activation_delay_set = false;
    bool cli_live_overlay_auto_delay_set = false;
    bool cli_live_overlay_auto_cooldown_set = false;
    uint32_t cli_live_overlay_activation_delay_ms = 0u;
    uint32_t cli_live_overlay_auto_delay_ms = 0u;
    uint32_t cli_live_overlay_auto_cooldown_ms = 60000u;
    std::string cli_live_overlay_command;
    std::string cli_live_overlay_cache;
    uint64_t budget = 4000000ull;
    bool serve = false;
    bool interactive = false;
    bool config_explicit = false;
    bool discover_static_misses = false;
    bool save_disabled = false;
    // One-shot batch-mode diagnostic: dump the (Wiimmfi M0) network event
    // ring to stderr at the end of a plain (non-serve, non-interactive) run,
    // same convention as the existing nds_dump_irq()/runtime_trace_dump_recent
    // tail (main.cpp end-of-run block). No call site pushes into this ring
    // yet, so a normal run's dump is expected to be empty — this flag and
    // its two companions below exist to prove the query surface works, not
    // to surface real traffic yet.
    bool net_ring_dump = false;
    uint64_t net_ring_last = 256;
    uint8_t net_ring_filter_kind = NDS_NET_EVENT_KIND_COUNT;  // "all"
    NdsFrontendOptions frontend_options{};
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
        } else if (a == "--save-path" && i + 1 < argc) {
            cli_save_path = argv[++i];
        } else if (a == "--diagnostics" && i + 1 < argc) {
            cli_diagnostics = argv[++i];
        } else if (a == "--diagnostics-dir" && i + 1 < argc) {
            cli_diagnostics_dir = argv[++i];
        } else if (a == "--diagnostics-interval-ms" && i + 1 < argc) {
            cli_diagnostics_interval_ms = argv[++i];
        } else if (a == "--coverage-manifest" && i + 1 < argc) {
            cli_coverage_manifest = argv[++i];
        } else if (a == "--no-coverage-manifest") {
            coverage_manifest_disabled = true;
        } else if (a == "--no-save") {
            save_disabled = true;
        } else if (a == "--firmware-path" && i + 1 < argc) {
            cli_firmware_path = argv[++i];
        } else if (a == "--firmware-state-path" && i + 1 < argc) {
            cli_firmware_state_path = argv[++i];
        } else if (a == "--config" && i + 1 < argc) {
            config_path = argv[++i];
            config_explicit = true;
        } else if (a == "--screen-layout" && i + 1 < argc) {
            cli_screen_layout = argv[++i];
        } else if (a == "--fullscreen" && i + 1 < argc) {
            cli_fullscreen = argv[++i];
        } else if (a == "--adaptive-widescreen" && i + 1 < argc) {
            cli_adaptive_screens = argv[++i];
        } else if (a == "--widescreen-width" && i + 1 < argc) {
            cli_widescreen_width = argv[++i];
        } else if (a == "--supersampling" && i + 1 < argc) {
            cli_supersampling = argv[++i];
        } else if (a == "--internal-resolution" && i + 1 < argc) {
            cli_internal_resolution = argv[++i];
        } else if (a == "--texture-upscale" && i + 1 < argc) {
            cli_texture_upscale = argv[++i];
        } else if (a == "--antialiasing" && i + 1 < argc) {
            cli_antialiasing = argv[++i];
        } else if (a == "--frame-interpolation" && i + 1 < argc) {
            cli_frame_interpolation = argv[++i];
        } else if (a == "--relative-mouse-touch" && i + 1 < argc) {
            cli_relative_mouse_touch = argv[++i];
        } else if (a == "--tab-turbo" && i + 1 < argc) {
            cli_tab_turbo = argv[++i];
        } else if (a == "--relative-mouse-sensitivity" && i + 1 < argc) {
            cli_relative_mouse_sensitivity = argv[++i];
        } else if (a == "--relative-mouse-invert-y" && i + 1 < argc) {
            cli_relative_mouse_invert_y = argv[++i];
        } else if (a == "--relative-mouse-fire-key" && i + 1 < argc) {
            cli_relative_mouse_fire_key = argv[++i];
        } else if (a == "--mph-prime-controls" && i + 1 < argc) {
            cli_mph_prime_controls = argv[++i];
        } else if (a == "--mph-prime-unified-window-focus" && i + 1 < argc) {
            cli_mph_prime_unified_window_focus = argv[++i];
        } else if (a == "--mph-virtual-stylus-sensitivity" && i + 1 < argc) {
            cli_mph_virtual_stylus_sensitivity = argv[++i];
        } else if (a == "--mph-pad-aim-sensitivity" && i + 1 < argc) {
            cli_mph_pad_aim_sensitivity = argv[++i];
        } else if (a.rfind("--mph-pad-bind-", 0) == 0 && i + 1 < argc) {
            const std::string action =
                a.substr(std::strlen("--mph-pad-bind-"));
            if (!nds_set_mph_prime_pad_binding(
                    &frontend_options, action, argv[++i])) {
                std::fprintf(stderr,
                    "invalid %s (unknown action or too-long value)\n",
                    a.c_str());
                return 1;
            }
        } else if (a.rfind("--mph-bind-", 0) == 0 && i + 1 < argc) {
            const std::string action = a.substr(std::strlen("--mph-bind-"));
            if (!nds_set_mph_prime_binding(
                    &frontend_options, action, argv[++i])) {
                std::fprintf(stderr,
                    "invalid %s (unknown action or too-long value)\n",
                    a.c_str());
                return 2;
            }
        } else if (a == "--startup-mode" && i + 1 < argc) {
            cli_startup_mode = argv[++i];
        } else if (a == "--boot" && i + 1 < argc) {
            cli_boot_mode = argv[++i];
        } else if (a == "--generated-firmware") {
            cli_generated_firmware = true;
        } else if (a == "--freebios") {
            cli_freebios = true;
        } else if (a == "--identity-mac" && i + 1 < argc) {
            cli_identity_mac = argv[++i];
        } else if (a == "--player-name" && i + 1 < argc) {
            cli_player_name = argv[++i];
        } else if (a == "--instance-index" && i + 1 < argc) {
            cli_instance_index = argv[++i];
        } else if (a == "--net-ring-dump") {
            // Plain action flag (Convention 2, main.cpp:242-252 style): no
            // value, dumps the network ring to stderr at end-of-run. A
            // "FILE" variant would redirect the same text; not implemented
            // separately since the text is already stderr-redirectable by
            // the caller (matches how runtime_trace_dump_recent works).
            net_ring_dump = true;
        } else if (a == "--net-ring-last" && i + 1 < argc) {
            net_ring_last = std::strtoull(argv[++i], nullptr, 0);
        } else if (a == "--net-ring-filter" && i + 1 < argc) {
            cli_net_ring_filter = argv[++i];
        } else if (a == "--network" && i + 1 < argc) {
            cli_network_enabled = argv[++i];
        } else if (a == "--network-backend" && i + 1 < argc) {
            cli_network_backend = argv[++i];
        } else if (a == "--pcap-adapter" && i + 1 < argc) {
            cli_pcap_adapter = argv[++i];
        } else if (a == "--wfc" && i + 1 < argc) {
            cli_wfc_enabled = argv[++i];
        } else if (a == "--wfc-provider" && i + 1 < argc) {
            cli_wfc_provider = argv[++i];
        } else if (a == "--local-wireless" && i + 1 < argc) {
            cli_local_wireless_enabled = argv[++i];
        } else if (a == "--local-wireless-port" && i + 1 < argc) {
            cli_local_wireless_base_port = argv[++i];
        } else if (a == "--net-capture-out" && i + 1 < argc) {
            cli_net_capture_out = argv[++i];
        } else if (a == "--net-capture-in" && i + 1 < argc) {
            cli_net_capture_in = argv[++i];
        } else if (a == "--net-capture-raw") {
            cli_net_capture_raw = true;
        } else if (a == "--net-capture-no-pcap") {
            cli_net_capture_no_pcap = true;
        } else if (a == "--net-capture-scenario" && i + 1 < argc) {
            cli_net_capture_scenario = argv[++i];
        } else if (a == "--live-overlay-enable") {
            cli_live_overlay_enable = true;
        } else if (a == "--live-overlay-auto") {
            cli_live_overlay_auto = true;
        } else if (a == "--live-overlay-activation-delay-ms" && i + 1 < argc) {
            cli_live_overlay_activation_delay_ms =
                static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
            cli_live_overlay_activation_delay_set = true;
        } else if (a == "--live-overlay-auto-delay-ms" && i + 1 < argc) {
            cli_live_overlay_auto_delay_ms =
                static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
            cli_live_overlay_auto_delay_set = true;
        } else if (a == "--live-overlay-auto-cooldown-ms" && i + 1 < argc) {
            cli_live_overlay_auto_cooldown_ms =
                static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
            cli_live_overlay_auto_cooldown_set = true;
        } else if (a == "--live-overlay-command" && i + 1 < argc) {
            cli_live_overlay_command = argv[++i];
        } else if (a == "--live-overlay-cache" && i + 1 < argc) {
            cli_live_overlay_cache = argv[++i];
        } else if (a == "--help" || a == "-h") {
            std::fprintf(stderr,
                "usage: %s [bios-dir] [cycle-budget] [--rom game.nds] "
                "[--serve|--interactive] [--port 19842] "
                "[--save-path game.sav|--no-save] "
                "[--diagnostics on|off] [--diagnostics-dir dir] "
                "[--diagnostics-interval-ms ms] "
                "[--coverage-manifest out.json|--no-coverage-manifest] "
                "[--firmware-path firmware.bin] "
                "[--firmware-state-path mutable-firmware.bin] "
                "[--config game.toml] "
                "[--screen-layout stacked|separate] "
                "[--fullscreen off|borderless|exclusive] "
                "[--adaptive-widescreen none|top|bottom|both] "
                "[--widescreen-width even:256..448] "
                "[--supersampling 1|2|3|4] "
                "[--internal-resolution 1|2|3|4] "
                "[--texture-upscale 1|2|4] "
                "[--antialiasing 0|2|4|8] "
                "[--frame-interpolation off|blend] "
                "[--relative-mouse-touch on|off] "
                "[--tab-turbo on|off] "
                "[--relative-mouse-sensitivity 10..400] "
                "[--relative-mouse-invert-y on|off] "
                "[--relative-mouse-fire-key none|a|b|l|r|x|y] "
                "[--mph-prime-controls on|off] "
                "[--mph-prime-unified-window-focus on|off] "
                "[--mph-virtual-stylus-sensitivity 10..400] "
                "[--mph-pad-aim-sensitivity 10..400] "
                "[--mph-bind-<action> <key-or-mouse>] "
                "[--mph-pad-bind-<action> <pad-button|None>] "
                "[--startup-mode preserve|manual|automatic] "
                "[--boot lle|direct] "
                "[--generated-firmware] [--identity-mac AA:BB:CC:DD:EE:FF] "
                "[--player-name NAME] "
                "[--freebios] "
                "[--instance-index 0..255] "
                "[--discover-static-misses] [--rtc-host] "
                "[--net-ring-dump] [--net-ring-last N] "
                "[--net-ring-filter <class>|all] "
                "[--network on|off] [--network-backend slirp|replay|pcap] "
                "[--pcap-adapter NAME] "
                "[--wfc on|off] "
                "[--wfc-provider kaeru|wiimmfi|wiimmfi-direct|local|<ipv4>] "
                "[--local-wireless on|off] [--local-wireless-port 1024..65520] "
                "[--net-capture-out FILE] [--net-capture-in FILE] "
                "[--net-capture-raw] [--net-capture-no-pcap] "
                "[--net-capture-scenario NAME] "
                "[--live-overlay-enable --live-overlay-command CMD "
                "--live-overlay-cache DIR] [--live-overlay-auto] "
                "[--live-overlay-activation-delay-ms N] "
                "[--live-overlay-auto-delay-ms N] "
                "[--live-overlay-auto-cooldown-ms N]\n",
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

    if (save_disabled && !cli_save_path.empty()) {
        std::fprintf(stderr,
                     "--save-path and --no-save cannot be used together\n");
        return 2;
    }

    if (config_explicit || std::filesystem::exists(config_path)) {
        std::string config_error;
        if (!nds_load_frontend_config(config_path, &frontend_options,
                                      &config_error)) {
            std::fprintf(stderr, "invalid frontend config %s: %s\n",
                         config_path.c_str(), config_error.c_str());
            return 2;
        }
    }
    NdsFullscreenOverrideError fullscreen_error =
        NdsFullscreenOverrideError::None;
    if (!nds_apply_fullscreen_overrides(
            &frontend_options, cli_fullscreen, &fullscreen_error)) {
        if (fullscreen_error == NdsFullscreenOverrideError::Environment) {
            std::fprintf(stderr,
                         "invalid NDS_FULLSCREEN "
                         "(expected off, borderless, or exclusive)\n");
        } else {
            std::fprintf(stderr,
                         "invalid --fullscreen "
                         "(expected off, borderless, or exclusive)\n");
        }
        return 2;
    }
    if (const char* value = std::getenv("NDS_SCREEN_LAYOUT")) {
        if (!nds_parse_screen_layout(value,
                                     &frontend_options.screen_layout)) {
            std::fprintf(stderr,
                         "invalid NDS_SCREEN_LAYOUT "
                         "(expected stacked or separate)\n");
            return 2;
        }
    }
    if (const char* value = std::getenv("NDS_ADAPTIVE_WIDESCREEN")) {
        if (!nds_parse_adaptive_screens(
                value, &frontend_options.adaptive_screens)) {
            std::fprintf(stderr,
                         "invalid NDS_ADAPTIVE_WIDESCREEN "
                         "(expected none, top, bottom, or both)\n");
            return 2;
        }
    }
    auto apply_adaptive_width = [&](uint16_t width) {
        if (frontend_options.adaptive_supported & NDS_ADAPTIVE_TOP)
            frontend_options.adaptive_max_width[0] = width;
        if (frontend_options.adaptive_supported & NDS_ADAPTIVE_BOTTOM)
            frontend_options.adaptive_max_width[1] = width;
    };
    if (const char* value = std::getenv("NDS_WIDESCREEN_WIDTH")) {
        uint16_t width = 0;
        if (!nds_parse_widescreen_width(value, &width)) {
            std::fprintf(stderr,
                         "invalid NDS_WIDESCREEN_WIDTH "
                         "(expected 256, 320, 384, or 448)\n");
            return 2;
        }
        apply_adaptive_width(width);
    }
    if (const char* value = std::getenv("NDS_SUPERSAMPLING")) {
        if (!nds_parse_supersampling(value,
                                     &frontend_options.supersampling)) {
            std::fprintf(stderr,
                         "invalid NDS_SUPERSAMPLING (expected 1..4)\n");
            return 2;
        }
    }
    if (const char* value = std::getenv("NDS_TEXTURE_UPSCALE")) {
        if (!nds_parse_texture_upscale(
                value, &frontend_options.texture_upscale)) {
            std::fprintf(stderr,
                         "invalid NDS_TEXTURE_UPSCALE "
                         "(expected 1, 2, or 4)\n");
            return 2;
        }
    }
    if (const char* value = std::getenv("NDS_INTERNAL_RESOLUTION")) {
        if (!nds_parse_internal_resolution(
                value, &frontend_options.internal_resolution)) {
            std::fprintf(stderr,
                         "invalid NDS_INTERNAL_RESOLUTION "
                         "(expected 1..4)\n");
            return 2;
        }
    }
    if (const char* value = std::getenv("NDS_ANTIALIASING")) {
        if (!nds_parse_antialiasing(value,
                                    &frontend_options.antialiasing)) {
            std::fprintf(stderr,
                         "invalid NDS_ANTIALIASING "
                         "(expected 0, 2, 4, or 8)\n");
            return 2;
        }
    }
    if (const char* value = std::getenv("NDS_FRAME_INTERPOLATION")) {
        if (!nds_parse_frame_interpolation(
                value, &frontend_options.frame_interpolation)) {
            std::fprintf(stderr,
                         "invalid NDS_FRAME_INTERPOLATION "
                         "(expected off or blend)\n");
            return 2;
        }
    }
    if (const char* value = std::getenv("NDS_STARTUP_MODE")) {
        if (!nds_parse_startup_mode(value,
                                    &frontend_options.startup_mode)) {
            std::fprintf(stderr,
                         "invalid NDS_STARTUP_MODE "
                         "(expected preserve, manual, or automatic)\n");
            return 2;
        }
    }
    if (const char* value = std::getenv("NDS_BOOT_MODE")) {
        if (!nds_parse_boot_mode(value, &frontend_options.boot_mode)) {
            std::fprintf(stderr,
                         "invalid NDS_BOOT_MODE (expected lle or direct)\n");
            return 2;
        }
    }
    if (const char* value = std::getenv("NDS_GENERATED_FIRMWARE")) {
        bool enabled = false;
        if (!nds_parse_on_off(value, &enabled)) {
            std::fprintf(stderr,
                         "invalid NDS_GENERATED_FIRMWARE "
                         "(expected on or off)\n");
            return 2;
        }
        frontend_options.generated_firmware = enabled;
    }
    if (const char* value = std::getenv("NDS_FREEBIOS")) {
        bool enabled = false;
        if (!nds_parse_on_off(value, &enabled)) {
            std::fprintf(stderr,
                         "invalid NDS_FREEBIOS (expected on or off)\n");
            return 2;
        }
        frontend_options.freebios = enabled;
    }
    if (const char* value = std::getenv("NDS_PLAYER_NAME")) {
        frontend_options.player_name = value;
    }
    if (!cli_player_name.empty()) frontend_options.player_name = cli_player_name;
    // Validate ONCE, here, so a bad name fails before any boot work happens
    // and the message names the offending value -- never silently truncated.
    if (!frontend_options.player_name.empty()) {
        std::string player_name_error;
        if (!nds_validate_player_name(frontend_options.player_name,
                                      &player_name_error)) {
            std::fprintf(stderr,
                "invalid player name \"%s\": %s\n",
                frontend_options.player_name.c_str(),
                player_name_error.c_str());
            return 2;
        }
    }
    if (!cli_screen_layout.empty() &&
        !nds_parse_screen_layout(cli_screen_layout,
                                 &frontend_options.screen_layout)) {
        std::fprintf(stderr,
                     "invalid --screen-layout "
                     "(expected stacked or separate)\n");
        return 2;
    }
    if (!cli_adaptive_screens.empty() &&
        !nds_parse_adaptive_screens(cli_adaptive_screens,
                                    &frontend_options.adaptive_screens)) {
        std::fprintf(stderr,
                     "invalid --adaptive-widescreen "
                     "(expected none, top, bottom, or both)\n");
        return 2;
    }
    if (!cli_widescreen_width.empty()) {
        uint16_t width = 0;
        if (!nds_parse_widescreen_width(cli_widescreen_width, &width)) {
            std::fprintf(stderr,
                         "invalid --widescreen-width "
                         "(expected 256, 320, 384, or 448)\n");
            return 2;
        }
        apply_adaptive_width(width);
    }
    if (!cli_supersampling.empty() &&
        !nds_parse_supersampling(cli_supersampling,
                                 &frontend_options.supersampling)) {
        std::fprintf(stderr,
                     "invalid --supersampling (expected 1..4)\n");
        return 2;
    }
    if (!cli_texture_upscale.empty() &&
        !nds_parse_texture_upscale(cli_texture_upscale,
                                   &frontend_options.texture_upscale)) {
        std::fprintf(stderr,
                     "invalid --texture-upscale "
                     "(expected 1, 2, or 4)\n");
        return 2;
    }
    if (!cli_internal_resolution.empty() &&
        !nds_parse_internal_resolution(
            cli_internal_resolution,
            &frontend_options.internal_resolution)) {
        std::fprintf(stderr,
                     "invalid --internal-resolution (expected 1..4)\n");
        return 2;
    }
    if (!cli_antialiasing.empty() &&
        !nds_parse_antialiasing(cli_antialiasing,
                                &frontend_options.antialiasing)) {
        std::fprintf(stderr,
                     "invalid --antialiasing "
                     "(expected 0, 2, 4, or 8)\n");
        return 2;
    }
    if (!cli_frame_interpolation.empty() &&
        !nds_parse_frame_interpolation(
            cli_frame_interpolation,
            &frontend_options.frame_interpolation)) {
        std::fprintf(stderr,
                     "invalid --frame-interpolation "
                     "(expected off or blend)\n");
        return 2;
    }
    if (!cli_relative_mouse_touch.empty() &&
        !nds_parse_on_off(cli_relative_mouse_touch,
                          &frontend_options.relative_mouse_touch)) {
        std::fprintf(stderr,
                     "invalid --relative-mouse-touch (expected on or off)\n");
        return 2;
    }
    if (!cli_tab_turbo.empty() &&
        !nds_parse_on_off(cli_tab_turbo, &frontend_options.tab_turbo)) {
        std::fprintf(stderr,
                     "invalid --tab-turbo (expected on or off)\n");
        return 2;
    }
    if (!cli_relative_mouse_sensitivity.empty() &&
        !nds_parse_mouse_sensitivity(
            cli_relative_mouse_sensitivity,
            &frontend_options.relative_mouse_sensitivity)) {
        std::fprintf(stderr,
                     "invalid --relative-mouse-sensitivity "
                     "(expected 10..400)\n");
        return 2;
    }
    if (!cli_relative_mouse_invert_y.empty() &&
        !nds_parse_on_off(cli_relative_mouse_invert_y,
                          &frontend_options.relative_mouse_invert_y)) {
        std::fprintf(stderr,
                     "invalid --relative-mouse-invert-y "
                     "(expected on or off)\n");
        return 2;
    }
    if (!cli_relative_mouse_fire_key.empty() &&
        !nds_parse_mouse_fire_key(
            cli_relative_mouse_fire_key,
            &frontend_options.relative_mouse_fire_mask)) {
        std::fprintf(stderr,
                     "invalid --relative-mouse-fire-key "
                     "(expected none, a, b, l, r, x, or y)\n");
        return 2;
    }
    if (!cli_mph_prime_controls.empty() &&
        !nds_parse_on_off(cli_mph_prime_controls,
                          &frontend_options.mph_prime_controls)) {
        std::fprintf(stderr,
                     "invalid --mph-prime-controls (expected on or off)\n");
        return 2;
    }
    if (!cli_mph_prime_unified_window_focus.empty() &&
        !nds_parse_on_off(
            cli_mph_prime_unified_window_focus,
            &frontend_options.mph_prime_unified_window_focus)) {
        std::fprintf(
            stderr,
            "invalid --mph-prime-unified-window-focus "
            "(expected on or off)\n");
        return 2;
    }
    if (!cli_mph_virtual_stylus_sensitivity.empty() &&
        !nds_parse_mouse_sensitivity(
            cli_mph_virtual_stylus_sensitivity,
            &frontend_options.mph_virtual_stylus_sensitivity)) {
        std::fprintf(stderr,
                     "invalid --mph-virtual-stylus-sensitivity "
                     "(expected 10..400)\n");
        return 2;
    }
    if (!cli_mph_pad_aim_sensitivity.empty() &&
        !nds_parse_mouse_sensitivity(
            cli_mph_pad_aim_sensitivity,
            &frontend_options.mph_pad_aim_sensitivity)) {
        std::fprintf(stderr,
                     "invalid --mph-pad-aim-sensitivity "
                     "(expected 10..400)\n");
        return 2;
    }
    if (frontend_options.mph_prime_controls &&
        cli_relative_mouse_sensitivity.empty()) {
        frontend_options.relative_mouse_sensitivity = 30;
    }
    if (!cli_startup_mode.empty() &&
        !nds_parse_startup_mode(cli_startup_mode,
                                &frontend_options.startup_mode)) {
        std::fprintf(stderr,
                     "invalid --startup-mode "
                     "(expected preserve, manual, or automatic)\n");
        return 2;
    }
    if (!cli_boot_mode.empty() &&
        !nds_parse_boot_mode(cli_boot_mode, &frontend_options.boot_mode)) {
        std::fprintf(stderr, "invalid --boot (expected lle or direct)\n");
        return 2;
    }
    if (!cli_instance_index.empty() &&
        !nds_parse_instance_index(cli_instance_index,
                                  &frontend_options.instance_index)) {
        std::fprintf(stderr, "invalid --instance-index (expected 0..255)\n");
        return 2;
    }
    if (!cli_net_ring_filter.empty() &&
        !nds_net_event_kind_parse(cli_net_ring_filter.c_str(),
                                  &net_ring_filter_kind)) {
        std::fprintf(stderr,
                     "invalid --net-ring-filter %s (expected 'all' or one "
                     "of the NdsNetEventKind names, e.g. wifi_reg_write, "
                     "dns_query, tcp_packet)\n",
                     cli_net_ring_filter.c_str());
        return 2;
    }
    if (!cli_network_enabled.empty() &&
        !nds_parse_on_off(cli_network_enabled, &frontend_options.network.enabled)) {
        std::fprintf(stderr, "invalid --network (expected on or off)\n");
        return 2;
    }
    if (!cli_network_backend.empty() &&
        !nds_parse_network_backend(cli_network_backend,
                                   &frontend_options.network.backend)) {
        std::fprintf(stderr,
                     "invalid --network-backend (expected slirp, replay, or "
                     "pcap)\n");
        return 2;
    }
    if (!cli_pcap_adapter.empty())
        frontend_options.network.pcap_adapter = cli_pcap_adapter;
    if (!cli_wfc_enabled.empty() &&
        !nds_parse_on_off(cli_wfc_enabled, &frontend_options.network.wfc_enabled)) {
        std::fprintf(stderr, "invalid --wfc (expected on or off)\n");
        return 2;
    }
    if (!cli_local_wireless_enabled.empty() &&
        !nds_parse_on_off(cli_local_wireless_enabled,
                          &frontend_options.local_wireless.enabled)) {
        std::fprintf(stderr,
                     "invalid --local-wireless (expected on or off)\n");
        return 2;
    }
    if (!cli_local_wireless_base_port.empty() &&
        !nds_parse_local_wireless_base_port(
            cli_local_wireless_base_port,
            &frontend_options.local_wireless.base_port)) {
        std::fprintf(stderr,
                     "invalid --local-wireless-port (expected 1024..65520)\n");
        return 2;
    }
    if (frontend_options.local_wireless.enabled &&
        frontend_options.instance_index > 15u) {
        std::fprintf(stderr,
            "--local-wireless on requires --instance-index 0..15\n");
        return 2;
    }
    if (!cli_wfc_provider.empty()) {
        uint32_t probe = 0;
        if (nds_parse_ipv4(cli_wfc_provider, &probe)) {
            // Raw dotted-quad: a direct per-endpoint DNS override (e.g. a
            // local test DWC server), provider name becomes "custom" for
            // logging/reporting purposes only.
            frontend_options.network.wfc_provider.name = "custom";
            frontend_options.network.wfc_provider.dns_server = cli_wfc_provider;
        } else if (nds_wfc_provider_lookup(cli_wfc_provider.c_str())) {
            frontend_options.network.wfc_provider.name = cli_wfc_provider;
            frontend_options.network.wfc_provider.dns_server.clear();
        } else {
            std::fprintf(stderr,
                "invalid --wfc-provider %s (expected a known provider name "
                "[kaeru, wiimmfi, wiimmfi-direct, local, local-oracle] or a "
                "dotted-quad IPv4 address)\n",
                cli_wfc_provider.c_str());
            return 2;
        }
    }
    // Wiimmfi M8: transfer the capture/replay CLI flags into
    // frontend_options (no parsing needed beyond what the arg loop already
    // did -- these are plain strings/bools, unlike backend/on-off, which
    // need nds_parse_* below).
    if (!cli_net_capture_out.empty())
        frontend_options.network.capture_out = cli_net_capture_out;
    if (!cli_net_capture_in.empty())
        frontend_options.network.capture_in = cli_net_capture_in;
    if (cli_net_capture_raw) frontend_options.network.capture_raw = true;
    if (cli_net_capture_no_pcap) frontend_options.network.capture_no_pcap = true;
    if (!cli_net_capture_scenario.empty())
        frontend_options.network.capture_scenario = cli_net_capture_scenario;
    bool diagnostics_enabled = true;
    if (!cli_diagnostics.empty() &&
        !nds_parse_on_off(cli_diagnostics, &diagnostics_enabled)) {
        std::fprintf(stderr, "invalid --diagnostics (expected on or off)\n");
        return 2;
    }
    uint32_t diagnostics_interval_ms = 2000u;
    if (!cli_diagnostics_interval_ms.empty()) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(
            cli_diagnostics_interval_ms.c_str(), &end, 10);
        if (end == cli_diagnostics_interval_ms.c_str() || *end != '\0' ||
            parsed < 250ul || parsed > 60000ul) {
            std::fprintf(stderr,
                "invalid --diagnostics-interval-ms (expected 250..60000)\n");
            return 2;
        }
        diagnostics_interval_ms = static_cast<uint32_t>(parsed);
    }
    nds_diagnostics_configure(diagnostics_enabled,
                              cli_diagnostics_dir.c_str(),
                              diagnostics_interval_ms);
    if (!diagnostics_enabled) coverage_manifest_disabled = true;
    nds_diagnostics_enable_profile_environment();

    // Resolve the network config to backend-ready numeric form. Provider
    // name -> table lookup, optional dns_server string -> IPv4, matching
    // the pipeline documented in wifi_net.h's NdsWifiNetworkConfig.
    NdsWifiNetworkConfig resolved_network{};
    resolved_network.enabled = frontend_options.network.enabled;
    resolved_network.wfc_enabled = frontend_options.network.wfc_enabled;
    resolved_network.wfc_clear_crt_errno_addr =
        frontend_options.network.wfc_clear_crt_errno_addr;
    resolved_network.pcap_adapter = frontend_options.network.pcap_adapter;
    resolved_network.slirp_virtual_network_instance =
        frontend_options.instance_index;
    resolved_network.local_wireless_enabled =
        frontend_options.local_wireless.enabled;
    resolved_network.local_wireless_instance =
        frontend_options.instance_index;
    resolved_network.local_wireless_base_port =
        frontend_options.local_wireless.base_port;

    // Resolve the user-facing backend name to the bridge enum. Replay is
    // always available; pcap is available only in builds that opt into the
    // dynamically-loaded Npcap/WinPcap backend.
    if (frontend_options.network.backend == "slirp") {
        resolved_network.backend = NdsNetBackendKind::Slirp;
    } else if (frontend_options.network.backend == "replay") {
        resolved_network.backend = NdsNetBackendKind::Replay;
    } else if (frontend_options.network.backend == "pcap") {
#if defined(NDS_ENABLE_PCAP_BACKEND)
        resolved_network.backend = NdsNetBackendKind::Pcap;
#else
        std::fprintf(stderr,
            "--network-backend pcap requested, but this runner was built "
            "without NDS_ENABLE_PCAP_BACKEND\n");
        return 2;
#endif
    } else {
        std::fprintf(stderr,
            "--network-backend %s is not recognized\n",
            frontend_options.network.backend.c_str());
        return 2;
    }

    if (resolved_network.backend == NdsNetBackendKind::Replay) {
        if (frontend_options.network.capture_in.empty()) {
            std::fprintf(stderr,
                "--network-backend replay requires --net-capture-in <file>\n");
            return 2;
        }
        // Load AND fully validate the capture file HERE, at CLI-validation
        // time -- never inside nds_wifi3d_attach(). A corrupt/truncated
        // capture is therefore a startup-time error with a clear message
        // (exit code 2, matching every other malformed-CLI-input case in
        // this file), never a silent "0 records replayed, looks like a
        // pass" mid-run surprise. See net_capture.h's ReadAll doc comment
        // for the exact corruption/truncation contract this enforces.
        NdsNetCaptureReader reader;
        std::string cap_error;
        if (!reader.Open(frontend_options.network.capture_in, &cap_error)) {
            std::fprintf(stderr, "--net-capture-in %s: %s\n",
                         frontend_options.network.capture_in.c_str(),
                         cap_error.c_str());
            return 2;
        }
        if (!reader.ReadAll(&resolved_network.replay_records, &cap_error)) {
            std::fprintf(stderr, "--net-capture-in %s: %s\n",
                         frontend_options.network.capture_in.c_str(),
                         cap_error.c_str());
            return 2;
        }
        resolved_network.replay_sanitized = reader.sanitized();
        std::fprintf(stderr,
            "[network] replay capture loaded: %s (%zu record(s), "
            "sanitized=%d)\n",
            frontend_options.network.capture_in.c_str(),
            resolved_network.replay_records.size(),
            reader.sanitized() ? 1 : 0);
    } else if (!frontend_options.network.capture_in.empty()) {
        std::fprintf(stderr,
            "--net-capture-in requires --network-backend replay\n");
        return 2;
    }

    if (!frontend_options.network.capture_out.empty()) {
        resolved_network.capture_out_path = frontend_options.network.capture_out;
        resolved_network.capture_sanitize = !frontend_options.network.capture_raw;
        resolved_network.capture_write_pcap = !frontend_options.network.capture_no_pcap;
        resolved_network.capture_scenario_tag = frontend_options.network.capture_scenario;
        if (frontend_options.network.capture_raw) {
            // "Make the safe thing the default" (M8 privacy requirement):
            // sanitize-on-write is the default at every call site; this is
            // the ONE explicit opt-out, so make it loud rather than quiet.
            std::fprintf(stderr,
                "[network] WARNING: --net-capture-raw requested -- capture "
                "'%s' will contain UNSANITIZED console-identifying data "
                "(real MAC/DHCP identifiers). Never commit it; run "
                "tools/net_capture_tool.py sanitize before publishing.\n",
                resolved_network.capture_out_path.c_str());
        }
    }

    if (resolved_network.wfc_enabled &&
        resolved_network.backend != NdsNetBackendKind::Replay) {
        const NdsWfcProvider& wfc = frontend_options.network.wfc_provider;
        std::string dns_str = wfc.dns_server;
        if (dns_str.empty()) {
            const NdsWfcProviderInfo* info = nds_wfc_provider_lookup(wfc.name.c_str());
            if (!info) {
                std::fprintf(stderr,
                    "network.wfc.provider %s is not a known provider name "
                    "(expected kaeru, wiimmfi, or wiimmfi-direct) and no "
                    "dns_server override was given\n", wfc.name.c_str());
                return 2;
            }
            dns_str = info->dns_server;
        }
        if (!nds_parse_ipv4(dns_str, &resolved_network.wfc_dns_ipv4)) {
            std::fprintf(stderr,
                "resolved WFC DNS address %s is not a valid dotted-quad "
                "IPv4 address\n", dns_str.c_str());
            return 2;
        }
        if (is_ipv4_loopback(resolved_network.wfc_dns_ipv4)) {
            std::fprintf(stderr,
                "WFC DNS address %s is guest loopback, not host loopback. "
                "For the Slirp local server use --wfc-provider local and "
                "configure the local DNS responder to answer 10.64.0.1; "
                "for pcap use a host LAN IPv4 address.\n",
                dns_str.c_str());
            return 2;
        }
        std::fprintf(stderr,
            "[network] WFC DNS provider: %s (%s)\n",
            wfc.name.c_str(), dns_str.c_str());
    }
    if (!resolved_network.enabled) {
        std::fprintf(stderr,
            "[network] disabled (--network off): no host networking "
            "backend will be attached\n");
    }
    nds_wifi_configure_network(resolved_network);

    g_discover_static_misses = discover_static_misses;

    const NdsGpu3dRendererPolicy renderer_policy =
        nds_gpu3d_renderer_policy();
    if (renderer_policy == NdsGpu3dRendererPolicy::Invalid) {
        std::fprintf(stderr,
                     "invalid NDS_3D_RENDERER "
                     "(expected auto, soft, or compute)\n");
        return 2;
    }
    if (renderer_policy == NdsGpu3dRendererPolicy::Compute &&
        !nds_gpu3d_compute_renderer_built()) {
        std::fprintf(stderr,
            "NDS_3D_RENDERER=compute requested but this runner was "
            "built without NDS_ENABLE_COMPUTE_RENDERER\n");
        return 2;
    }
    const bool compute_preferred =
        renderer_policy != NdsGpu3dRendererPolicy::Soft &&
        nds_gpu3d_compute_renderer_built();
    std::fprintf(stderr, "[gpu3d] renderer policy: %s (preferred: %s)\n",
                 nds_gpu3d_renderer_policy_name(renderer_policy),
                 compute_preferred ? "OpenGL 4.3 compute"
                                   : "threaded software");
    for (const char* name : {
             "NDS_COMPUTE_READBACK_OVERLAP",
             "NDS_COMPUTE_DIRECT_PRESENT",
         }) {
        if (const char* value = std::getenv(name);
            value && *value &&
            std::strcmp(value, "0") != 0 &&
            std::strcmp(value, "1") != 0) {
            std::fprintf(stderr, "invalid %s (expected 0 or 1)\n", name);
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

    if (cli_generated_firmware) frontend_options.generated_firmware = true;
    if (cli_freebios) frontend_options.freebios = true;
    if (frontend_options.freebios &&
        frontend_options.boot_mode != NdsBootMode::Direct) {
        // A reimplemented BIOS cannot boot the firmware menu (melonDS
        // NeedsDirectBoot); refuse rather than silently switch paths.
        std::fprintf(stderr,
            "refusing to start: --freebios requires --boot direct\n");
        return 1;
    }
    if (frontend_options.generated_firmware) {
        // Generated firmware carries no boot code, so the pairing is
        // enforced rather than silently switched (melonDS NeedsDirectBoot).
        if (frontend_options.boot_mode != NdsBootMode::Direct) {
            std::fprintf(stderr,
                "refusing to start: --generated-firmware requires "
                "--boot direct (a generated image has no boot code)\n");
            return 1;
        }
        if (!cli_firmware_path.empty()) {
            std::fprintf(stderr,
                "refusing to start: --generated-firmware and "
                "--firmware-path are mutually exclusive\n");
            return 1;
        }
        if (!cli_identity_mac.empty() && !cli_firmware_state_path.empty()) {
            std::fprintf(stderr,
                "refusing to start: --identity-mac cannot be combined with "
                "--firmware-state-path\n");
            return 1;
        }
    } else if (!cli_identity_mac.empty()) {
        std::fprintf(stderr,
            "refusing to start: --identity-mac only applies to "
            "--generated-firmware (a firmware dump carries its own MAC)\n");
        return 1;
    }

    std::vector<uint8_t> a9, a7;
    if (frontend_options.freebios) {
        a9.assign(nds_freebios9, nds_freebios9 + sizeof(nds_freebios9));
        a7.assign(nds_freebios7, nds_freebios7 + sizeof(nds_freebios7));
        std::fprintf(stderr,
            "[bios] FREEBIOS (opt-in, no dumps): arm9 %zu bytes SHA-1 %s, "
            "arm7 %zu bytes SHA-1 %s\n",
            a9.size(), gba::sha1(a9.data(), a9.size()).hex().c_str(),
            a7.size(), gba::sha1(a7.data(), a7.size()).hex().c_str());
    } else {
        a9 = read_file(dir + "/biosnds9.rom");
        a7 = read_file(dir + "/biosnds7.rom");
    }
    std::vector<uint8_t> fw;
    if (frontend_options.generated_firmware) {
        std::array<uint8_t, 6> identity_mac{};
        if (!cli_identity_mac.empty()) {
            if (!parse_identity_mac(cli_identity_mac, &identity_mac)) {
                std::fprintf(stderr,
                    "invalid --identity-mac (expected a unicast "
                    "AA:BB:CC:DD:EE:FF)\n");
                return 2;
            }
            std::fprintf(stderr,
                "[identity] explicit MAC %s (not persisted)\n",
                cli_identity_mac.c_str());
        } else if (!load_or_create_identity_mac(
                       dir + "/generated-identity.bin", &identity_mac)) {
            return 1;
        }
        fw = nds_generate_firmware(identity_mac.data());
        // Session provenance: a generated-firmware session must never be
        // mistaken for a dump-verified one.
        std::fprintf(stderr,
            "[firmware] GENERATED image (opt-in, no dump), %zu bytes, "
            "SHA-1 %s\n", fw.size(),
            gba::sha1(fw.data(), fw.size()).hex().c_str());
    } else {
        fw = read_file(cli_firmware_path.empty() ? (dir + "/firmware.bin")
                                                 : cli_firmware_path);
    }
    auto rom = rom_path.empty() ? std::vector<uint8_t>{} : read_file(rom_path);
    std::string rom_sha1;
    bool sm64ds_wide_policy = false;
    bool mph_mouse_aim_policy = false;
#ifdef NDS_HAVE_SM64DS_BANKS
    bool sm64ds_title = false;
#endif
    // Per-backend identity check (psxrecomp bios_backend_for_file model):
    // the retail dumps and the embedded FreeBIOS each verify against their
    // own pinned hashes -- the loaded bytes must match the recompiled banks
    // that will execute them.
    bool ok = frontend_options.freebios
        ? verify(a9, "b85d6afdbf65d87c9ab11d4e7fb5ecfd6192ccf8", "freebios arm9")
        & verify(a7, "eaab9b21610978f05a0eb14a9c8ff345698f56a2", "freebios arm7")
        : verify(a9, "bfaac75f101c135e32e2aaf541de6b1be4c8c62d", "arm9 bios")
        & verify(a7, "24f67bdea115a2c847c8813a262502ee1607b7df", "arm7 bios");
    if (frontend_options.generated_firmware) {
        // Already synthesized and reported above; nothing to verify.
    } else if (cli_firmware_path.empty()) {
        ok = ok & verify(fw, "ae22de59fbf3f35ccfbeacaeba6fa87ac5e7b14b", "firmware");
    } else if (fw.size() != 262144u) {
        std::fprintf(stderr,
            "refusing to start: --firmware-path %s has %zu bytes, expected 262144\n",
            cli_firmware_path.c_str(), fw.size());
        return 1;
    } else {
        std::fprintf(stderr,
            "[firmware] using explicit firmware image %s, SHA-1 %s\n",
            cli_firmware_path.c_str(),
            gba::sha1(fw.data(), fw.size()).hex().c_str());
    }
    if (!ok) { std::fprintf(stderr, "refusing to start: dump verification failed\n"); return 1; }
    if (!cli_firmware_state_path.empty()) {
        std::string state_error;
        const NdsFirmwareStateLoadResult state_result =
            nds_firmware_state_load_or_seed(
                cli_firmware_state_path, fw, &fw, &state_error);
        if (state_result == NdsFirmwareStateLoadResult::Error) {
            std::fprintf(stderr,
                "refusing to start: firmware state %s: %s\n",
                cli_firmware_state_path.c_str(), state_error.c_str());
            return 1;
        }
        std::fprintf(stderr, "[firmware] %s mutable state %s (%zu bytes)\n",
                     state_result == NdsFirmwareStateLoadResult::Loaded
                         ? "loaded" : "seeded",
                     cli_firmware_state_path.c_str(), fw.size());
    }
    if (!rom_path.empty()) {
        if (rom.size() < 0x200u) {
            std::fprintf(stderr, "refusing to start: cartridge image is missing or truncated\n");
            return 1;
        }
        rom_sha1 = gba::sha1(rom.data(), rom.size()).hex();
        std::fprintf(stderr, "[load] cartridge: %zu bytes, SHA-1 %s\n",
                     rom.size(), rom_sha1.c_str());
        if (!frontend_options.expected_rom_sha1.empty() &&
            rom_sha1 != frontend_options.expected_rom_sha1) {
            std::fprintf(stderr,
                         "refusing to start: game config expects ROM SHA-1 "
                         "%s, got %s\n",
                         frontend_options.expected_rom_sha1.c_str(),
                         rom_sha1.c_str());
            return 1;
        }
    }
    std::string save_path;
    if (!rom.empty() && !save_disabled) {
        if (!cli_save_path.empty()) {
            save_path = cli_save_path;
        } else if (interactive) {
            std::filesystem::path derived(rom_path);
            derived.replace_extension(".sav");
            save_path = derived.string();
        }
    }
    const std::string rom_name =
        rom_path.empty()
            ? std::string()
            : std::filesystem::path(rom_path).filename().string();
    nds_diagnostics_set_identity(rom_sha1.c_str(), rom_name.c_str(),
                                 NDS_RUNNER_BUILD_ID);
    std::string rom_game_code;
    uint32_t rom_revision = 0;
    if (rom.size() >= 0x20) {
        for (size_t i = 0; i < 4; ++i) {
            const uint8_t ch = rom[0x0C + i];
            rom_game_code.push_back(
                ch >= 0x20 && ch <= 0x7E ? static_cast<char>(ch) : '?');
        }
        rom_revision = rom[0x1E];
    }
    nds_diagnostics_set_rom_header(
        rom_game_code.c_str(), rom_revision,
        static_cast<uint64_t>(rom.size()));
    // beads-yjp.28: the coverage manifest a player can hand back. Derived
    // rather than required, because the whole point is that a stock launch
    // with no flags produces one. Prefer the save's directory (the launcher
    // keeps it next to the ROM), then the ROM's, then the working directory.
    std::string coverage_manifest_path;
    if (!coverage_manifest_disabled) {
        if (!cli_coverage_manifest.empty()) {
            coverage_manifest_path = cli_coverage_manifest;
        } else {
            std::filesystem::path base;
            if (!nds_diagnostics_directory().empty()) {
                const std::filesystem::path rom_file(rom_path);
                const std::string stem = rom_file.stem().empty()
                    ? std::string("nds")
                    : rom_file.stem().string();
                base = std::filesystem::path(
                    nds_diagnostics_directory()) / stem;
            } else if (!save_path.empty()) {
                base = std::filesystem::path(save_path);
            } else if (!rom_path.empty()) {
                base = std::filesystem::path(rom_path);
            }
            // Parts are named <base>-coverage-<runstamp>-partNN.json. A fixed
            // filename overwrote the previous session's manifest, so anyone
            // playing twice silently lost the first run; and once the page
            // store filled, everything after was dropped. Rotation fixes both.
            base.replace_extension();
            coverage_manifest_set_output(
                base.empty() ? "nds" : base.string().c_str());
        }
        coverage_manifest_set_identity(rom_sha1.c_str(), rom_name.c_str(),
                                       NDS_RUNNER_BUILD_ID);
    }
    // Written from every exit path below. Failure is reported and never
    // changes the process's exit status: losing a diagnostic dump must not
    // turn a good run into a failed one for the player.
    auto write_coverage_manifest = [&]() {
        if (coverage_manifest_disabled) return;
        char error[256] = {};
        // An explicit --coverage-manifest path is honoured verbatim (scripts
        // depend on knowing the filename). Otherwise flush the final rotating
        // part; earlier parts were already written when the store filled.
        if (!coverage_manifest_path.empty()) {
            if (coverage_manifest_write(coverage_manifest_path.c_str(), error,
                                        sizeof(error))) {
                const CoveragePageStats pages = coverage_page_stats();
                std::fprintf(stderr,
                             "[coverage] wrote %s (%llu code pages, %llu bytes"
                             "%s)\n",
                             coverage_manifest_path.c_str(),
                             (unsigned long long)pages.captured,
                             (unsigned long long)pages.bytes,
                             pages.dropped ? ", STORE CAP HIT - manifest is "
                                             "incomplete" : "");
            } else {
                std::fprintf(stderr, "[coverage] could not write %s: %s\n",
                             coverage_manifest_path.c_str(), error);
            }
            return;
        }
        if (!coverage_manifest_flush_part(error, sizeof(error)))
            std::fprintf(stderr, "[coverage] could not write manifest: %s\n",
                         error);
    };
    nds_io_set_cartridge_save_path(save_path.c_str());
    nds_io_set_firmware_save_path(cli_firmware_state_path.c_str());
    nds_io_configure_cartridge_save(frontend_options.cartridge_save);
    if (!rom.empty()) {
        const char* type = "none";
        switch (frontend_options.cartridge_save.type) {
            case NdsCartridgeSaveType::EepromTiny:
                type = "eeprom-tiny";
                break;
            case NdsCartridgeSaveType::Eeprom:
                type = "eeprom";
                break;
            case NdsCartridgeSaveType::Flash:
                type = "flash";
                break;
            case NdsCartridgeSaveType::None:
                break;
        }
        std::fprintf(stderr, "[save] cartridge backup: %s, %u bytes\n",
                     type, frontend_options.cartridge_save.size);
    }
    if (!rom.empty() && save_path.empty())
        std::fprintf(stderr, "[save] battery persistence disabled\n");
    if (interactive && cli_live_overlay_enable &&
        !cli_live_overlay_activation_delay_set) {
        cli_live_overlay_activation_delay_ms = 90000u;
    }
    if (interactive && cli_live_overlay_auto &&
        !cli_live_overlay_auto_delay_set) {
        cli_live_overlay_auto_delay_ms = cli_live_overlay_activation_delay_ms;
    }
    if (!cli_live_overlay_auto_cooldown_set &&
        cli_live_overlay_auto_cooldown_ms == 0u) {
        cli_live_overlay_auto_cooldown_ms = 60000u;
    }
    live_overlay_configure(cli_live_overlay_enable, cli_live_overlay_auto,
                           cli_live_overlay_activation_delay_ms,
                           cli_live_overlay_auto_delay_ms,
                           cli_live_overlay_auto_cooldown_ms,
                           cli_live_overlay_command.c_str(),
                           cli_live_overlay_cache.c_str(),
                           rom_sha1.c_str());
    mph_mouse_aim_policy =
        rom_sha1 == "90164d1ac127ee5f9815ea4ae7de798c7b5fc629" &&
        frontend_options.relative_mouse_touch;
    frontend_options.relative_mouse_direct_aim = mph_mouse_aim_policy;
    frontend_options.mph_prime_controls =
        frontend_options.mph_prime_controls && mph_mouse_aim_policy;
#ifdef NDS_HAVE_SM64DS_BANKS
    // Static title banks and host-side enhancements are valid only for the
    // exact image they were generated and audited against. In particular,
    // many DS titles share the standard ARM7 load address; registering one
    // title's unguarded immutable bank for another cartridge corrupts guest
    // execution before the runtime can fall back to Tier 3.
    sm64ds_title =
        rom_sha1 == "1367529f2cb23e76ef295cb1727333ae8f0a6cd7";
    if (sm64ds_title) {
        frontend_options.adaptive_supported = NDS_ADAPTIVE_TOP;
        frontend_options.adaptive_max_width[0] = 448;  // 21:9 at 192 high
        sm64ds_wide_policy = true;
    }
#endif
    if (interactive &&
        (frontend_options.adaptive_screens &
         ~frontend_options.adaptive_supported) != 0u) {
        std::fprintf(stderr,
            "adaptive widescreen '%s' is unsupported by this title "
            "(supported: %s); refusing to stretch the native framebuffer\n",
            nds_adaptive_screens_name(frontend_options.adaptive_screens),
            nds_adaptive_screens_name(frontend_options.adaptive_supported));
        return 2;
    }
    bool adaptive_sky_repair = true;
    if (const char* value = std::getenv("NDS_ADAPTIVE_SKY_REPAIR")) {
        if (value[0] == '0' && value[1] == '\0')
            adaptive_sky_repair = false;
        else if (!(value[0] == '1' && value[1] == '\0')) {
            std::fprintf(stderr,
                "invalid NDS_ADAPTIVE_SKY_REPAIR "
                "(expected 0 or 1)\n");
            return 2;
        }
    }
    bool adaptive_guest_culling = frontend_options.adaptive_guest_culling;
    if (const char* value = std::getenv("NDS_ADAPTIVE_GUEST_CULLING");
        value && !nds_parse_on_off(value, &adaptive_guest_culling)) {
        std::fprintf(stderr,
                     "invalid NDS_ADAPTIVE_GUEST_CULLING "
                     "(expected on or off)\n");
        return 2;
    }
    nds_gpu2d_set_adaptive_skybox_fill(
        (frontend_options.adaptive_skybox_fill || sm64ds_wide_policy) &&
        adaptive_sky_repair &&
        (frontend_options.adaptive_screens & NDS_ADAPTIVE_TOP) != 0u);
    nds_gpu2d_set_adaptive_hud_anchor(
        frontend_options.adaptive_hud_anchor &&
        (frontend_options.adaptive_screens & NDS_ADAPTIVE_TOP) != 0u,
        frontend_options.adaptive_hud_center_width);
    nds_gpu2d_set_adaptive_center_native(
        frontend_options.adaptive_center_native &&
        (frontend_options.adaptive_screens & NDS_ADAPTIVE_TOP) != 0u);
    nds_gpu2d_set_adaptive_center_max_polygons(
        (frontend_options.adaptive_screens & NDS_ADAPTIVE_TOP) != 0u
            ? frontend_options.adaptive_center_max_polygons
            : 0u);
    nds_title_patches_set_sm64ds_adaptive(
        sm64ds_wide_policy &&
        (frontend_options.adaptive_screens & NDS_ADAPTIVE_TOP) != 0u);
    nds_title_patches_set_mph_mouse_aim(mph_mouse_aim_policy);
    nds_title_patches_set_mph_adventure_wide(
        rom_sha1 == "90164d1ac127ee5f9815ea4ae7de798c7b5fc629" &&
            adaptive_guest_culling &&
            (frontend_options.adaptive_screens & NDS_ADAPTIVE_TOP) != 0u,
        frontend_options.adaptive_max_width[0]);
    nds_title_patches_set_mph_adaptive(
        rom_sha1 == "90164d1ac127ee5f9815ea4ae7de798c7b5fc629" &&
        (frontend_options.adaptive_screens & NDS_ADAPTIVE_TOP) != 0u);
    if (!nds_normalize_touch_calibration(fw)) {
        std::fprintf(stderr, "refusing to start: malformed firmware user-settings layout\n");
        return 1;
    }
    if (!nds_apply_startup_mode(fw, frontend_options.startup_mode)) {
        std::fprintf(stderr,
                     "refusing to start: cannot apply firmware startup mode\n");
        return 1;
    }
    std::fprintf(stderr, "[firmware] startup mode: %s\n",
                 nds_startup_mode_name(frontend_options.startup_mode));
    // Console nickname (beads-yjp.16): the name games surface as the
    // player's default and that WFC/Wiimmfi shows to peers. Same
    // in-memory, both-copies, CRC-resealed mechanism as the two patches
    // above, applied at the same point, so it works identically for a
    // retail dump and for a generated image. Empty = untouched.
    if (!nds_apply_player_name(fw, frontend_options.player_name)) {
        std::fprintf(stderr,
            "refusing to start: cannot apply player name (malformed "
            "firmware user-settings layout)\n");
        return 1;
    }
    if (!frontend_options.player_name.empty()) {
        std::fprintf(stderr,
            "[firmware] player name: \"%s\" written to both user-settings "
            "copies (in-memory only; the dump on disk is untouched)\n",
            frontend_options.player_name.c_str());
    }
    if (!nds_apply_instance_mac(fw, frontend_options.instance_index)) {
        std::fprintf(stderr,
            "refusing to start: cannot apply per-instance guest MAC "
            "(malformed firmware Wi-Fi calibration block)\n");
        return 1;
    }
    if (frontend_options.instance_index != 0) {
        std::fprintf(stderr,
            "[firmware] instance index %u: perturbed guest MAC (bytes "
            "3/4/5) for multi-instance Wi-Fi identity\n",
            frontend_options.instance_index);
    } else if (frontend_options.generated_firmware) {
        std::fprintf(stderr,
            "[firmware] instance index 0: guest MAC is the per-install "
            "identity baked into the generated image\n");
    } else {
        std::fprintf(stderr,
            "[firmware] instance index 0: guest MAC left untouched "
            "(LLE-faithful, reads real firmware dump over SPI)\n");
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
        live_overlay_runtime_reset();
        runtime_trace_reset();
        net_ring_reset();

        // Both BIOS backends are linked; the one matching the loaded (and
        // hash-verified) images is registered — the psxrecomp runtime-
        // selection model, never a silent substitution.
        if (frontend_options.freebios) {
            nds_register_dispatch(NDS_ARM9, g_dispatch_freebios_arm9,
                                  g_dispatch_freebios_arm9_len, 0xFFFF0000u);
            nds_register_dispatch(NDS_ARM7, g_dispatch_freebios_arm7,
                                  g_dispatch_freebios_arm7_len, 0x00000000u);
        } else {
            nds_register_dispatch(NDS_ARM9, g_dispatch_arm9_bios,
                                  g_dispatch_arm9_bios_len, 0xFFFF0000u);
            nds_register_dispatch(NDS_ARM7, g_dispatch_arm7_bios,
                                  g_dispatch_arm7_bios_len, 0x00000000u);
        }
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
        if (nds_register_configured_title_banks(rom_sha1.c_str())) {
            std::fprintf(stderr,
                         "[dispatch] registered configured title banks for %s\n",
                         rom_sha1.c_str());
        }
        live_overlay_register_cached_banks();
#ifdef NDS_HAVE_SM64DS_BANKS
        if (sm64ds_title) {
            nds_register_dispatch(NDS_ARM9, g_dispatch_sm64ds_arm9,
                                  g_dispatch_sm64ds_arm9_len, 0xFFFF0000u);
#if defined(NDS_PROFILE_HLE_HEAT)
            nds_register_hle_profile_descriptors(
                NDS_ARM9, g_hle_profile_sm64ds_arm9,
                g_hle_profile_sm64ds_arm9_len);
#endif
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
            // A later gameplay capture carries different overlay generations
            // at many of the same virtual addresses. Keep it in a separate
            // content-validated bank: boot/title bytes above win when present,
            // then this generation becomes eligible after the guest swaps it.
            nds_register_dispatch(NDS_ARM9,
                                  g_dispatch_sm64ds_arm9_ram_gameplay,
                                  g_dispatch_sm64ds_arm9_ram_gameplay_len,
                                  0xFFFF0000u);
#endif
        }
#endif

        // Reset both cores: SVC mode, IRQ+FIQ masked, ARM state, reset vector.
        const uint32_t reset_cpsr = 0x13u | CPSR_I_BIT | CPSR_F_BIT;
        scheduler_init();
        scheduler_reset_cpu(0, 0xFFFF0000u, reset_cpsr);  // ARM9
        scheduler_reset_cpu(1, 0x00000000u, reset_cpsr);  // ARM7

        // Opt-in direct boot (beads-yjp.15): establish the post-firmware
        // machine state and start the cartridge binaries immediately. Any
        // failure is a refusal, never a fallback to another boot path.
        if (frontend_options.boot_mode == NdsBootMode::Direct) {
            if (rom.empty()) {
                std::fprintf(stderr,
                             "refusing to boot: --boot direct requires --rom\n");
                std::exit(1);
            }
            const uint32_t arm9_rom_offset =
                rom.size() >= 0x24u
                    ? (uint32_t{rom[0x20]} | (uint32_t{rom[0x21]} << 8) |
                       (uint32_t{rom[0x22]} << 16) | (uint32_t{rom[0x23]} << 24))
                    : 0u;
            std::vector<uint8_t> secure_area(0x800);
            const bool has_secure_area =
                arm9_rom_offset >= 0x4000u && arm9_rom_offset < 0x8000u;
            if (has_secure_area &&
                !nds_cart_secure_area_plaintext(secure_area.data())) {
                std::fprintf(stderr,
                             "refusing to boot: cartridge secure area could "
                             "not be decrypted for direct boot\n");
                std::exit(1);
            }
            NdsDirectBootInputs inputs;
            inputs.rom = rom.data();
            inputs.rom_size = static_cast<uint32_t>(rom.size());
            inputs.secure_area =
                has_secure_area ? secure_area.data() : nullptr;
            inputs.firmware = fw.data();
            inputs.firmware_size = static_cast<uint32_t>(fw.size());
            inputs.cart_chip_id = nds_cart_chip_id();
            // FreeBIOS has no Nintendo logo, so direct boot copies the
            // cartridge's into the ARM9 BIOS image (DS<->GBA comm reads it).
            inputs.arm9_bios_is_native = !frontend_options.freebios;
            RunnerDirectBootMachine machine;
            std::string boot_error;
            if (!nds_direct_boot(machine, inputs, &boot_error)) {
                std::fprintf(stderr, "refusing to boot: direct boot: %s\n",
                             boot_error.c_str());
                std::exit(1);
            }
            std::fprintf(stderr, "[boot] direct boot (opt-in): cartridge "
                                 "entry state established, firmware not "
                                 "executed\n");
        }
        nds_gpu3d_set_threaded(gpu3d_threaded);
    };
    boot();

#if defined(NDS_HAVE_COMPUTE_RENDERER)
    // Interactive mode creates the hidden context only after its visible SDL
    // allocations succeed. Headless/serve modes own it here so forced
    // compute selection is observable by the parity and perf harnesses.
    //
    // The internal-resolution scale must be established before the renderer
    // exists in this path too. Headless runs present nothing, so the scale is
    // pure cost here -- but it is exactly how the parity harness proves that
    // the native surface is scale-invariant, so it must be honoured, not
    // quietly dropped.
    if (!interactive &&
        !nds_gpu3d_set_internal_scale(
            frontend_options.internal_resolution)) {
        std::fprintf(stderr,
                     "internal resolution %u is unavailable\n",
                     static_cast<unsigned>(
                         frontend_options.internal_resolution));
        return 1;
    }
    if (!interactive)
        nds_texture_upscale_set_factor(frontend_options.texture_upscale);
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
        const int rc = nds_run_interactive_frontend(frontend_options);
        debug_pump_stop();
        const bool save_ok = nds_io_flush_cartridge_save();
        const bool firmware_ok = nds_io_flush_firmware_save();
        write_coverage_manifest();
        return rc != 0 ? rc : (save_ok && firmware_ok) ? 0 : 1;
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
        dump_replay_status();
        const bool save_ok = nds_io_flush_cartridge_save();
        const bool firmware_ok = nds_io_flush_firmware_save();
#if defined(NDS_HAVE_COMPUTE_RENDERER)
        const bool compute_failed = nds_gpu3d_compute_runtime_failed();
        nds_compute_host_stop();
#else
        const bool compute_failed = false;
#endif
        write_coverage_manifest();
        return (compute_failed || !save_ok || !firmware_ok) ? 1 : 0;
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
    if (net_ring_dump) {
        std::fprintf(stderr, "\n== network event ring (Wiimmfi M0) ==\n");
        net_ring_dump_recent(static_cast<uint32_t>(
            std::min<uint64_t>(net_ring_last, UINT32_MAX)), net_ring_filter_kind);
    }
    dump_replay_status();
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    const bool compute_failed = nds_gpu3d_compute_runtime_failed();
    nds_compute_host_stop();
#else
    const bool compute_failed = false;
#endif
    const bool save_ok = nds_io_flush_cartridge_save();
    const bool firmware_ok = nds_io_flush_firmware_save();
    write_coverage_manifest();
    return (compute_failed || !save_ok || !firmware_ok) ? 1 : 0;
}
