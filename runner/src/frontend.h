#pragma once

#include <cstdint>
#include <string>

#include "cartridge_config.h"

// Run the native, human-facing firmware preview. The emulation remains on the
// same scheduler/device path used by the deterministic debug verifier; SDL is
// only the host presentation and input/audio transport.
enum class NdsScreenLayout : uint8_t {
    Stacked,
    Separate,
};

// How the authentic firmware handles an inserted Slot-1 cartridge. Preserve
// leaves the dumped user setting untouched; Manual and Automatic override the
// corresponding retail firmware flag in the runner's private in-memory copy.
enum class NdsStartupMode : uint8_t {
    Preserve,
    Manual,
    Automatic,
};

enum NdsAdaptiveScreen : uint8_t {
    NDS_ADAPTIVE_NONE = 0,
    NDS_ADAPTIVE_TOP = 1u << 0,
    NDS_ADAPTIVE_BOTTOM = 1u << 1,
    NDS_ADAPTIVE_BOTH = NDS_ADAPTIVE_TOP | NDS_ADAPTIVE_BOTTOM,
};

// Named WFC DNS provider (Wiimmfi M4): configuration, not compiled-in
// constants. `name` selects a built-in provider's default DNS address
// (see runner/src/net/wfc_provider.h for the table); `dns_server` is an
// optional per-endpoint override (dotted-quad IPv4) that always wins over
// the named provider's built-in default when non-empty -- used for
// pointing at a local/test DWC server without inventing a second config
// shape. Default provider is "kaeru" (178.62.43.212), the DNS-only,
// no-ROM-patch WFC service verified appropriate for DS; "wiimmfi" is also
// selectable (its own service, DS-compatible for DNS purposes) but is
// NOT the default -- Wiimmfi's own primary patcher targets Wii/WiiU.
struct NdsWfcProvider {
    std::string name = "kaeru";
    std::string dns_server;  // optional override; empty = use `name`'s default
};

// [network] / [network.wfc] settings (game.toml) and their CLI overrides
// (--network, --network-backend, --wfc, --wfc-provider). `enabled=false`
// leaves the Wi-Fi *device* registers live (so the guest still sees the
// hardware) but never constructs a host networking backend -- every
// guest TX is silently absorbed and no host RX ever arrives, matching "no
// AP-independent host networking" for deterministic/offline runs.
//
// DEFAULTS TO OFF, and deliberately differs from upstream melonDS here
// (owner decision 2026-08-11). Offline single-player is this runner's
// primary product, and `enabled=true` made every offline session construct
// a libslirp NAT and start the host networking worker thread it would
// never use -- note nds_wifi_reset() attaches the bridge unconditionally,
// so that happened at reset regardless of whether the guest ever powered
// the Wi-Fi block up.
//
// What this default actually buys, measured on an offline MKDS boot
// (3 stable samples per configuration, pre-feature main 9d33234 as the
// reference): the worker thread goes away, 11 threads -> 10, matching main
// exactly. It does NOT reduce memory. Commit is 330.9 MB and working set
// 224.5 MB whether networking is on or off, against main's 238.5 / 211.2 --
// that +92.4 MB is the always-on ring's static BSS (net_ring.cpp's
// g_net_trace and g_net_hostname_pool), which is allocated unconditionally
// and is therefore untouched by this flag. Fixing that is beads-yjp.1.17,
// still open; do not read this default as having solved it.
//
// Nothing about the *guest-visible* device model changes either way: the
// offline event-counter comparison against pre-feature main is
// instruction-exact in all three configurations (main, default-off, and
// --network on), so this is purely about not paying for a host backend
// nobody asked for. Anything that wants host networking now asks for it:
// `--network on` (plus `--wfc on` for WFC DNS redirection), or an explicit
// `[network] enabled = true` in game.toml.
struct NdsNetworkOptions {
    bool enabled = false;
    std::string backend = "slirp";  // "slirp" (default), "replay" (Wiimmfi
                                     // M8), or, if built with
                                     // NDS_ENABLE_PCAP_BACKEND, "pcap"
    std::string pcap_adapter;       // backend == "pcap": empty = auto-select
    bool wfc_enabled = false;
    NdsWfcProvider wfc_provider{};

    // ---- Wiimmfi M8: capture/replay at the Ethernet backend boundary ----
    std::string capture_out;       // --net-capture-out; empty = no live
                                     // recording
    std::string capture_in;        // --net-capture-in; required when
                                     // backend == "replay"
    bool capture_raw = false;      // --net-capture-raw; opts OUT of
                                     // sanitize-by-default
    bool capture_no_pcap = false;  // --net-capture-no-pcap; opts out of the
                                     // Wireshark-openable sibling file
    std::string capture_scenario;  // --net-capture-scenario; free-form
                                     // human label stored in the capture
                                     // header, e.g. "dhcp"
};

struct NdsFrontendOptions {
    // Optional exact cartridge identity from [game]. When present, every
    // title-owned setting in this config is rejected for any other ROM.
    std::string expected_rom_sha1;
    NdsScreenLayout screen_layout = NdsScreenLayout::Stacked;
    NdsStartupMode startup_mode = NdsStartupMode::Preserve;
    // Wiimmfi: which of several concurrently-run instances this process is
    // (--instance-index CLI flag / [system] instance_index game.toml key).
    // 0 (the default) is a deliberate no-op everywhere this is consumed:
    // the owner wants instance 0 to stay byte-for-byte LLE-faithful, so the
    // guest reads its console MAC straight off the real firmware dump over
    // SPI, unperturbed, exactly like a physical console. A nonzero index
    // perturbs the in-memory firmware image's MAC (see main.cpp's
    // apply_instance_mac(), mirroring melonDS's own multi-instance MAC
    // scheme) so two instances booted from the same firmware dump present
    // different console identities to Wiimmfi/WFC and can see each other.
    uint32_t instance_index = 0;
    uint8_t adaptive_screens = NDS_ADAPTIVE_NONE;
    // Title-owned capability mask. A requested screen must be present here;
    // unsupported adaptive output fails closed instead of stretching pixels.
    uint8_t adaptive_supported = NDS_ADAPTIVE_NONE;
    uint16_t adaptive_max_width[2] = {256, 256};
    // Optional title-owned compositor repair for cylindrical sky geometry.
    // This is deliberately separate from generic adaptive output: most games
    // should leave the heuristic off.
    bool adaptive_skybox_fill = false;
    // Re-anchor transparent text-tile HUD bands over a wide 3D scene. This
    // remains title-owned because arbitrary 2D backgrounds are not safe to
    // split or reposition.
    bool adaptive_hud_anchor = false;
    // Width of the authored center HUD band that stays centered. Pixels on
    // either side are anchored to the corresponding wide edge.
    uint16_t adaptive_hud_center_width = 64;
    // Host presentation quality. These are deliberately post-composition:
    // they never alter guest-visible DS rasterization or framebuffer bytes.
    uint8_t supersampling = 1;  // 1x..4x presentation reconstruction
    uint8_t antialiasing = 0;   // 0/2/4/8 sample-quality preset
    // Optional host FPS-control transport. This stays default-off and is
    // selected by a title launcher; deterministic/headless routes never
    // synthesize mouse input.
    bool relative_mouse_touch = false;
    uint16_t relative_mouse_sensitivity = 100;  // 10%..400%
    bool relative_mouse_invert_y = false;
    // Active-high frontend pressed-bit mask (same layout as key_bit()).
    uint16_t relative_mouse_fire_mask = 0;
    // Internal exact-ROM capability selected after cartridge verification.
    // MPH consumes unbounded host deltas through its native aim fields.
    bool relative_mouse_direct_aim = false;
    NdsCartridgeSaveConfig cartridge_save{};
    NdsNetworkOptions network{};
};

// Parse [display] settings from a game TOML. Missing [display] is valid.
// Returns false for an unreadable file or an invalid recognized value.
bool nds_load_frontend_config(const std::string& path,
                              NdsFrontendOptions* options,
                              std::string* error);
bool nds_parse_screen_layout(const std::string& value,
                             NdsScreenLayout* out);
bool nds_parse_startup_mode(const std::string& value,
                            NdsStartupMode* out);
// 0..255 (fits the byte-wise MAC perturbation in main.cpp's
// apply_instance_mac(), which wraps mod-256 exactly like melonDS's own u8
// arithmetic -- see that function's comment for why a wider range would
// still be harmless but is rejected anyway, to keep bad CLI input loud).
bool nds_parse_instance_index(const std::string& value, uint32_t* out);
bool nds_parse_adaptive_screens(const std::string& value, uint8_t* out);
bool nds_parse_supersampling(const std::string& value, uint8_t* out);
bool nds_parse_antialiasing(const std::string& value, uint8_t* out);
bool nds_parse_on_off(const std::string& value, bool* out);
bool nds_parse_mouse_sensitivity(const std::string& value, uint16_t* out);
bool nds_parse_mouse_fire_key(const std::string& value, uint16_t* out);
bool nds_parse_cartridge_save_type(const std::string& value,
                                   NdsCartridgeSaveType* out);
// Strict dotted-quad IPv4 ("a.b.c.d", each octet 0..255, no leading zeros
// beyond a single "0", no surrounding whitespace). *out is host byte
// order. Used for game.toml's [network.wfc] dns_server and the
// --wfc-provider CLI flag's raw-IP form.
bool nds_parse_ipv4(const std::string& value, uint32_t* out);
bool nds_parse_network_backend(const std::string& value, std::string* out);
const char* nds_screen_layout_name(NdsScreenLayout value);
const char* nds_startup_mode_name(NdsStartupMode value);
const char* nds_adaptive_screens_name(uint8_t value);

int nds_run_interactive_frontend(const NdsFrontendOptions& options);

// Request the interactive frontend's normal SDL quit path. Debug-driven
// scenario harnesses use this instead of TerminateProcess so runtime teardown
// (including compiler profile-data flushing) completes normally.
bool nds_frontend_request_exit();

// Live frontend counters for the play-mode debug surface (`frontend_stats`).
// Cumulative since frontend start; a client samples twice and derives fps /
// phase shares over its own window. Zeros (active=0) when no frontend runs.
// Written and read on the frontend thread only (debug_pump() safe point).
struct NdsFrontendLiveStats {
    int active;
    uint64_t frames;          // presented frames
    uint64_t emu_ticks;       // cumulative emulation phase (perf ticks)
    uint64_t present_ticks;   // cumulative present phase
    uint64_t adaptive_ticks;  // adaptive framebuffer composition
    uint64_t upload_ticks;    // host texture uploads
    uint64_t draw_ticks;      // renderer clear/copy work
    uint64_t swap_ticks;      // window presents
    uint64_t drain_ticks;     // cumulative audio-drain phase
    uint64_t now_ticks;       // performance counter at query time
    uint64_t freq;            // performance frequency (ticks/second)
    uint64_t underruns;       // audio underruns so far
};
void nds_frontend_live_stats(NdsFrontendLiveStats* out);

// Opt-in visual-artifact observer for the interactive TCP surface. A partial
// black band is a run of 8..191 nearly-all-black top-screen rows; full-black
// frames are excluded because fades legitimately produce them. Observation is
// disabled until `black_band_scan` arms it, so normal perf runs pay nothing.
struct NdsFrontendBlackBandCapture {
    int enabled;
    int has_capture;
    uint64_t scanned_frames;
    uint64_t band_frames;
    uint64_t worst_frame;
    uint64_t worst_system_timestamp;
    uint32_t worst_start_row;
    uint32_t worst_row_count;
    uint32_t top_pixels[256 * 192];
};
void nds_frontend_black_band_scan(bool enabled, bool reset);
void nds_frontend_black_band_capture(NdsFrontendBlackBandCapture* out);
