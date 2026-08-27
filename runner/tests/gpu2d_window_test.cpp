#include "gpu2d_window.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>

// Compile the production implementation into this focused test with device
// stubs below. This keeps the OBJ-window and compose assertions on the exact
// raster/compositor code rather than a test-only reimplementation.
#include "../src/gpu2d.cpp"

namespace {

std::array<uint8_t, 0x400> g_palette{};
std::array<uint8_t, 0x400> g_oam{};
NdsVramRendererView g_view{};
std::array<uint32_t, 448> g_3d_line{};
std::array<uint8_t, 0x20000> g_capture_bank{};
bool g_lcdc_mapped = false;
uint16_t g_3d_output_width = 256;
uint16_t g_3d_render_xpos = 0;
uint32_t g_3d_render_polygon_count = 0;

bool require(bool value) { return value; }

NdsGpu2dWindowState make_state(uint32_t dispcnt,
                                std::array<uint8_t, 12>& win) {
    return {dispcnt, win.data()};
}

void raw_rect(std::array<uint8_t, 12>& win, int window,
              uint8_t x_start, uint8_t x_end,
              uint8_t y_start, uint8_t y_end) {
    const int h = window ? 2 : 0;
    const int v = window ? 6 : 4;
    win[h] = x_end;
    win[h + 1] = x_start;
    win[v] = y_end;
    win[v + 1] = y_start;
}

void write16(std::array<uint8_t, 0x400>& bytes, size_t offset,
             uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void reset_obj_fixture(std::array<uint8_t, 0x4000>& obj_vram) {
    g_unit = {};
    g_unit[0].dispcnt = 0x1000u;  // OBJ enable
    g_oam.fill(0);
    g_palette.fill(0);
    obj_vram.fill(0);
    g_view = {};
    g_view.obj[0] = obj_vram.data();
}

void set_obj(unsigned number, uint16_t a0, uint16_t a1, uint16_t a2) {
    const size_t offset = number * 8u;
    write16(g_oam, offset, a0);
    write16(g_oam, offset + 2u, a1);
    write16(g_oam, offset + 4u, a2);
}

void set_4bpp_index(std::array<uint8_t, 0x4000>& obj_vram,
                     int x, int y, uint8_t index) {
    const size_t offset = static_cast<size_t>(y) * 4u +
                          static_cast<size_t>(x >> 1);
    const uint8_t shift = (x & 1) ? 4u : 0u;
    obj_vram[offset] = static_cast<uint8_t>(
        (obj_vram[offset] & ~(0xFu << shift)) | ((index & 0xFu) << shift));
}

unsigned coverage_count(const std::array<uint8_t, 256>& coverage) {
    unsigned count = 0;
    for (uint8_t value : coverage) count += value != 0u;
    return count;
}

bool test_window_raw_decode() {
    std::array<uint8_t, 12> win{};
    win[8] = 0x01;
    win[9] = 0x02;
    win[10] = 0x04;
    win[11] = 0x08;
    raw_rect(win, 0, 10, 30, 20, 40);
    raw_rect(win, 1, 50, 70, 60, 80);
    auto state = make_state(0xE000u, win);

    if (!require(nds_gpu2d_window_mask(state, 10, 20, true) == 0x01) ||
        !require(nds_gpu2d_window_mask(state, 29, 39, true) == 0x01) ||
        !require(nds_gpu2d_window_mask(state, 30, 20, true) != 0x01) ||
        !require(nds_gpu2d_window_mask(state, 50, 60, true) == 0x02) ||
        !require(nds_gpu2d_window_mask(state, 70, 60, true) != 0x02))
        return false;

    raw_rect(win, 0, 240, 16, 180, 12);
    raw_rect(win, 1, 200, 32, 170, 20);
    state = make_state(0x6000u, win);
    return require(nds_gpu2d_window_mask(state, 250, 190, false) == 0x01) &&
           require(nds_gpu2d_window_mask(state, 4, 4, false) == 0x01) &&
           require(nds_gpu2d_window_mask(state, 128, 96, false) == 0x04) &&
           require(nds_gpu2d_window_mask(state, 220, 180, false) == 0x02);
}

bool test_window_enable_and_precedence() {
    std::array<uint8_t, 12> win{};
    win[8] = 0x01;
    win[9] = 0x02;
    win[10] = 0x04;
    win[11] = 0x08;
    raw_rect(win, 0, 10, 30, 10, 30);
    raw_rect(win, 1, 10, 30, 10, 30);
    auto state = make_state(0x2000u, win);
    if (!require(nds_gpu2d_window_mask(state, 15, 15, true) == 0x01))
        return false;
    state = make_state(0x4000u, win);
    if (!require(nds_gpu2d_window_mask(state, 15, 15, true) == 0x02))
        return false;
    state = make_state(0x8000u, win);
    if (!require(nds_gpu2d_window_mask(state, 15, 15, true) == 0x08) ||
        !require(nds_gpu2d_window_mask(state, 15, 15, false) == 0x04))
        return false;
    state = make_state(0xE000u, win);
    return require(nds_gpu2d_window_mask(state, 15, 15, true) == 0x01) &&
           require(nds_gpu2d_window_mask(state, 15, 15, false) == 0x01) &&
           require(nds_gpu2d_window_mask(state, 5, 5, true) == 0x08) &&
           require(nds_gpu2d_window_mask(state, 5, 5, false) == 0x04);
}

bool test_window_empty_and_scanner_eligibility() {
    std::array<uint8_t, 12> win{};
    win[8] = 0x00;
    win[9] = 0x00;
    win[10] = 0x2Fu;
    win[11] = 0x33u;
    raw_rect(win, 0, 7, 7, 10, 30);
    raw_rect(win, 1, 10, 30, 7, 7);
    auto state = make_state(0xE000u, win);
    if (!require(nds_gpu2d_window_mask(state, 7, 10, false) == 0x2F) ||
        !require(nds_gpu2d_window_mask(state, 10, 7, false) == 0x2F) ||
        !require(nds_gpu2d_windows_support_hd(state)))
        return false;

    // Metroid Prime Hunters scanner: WININ=0x3F3F, WINOUT=0x332F.
    win[8] = 0x3F;
    win[9] = 0x3F;
    win[10] = 0x2F;
    win[11] = 0x33;
    state = make_state(0xE000u, win);
    if (!require(nds_gpu2d_window_mask(state, 0, 0, false) == 0x2F) ||
        !require(nds_gpu2d_window_mask(state, 0, 0, true) == 0x33) ||
        !require(nds_gpu2d_windows_support_hd(state)))
        return false;
    win[11] = 0x32;  // BG0 disabled in potentially reachable OBJ window.
    if (!require(!nds_gpu2d_windows_support_hd(state))) return false;
    win[11] = 0x13;  // Effects disabled but BG0 remains visible.
    if (!require(nds_gpu2d_windows_support_hd(state))) return false;
    win[11] = 0x17;  // MPH tutorial/transmission OBJ window mask.
    return require(nds_gpu2d_windows_support_hd(state));
}

bool test_direct_scene_rejects_windows() {
    g_unit = {};
    g_direct_present_enabled = true;
    g_3d_output_width = 448;
    g_3d_render_xpos = 0;
    g_unit[0].dispcnt = 0x00011108u;  // mode 1, BG0/3D, OBJ.
    if (!require(direct_scene_class() == NDS_GPU2D_DIRECT_SUPPORTED))
        return false;

    for (uint32_t windows : {0x2000u, 0x4000u, 0x8000u,
                             NDS_GPU2D_DISPCNT_WINDOW_ENABLE_MASK}) {
        g_unit[0].dispcnt = 0x00011108u | windows;
        if (!require(direct_scene_class() == NDS_GPU2D_DIRECT_WINDOWS))
            return false;
    }

    // MPH scanner state is rejected for windows before its extra BGs can
    // independently reject the direct path.
    g_unit[0].dispcnt = 0x0001FF18u;
    const bool scanner_rejected =
        direct_scene_class() == NDS_GPU2D_DIRECT_WINDOWS;
    g_direct_present_enabled = false;
    g_3d_output_width = 256;
    g_3d_render_xpos = 0;
    return require(scanner_rejected);
}

bool test_direct_scene_rejects_render_xpos() {
    g_unit = {};
    g_direct_present_enabled = true;
    g_3d_output_width = 448;
    g_3d_render_xpos = 1;
    g_unit[0].dispcnt = 0x00011108u;  // mode 1, BG0/3D, OBJ.
    const bool rejected =
        direct_scene_class() == NDS_GPU2D_DIRECT_RENDER_XPOS;
    g_direct_present_enabled = false;
    g_3d_output_width = 256;
    g_3d_render_xpos = 0;
    return require(rejected);
}

bool test_direct_scene_rejects_center_native() {
    g_unit = {};
    g_direct_present_enabled = true;
    g_adaptive_center_native = true;
    g_3d_output_width = 448;
    g_3d_render_xpos = 0;
    g_unit[0].dispcnt = 0x00011108u;  // mode 1, BG0/3D, OBJ.
    const bool rejected =
        direct_scene_class() == NDS_GPU2D_DIRECT_CENTER_NATIVE;
    g_direct_present_enabled = false;
    g_adaptive_center_native = false;
    g_3d_output_width = 256;
    return require(rejected);
}

bool test_direct_scene_centers_low_polygon_frames() {
    g_unit = {};
    g_direct_present_enabled = true;
    g_adaptive_center_max_polygons = 64;
    g_3d_output_width = 448;
    g_3d_render_xpos = 0;
    g_unit[0].dispcnt = 0x00011108u;  // mode 1, BG0/3D, OBJ.

    g_3d_render_polygon_count = 31;
    const bool low_polygon_centered =
        direct_scene_class() == NDS_GPU2D_DIRECT_CENTER_NATIVE;

    g_3d_render_polygon_count = 65;
    const bool gameplay_allowed =
        direct_scene_class() == NDS_GPU2D_DIRECT_SUPPORTED;

    g_direct_present_enabled = false;
    g_adaptive_center_max_polygons = 0;
    g_3d_render_polygon_count = 0;
    g_3d_output_width = 256;
    return require(low_polygon_centered && gameplay_allowed);
}

bool test_compose_window_effects() {
    Unit unit{};
    unit.evy = 16;
    Pixel top{0x00101010u, 0x01u, 0, 0, 1, true, 1};
    Pixel below{0x00303030u, 0x02u, 0, 1, 2, true};

    unit.bldcnt = 0x01u | (2u << 6u);  // BG0 brighten.
    if (!require(compose(unit, top, below, false) == top.color) ||
        !require(compose(unit, top, below, true) != top.color))
        return false;
    unit.bldcnt = 0x01u | (3u << 6u);  // BG0 darken.
    if (!require(compose(unit, top, below, false) == top.color) ||
        !require(compose(unit, top, below, true) != top.color))
        return false;

    unit.bldcnt = 0x0200u;  // BG1 as second target: forced 3D alpha blend.
    if (!require(compose(unit, top, below, false) != top.color)) return false;
    top.alpha5 = 0;
    top.alpha = 0xFFu;      // Forced semi-transparent OBJ blend.
    top.target = 0x10u;
    unit.eva = 8;
    unit.evb = 8;
    return require(compose(unit, top, below, false) != top.color);
}

bool test_obj_window_coverage() {
    std::array<uint8_t, 0x4000> obj_vram{};
    std::array<Pixel, 256> pixels{};
    std::array<uint8_t, 256> coverage{};

    // Index zero is transparent and produces neither output nor coverage.
    reset_obj_fixture(obj_vram);
    set_obj(0, 2u << 10u, 0, 0);
    render_obj_line(g_unit[0], 0, 0, pixels.data(), 256, g_oam.data(),
                    g_palette.data(), g_view, coverage.data());
    if (!require(coverage_count(coverage) == 0)) return false;

    // Non-affine H/V flips use the same source geometry for coverage.
    reset_obj_fixture(obj_vram);
    set_4bpp_index(obj_vram, 0, 0, 1);
    set_obj(0, 2u << 10u, 0x1000u, 0);
    render_obj_line(g_unit[0], 0, 0, pixels.data(), 256, g_oam.data(),
                    g_palette.data(), g_view, coverage.data());
    if (!require(coverage[7] && coverage_count(coverage) == 1)) return false;
    reset_obj_fixture(obj_vram);
    set_4bpp_index(obj_vram, 0, 0, 1);
    set_obj(0, 2u << 10u, 0x2000u, 0);
    render_obj_line(g_unit[0], 0, 7, pixels.data(), 256, g_oam.data(),
                    g_palette.data(), g_view, coverage.data());
    if (!require(coverage[0] && coverage_count(coverage) == 1)) return false;

    // Affine identity, transformed, and double-size objects all retain
    // coverage semantics while ignoring the regular flip bits.
    reset_obj_fixture(obj_vram);
    set_4bpp_index(obj_vram, 0, 0, 1);
    set_obj(0, 0x0100u | (2u << 10u), 0, 0);
    write16(g_oam, 6, 0x0100u); write16(g_oam, 14, 0);
    write16(g_oam, 22, 0); write16(g_oam, 30, 0x0100u);
    render_obj_line(g_unit[0], 0, 0, pixels.data(), 256, g_oam.data(),
                    g_palette.data(), g_view, coverage.data());
    if (!require(coverage[0] && coverage_count(coverage) == 1)) return false;
    reset_obj_fixture(obj_vram);
    set_4bpp_index(obj_vram, 7, 0, 1);
    set_obj(0, 0x0100u | (2u << 10u), 0, 0);
    write16(g_oam, 6, 0xFF00u); write16(g_oam, 14, 0);
    write16(g_oam, 22, 0); write16(g_oam, 30, 0x0100u);
    render_obj_line(g_unit[0], 0, 0, pixels.data(), 256, g_oam.data(),
                    g_palette.data(), g_view, coverage.data());
    if (!require(coverage[1] && coverage_count(coverage) == 1)) return false;
    reset_obj_fixture(obj_vram);
    for (int x = 0; x < 8; ++x) set_4bpp_index(obj_vram, x, 0, 1);
    set_obj(0, 0x0300u | (2u << 10u), 0, 0);
    write16(g_oam, 6, 0x0100u); write16(g_oam, 14, 0);
    write16(g_oam, 22, 0); write16(g_oam, 30, 0x0100u);
    render_obj_line(g_unit[0], 0, 4, pixels.data(), 256, g_oam.data(),
                    g_palette.data(), g_view, coverage.data());
    if (!require(coverage[4] && coverage[11] && coverage_count(coverage) == 8))
        return false;

    // Screen clipping, signed 9-bit X wrap, and modulo-256 Y selection.
    reset_obj_fixture(obj_vram);
    for (int x = 0; x < 8; ++x) set_4bpp_index(obj_vram, x, 0, 1);
    set_obj(0, 2u << 10u, 0x01FCu, 0);
    render_obj_line(g_unit[0], 0, 0, pixels.data(), 256, g_oam.data(),
                    g_palette.data(), g_view, coverage.data());
    if (!require(coverage_count(coverage) == 4 && coverage[0] && coverage[3]))
        return false;
    reset_obj_fixture(obj_vram);
    for (int x = 0; x < 8; ++x) set_4bpp_index(obj_vram, x, 0, 1);
    set_obj(0, 2u << 10u, 0x01FFu, 0);
    render_obj_line(g_unit[0], 0, 0, pixels.data(), 256, g_oam.data(),
                    g_palette.data(), g_view, coverage.data());
    if (!require(coverage_count(coverage) == 7 && coverage[0] && coverage[6]))
        return false;
    reset_obj_fixture(obj_vram);
    // In 2D OBJ mapping row 8 comes from tile row 0x20, not byte row 8 of
    // tile zero. The scanline below wraps from Y=250 to source row 8.
    obj_vram[0x400] = 1;
    set_obj(0, 250u | (2u << 10u), 0x4000u, 0);  // 16x16 OBJ at Y=250.
    render_obj_line(g_unit[0], 0, 2, pixels.data(), 256, g_oam.data(),
                    g_palette.data(), g_view, coverage.data());
    if (!require(coverage[0] && coverage_count(coverage) == 1)) return false;

    // Coverage is a union; OAM priority never suppresses a mode-2 sprite.
    reset_obj_fixture(obj_vram);
    set_4bpp_index(obj_vram, 0, 0, 1);
    set_4bpp_index(obj_vram, 1, 0, 1);
    set_obj(0, 2u << 10u, 0, 3u << 10u);
    set_obj(1, 2u << 10u, 0, 0u << 10u);
    render_obj_line(g_unit[0], 0, 0, pixels.data(), 256, g_oam.data(),
                    g_palette.data(), g_view, coverage.data());
    return require(coverage[0] && coverage[1]);
}

}  // namespace

// DISPCAPCNT display capture writes back into guest-visible VRAM and bumps the
// texture generation the 3D engine reads, so capture lines must be rendered on
// the scheduler thread even when threaded scanline rendering is on. This pins
// both halves: the captured bytes and the framebuffers must be identical to
// the single-threaded path, and the classification counters must show the
// capture lines going inline while the non-capture lines go to a worker.
struct CaptureRun {
    std::array<uint8_t, 0x20000> bank{};
    std::array<uint32_t, 256 * 192> top{};
    std::array<uint32_t, 256 * 192> bottom{};
    uint64_t threaded_lines = 0;
    uint64_t inline_lines = 0;
};

void run_capture_frame(uint32_t capture_size, CaptureRun& out) {
    nds_gpu2d_reset();
    g_capture_bank.fill(0);
    g_palette.fill(0);
    g_oam.fill(0);
    g_view = {};
    g_lcdc_mapped = true;
    // A non-black backdrop so the composite the capture unit reads is not all
    // zero, and a distinguishable per-line 3D source.
    write16(g_palette, 0, 0x3DEFu);
    for (size_t i = 0; i < g_3d_line.size(); ++i)
        g_3d_line[i] = static_cast<uint32_t>((i * 0x00010203u) | 0x1F000000u);

    Unit& u = g_unit[0];
    u.dispcnt = 0x00010000u;   // display mode 1 (composite), BG mode 0
    u.master_bright = 0;
    // Enable + size + source A = composite, dest bank 0, no offsets.
    u.capture = 0x80000000u | (capture_size << 20);
    g_unit[1].dispcnt = 0x00010000u;

    nds_gpu2d_start_frame();
    for (int y = 0; y < 192; ++y) nds_gpu2d_render_scanline(y);

    NdsGpu2dProfile prof{};
    nds_gpu2d_profile(&prof);
    out.threaded_lines = prof.threaded_lines;
    out.inline_lines = prof.inline_lines;
    out.bank = g_capture_bank;
    // Rasterization targets the back buffer; the front flip happens at the
    // frame wrap, which this fixture does not reach.
    std::copy_n(g_fb[g_front ^ 1][0].data(), 256 * 192, out.top.data());
    std::copy_n(g_fb[g_front ^ 1][1].data(), 256 * 192, out.bottom.data());
}

bool test_capture_serializes_and_matches() {
    // size 3 = 256x192: every line captures.
    // size 0 = 128x128: lines 0..127 capture, 128..191 do not.
    for (const uint32_t size : {3u, 0u}) {
        CaptureRun single{};
        nds_gpu2d_set_threaded(false, 1);
        run_capture_frame(size, single);

        CaptureRun threaded{};
        nds_gpu2d_set_threaded(true, 2);
        run_capture_frame(size, threaded);
        nds_gpu2d_set_threaded(false, 1);

        std::fprintf(stderr,
            "[capture size %u] single inline=%llu threaded=%llu | "
            "threaded inline=%llu threaded=%llu | bank_eq=%d top_eq=%d "
            "bot_eq=%d nonzero=%d\n",
            size,
            (unsigned long long)single.inline_lines,
            (unsigned long long)single.threaded_lines,
            (unsigned long long)threaded.inline_lines,
            (unsigned long long)threaded.threaded_lines,
            (int)(single.bank == threaded.bank),
            (int)(single.top == threaded.top),
            (int)(single.bottom == threaded.bottom),
            (int)std::any_of(single.bank.begin(), single.bank.end(),
                             [](uint8_t v) { return v != 0u; }));
        if (!require(single.bank == threaded.bank)) return false;
        if (!require(single.top == threaded.top)) return false;
        if (!require(single.bottom == threaded.bottom)) return false;
        // The capture must actually have written something, or the comparison
        // is vacuous.
        if (!require(std::any_of(single.bank.begin(), single.bank.end(),
                                 [](uint8_t v) { return v != 0u; })))
            return false;
        // Single-threaded: every line inline, none threaded.
        if (!require(single.threaded_lines == 0u)) return false;
        if (!require(single.inline_lines == 192u)) return false;
        // Threaded: exactly the capturing lines went inline.
        const uint64_t expected_inline = size == 3u ? 192u : 128u;
        if (!require(threaded.inline_lines == expected_inline)) return false;
        if (!require(threaded.threaded_lines == 192u - expected_inline))
            return false;
    }
    return true;
}

// Minimal device implementations required by the production renderer object.
uint16_t nds_powercontrol9() { return 0x8002u; }
const uint8_t* nds_vram_renderer_palette(int) { return g_palette.data(); }
const uint8_t* nds_vram_renderer_oam(int) { return g_oam.data(); }
const NdsVramRendererView* nds_vram_renderer_view(int) { return &g_view; }
uint32_t nds_vram_read_bg(int, uint32_t, uint32_t) { return 0; }
uint32_t nds_vram_read_obj(int, uint32_t, uint32_t) { return 0; }
uint32_t nds_vram_read_bg_extpal(int, uint32_t, uint32_t) { return 0; }
uint32_t nds_vram_read_obj_extpal(int, uint32_t, uint32_t) { return 0; }
bool nds_vram_lcdc_mapped(unsigned) { return g_lcdc_mapped; }
uint8_t* nds_vram_bank_data(unsigned) { return g_capture_bank.data(); }
void nds_vram_note_capture_write() {}
uint32_t nds_video_read(int, uint32_t, uint32_t) { return 0; }
const uint32_t* nds_gpu3d_line(int) { return g_3d_line.data(); }
uint16_t nds_gpu3d_output_width() { return g_3d_output_width; }
const uint32_t* nds_gpu3d_wide_line(int) { return g_3d_line.data(); }
const uint32_t* nds_gpu3d_wide_attr_line(int) { return g_3d_line.data(); }
uint16_t nds_gpu3d_render_xpos() { return g_3d_render_xpos; }
uint32_t nds_gpu3d_render_polygon_count() { return g_3d_render_polygon_count; }
bool nds_title_patches_mph_adaptive_centered_native() { return false; }
void nds_gpu3d_set_render_xpos(uint16_t) {}

int main() {
    if (!test_window_raw_decode()) return 1;
    if (!test_window_enable_and_precedence()) return 2;
    if (!test_window_empty_and_scanner_eligibility()) return 3;
    if (!test_direct_scene_rejects_windows()) return 4;
    if (!test_direct_scene_rejects_render_xpos()) return 5;
    if (!test_direct_scene_rejects_center_native()) return 6;
    if (!test_direct_scene_centers_low_polygon_frames()) return 7;
    if (!test_compose_window_effects()) return 8;
    if (!test_obj_window_coverage()) return 9;
    if (!test_capture_serializes_and_matches()) return 10;
    return 0;
}
