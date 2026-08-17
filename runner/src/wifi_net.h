// wifi_net.h -- bridge for the vendored melonDS Wifi device model + net
// glue (runner/vendor/melonds/{Wifi,WifiAP}.cpp, net/{Net,Net_Slirp,
// PacketDispatcher}.cpp).
//
// STATUS (2026-08-10): this bridge is now the SOLE Wi-Fi device model on
// the live bus. runner/src/wifi.cpp (the hand-written model) has been
// retired -- see runner/src/wifi.h for the retained bus-facing API
// (nds_wifi_read/write/reset/load_firmware/set_power_control/
// next_event_time/run_events/address/debug_if/debug_ie), which is now
// implemented in wifi_net.cpp instead of wifi.cpp. bus.cpp, io.cpp, and
// scheduler.cpp did not need to change: they already called that exact
// C API by name.
//
// nds_wifi3d_attach() constructs (once, idempotently) the vendored
// melonDS::NDS + melonDS::Wifi + melonDS::Net/Net_Slirp instances, binds
// the SPI firmware view to the runner's own firmware buffer, and starts
// the host networking worker thread (see the design comment above
// WifiBridgeState in wifi_net.cpp). Every nds_wifi_* entry point in
// wifi.h calls it first (cheap no-op after the first call) so the device
// model is always ready before any bus access, register reset, or
// firmware load reaches it.
//
// STATUS (2026-08-10, updated): host socket polling (Net_Slirp's former
// RecvCheck() body, now PollHostSockets() -- see
// runner/vendor/melonds/patches/0005-net-slirp-worker-thread-poll.patch)
// and all guest->host packet sends run exclusively on that worker thread.
// The emulation thread only ever touches two bounded, mutex-protected
// packet queues; it never calls into libslirp or a host socket API
// directly. See wifi_net.cpp's design comment for the full rationale.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/net_capture.h"

namespace melonDS { class Wifi; }

// Resolved (numeric, backend-ready) network configuration -- the endpoint
// of the config pipeline that starts at game.toml's [network]/[network.wfc]
// tables and the --network/--network-backend/--wfc/--wfc-provider CLI
// flags (frontend.h's NdsNetworkOptions/NdsWfcProvider), resolved by
// main.cpp (provider name -> table lookup, dns_server string -> IPv4) so
// this header never needs to know about TOML, provider name tables, or
// string parsing. Must be set via nds_wifi_configure_network() BEFORE the
// first nds_wifi3d_attach() call (i.e. before nds_io_reset()) -- attach is
// idempotent and only reads this configuration once, at construction.
// Wiimmfi M8: which NetDriver implementation nds_wifi3d_attach() should
// construct. Slirp is the existing live-Internet backend (Net_Slirp,
// vendored); Replay is the new sibling implementation (net/net_replay.h)
// that replays a previously captured session with host networking fully
// disabled -- see docs/m8-capture-replay-design.md.
enum class NdsNetBackendKind : uint8_t {
    Slirp = 0,
    Replay = 1,
    Pcap = 2,
};

struct NdsWifiNetworkConfig {
    bool enabled = true;             // network.enabled
    // wfc_dns_ipv4 is host byte order, 0 = "no override" (upstream
    // melonDS's own local-getaddrinfo DNS synthesis via the internal fake
    // address -- see patches/0006-net-slirp-configurable-nameserver.patch).
    // Only meaningful/nonzero when wfc_enabled is true.
    bool wfc_enabled = false;
    uint32_t wfc_dns_ipv4 = 0;

    // ---- Wiimmfi M8: capture/replay at the Ethernet backend boundary ----
    NdsNetBackendKind backend = NdsNetBackendKind::Slirp;

    // backend == Replay only: the already-loaded, already-validated
    // capture to replay. Loaded and fully parsed by main.cpp's CLI
    // validation (NdsNetCaptureReader::ReadAll), so a corrupt/truncated
    // FILE is a startup-time CLI error, never a mid-run surprise here --
    // by the time this reaches nds_wifi3d_attach(), it is known-good
    // record data. Moved out of this vector by nds_wifi3d_attach(); left
    // empty afterward.
    std::vector<NdsNetCaptureRecord> replay_records;
    // Whether replay_records came from a capture whose header said
    // sanitized=1 (NdsNetCaptureReader::sanitized(), set by main.cpp).
    // Threaded into melonDS::NetReplay's constructor -- see its own doc
    // comment (net/net_replay.h) for why the comparison must sanitize the
    // live guest's bytes the same way before comparing when this is true.
    bool replay_sanitized = false;

    // If non-empty, every frame crossing Net_SendPacket/Net_RecvPacket is
    // ALSO appended to a capture file at this path -- independent of
    // `backend` (the natural use is backend==Slirp, recording a live
    // session; backend==Replay + capture_out is also valid, e.g. to
    // re-export a sanitized copy of an already-replayed session).
    std::string capture_out_path;
    bool capture_sanitize = true;    // default-safe; the only opt-out is
                                       // an explicit --net-capture-raw
    bool capture_write_pcap = true;  // Wireshark-openable sibling; default on
    std::string capture_scenario_tag;  // free-form label, e.g. "dhcp"
    std::string rom_sha1;               // capture header provenance field

    // backend == Pcap only: empty means choose the first usable non-loopback
    // adapter; otherwise match this exact device, friendly, or description
    // string from LibPCap::GetAdapters().
    std::string pcap_adapter;

    // backend == Slirp only: instance 0 preserves melonDS's default
    // 10.64.0.0/24 virtual LAN. Nonzero instances use 10.64.N.0/24 so
    // multiple local runners do not present identical guest LAN endpoints
    // to DWC/Wiimmfi NAT negotiation.
    uint32_t slirp_virtual_network_instance = 0;

    // Local wireless / Download Play / NiFi transport. Disabled by default;
    // when enabled, each runner process binds localhost UDP port
    // local_wireless_base_port + instance_index and fans MP frames out to
    // the other local instance slots.
    bool local_wireless_enabled = false;
    uint32_t local_wireless_instance = 0;
    uint16_t local_wireless_base_port = 26710;
};

// Wiimmfi M8: query surface for a --network-backend replay run's outcome.
// `active` is false whenever the resolved backend isn't Replay (or no
// backend was ever attached at all) -- every other field is meaningless in
// that case. See net/net_replay.h's NdsNetReplayMismatch for what each
// mismatch_* field means; they are a direct copy of that struct's fields,
// kept as a separate POD here so wifi_net.h itself never needs to expose
// melonDS::NetReplay's type to callers outside the bridge (main.cpp).
struct NdsNetReplayStatus {
    bool active = false;
    bool mismatch = false;
    uint64_t tx_matched = 0;
    uint64_t tx_total = 0;
    uint64_t rx_delivered = 0;
    uint64_t rx_total = 0;
    uint64_t mismatch_tx_frame_index = 0;
    uint64_t mismatch_guest_cycle = 0;
    uint32_t mismatch_arm9_pc = 0;
    uint32_t mismatch_arm7_pc = 0;
    std::string mismatch_reason;
};

struct NdsWifiNetworkState {
    bool attached = false;
    bool network_enabled = false;
    bool live_backend_active = false;
    bool replay_backend_active = false;
    bool worker_active = false;
    NdsNetBackendKind backend = NdsNetBackendKind::Slirp;
    bool wfc_enabled = false;
    uint32_t wfc_dns_ipv4 = 0;
    std::string pcap_adapter_requested;
    bool pcap_adapter_selected = false;
    std::string pcap_device_name;
    std::string pcap_friendly_name;
    std::string pcap_description;
    std::string pcap_ipv4;
};

// Returns false (no fields populated) if no bridge has been attached yet,
// or the attached backend isn't Replay. Read-only; never advances
// execution -- safe to call at any point, including mid-run over the debug
// server, matching every other query-surface function in this codebase.
bool nds_wifi_replay_status(NdsNetReplayStatus* out);

// Returns the attached Wi-Fi backend/runtime state. Read-only and safe to
// query through the debug server while the guest is running.
bool nds_wifi_network_state(NdsWifiNetworkState* out);

// ── Process-wide Winsock lifecycle (Windows only) ───────────────────────
// MUST be called exactly once, successfully, before ANY Winsock API call
// anywhere in this process -- including WSAPoll inside
// Net_Slirp::PollHostSockets(), reachable from the host networking worker
// thread nds_wifi3d_attach() starts. main() calls this first, before
// boot(), on every run mode (see main.cpp's call site comment for why:
// debug_server.cpp's own pre-existing WSAStartup/WSACleanup pair, inside
// debug_serve()/debug_pump_start(), both run too late -- after boot() --
// and a plain batch run never reached either of those functions at all).
// Implemented in wifi_net.cpp because that translation unit already
// transitively includes winsock2.h (via Net_Slirp.h -> libslirp.h) for
// every Windows build that links networking at all, so no new Winsock
// header needs to reach main.cpp. A no-op that unconditionally returns
// true on non-Windows builds, so callers never need an #ifdef _WIN32 of
// their own.
bool nds_net_platform_init();

// Balances nds_net_platform_init(). A no-op on non-Windows builds. Safe to
// register directly with std::atexit (matches its signature); main.cpp
// does exactly that so every return path out of main(), including the one
// std::exit() call in boot()'s cartridge-init-failure branch, still
// balances the WSAStartup call above.
void nds_net_platform_shutdown();

// Stores the resolved network configuration for the next nds_wifi3d_attach()
// call to consume. Safe to call at any time before that first attach;
// calling it after attach has no effect (attach is idempotent and does not
// re-read this after constructing the backend), matching every other
// runner config knob that must be set before boot.
void nds_wifi_configure_network(const NdsWifiNetworkConfig& config);

// Constructs (once) the vendored Wifi device model bound to the runner's
// firmware buffer and the selected network backend, and returns it.
// Idempotent: safe to call from every nds_wifi_* entry point.
melonDS::Wifi* nds_wifi3d_attach();

// Tears the above down (unused by the current boot/reset path -- Wifi and
// Net_Slirp both persist across nds_wifi_reset(), matching real melonDS's
// NDS::Reset(), which does not reconstruct its Wifi/Net members either).
// Retained for a future savestate-load/shutdown path.
void nds_wifi3d_detach();
