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

// Which boot path establishes the pre-game machine state. Lle executes the
// real BIOS + firmware boot (the default and the oracle-diffed source of
// truth). Direct is the opt-in melonDS SetupDirectBoot equivalent: the
// runner establishes the post-firmware state itself and starts the
// cartridge binaries immediately (runner/src/direct_boot.cpp,
// beads-yjp.15). Never a silent fallback — every input dump is still
// mandatory and verified in either mode.
enum class NdsBootMode : uint8_t {
    Lle,
    Direct,
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
// shape. For Slirp-backed local tests, use provider "local" instead of a
// raw 127.x.x.x address: 127/8 is the guest's own loopback, while the
// provider's 10.64.0.1 address is libslirp's guest-visible host alias.
// Default provider is "kaeru" (178.62.43.212), the DNS-only,
// no-ROM-patch WFC service verified appropriate for DS; "wiimmfi" is also
// selectable and aliases that DS-compatible Wiimmfi ecosystem route.
// "wiimmfi-direct" selects Wiimmfi's raw official DNS endpoint for
// diagnostics/future patched-client work, not for stock DS bring-up.
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
// exactly. The default itself did NOT reduce memory: commit was 330.9 MB
// and working set 224.5 MB whether networking was on or off, against main's
// 238.5 / 211.2. That was the always-on ring's static BSS, which is
// allocated unconditionally and therefore untouched by this flag. The
// hostname side-table part of that cost is tracked separately in
// beads-yjp.1.17.
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

struct NdsLocalWirelessOptions {
    bool enabled = false;
    uint16_t base_port = 26710;
};

struct NdsMphPrimeControlBindings {
    std::string move_forward = "W";
    std::string move_back = "S";
    std::string move_left = "A";
    std::string move_right = "D";
    std::string jump = "Space";
    std::string morph_ball = "Left Ctrl";
    std::string boost_zoom = "Left Shift";
    std::string scan_visor = "C";
    std::string ui_left = "Q";
    std::string ui_right = "E";
    std::string ui_ok = "F";
    std::string shoot = "Mouse Left";
    std::string scan_shoot = "Mouse Right";
    std::string beam = "Mouse 5";
    std::string missile = "Mouse 4";
    std::string weapon1 = "1";
    std::string weapon2 = "2";
    std::string weapon3 = "3";
    std::string weapon4 = "4";
    std::string weapon5 = "5";
    std::string weapon6 = "6";
    std::string virtual_stylus = "Tab";
    std::string menu = "V";
};

struct NdsFrontendOptions {
    // Optional exact cartridge identity from [game]. When present, every
    // title-owned setting in this config is rejected for any other ROM.
    std::string expected_rom_sha1;
    NdsScreenLayout screen_layout = NdsScreenLayout::Stacked;
    NdsStartupMode startup_mode = NdsStartupMode::Preserve;
    NdsBootMode boot_mode = NdsBootMode::Lle;
    // Opt-in synthesized firmware image (no firmware dump). Forces direct
    // boot — the generated image carries no boot code — and the runner
    // refuses any other pairing rather than silently switching paths.
    bool generated_firmware = false;
    // Opt-in vendored FreeBIOS (no BIOS dumps): a real BSD-2-Clause BIOS
    // reimplementation executed through its own recompiled banks. It cannot
    // boot the firmware menu, so it also requires --boot direct.
    bool freebios = false;
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
    // beads-yjp.16: the DS firmware console nickname, which games surface as
    // the player's default name and which WFC/Wiimmfi shows to peers.
    // Applied to the IN-MEMORY firmware image on every boot (main.cpp, via
    // nds_apply_player_name) so it works identically for a retail dump and a
    // generated image. Empty (the default) is a deliberate no-op: a dump
    // keeps its console's real nickname, a generated image keeps
    // "ndsrecomp". Sources, lowest to highest precedence: game.toml
    // [system] player_name, NDS_PLAYER_NAME, --player-name.
    std::string player_name;
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
    bool mph_prime_controls = false;
    uint16_t mph_virtual_stylus_sensitivity = 20;  // 10%..400%
    // Right-stick camera speed for Prime Controls on a gamepad (percent of
    // the built-in full-deflection turn rate). The left stick always maps
    // to the D-pad; the right stick and triggers only act on titles where
    // Prime Controls is active.
    uint16_t mph_pad_aim_sensitivity = 100;       // 10%..400%
    NdsMphPrimeControlBindings mph_bindings{};
    // Internal exact-ROM capability selected after cartridge verification.
    // MPH consumes unbounded host deltas through its native aim fields.
    bool relative_mouse_direct_aim = false;
    NdsCartridgeSaveConfig cartridge_save{};
    NdsNetworkOptions network{};
    NdsLocalWirelessOptions local_wireless{};
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
bool nds_parse_boot_mode(const std::string& value, NdsBootMode* out);
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
bool nds_set_mph_prime_binding(NdsFrontendOptions* options,
                               const std::string& action,
                               const std::string& value);
bool nds_parse_cartridge_save_type(const std::string& value,
                                   NdsCartridgeSaveType* out);
// Strict dotted-quad IPv4 ("a.b.c.d", each octet 0..255, no leading zeros
// beyond a single "0", no surrounding whitespace). *out is host byte
// order. Used for game.toml's [network.wfc] dns_server and the
// --wfc-provider CLI flag's raw-IP form.
bool nds_parse_ipv4(const std::string& value, uint32_t* out);
bool nds_parse_network_backend(const std::string& value, std::string* out);
bool nds_parse_local_wireless_base_port(const std::string& value,
                                        uint16_t* out);
const char* nds_screen_layout_name(NdsScreenLayout value);
const char* nds_startup_mode_name(NdsStartupMode value);
const char* nds_boot_mode_name(NdsBootMode value);
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

// SDL-event injection and counters for play-mode TCP validation. These
// commands intentionally enter through the frontend's normal event path, so
// game-specific input layers such as MPH Prime Controls are exercised the same
// way as real host input.
struct NdsFrontendInputDebugState {
    int active;
    int mph_prime_controls_available;
    int mph_prime_controls_active;
    int relative_mouse_captured;
    uint16_t keyboard_pressed;
    uint16_t mouse_pressed;
    uint16_t mph_prime_pressed;
    uint16_t stick_pressed;
    int pad_engaged;
    uint64_t pad_aim_writes;
    uint16_t published_key_mask;
    uint64_t relative_direct_writes;
    uint64_t mph_prime_key_downs;
    uint64_t mph_prime_mouse_downs;
    uint64_t debug_key_events;
    uint64_t debug_mouse_button_events;
    uint64_t debug_mouse_motion_events;
    uint64_t debug_touch_events;
    uint64_t debug_capture_events;
    uint64_t debug_release_events;
    uint64_t debug_event_errors;
    uint32_t debug_last_key_scancode;
    int virtual_stylus_x;
    int virtual_stylus_y;
    uint32_t top_window_id;
    uint32_t bottom_window_id;
    int bottom_content_left;
    int separate;
};

bool nds_frontend_debug_key(const char* key_name, bool down);
bool nds_frontend_debug_mouse_button(uint8_t button, bool down);
bool nds_frontend_debug_mouse_motion(int dx, int dy);
bool nds_frontend_debug_touch(uint16_t x, uint16_t y, bool down);
bool nds_frontend_debug_capture_mouse();
bool nds_frontend_debug_release_mouse();
void nds_frontend_input_debug_state(NdsFrontendInputDebugState* out);

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
