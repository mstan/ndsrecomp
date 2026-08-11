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
#include "runtime_arm.h"
#include "io.h"
#include "debug_server.h"
#include "frontend.h"
#include "gpu2d.h"
#include "gpu3d.h"
#include "net/net_ring.h"
#include "net/net_capture.h"
#include "net/wfc_provider.h"
#include "wifi_net.h"
#include "profile_report.h"
#include "sha1.h"
#include "title_banks.h"
#include "title_patches.h"
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

bool apply_startup_mode(std::vector<uint8_t>& fw, NdsStartupMode mode) {
    if (mode == NdsStartupMode::Preserve) return true;
    if (fw.size() < 0x22u) return false;
    const size_t user = size_t{fw[0x20]} << 3u |
                        size_t{fw[0x21]} << 11u;
    if (user + 0x200u > fw.size()) return false;

    auto get16 = [&](size_t off) {
        return static_cast<uint16_t>(
            uint16_t{fw[off]} | (uint16_t{fw[off + 1u]} << 8u));
    };
    auto put16 = [&](size_t off, uint16_t value) {
        fw[off] = static_cast<uint8_t>(value);
        fw[off + 1u] = static_cast<uint8_t>(value >> 8u);
    };
    for (unsigned copy = 0; copy < 2; ++copy) {
        const size_t base = user + size_t{copy} * 0x100u;
        uint16_t language_and_flags = get16(base + 0x64u);
        if (mode == NdsStartupMode::Automatic)
            language_and_flags |= 1u << 6u;
        else
            language_and_flags &= ~(1u << 6u);
        put16(base + 0x64u, language_and_flags);
        put16(base + 0x72u,
              firmware_crc16(fw.data() + base, 0x70u, 0xFFFFu));
    }
    return true;
}

// Wiimmfi (beads-yjp.1.11): two ndsrecomp instances booted from the SAME
// firmware dump present the SAME console MAC, so Wiimmfi identity and DS
// friend codes -- both derived from it -- collide: two such clients look
// like one console appearing twice and cannot match with each other
// online. This mirrors melonDS's own proven fix for exactly this problem
// (multi-instance local testing) byte-for-byte -- see
// ndsref/third_party/melonDS/src/frontend/qt_sdl/EmuInstance.cpp:1739-1752
// -- rather than inventing a new perturbation scheme: add the instance
// index into MAC bytes 3/4/5 (melonDS's own wrap-mod-256 u8 arithmetic,
// reproduced here with explicit truncating casts), then mask byte 0 so
// the result can never be a broadcast/multicast address.
//
// Instance 0 is a deliberate, total no-op (early return): the owner
// chose LLE-faithful as the default, so the guest reads its REAL MAC off
// the real firmware dump over its own ordinary SPI path, unperturbed,
// exactly like a physical console. Only a nonzero --instance-index
// perturbs anything.
//
// This is an in-memory FIRMWARE patch, applied to this process's private
// copy of `fw` only -- never a ROM patch (no cartridge byte is ever
// touched) and never guest interception (no HLE, no faking a register
// read/IPC value the guest "expects"; the guest still reads the MAC the
// only way real hardware does, via SPI -- see nds_wifi_load_firmware's
// SPI.SetFirmwareSource binding). Same category of change as
// normalize_touch_calibration/apply_startup_mode above, and applied at
// the same call site, for the same reason: the SHA-1 dump verification
// above already ran against the pristine on-disk bytes before any of
// these three patches touch `fw`, so patching here can never desync from
// that check.
//
// The MAC lives inside the firmware HEADER's Wi-Fi calibration block
// (GBATek "DS Firmware Header" / melonDS's SPI_Firmware.h
// FirmwareHeader::MacAddr), a completely different region with a
// DIFFERENT checksum than the per-boot user-settings block the other two
// patches touch (UserData::Checksum, CRC16 start 0xFFFF over 0x70 bytes).
// The header's own WifiConfigChecksum (offset 0x2A) instead covers
// WifiConfigLength bytes (a value stored IN the header, read here rather
// than hardcoded -- default firmware ships 0x138) starting at 0x2C (the
// length field itself is inside its own checksummed range), CRC16 start
// 0x0000 -- confirmed against melonDS's own verification call,
// SPI.cpp:99: `VerifyCRC16(0x0000, 0x2C, *(u16*)&Buffer[0x2C], 0x2A)`.
// firmware_crc16() above is byte-for-byte the same polynomial table and
// bit loop as melonDS's SPI.cpp CRC16(), so it is reused here rather than
// duplicated a third time.
bool apply_instance_mac(std::vector<uint8_t>& fw, uint32_t instance_index) {
    if (instance_index == 0) return true;  // instance 0: untouched, LLE-faithful
    constexpr size_t kMacOffset = 0x36u;
    constexpr size_t kWifiConfigChecksumOffset = 0x2Au;
    constexpr size_t kWifiConfigLenOffset = 0x2Cu;
    if (fw.size() < kWifiConfigLenOffset + 2u) return false;
    const uint16_t wifi_config_length = static_cast<uint16_t>(
        uint16_t{fw[kWifiConfigLenOffset]} |
        (uint16_t{fw[kWifiConfigLenOffset + 1u]} << 8u));
    if (kWifiConfigLenOffset + wifi_config_length > fw.size()) return false;
    if (kMacOffset + 6u > kWifiConfigLenOffset + wifi_config_length)
        return false;  // MacAddr must fall inside the checksummed region

    uint8_t mac[6];
    std::memcpy(mac, fw.data() + kMacOffset, 6u);
    // Exact melonDS perturbation (EmuInstance.cpp:1739-1752): u8 arithmetic
    // wraps mod 256 on both sides, reproduced here with explicit
    // truncating casts since these are plain uint8_t, not melonDS's
    // MacAddress element type with the same underlying width.
    mac[3] = static_cast<uint8_t>(mac[3] + instance_index);
    mac[4] = static_cast<uint8_t>(mac[4] + instance_index * 0x44u);
    mac[5] = static_cast<uint8_t>(mac[5] + instance_index * 0x10u);
    mac[0] &= 0xFCu;  // never a broadcast/multicast address
    std::memcpy(fw.data() + kMacOffset, mac, 6u);

    const uint16_t crc = firmware_crc16(
        fw.data() + kWifiConfigLenOffset, wifi_config_length, 0u);
    fw[kWifiConfigChecksumOffset] = static_cast<uint8_t>(crc);
    fw[kWifiConfigChecksumOffset + 1u] = static_cast<uint8_t>(crc >> 8u);
    return true;
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

    std::string dir = "bios";
    std::string rom_path;
    std::string config_path = "game.toml";
    std::string cli_screen_layout;
    std::string cli_adaptive_screens;
    std::string cli_supersampling;
    std::string cli_antialiasing;
    std::string cli_relative_mouse_touch;
    std::string cli_relative_mouse_sensitivity;
    std::string cli_relative_mouse_invert_y;
    std::string cli_relative_mouse_fire_key;
    std::string cli_startup_mode;
    std::string cli_instance_index;
    std::string cli_save_path;
    std::string cli_net_ring_filter;
    std::string cli_network_enabled;
    std::string cli_network_backend;
    std::string cli_wfc_enabled;
    std::string cli_wfc_provider;
    // Wiimmfi M8: capture/replay at the Ethernet backend boundary.
    std::string cli_net_capture_out;
    std::string cli_net_capture_in;
    bool cli_net_capture_raw = false;
    bool cli_net_capture_no_pcap = false;
    std::string cli_net_capture_scenario;
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
        } else if (a == "--no-save") {
            save_disabled = true;
        } else if (a == "--config" && i + 1 < argc) {
            config_path = argv[++i];
            config_explicit = true;
        } else if (a == "--screen-layout" && i + 1 < argc) {
            cli_screen_layout = argv[++i];
        } else if (a == "--adaptive-widescreen" && i + 1 < argc) {
            cli_adaptive_screens = argv[++i];
        } else if (a == "--supersampling" && i + 1 < argc) {
            cli_supersampling = argv[++i];
        } else if (a == "--antialiasing" && i + 1 < argc) {
            cli_antialiasing = argv[++i];
        } else if (a == "--relative-mouse-touch" && i + 1 < argc) {
            cli_relative_mouse_touch = argv[++i];
        } else if (a == "--relative-mouse-sensitivity" && i + 1 < argc) {
            cli_relative_mouse_sensitivity = argv[++i];
        } else if (a == "--relative-mouse-invert-y" && i + 1 < argc) {
            cli_relative_mouse_invert_y = argv[++i];
        } else if (a == "--relative-mouse-fire-key" && i + 1 < argc) {
            cli_relative_mouse_fire_key = argv[++i];
        } else if (a == "--startup-mode" && i + 1 < argc) {
            cli_startup_mode = argv[++i];
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
        } else if (a == "--wfc" && i + 1 < argc) {
            cli_wfc_enabled = argv[++i];
        } else if (a == "--wfc-provider" && i + 1 < argc) {
            cli_wfc_provider = argv[++i];
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
        } else if (a == "--help" || a == "-h") {
            std::fprintf(stderr,
                "usage: %s [bios-dir] [cycle-budget] [--rom game.nds] "
                "[--serve|--interactive] [--port 19842] "
                "[--save-path game.sav|--no-save] "
                "[--config game.toml] "
                "[--screen-layout stacked|separate] "
                "[--adaptive-widescreen none|top|bottom|both] "
                "[--supersampling 1|2|3|4] "
                "[--antialiasing 0|2|4|8] "
                "[--relative-mouse-touch on|off] "
                "[--relative-mouse-sensitivity 10..400] "
                "[--relative-mouse-invert-y on|off] "
                "[--relative-mouse-fire-key none|a|b|l|r|x|y] "
                "[--startup-mode preserve|manual|automatic] "
                "[--instance-index 0..255] "
                "[--discover-static-misses] [--rtc-host] "
                "[--net-ring-dump] [--net-ring-last N] "
                "[--net-ring-filter <class>|all] "
                "[--network on|off] [--network-backend slirp|replay|pcap] "
                "[--wfc on|off] [--wfc-provider kaeru|wiimmfi|<ipv4>] "
                "[--net-capture-out FILE] [--net-capture-in FILE] "
                "[--net-capture-raw] [--net-capture-no-pcap] "
                "[--net-capture-scenario NAME]\n",
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
    if (const char* value = std::getenv("NDS_SUPERSAMPLING")) {
        if (!nds_parse_supersampling(value,
                                     &frontend_options.supersampling)) {
            std::fprintf(stderr,
                         "invalid NDS_SUPERSAMPLING (expected 1..4)\n");
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
    if (const char* value = std::getenv("NDS_STARTUP_MODE")) {
        if (!nds_parse_startup_mode(value,
                                    &frontend_options.startup_mode)) {
            std::fprintf(stderr,
                         "invalid NDS_STARTUP_MODE "
                         "(expected preserve, manual, or automatic)\n");
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
    if (!cli_supersampling.empty() &&
        !nds_parse_supersampling(cli_supersampling,
                                 &frontend_options.supersampling)) {
        std::fprintf(stderr,
                     "invalid --supersampling (expected 1..4)\n");
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
    if (!cli_relative_mouse_touch.empty() &&
        !nds_parse_on_off(cli_relative_mouse_touch,
                          &frontend_options.relative_mouse_touch)) {
        std::fprintf(stderr,
                     "invalid --relative-mouse-touch (expected on or off)\n");
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
    if (!cli_startup_mode.empty() &&
        !nds_parse_startup_mode(cli_startup_mode,
                                &frontend_options.startup_mode)) {
        std::fprintf(stderr,
                     "invalid --startup-mode "
                     "(expected preserve, manual, or automatic)\n");
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
    if (frontend_options.relative_mouse_touch &&
        frontend_options.screen_layout != NdsScreenLayout::Separate) {
        std::fprintf(stderr,
                     "relative mouse touch requires --screen-layout separate\n");
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
    if (!cli_wfc_enabled.empty() &&
        !nds_parse_on_off(cli_wfc_enabled, &frontend_options.network.wfc_enabled)) {
        std::fprintf(stderr, "invalid --wfc (expected on or off)\n");
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
                "[kaeru, wiimmfi] or a dotted-quad IPv4 address)\n",
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

    // Resolve the network config to backend-ready numeric form. Provider
    // name -> table lookup, optional dns_server string -> IPv4, matching
    // the pipeline documented in wifi_net.h's NdsWifiNetworkConfig.
    NdsWifiNetworkConfig resolved_network{};
    resolved_network.enabled = frontend_options.network.enabled;
    resolved_network.wfc_enabled = frontend_options.network.wfc_enabled;

    // Wiimmfi M8: "replay" is now fully wired into nds_wifi3d_attach()
    // alongside the pre-existing "slirp"; "pcap" is accepted by
    // nds_parse_network_backend (frontend_config.cpp) but is deliberately
    // left NOT wired into the bridge -- see docs/adr-melonds-wifi-
    // vendoring.md §2 and docs/m8-capture-replay-design.md's "unresolved"
    // list. This is the same shape of gate the pre-existing code already
    // had for "pcap", just extended with a real "replay" branch instead of
    // rejecting it too.
    if (frontend_options.network.backend == "slirp") {
        resolved_network.backend = NdsNetBackendKind::Slirp;
    } else if (frontend_options.network.backend == "replay") {
        resolved_network.backend = NdsNetBackendKind::Replay;
    } else {
        std::fprintf(stderr,
            "--network-backend %s is not wired into the bridge yet -- only "
            "\"slirp\" and \"replay\" are currently constructed by "
            "nds_wifi3d_attach()\n",
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
                    "(expected kaeru or wiimmfi) and no dns_server override "
                    "was given\n", wfc.name.c_str());
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

    auto a9 = read_file(dir + "/biosnds9.rom");
    auto a7 = read_file(dir + "/biosnds7.rom");
    auto fw = read_file(dir + "/firmware.bin");
    auto rom = rom_path.empty() ? std::vector<uint8_t>{} : read_file(rom_path);
    std::string rom_sha1;
    bool sm64ds_wide_policy = false;
    bool mph_mouse_aim_policy = false;
#ifdef NDS_HAVE_SM64DS_BANKS
    bool sm64ds_title = false;
#endif
    bool ok = verify(a9, "bfaac75f101c135e32e2aaf541de6b1be4c8c62d", "arm9 bios")
            & verify(a7, "24f67bdea115a2c847c8813a262502ee1607b7df", "arm7 bios")
            & verify(fw, "ae22de59fbf3f35ccfbeacaeba6fa87ac5e7b14b", "firmware");
    if (!ok) { std::fprintf(stderr, "refusing to start: dump verification failed\n"); return 1; }
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
    nds_io_set_cartridge_save_path(save_path.c_str());
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
    mph_mouse_aim_policy =
        rom_sha1 == "90164d1ac127ee5f9815ea4ae7de798c7b5fc629" &&
        frontend_options.relative_mouse_touch;
    frontend_options.relative_mouse_direct_aim = mph_mouse_aim_policy;
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
    nds_gpu2d_set_adaptive_skybox_fill(
        (frontend_options.adaptive_skybox_fill || sm64ds_wide_policy) &&
        adaptive_sky_repair &&
        (frontend_options.adaptive_screens & NDS_ADAPTIVE_TOP) != 0u);
    nds_gpu2d_set_adaptive_hud_anchor(
        frontend_options.adaptive_hud_anchor &&
        (frontend_options.adaptive_screens & NDS_ADAPTIVE_TOP) != 0u,
        frontend_options.adaptive_hud_center_width);
    nds_title_patches_set_sm64ds_adaptive(
        sm64ds_wide_policy &&
        (frontend_options.adaptive_screens & NDS_ADAPTIVE_TOP) != 0u);
    nds_title_patches_set_mph_mouse_aim(mph_mouse_aim_policy);
    if (!normalize_touch_calibration(fw)) {
        std::fprintf(stderr, "refusing to start: malformed firmware user-settings layout\n");
        return 1;
    }
    if (!apply_startup_mode(fw, frontend_options.startup_mode)) {
        std::fprintf(stderr,
                     "refusing to start: cannot apply firmware startup mode\n");
        return 1;
    }
    std::fprintf(stderr, "[firmware] startup mode: %s\n",
                 nds_startup_mode_name(frontend_options.startup_mode));
    if (!apply_instance_mac(fw, frontend_options.instance_index)) {
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
        runtime_trace_reset();
        net_ring_reset();

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
        if (nds_register_configured_title_banks(rom_sha1.c_str())) {
            std::fprintf(stderr,
                         "[dispatch] registered configured title banks for %s\n",
                         rom_sha1.c_str());
        }
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
        const int rc = nds_run_interactive_frontend(frontend_options);
        debug_pump_stop();
        const bool save_ok = nds_io_flush_cartridge_save();
        return rc != 0 ? rc : save_ok ? 0 : 1;
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
#if defined(NDS_HAVE_COMPUTE_RENDERER)
        const bool compute_failed = nds_gpu3d_compute_runtime_failed();
        nds_compute_host_stop();
#else
        const bool compute_failed = false;
#endif
        return (compute_failed || !save_ok) ? 1 : 0;
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
    return (compute_failed || !save_ok) ? 1 : 0;
}
