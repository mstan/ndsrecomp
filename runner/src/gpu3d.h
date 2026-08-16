#pragma once

#include <cstdint>

// Bridge to the vendored melonDS GPU3D geometry/rasterizer device model
// (runner/vendor/melonds/, GPL-3.0-or-later — see THIRD_PARTY_ATTRIBUTION.md).
// The guest produces every register/GXFIFO write; this is a device model,
// not HLE.

void nds_gpu3d_reset();

// Host scheduling policy for the vendored soft rasterizer. Threading changes
// only where the byte-identical renderer work runs; guest-visible device
// timing and state remain on the scheduler thread.
void nds_gpu3d_set_threaded(bool threaded);
void nds_gpu3d_use_soft_renderer(bool threaded);
// Restore the faithful renderer with the last host-threading preference set
// by nds_gpu3d_set_threaded/use_soft_renderer. Auto-selection uses this after
// an OpenGL startup failure.
void nds_gpu3d_restore_soft_renderer();

enum class NdsGpu3dRendererPolicy : uint8_t {
    Auto,
    Soft,
    Compute,
    Invalid,
};

// NDS_3D_RENDERER is intentionally an override: an absent/empty value means
// Auto, which prefers OpenGL ComputeRenderer when it was built and otherwise
// retains threaded software. Explicit Compute is fail-loud.
NdsGpu3dRendererPolicy nds_gpu3d_renderer_policy();
const char* nds_gpu3d_renderer_policy_name(NdsGpu3dRendererPolicy policy);
bool nds_gpu3d_renderer_prefers_compute();
bool nds_gpu3d_renderer_requires_compute();

// Performance-HLE renderer. The OpenGL context and function loader
// must be current before activation. Failure leaves the faithful soft
// renderer selected. use_soft_renderer() explicitly restores the fallback.
bool nds_gpu3d_use_compute_renderer();
bool nds_gpu3d_compute_renderer_built();
bool nds_gpu3d_compute_runtime_failed();
// Host presentation handle for the accelerated renderer's integer
// RGB6/alpha5 surface. Valid only while its OpenGL context is current.
uint32_t nds_gpu3d_compute_output_texture();

// Opt-in internal-resolution (HD) scaling for the accelerated renderer.
//
// This multiplies 3D sample density only. The native RenderWidth x 192
// surface every faithful consumer reads -- the 2D compositor's scanline
// fetches, display capture, and the wide_line/wide_attr_line readback -- is
// still produced by the same render pass, point-sampled at the top-left
// texel of each scale x scale block, so it is bit-identical to what scale 1
// produces and no accuracy path observes the change. Only the direct GPU
// presenter consumes the extra density.
//
// Must be set before nds_gpu3d_use_compute_renderer(): the scale is a baked
// shader constant and sizes every framebuffer allocation. Returns false for
// an out-of-range scale (valid range 1..4) or after the renderer exists.
bool nds_gpu3d_set_internal_scale(uint8_t scale);
uint8_t nds_gpu3d_internal_scale();
// Presentation handle for the high-resolution RGBA8 surface, whose channels
// carry the same RGB6/alpha5 values as the integer surface above, normalized
// by 63 and 31. Zero unless an accelerated renderer is active at scale > 1.
uint32_t nds_gpu3d_compute_output_texture_hires();

struct NdsGpu3dProfile {
    uint64_t vcount215_ns;
    uint64_t vcount215_calls;
    uint64_t getline_ns;
    uint64_t getline_calls;
    uint64_t vcount144_ns;
    uint64_t vcount144_calls;
    // Accelerated frame-boundary barrier + capture submission + PBO readback.
    // This is where ComputeRenderer's GPU wait/readback is paid.
    uint64_t compute_sync_ns;
    uint64_t compute_sync_calls;
    uint64_t compute_submit_ns;
    uint64_t compute_submit_calls;
    uint64_t compute_map_ns;
    uint64_t compute_map_calls;
};
void nds_gpu3d_profile(NdsGpu3dProfile* out);

// ARM9 3D register window: DISP3DCNT (0x04000060..63) plus the melonDS
// dispatch range 0x04000320..0x040006A3 (tables, clear/fog, GXFIFO at
// 0x400..0x43F, direct command ports at 0x440..0x5CB, GXSTAT/results at
// 0x600..0x6A3).
bool nds_gpu3d_reg_addr(uint32_t addr);
uint32_t nds_gpu3d_read(uint32_t addr, uint32_t width);
void nds_gpu3d_write(uint32_t addr, uint32_t value, uint32_t width);

// POWCNT1 (ARM9 0x04000304): bit3 enables the geometry engine, bit2 the
// rendering engine, matching melonDS GPU::SetPowerCnt.
void nds_gpu3d_set_power(uint16_t powcnt1);

// Geometry engine clock. Called once per scheduler round after the ARM9
// phase and its timer catch-up, mirroring melonDS NDS::RunSystem
// (ARM9.Execute / DMA / GXFIFO-stall advance, then RunTimers(0), then
// GPU.GPU3D.Run()). arm9_cycles is the ARM9 timestamp in its 2x domain.
void nds_gpu3d_run(unsigned long long arm9_cycles);

// While the GXFIFO stall owns the ARM9, the CPU timestamp advances by the
// engine's pending cycle debt instead of executing (melonDS CPUStop_GXStall
// branch). Returns GPU3D::CyclesToRunFor() in system cycles.
int32_t nds_gpu3d_cycles_to_run();

// GXFIFO DMA (ARM9 start mode 7): enabling the channel or exhausting a
// 112-unit iteration re-arms through the engine's FIFO-level check rather
// than starting unconditionally (melonDS DMA::WriteCnt / DMA::Run9).
void nds_gpu3d_check_fifo_dma();

// Level-triggered GXFIFO IRQ re-evaluation, called from the ARM9 IF
// acknowledge path (melonDS ARM9IOWrite16/32 case 0x04000214/216).
void nds_gpu3d_check_fifo_irq();

// Display hooks, in melonDS GPU scanline order: VCount144 (renderer sync
// point), VBlank at line 192 after the 2D engines (polygon sort + render
// state latch + bank flip), VCount215 (rasterize the latched frame),
// frame start (AbortFrame -> RestartFrame).
void nds_gpu3d_vcount144();
void nds_gpu3d_vblank();
void nds_gpu3d_vcount215();
void nds_gpu3d_start_frame();

// Rasterized 3D scanline for the 2D compositor (GPU3D::GetLine: applies the
// RenderXPos horizontal scroll; zero-filled while AbortFrame is set). The
// returned 256-entry buffer is valid until the next call.
const uint32_t* nds_gpu3d_line(int line);

// Opt-in host-only widened raster surface. The native line API above always
// returns the centered 256-pixel hardware viewport for the faithful 2D
// compositor; wide_line exposes the complete enhanced surface.
bool nds_gpu3d_set_output_width(uint16_t width);
uint16_t nds_gpu3d_output_width();
const uint32_t* nds_gpu3d_wide_line(int line);
const uint32_t* nds_gpu3d_wide_attr_line(int line);

// Engine A BG0HOFS writes dual-purpose into the 3D scroll register
// (melonDS GPU2D Write8/16 case 0x010, forwarded before the power gate;
// SetRenderXPos itself ignores writes while rendering is powered off).
void nds_gpu3d_set_render_xpos(uint16_t value);
uint16_t nds_gpu3d_render_xpos();

// Always-on ring of geometry-engine Run() invocations. The engine's
// guest-visible busy/drain state depends on WHEN Run() is called (melonDS
// FinishWork is call-time dependent), so scheduler round boundaries must
// match melonDS's iteration boundaries exactly during 3D activity; this ring
// is the native half of that comparison. `count` is the absolute invocation
// ordinal from reset.
struct NdsGxRunTraceEntry {
    uint64_t count;
    uint64_t arm9_cycles;    // ARM9Timestamp handed to Run() (2x domain)
    uint32_t gxstat_before;
    uint32_t gxstat_after;
    int32_t cycle_count_before;  // engine CycleCount before Run()
    int32_t cycle_count_after;
};
uint64_t nds_gpu3d_run_trace_count();
bool nds_gpu3d_run_trace_get(uint64_t count, NdsGxRunTraceEntry* out);

// Always-on ring of ARM9 writes into the 3D register window, with the
// engine's gate/queue state at that instant. Mirrored by ndsref.
struct NdsGxWriteTraceEntry {
    uint64_t count;
    uint64_t arm9_cycles;
    uint32_t addr;
    uint32_t val;
    uint32_t width;
    uint32_t geometry_enabled;
    uint32_t gxstat;
    uint32_t pipe_level;
    uint32_t gxstat_after;
    uint32_t pipe_after;
};
uint64_t nds_gpu3d_write_trace_count();
bool nds_gpu3d_write_trace_get(uint64_t count, NdsGxWriteTraceEntry* out);

// Engine-internal snapshot for the debug server (fields that guest register
// reads cannot observe directly), mirrored by ndsref's gx_state command.
struct NdsGxStateSnapshot {
    uint32_t geometry_enabled;
    uint32_t rendering_enabled;
    uint32_t gxstat;
    int32_t cycle_count;
    uint32_t fifo_level;
    uint32_t pipe_level;
    uint32_t num_polygons;
    uint32_t num_vertices;
    uint32_t flush_request;
    // Packed GXFIFO command-word protocol state (persists across writes).
    uint32_t num_commands;
    uint32_t cur_command;
    uint32_t param_count;
    uint32_t total_params;
};
void nds_gpu3d_state(NdsGxStateSnapshot* out);

// Read-only inspection of the most recently submitted render list. `index`
// follows the renderer's draw order; submission_index recovers the guest FIFO
// order before opaque/translucent partitioning and Y sorting.
struct NdsGpu3dPolygonSnapshot {
    uint32_t submission_index;
    uint32_t vertex_count;
    uint32_t attr;
    uint32_t tex_param;
    uint32_t tex_palette;
    int32_t min_x;
    int32_t max_x;
    int32_t min_y;
    int32_t max_y;
    uint32_t min_z;
    uint32_t max_z;
};
uint32_t nds_gpu3d_render_polygon_count();
bool nds_gpu3d_render_polygon(uint32_t index,
                              NdsGpu3dPolygonSnapshot* out);
