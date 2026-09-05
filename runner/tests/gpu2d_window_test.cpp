#include "gpu2d_window.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

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
// Optional per-test adjustment of the adaptive fixture scene, applied after
// the base registers are written and before the frame is rendered.
std::function<void(Unit&)> g_scene_tweak;

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
    uint64_t staged_captures = 0;
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
    out.staged_captures = prof.staged_captures;
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
        std::fprintf(stderr,
            "[capture size %u] applied single=%llu threaded=%llu\n",
            size, (unsigned long long)single.staged_captures,
            (unsigned long long)threaded.staged_captures);
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
        // Threaded: capture no longer forces a line inline. The pixels are
        // produced on a worker and only the VRAM write is applied by the
        // scheduler thread at the drain, so every line is threaded.
        if (!require(threaded.inline_lines == 0u)) return false;
        if (!require(threaded.threaded_lines == 192u)) return false;
        // And every capturing line must actually have had its write applied.
        const uint64_t expected_applied = size == 3u ? 192u : 128u;
        if (!require(threaded.staged_captures == expected_applied))
            return false;
        if (!require(single.staged_captures == expected_applied))
            return false;
    }
    return true;
}

// The adaptive (widened) presentation compositor runs its 192 lines on a band
// pool of helper threads. Every line's writes are indexed by y -- the
// destination row and the HD layer surfaces at (y * width + x) * 2 -- so any
// execution order must produce the identical frame. This pins that: the same
// scene composited with 0 helpers and with 3 must be byte-identical, in the
// presented surface AND in the HD layer surfaces the accelerated presenter
// consumes.
struct AdaptiveRun {
    std::vector<uint32_t> surface;
    std::vector<uint32_t> hd_top;
    std::vector<uint32_t> hd_below;
    uint16_t width = 0;
    bool hd_valid = false;
    uint64_t band_frames = 0;
    uint64_t serial_frames = 0;
    uint64_t helper_lines = 0;
};

void run_adaptive_frame(AdaptiveRun& out) {
    nds_gpu2d_reset();
    nds_gpu2d_profile_reset();
    g_lcdc_mapped = false;
    g_3d_output_width = 448;
    g_3d_render_xpos = 0;
    g_3d_render_polygon_count = 4096;
    g_palette.fill(0);
    g_oam.fill(0);
    // Backdrop plus a text-BG palette so the HUD layer is not all transparent.
    write16(g_palette, 0, 0x3DEFu);
    for (size_t i = 1; i < 16; ++i)
        write16(g_palette, i * 2u, static_cast<uint16_t>(0x8000u | (i * 0x0421u)));
    // A 3D line with a mix of opaque, semi-transparent and fully transparent
    // pixels, so the layer resolution actually has something to resolve and
    // the skybox/black-run scan sees both cases.
    for (size_t i = 0; i < g_3d_line.size(); ++i) {
        const uint32_t alpha = (i % 7u == 0u) ? 0u : ((i % 5u) + 1u) * 6u;
        g_3d_line[i] = static_cast<uint32_t>((i * 0x00010203u) & 0x003F3F3Fu) |
                       (alpha << 24);
    }
    // Two visible OBJ sprites, so render_obj_line has real work per line.
    set_obj(0, 0x0020u, 0x4010u, 0x0200u);
    set_obj(1, 0x0060u, 0x40A0u, 0x0401u);

    Unit& u = g_unit[0];
    // Display mode 1 (composite), BG0 = 3D, BG0/BG1 enabled, OBJ enabled.
    u.dispcnt = 0x00010000u | 0x8u | 0x100u | 0x200u | 0x1000u;
    u.bgcnt[0] = 0;
    u.bgcnt[1] = 0x0001u;   // text BG, priority 1
    u.master_bright = 0;
    u.capture = 0;
    g_unit[1].dispcnt = 0x00010000u;
    if (g_scene_tweak) g_scene_tweak(u);

    nds_gpu2d_start_frame();
    for (int y = 0; y < 192; ++y) nds_gpu2d_render_scanline(y);
    nds_gpu2d_finish_frame();

    nds_gpu2d_set_hd_emit(true);
    nds_gpu2d_invalidate_hd_frame();
    uint16_t width = 0;
    const uint32_t* const fb = nds_gpu2d_adaptive_framebuffer(0, &width);
    out.width = width;
    out.surface.assign(fb, fb + static_cast<size_t>(width) * 192u);
    NdsGpu2dHdFrame hd{};
    out.hd_valid = nds_gpu2d_hd_frame_peek(&hd);
    if (out.hd_valid) {
        const size_t words = static_cast<size_t>(hd.width) * 192u * 2u;
        out.hd_top.assign(hd.top_pixels, hd.top_pixels + words);
        out.hd_below.assign(hd.below_pixels, hd.below_pixels + words);
    }
    NdsGpu2dProfile prof{};
    nds_gpu2d_profile(&prof);
    out.band_frames = prof.adaptive_band_frames;
    out.serial_frames = prof.adaptive_serial_frames;
    out.helper_lines = prof.adaptive_helper_lines;
    nds_gpu2d_set_hd_emit(false);
}

bool test_adaptive_window_x() {
    // HUD-sourced columns keep their native coordinate; margins clamp to the
    // nearest native edge column instead of wrapping through uint8_t.
    constexpr int extra = (448 - 256) / 2;
    return require(nds_gpu2d_adaptive_window_x(100, extra, 4) == 4) &&
           require(nds_gpu2d_adaptive_window_x(0, extra, -1) == 0) &&
           require(nds_gpu2d_adaptive_window_x(95, extra, -1) == 0) &&
           require(nds_gpu2d_adaptive_window_x(96, extra, -1) == 0) &&
           require(nds_gpu2d_adaptive_window_x(200, extra, -1) == 104) &&
           require(nds_gpu2d_adaptive_window_x(351, extra, -1) == 255) &&
           require(nds_gpu2d_adaptive_window_x(352, extra, -1) == 255) &&
           require(nds_gpu2d_adaptive_window_x(447, extra, -1) == 255);
}

// Mario Kart DS races with Win0/Win1/OBJ-window enabled on every frame
// (DISPCNT 0x0001F108, WININ 0x3F3F, WINOUT 0x322F): the OBJ window is the
// item-box hole whose mask drops BG0/3D. Such a scene must still be
// composited at the adaptive width -- with the OBJ window honoured per pixel
// -- and only the HD surface emission may stand down for it.
bool test_adaptive_windowed_scene_composites_wide() {
    static std::array<uint8_t, 0x4000> obj_vram{};
    obj_vram.fill(0);
    for (int x = 0; x < 8; ++x)
        for (int y = 0; y < 8; ++y) set_4bpp_index(obj_vram, x, y, 1);
    nds_gpu2d_set_adaptive_workers(0);

    AdaptiveRun plain{};
    g_scene_tweak = [](Unit&) { g_view.obj[0] = obj_vram.data(); };
    run_adaptive_frame(plain);
    g_scene_tweak = nullptr;

    auto windowed = [&](uint8_t obj_mask, AdaptiveRun& out,
                        NdsGpu2dProfile& prof) {
        g_scene_tweak = [obj_mask](Unit& u) {
            g_view.obj[0] = obj_vram.data();
            u.dispcnt |= NDS_GPU2D_DISPCNT_WINDOW_ENABLE_MASK;
            u.win[8] = 0x3Fu;   // WININ  window 0: everything
            u.win[9] = 0x3Fu;   //        window 1: everything
            u.win[10] = 0x2Fu;  // WINOUT outside: BG0-3 + effects
            u.win[11] = obj_mask;  // OBJ window
            // Rectangles left empty: only the OBJ window is reachable. An
            // 8x8 OBJ-window sprite at native (16, 32) -> wide x 112..119.
            set_obj(2, 0x0020u | (2u << 10u), 0x0010u, 0x0000u);
        };
        run_adaptive_frame(out);
        nds_gpu2d_profile(&prof);
        g_scene_tweak = nullptr;
    };

    constexpr int extra = (448 - 256) / 2;
    AdaptiveRun mkds{};
    NdsGpu2dProfile mkds_prof{};
    windowed(0x32u, mkds, mkds_prof);
    if (!require(plain.width == 448) || !require(mkds.width == 448))
        return false;
    // Composited wide, not centred: the frame counter says so and the 3D
    // margins carry the same content as the unwindowed reference.
    if (!require(mkds_prof.adaptive_fallback_frames[NDS_GPU2D_ADAPTIVE_WIDE] ==
                 1u))
        return false;
    for (int y : {8, 100, 180}) {
        for (int x : {0, 10, extra - 1, extra + 256, 440, 447}) {
            const size_t i = static_cast<size_t>(y) * 448u + x;
            if (!require(mkds.surface[i] == plain.surface[i])) return false;
            if (!require(mkds.surface[i] != 0xFF000000u)) return false;
        }
    }
    // Inside the OBJ window the mask 0x32 removes BG0/3D, so the pixel
    // differs from the reference wherever the 3D layer is opaque there.
    bool masked = false;
    for (int y = 32; y < 40; ++y) {
        for (int x = extra + 16; x < extra + 24; ++x) {
            const size_t i = static_cast<size_t>(y) * 448u + x;
            if (((g_3d_line[x] >> 24) & 0x1Fu) != 0u &&
                mkds.surface[i] != plain.surface[i])
                masked = true;
        }
    }
    if (!require(masked)) return false;
    // Outside the sprite the same rows match the reference exactly.
    for (int y = 32; y < 40; ++y) {
        const size_t i = static_cast<size_t>(y) * 448u + extra + 40;
        if (!require(mkds.surface[i] == plain.surface[i])) return false;
    }
    // HD stands down for the BG0-less OBJ-window mask, but not for one that
    // keeps BG0 (0x33), and the CPU composite is wide in both cases.
    if (!require(plain.hd_valid) || !require(!mkds.hd_valid)) return false;
    AdaptiveRun hd_ok{};
    NdsGpu2dProfile hd_prof{};
    windowed(0x33u, hd_ok, hd_prof);
    return require(hd_ok.width == 448) && require(hd_ok.hd_valid) &&
           require(hd_prof.adaptive_fallback_frames[NDS_GPU2D_ADAPTIVE_WIDE] ==
                   1u);
}

bool test_adaptive_helpers_match_serial() {
    AdaptiveRun serial{};
    nds_gpu2d_set_adaptive_workers(0);
    run_adaptive_frame(serial);

    // Composited repeatedly rather than once. This fixture's frame is small
    // enough that the calling thread can drain all 192 lines before a helper
    // wakes from its condition-variable wait, so a single frame is not a
    // reliable witness that the helpers ran -- and a test that asserts they
    // did on one frame is just flaky. Repeating exercises many different
    // interleavings and gives the pool time to participate at all: every
    // frame must equal the serial reference, and the helpers must have taken
    // lines somewhere across the run.
    constexpr int kFrames = 24;
    uint64_t helper_lines = 0;
    uint64_t band_frames = 0;
    uint64_t serial_frames = 0;
    int mismatched = 0;
    AdaptiveRun threaded{};
    nds_gpu2d_set_adaptive_workers(3);
    for (int i = 0; i < kFrames; ++i) {
        run_adaptive_frame(threaded);
        helper_lines += threaded.helper_lines;
        band_frames += threaded.band_frames;
        serial_frames += threaded.serial_frames;
        if (threaded.surface != serial.surface ||
            threaded.hd_top != serial.hd_top ||
            threaded.hd_below != serial.hd_below ||
            threaded.width != serial.width ||
            threaded.hd_valid != serial.hd_valid)
            ++mismatched;
    }
    nds_gpu2d_set_adaptive_workers(0);

    std::fprintf(stderr,
        "[adaptive] width=%u hd=%d | %d of %d threaded frames differed from "
        "the serial reference\n",
        serial.width, (int)serial.hd_valid, mismatched, kFrames);
    std::fprintf(stderr,
        "[adaptive] serial band=%llu serial_frames=%llu helper_lines=%llu | "
        "threaded band=%llu serial_frames=%llu helper_lines=%llu of %d\n",
        (unsigned long long)serial.band_frames,
        (unsigned long long)serial.serial_frames,
        (unsigned long long)serial.helper_lines,
        (unsigned long long)band_frames,
        (unsigned long long)serial_frames,
        (unsigned long long)helper_lines, kFrames * 192);

    // The scene must be the widened, HD-emitting one, or the comparison is
    // vacuous.
    if (!require(serial.width == 448u)) return false;
    if (!require(serial.hd_valid)) return false;
    // ... and it must not be a flat fill.
    if (!require(std::adjacent_find(serial.surface.begin(),
                                    serial.surface.end(),
                                    std::not_equal_to<uint32_t>()) !=
                 serial.surface.end()))
        return false;
    // With no helpers every line runs on the calling thread.
    if (!require(serial.helper_lines == 0u)) return false;
    if (!require(serial.serial_frames == 1u)) return false;
    // With helpers the band path must be the one taken, and the helpers must
    // have executed lines.
    if (!require(band_frames == static_cast<uint64_t>(kFrames))) return false;
    if (!require(serial_frames == 0u)) return false;
    if (!require(helper_lines > 0u)) return false;
    // The whole point.
    if (!require(mismatched == 0)) return false;
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
bool nds_gpu3d_display_readback_latency() { return false; }
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
    if (!test_adaptive_helpers_match_serial()) return 11;
    if (!test_adaptive_window_x()) return 12;
    if (!test_adaptive_windowed_scene_composites_wide()) return 13;
    return 0;
}
