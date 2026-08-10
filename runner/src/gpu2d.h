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
    NDS_GPU2D_DIRECT_EXTRA_BG,
    NDS_GPU2D_DIRECT_WIDTH,
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
