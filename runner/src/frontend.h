#pragma once

#include <cstdint>
#include <string>

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

struct NdsFrontendOptions {
    NdsScreenLayout screen_layout = NdsScreenLayout::Stacked;
    NdsStartupMode startup_mode = NdsStartupMode::Preserve;
    uint8_t adaptive_screens = NDS_ADAPTIVE_NONE;
    // Title-owned capability mask. A requested screen must be present here;
    // unsupported adaptive output fails closed instead of stretching pixels.
    uint8_t adaptive_supported = NDS_ADAPTIVE_NONE;
    uint16_t adaptive_max_width[2] = {256, 256};
    // Host presentation quality. These are deliberately post-composition:
    // they never alter guest-visible DS rasterization or framebuffer bytes.
    uint8_t supersampling = 1;  // 1x..4x presentation reconstruction
    uint8_t antialiasing = 0;   // 0/2/4/8 sample-quality preset
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
bool nds_parse_adaptive_screens(const std::string& value, uint8_t* out);
bool nds_parse_supersampling(const std::string& value, uint8_t* out);
bool nds_parse_antialiasing(const std::string& value, uint8_t* out);
const char* nds_screen_layout_name(NdsScreenLayout value);
const char* nds_startup_mode_name(NdsStartupMode value);
const char* nds_adaptive_screens_name(uint8_t value);

int nds_run_interactive_frontend(const NdsFrontendOptions& options);

// Live frontend counters for the play-mode debug surface (`frontend_stats`).
// Cumulative since frontend start; a client samples twice and derives fps /
// phase shares over its own window. Zeros (active=0) when no frontend runs.
// Written and read on the frontend thread only (debug_pump() safe point).
struct NdsFrontendLiveStats {
    int active;
    uint64_t frames;          // presented frames
    uint64_t emu_ticks;       // cumulative emulation phase (perf ticks)
    uint64_t present_ticks;   // cumulative present phase
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
