#pragma once

#include <atomic>
#include <cstdint>

// ---- Threaded scanline rendering -----------------------------------------
// Per-scanline rendering can run on worker threads driven from a per-line
// latch taken on the scheduler thread. The latch owns every piece of
// renderer-mutable state (BG2/BG3 affine accumulators, the DISPCAPCNT enable
// latch) and the 3D line fetch; workers are pure producers of host pixels.
//
// The renderer still reads guest VRAM, palette and OAM live, so any guest
// write to those regions -- or a VRAMCNT remap -- must not be applied while
// jobs are outstanding. Call nds_gpu2d_memory_fence() before applying such a
// write; it is a single relaxed load in the common case.
//
// See docs/device_work_parallelization.md.
extern std::atomic<uint32_t> nds_gpu2d_jobs_outstanding;
// Non-zero only while a display-capture line has been rendered but its write
// into guest VRAM has not been applied yet. Guest READS of VRAM must fence on
// this; guest writes already fence on nds_gpu2d_jobs_outstanding.
extern std::atomic<uint32_t> nds_gpu2d_staged_captures;
enum NdsGpu2dFenceCause : uint32_t {
    NDS_GPU2D_FENCE_VRAM = 0,
    NDS_GPU2D_FENCE_VRAMCNT,
    NDS_GPU2D_FENCE_PALETTE,
    NDS_GPU2D_FENCE_OAM,
    NDS_GPU2D_FENCE_FRAME,
    NDS_GPU2D_FENCE_PRESENT,
    NDS_GPU2D_FENCE_SLOTS,
    NDS_GPU2D_FENCE_CAPTURE,
    NDS_GPU2D_FENCE_CAUSE_COUNT,
};
const char* nds_gpu2d_fence_cause_name(uint32_t index);
// Blocks until every outstanding line job has been rendered, executing
// unclaimed jobs on the calling (scheduler) thread rather than idling.
void nds_gpu2d_drain(uint32_t cause);
inline void nds_gpu2d_memory_fence(uint32_t cause) {
    if (nds_gpu2d_jobs_outstanding.load(std::memory_order_relaxed) != 0u)
        nds_gpu2d_drain(cause);
}
// Read-side fence: a guest read of VRAM must not observe memory that a staged
// capture write has not been applied to yet.
inline void nds_gpu2d_read_fence() {
    if (nds_gpu2d_staged_captures.load(std::memory_order_relaxed) != 0u)
        nds_gpu2d_drain(NDS_GPU2D_FENCE_CAPTURE);
}
// Startup configuration. Threading is off by default; the inline path is the
// same latch/execute sequence with the execute taken immediately.
void nds_gpu2d_set_threaded(bool enabled, unsigned workers);
bool nds_gpu2d_threaded();
// Helper threads for the adaptive (widened) presentation compositor, which is
// a separate, purely presentation-side per-line pass run at present time --
// not part of the scanline ring. 0 keeps it entirely on the calling thread.
// The calling thread always participates and always waits for completion, so
// this changes only where the work runs, never which frame is presented.
void nds_gpu2d_set_adaptive_workers(unsigned helpers);
unsigned nds_gpu2d_adaptive_workers();
void nds_gpu2d_shutdown_workers();

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
// Same snapshot without the consumption counter bump, for observers (the
// presented-frame digest ring) that must not look like a presenter.
bool nds_gpu2d_hd_frame_peek(NdsGpu2dHdFrame* out);
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
    // Threaded-render accounting. Always maintained (not gated on
    // NDS_PROFILE_GPU) because fence frequency is the whole performance
    // question and must be observable on any run.
    uint64_t threaded_lines;
    uint64_t inline_lines;
    uint64_t fence_drains[NDS_GPU2D_FENCE_CAUSE_COUNT];
    uint64_t fenced_lines[NDS_GPU2D_FENCE_CAUSE_COUNT];
    uint64_t fence_wait_ns;
    uint64_t fence_helped_lines;
    uint64_t staged_captures;
    // Adaptive (widened presentation) compositor band pool: frames composited
    // with worker help, frames that had to stay serial because no wide-3D
    // snapshot matched, and lines the helpers took off the calling thread.
    uint64_t adaptive_band_frames;
    uint64_t adaptive_serial_frames;
    uint64_t adaptive_helper_lines;
};
void nds_gpu2d_profile(NdsGpu2dProfile* out);
void nds_gpu2d_profile_reset();
