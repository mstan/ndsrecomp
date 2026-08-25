#pragma once

#include <cstdint>

void nds_gpu2d_reset();
// Console power-off clears both physical front/back buffers without resetting
// GPU register state, matching melonDS GPU::Stop().
void nds_gpu2d_stop();
uint32_t nds_gpu2d_read(uint32_t addr, uint32_t width);
void nds_gpu2d_write(uint32_t addr, uint32_t value, uint32_t width);
void nds_gpu2d_render_scanline(int line);
void nds_gpu2d_render_frame();
void nds_gpu2d_start_frame();
void nds_gpu2d_finish_frame();
// Line-192 hook: auto-clears a latched DISPCAPCNT enable (melonDS
// Unit::VBlank). Called before the 3D engine's VBlank, matching melonDS
// StartScanline order.
void nds_gpu2d_vblank();
const uint32_t* nds_gpu2d_framebuffer(int screen);
// Host-only adaptive presentation surface. Returns the native framebuffer
// with width=256 when no enhanced surface is active for this physical LCD.
const uint32_t* nds_gpu2d_adaptive_framebuffer(int screen, uint16_t* width);
// Title capability for repairing uncovered pixels at a cylindrical skybox
// seam after the host widens the horizontal field of view.
void nds_gpu2d_set_adaptive_skybox_fill(bool enabled);
// Title-audited transparent text HUD composition for wide 3D scenes.
void nds_gpu2d_set_adaptive_hud_anchor(bool enabled,
                                       uint16_t center_width);
// Title-owned escape hatch for adaptive scenes whose widened 3D projection has
// not been audited yet. The presenter still uses the adaptive window width,
// but the source pixels remain a centered native-width composite.
void nds_gpu2d_set_adaptive_center_native(bool enabled);
void nds_gpu2d_set_adaptive_center_max_polygons(uint32_t max_polygons);

// Narrow host-only seam used by the accelerated separate-window presenter.
// The eligibility decision is latched at frame start. While active, engine A
// has no guest-visible display-capture consumer, so its CPU framebuffer can
// be replaced by this packed OBJ/HUD description over the GPU-resident 3D
// surface. Any unsupported state keeps the normal CPU compositor/readback.
struct NdsGpu2dDirectFrame {
    const uint32_t* object_pixels;  // RGBA8UI: RGB6/priority + OBJ alpha code
    uint16_t width;
    uint32_t backdrop_color;        // RGB6 channels in bits 0/8/16
    uint16_t bldcnt;
    uint16_t master_bright;
    uint8_t eva;
    uint8_t evb;
    uint8_t evy;
    uint8_t priority_3d;
    uint16_t render_xpos;
};
// Host-only internal-resolution (HD) composite description, emitted by the
// adaptive wide compositor.
//
// The DS resolves its layer stack per pixel to a "top" and a "below", then
// blends them. Only one layer in that stack -- the 3D layer -- has any extra
// sample density available. So the compositor resolves the stack with the 3D
// layer REMOVED and hands the presenter the top two survivors per native
// pixel; the presenter re-inserts the 3D layer at its own resolution, using
// the same (priority, order) comparison, and finishes the blend.
//
// This is exact rather than an approximation: (priority, order) is a total
// order over the layers -- OBJ 0, BG0/3D 1, BG1..3 2..4, backdrop 5 -- so
// selecting the top two is independent of insertion order, and inserting the
// 3D layer last yields the same pair the CPU path computes.
//
// The 2D layers stay at native density, which is what the hardware draws;
// only the 3D layer gains resolution.
struct NdsGpu2dHdFrame {
    // RG32UI pairs, width x 192: [0] = RGB6 colour (channels at bits
    // 0/8/16), [1] = BLDCNT target in bits 0..5 | effects enable bit 7 |
    // alpha << 8 | priority << 16 | order << 24. Alpha follows the CPU Pixel
    // convention: 0 = no alpha blend, 1..16 = OBJ alpha, 0xFF = the BLDCNT
    // EVA/EVB pair.
    const uint32_t* top_pixels;
    const uint32_t* below_pixels;
    uint16_t width;
    uint8_t priority_3d;
    uint8_t order_3d;
    uint16_t bldcnt;
    uint16_t master_bright;
    uint8_t eva;
    uint8_t evb;
    uint8_t evy;
    // The hi-res 3D surface is unscrolled, unlike the CPU wide line which has
    // RenderXPos already applied, so the presenter applies it itself.
    uint16_t render_xpos;
};
// Enabled only while internal resolution > 1. Emission costs an extra pair of
// stores per pixel in the adaptive loop, so it stays off at 1x.
void nds_gpu2d_set_hd_emit(bool enabled);
// Valid only after nds_gpu2d_adaptive_framebuffer() has run for this frame,
// which is where the surfaces are filled.
bool nds_gpu2d_hd_frame(NdsGpu2dHdFrame* out);
// Must be called by the frontend once per presented frame, before deciding
// whether to run the adaptive compositor at all. The adaptive path is skipped
// entirely on direct-present frames, so without this the surfaces from the
// last non-direct frame stay marked valid and the presenter composites live
// 3D against a stale 2D stack and stale blend registers.
void nds_gpu2d_invalidate_hd_frame();

void nds_gpu2d_set_direct_present(bool enabled);
bool nds_gpu2d_direct_frame_active();
// Presentation trails rasterization by one frame boundary. This reports the
// eligibility latched for the framebuffer currently exposed by
// nds_gpu2d_framebuffer(), rather than the frame now being rasterized.
bool nds_gpu2d_direct_present_frame_active();
bool nds_gpu2d_direct_frame(NdsGpu2dDirectFrame* out);
bool nds_gpu2d_requires_3d_readback();
void nds_gpu2d_force_cpu_frames(uint32_t frames);

enum NdsGpu2dDirectClass : uint8_t {
    NDS_GPU2D_DIRECT_SUPPORTED = 0,
    NDS_GPU2D_DIRECT_DISABLED,
    NDS_GPU2D_DIRECT_FORCE_CPU,
    NDS_GPU2D_DIRECT_SCREEN_ROUTE,
    NDS_GPU2D_DIRECT_ENGINE_OFF,
    NDS_GPU2D_DIRECT_CAPTURE,
    NDS_GPU2D_DIRECT_RENDERER_VIEW,
    NDS_GPU2D_DIRECT_FORCE_BLANK,
    NDS_GPU2D_DIRECT_DISPLAY_MODE,
    NDS_GPU2D_DIRECT_NO_BG0_3D,
    NDS_GPU2D_DIRECT_WINDOWS,
    NDS_GPU2D_DIRECT_EXTRA_BG,
    NDS_GPU2D_DIRECT_WIDTH,
    NDS_GPU2D_DIRECT_RENDER_XPOS,
    NDS_GPU2D_DIRECT_CENTER_NATIVE,
    NDS_GPU2D_DIRECT_CLASS_COUNT,
};
const char* nds_gpu2d_direct_class_name(uint32_t index);
constexpr uint32_t NDS_GPU2D_DIRECT_BG_MASK_COUNT = 8;
constexpr uint32_t NDS_GPU2D_DIRECT_BG_MODE_COUNT = 8;
constexpr uint32_t NDS_GPU2D_DIRECT_EFFECT_MODE_COUNT = 4;

struct NdsGpu2dProfile {
    uint64_t render_ns;
    uint64_t obj_ns;
    uint64_t engine_ns[2];
    uint64_t text_lines[2][5];
    uint64_t no_effect_lines[2];
    uint64_t scanlines;
    uint64_t direct_frames;
    // Frames the adaptive compositor emitted HD layer surfaces for.
    uint64_t hd_frames;
    // Frames the presenter actually consumed those surfaces for.
    uint64_t hd_presented;
    uint64_t direct_overlay_ns;
    uint64_t direct_class_frames[NDS_GPU2D_DIRECT_CLASS_COUNT];
    uint64_t direct_class_engine_a_ns[NDS_GPU2D_DIRECT_CLASS_COUNT];
    uint64_t direct_class_transitions;
    uint64_t direct_extra_bg_mask_frames[NDS_GPU2D_DIRECT_BG_MASK_COUNT];
    uint64_t direct_extra_bg_mask_engine_a_ns[
        NDS_GPU2D_DIRECT_BG_MASK_COUNT];
    uint64_t direct_extra_bg_mode_frames[NDS_GPU2D_DIRECT_BG_MODE_COUNT];
    uint64_t direct_extra_effect_frames[
        NDS_GPU2D_DIRECT_EFFECT_MODE_COUNT];
    uint64_t direct_extra_master_bright_frames[
        NDS_GPU2D_DIRECT_EFFECT_MODE_COUNT];
};
void nds_gpu2d_profile(NdsGpu2dProfile* out);
