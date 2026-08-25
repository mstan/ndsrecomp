// Bridge between the runner and the vendored melonDS GPU3D device model.
// Owns the melonDS::NDS shim instance and implements the shim interfaces
// declared in runner/vendor/melonds/{NDS.h, GPU.h, Platform.h} in terms of
// the runner's own device models (io.cpp IRQ/DMA/stall, vram.cpp texture
// slots). The vendored translation units are unmodified melonDS 1.0rc.

#include "gpu3d.h"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include "io.h"
#include "gpu2d.h"
#include "scheduler.h"
#include "state.h"
#include "vram.h"
#include "net/net_ring.h"

#include "NDS.h"
#include "GPU3D_Soft.h"
#if defined(NDS_HAVE_COMPUTE_RENDERER)
#include "GPU3D_Compute.h"
#endif

namespace {

melonDS::NDS g_nds;

int g_log_budget = 64;
bool g_soft_threaded = false;

// Last VRAM texture generation reflected into each flat view (0 = never
// refreshed; the live counter starts at 1).
uint64_t g_texture_flat_gen = 0;
uint64_t g_texpal_flat_gen = 0;

// ~7 frames of scheduler rounds at the 64-cycle rendezvous grid.
constexpr uint32_t kGxRunTraceSize = 65536;
NdsGxRunTraceEntry g_gx_run_trace[kGxRunTraceSize] = {};
uint64_t g_gx_run_trace_count = 0;

constexpr uint32_t kGxWriteTraceSize = 8192;
NdsGxWriteTraceEntry g_gx_write_trace[kGxWriteTraceSize] = {};
uint64_t g_gx_write_trace_count = 0;

NdsGpu3dProfile g_gpu3d_profile{};
using ProfileClock = std::chrono::steady_clock;
bool profiling();
void profile_add(uint64_t& dst, ProfileClock::time_point start);

// Internal-resolution (HD) multiplier for the accelerated renderer. 4x of a
// 448-wide adaptive raster is 1792x768; the compute renderer's tile and span
// buffers grow with it, so this is deliberately capped well below what the
// shader constants would otherwise permit.
constexpr uint8_t kMaxInternalScale = 4u;
uint8_t g_internal_scale = 1u;
#if defined(NDS_HAVE_COMPUTE_RENDERER)
bool g_compute_rendered_frame = false;
bool g_compute_readback_pending = false;
bool g_compute_frame_ready = false;
bool g_compute_shader_setup_failed = false;
bool g_compute_runtime_failed = false;
constexpr uint32_t kComputeMaxWidth = 448u;
uint32_t g_compute_zero_line[kComputeMaxWidth] = {};
uint32_t g_compute_frame[kComputeMaxWidth * 192u] = {};
uint32_t g_compute_attr_frame[kComputeMaxWidth * 192u] = {};
uint32_t g_compute_scrolled_line[kComputeMaxWidth] = {};
uint32_t g_compute_scrolled_attr_line[kComputeMaxWidth] = {};

void clear_compute_gl_errors() {
    while (glGetError() != GL_NO_ERROR) {}
}

bool compute_gl_stage_failed(const char* stage) {
    bool failed = false;
    for (GLenum error = glGetError(); error != GL_NO_ERROR;
         error = glGetError()) {
        std::fprintf(stderr, "[gpu3d] compute %s GL error: 0x%04X\n",
                     stage, static_cast<unsigned>(error));
        failed = true;
    }
    return failed;
}

bool compute_readback_overlap() {
    static const bool enabled = [] {
        const char* value = std::getenv("NDS_COMPUTE_READBACK_OVERLAP");
        if (!value || !*value || std::strcmp(value, "1") == 0) return true;
        if (std::strcmp(value, "0") == 0) return false;
        std::fprintf(stderr,
            "[gpu3d] invalid NDS_COMPUTE_READBACK_OVERLAP "
            "(expected 0/1); using default 1\n");
        return true;
    }();
    return enabled;
}

void compute_readback_failed(const char* stage) {
    g_compute_rendered_frame = false;
    g_compute_readback_pending = false;
    g_compute_frame_ready = false;
    if (!g_compute_runtime_failed) {
        std::fprintf(stderr, "[gpu3d] compute frame %s failed\n", stage);
        g_compute_runtime_failed = true;
    }
    scheduler_terminal_halt_all("compute frame render/readback failure");
}

void compute_submit_readback() {
    if (!g_compute_rendered_frame || g_compute_readback_pending) return;
    const auto start = profiling() ? ProfileClock::now()
                                   : ProfileClock::time_point{};
    auto& renderer = g_nds.GPU.GPU3D.GetCurrentRenderer();
    // Order the compute shader's image stores before queuing the low-resolution
    // texture copy into its pixel-pack buffer.
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    renderer.PrepareCaptureFrame();
    g_compute_rendered_frame = false;
    const bool failed =
        compute_gl_stage_failed("frame render/readback submit");
    if (profiling()) {
        profile_add(g_gpu3d_profile.compute_submit_ns, start);
        profile_add(g_gpu3d_profile.compute_sync_ns, start);
        ++g_gpu3d_profile.compute_submit_calls;
    }
    if (failed) {
        if (profiling()) ++g_gpu3d_profile.compute_sync_calls;
        compute_readback_failed("render/readback submit");
        return;
    }
    g_compute_readback_pending = true;
}

void compute_finish_readback() {
    if (!g_compute_readback_pending) return;
    const auto start = profiling() ? ProfileClock::now()
                                   : ProfileClock::time_point{};
    // PrepareCaptureFrame left the renderer's PBO bound. Mapping waits only
    // for copy work that did not finish during the scanline overlap.
    void* mapped = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    bool valid = mapped != nullptr;
    if (mapped) {
        const size_t frame_bytes =
            static_cast<size_t>(g_nds.GPU.GPU3D.GetRenderWidth()) *
            192u * sizeof(uint32_t);
        std::memcpy(g_compute_frame, mapped, frame_bytes);
        const size_t pixel_count = frame_bytes / sizeof(uint32_t);
        for (size_t i = 0; i < pixel_count; ++i) {
            const uint32_t packed = g_compute_frame[i];
            const uint32_t polygon_id =
                ((packed >> 6) & 0x03u) |
                (((packed >> 14) & 0x03u) << 2) |
                (((packed >> 22) & 0x03u) << 4);
            g_compute_attr_frame[i] = polygon_id << 24;
            g_compute_frame[i] = packed & 0xFF3F3F3Fu;
        }
        if (glUnmapBuffer(GL_PIXEL_PACK_BUFFER) != GL_TRUE) valid = false;
    }
    g_compute_readback_pending = false;
    const bool failed = compute_gl_stage_failed("frame readback map");
    if (profiling()) {
        profile_add(g_gpu3d_profile.compute_map_ns, start);
        profile_add(g_gpu3d_profile.compute_sync_ns, start);
        ++g_gpu3d_profile.compute_map_calls;
        ++g_gpu3d_profile.compute_sync_calls;
    }
    if (!valid || failed) {
        compute_readback_failed("readback map");
        return;
    }
    g_compute_frame_ready = true;
}
#endif

bool profiling() {
    static const bool enabled = std::getenv("NDS_PROFILE_GPU") != nullptr;
    return enabled;
}

void profile_add(uint64_t& dst, ProfileClock::time_point start) {
    dst += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            ProfileClock::now() - start).count());
}

}  // namespace

// ── melonDS::NDS shim methods ───────────────────────────────────────────

namespace melonDS {

// NDS::SetIRQ is shared by every melonDS::NDS instance in this build (the
// GPU3D bridge's own g_nds above, and the Wi-Fi bridge's g_bridge->nds in
// wifi_net.cpp) since the method body touches no instance state. That
// makes this the single, uniform call site every Wi-Fi IRQ assertion
// passes through regardless of which NDS object raised it (Wifi::SetIRQ ->
// NDS.SetIRQ(1, IRQ_Wifi), Wifi.cpp:384-390) -- recording it here, rather
// than as a patch inside vendored Wifi.cpp, needs no vendored-file change
// at all. IRQ_Wifi (melonDS's real IRQ bit 24, NDS.h) is ARM7-only by
// construction (Wifi never raises any other IRQ number), so cpu is always
// 1 here for this event; recorded as `aux` for completeness rather than
// assumed by a reader.
void NDS::SetIRQ(u32 cpu, u32 irq) {
    if (irq == IRQ_Wifi) {
        net_ring_push(NDS_NET_EVENT_WIFI_IRQ, /*direction=host->guest*/1,
                       0, 0, nullptr, nullptr, 0, 0, 0, 0, 0,
                       /*aux=*/cpu);
    }
    nds_raise_irq(static_cast<int>(cpu), 1u << irq);
}

void NDS::ClearIRQ(u32 cpu, u32 irq) { nds_clear_irq(static_cast<int>(cpu), 1u << irq); }

void NDS::CheckDMAs(u32 cpu, u32 mode) { nds_dma_trigger(static_cast<int>(cpu), mode); }

void NDS::GXFIFOStall() { nds_gxfifo_set_stall(true); }

void NDS::GXFIFOUnstall() { nds_gxfifo_set_stall(false); }

// ── melonDS::GPU flat-VRAM coherence ────────────────────────────────────
// The runner has no per-write dirty tracking on the texture slots, so the
// coherence pass refreshes the whole flat view from the live VRAM mapping
// and derives the renderer's "textures changed" input by comparing bytes.

bool GPU::MakeVRAMFlat_TextureCoherent(
    NonStupidBitField<512*1024/VRAMDirtyGranularity>& dirty) noexcept {
    const uint64_t gen = nds_vram_texture_generation();
    if (gen == g_texture_flat_gen) return false;
    g_texture_flat_gen = gen;
    static u8 fresh[512*1024];
    nds_vram_copy_texture(fresh);
    bool changed = false;
    for (size_t offset = 0; offset < sizeof fresh;
         offset += VRAMDirtyGranularity) {
        if (std::memcmp(fresh + offset, VRAMFlat_Texture + offset,
                        VRAMDirtyGranularity) == 0)
            continue;
        dirty[static_cast<u32>(offset / VRAMDirtyGranularity)] = true;
        std::memcpy(VRAMFlat_Texture + offset, fresh + offset,
                    VRAMDirtyGranularity);
        changed = true;
    }
    return changed;
}

bool GPU::MakeVRAMFlat_TexPalCoherent(
    NonStupidBitField<128*1024/VRAMDirtyGranularity>& dirty) noexcept {
    const uint64_t gen = nds_vram_texture_generation();
    if (gen == g_texpal_flat_gen) return false;
    g_texpal_flat_gen = gen;
    static u8 fresh[128*1024];
    nds_vram_copy_texpal(fresh);
    bool changed = false;
    for (size_t offset = 0; offset < sizeof fresh;
         offset += VRAMDirtyGranularity) {
        if (std::memcmp(fresh + offset, VRAMFlat_TexPal + offset,
                        VRAMDirtyGranularity) == 0)
            continue;
        dirty[static_cast<u32>(offset / VRAMDirtyGranularity)] = true;
        std::memcpy(VRAMFlat_TexPal + offset, fresh + offset,
                    VRAMDirtyGranularity);
        changed = true;
    }
    return changed;
}

}  // namespace melonDS

// ── melonDS::Platform shim ──────────────────────────────────────────────
// Real primitives for the vendored renderer's optional host worker thread.
// The runner selects this through SoftRenderer's public API; serve-mode
// parity runs remain single-threaded unless explicitly forced.

namespace melonDS::Platform {

void Log(LogLevel level, const char* fmt, ...) {
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    // OpenGLSupport reports a cache miss at Error level even on the normal
    // successful uncached path. Latch only its real compiler/linker failures
    // so the runner can reject an unusable forced compute backend.
    if (level == Error &&
        (std::strstr(fmt, "OpenGL: failed to compile") != nullptr ||
         std::strstr(fmt, "OpenGL: failed to link") != nullptr ||
         std::strstr(fmt, "OpenGL: Cannot") != nullptr))
        g_compute_shader_setup_failed = true;
#endif
    if (level == Debug && g_log_budget <= 0) return;
    if (level == Debug) --g_log_budget;
    std::fprintf(stderr, "[gpu3d] ");
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
}

struct Thread {
    std::thread t;
};

Thread* Thread_Create(std::function<void()> func) {
    return new Thread{std::thread(std::move(func))};
}

void Thread_Free(Thread* thread) { delete thread; }

void Thread_Wait(Thread* thread) {
    if (thread && thread->t.joinable()) thread->t.join();
}

struct Semaphore {
    std::mutex m;
    std::condition_variable cv;
    int count = 0;
};

Semaphore* Semaphore_Create() { return new Semaphore(); }

void Semaphore_Free(Semaphore* sema) { delete sema; }

void Semaphore_Reset(Semaphore* sema) {
    std::lock_guard<std::mutex> lock(sema->m);
    sema->count = 0;
}

void Semaphore_Wait(Semaphore* sema) {
    std::unique_lock<std::mutex> lock(sema->m);
    sema->cv.wait(lock, [sema] { return sema->count > 0; });
    --sema->count;
}

void Semaphore_Post(Semaphore* sema, int count) {
    std::lock_guard<std::mutex> lock(sema->m);
    sema->count += count;
    sema->cv.notify_all();
}

}  // namespace melonDS::Platform

// ── Runner-facing bridge API ────────────────────────────────────────────

void nds_gpu3d_set_threaded(bool threaded) {
    g_soft_threaded = threaded;
    auto* renderer = dynamic_cast<melonDS::SoftRenderer*>(
        &g_nds.GPU.GPU3D.GetCurrentRenderer());
    if (renderer) renderer->SetThreaded(threaded, g_nds.GPU);
}

void nds_gpu3d_use_soft_renderer(bool threaded) {
    g_soft_threaded = threaded;
    auto* renderer = dynamic_cast<melonDS::SoftRenderer*>(
        &g_nds.GPU.GPU3D.GetCurrentRenderer());
    if (!renderer) {
        auto replacement = std::make_unique<melonDS::SoftRenderer>();
        renderer = replacement.get();
        g_nds.GPU.GPU3D.SetCurrentRenderer(std::move(replacement));
#if defined(NDS_HAVE_COMPUTE_RENDERER)
        g_compute_rendered_frame = false;
        g_compute_readback_pending = false;
        g_compute_frame_ready = false;
#endif
    }
    renderer->SetThreaded(threaded, g_nds.GPU);
}

void nds_gpu3d_restore_soft_renderer() {
    nds_gpu3d_use_soft_renderer(g_soft_threaded);
}

NdsGpu3dRendererPolicy nds_gpu3d_renderer_policy() {
    const char* const value = std::getenv("NDS_3D_RENDERER");
    if (!value || !*value || std::strcmp(value, "auto") == 0)
        return NdsGpu3dRendererPolicy::Auto;
    if (std::strcmp(value, "soft") == 0)
        return NdsGpu3dRendererPolicy::Soft;
    if (std::strcmp(value, "compute") == 0)
        return NdsGpu3dRendererPolicy::Compute;
    return NdsGpu3dRendererPolicy::Invalid;
}

const char* nds_gpu3d_renderer_policy_name(
        NdsGpu3dRendererPolicy policy) {
    switch (policy) {
        case NdsGpu3dRendererPolicy::Auto: return "auto";
        case NdsGpu3dRendererPolicy::Soft: return "soft";
        case NdsGpu3dRendererPolicy::Compute: return "compute";
        default: return "invalid";
    }
}

bool nds_gpu3d_renderer_prefers_compute() {
    const NdsGpu3dRendererPolicy policy = nds_gpu3d_renderer_policy();
    return policy != NdsGpu3dRendererPolicy::Soft &&
           policy != NdsGpu3dRendererPolicy::Invalid &&
           nds_gpu3d_compute_renderer_built();
}

bool nds_gpu3d_renderer_requires_compute() {
    return nds_gpu3d_renderer_policy() ==
           NdsGpu3dRendererPolicy::Compute;
}

bool nds_gpu3d_compute_renderer_built() {
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    return true;
#else
    return false;
#endif
}

bool nds_gpu3d_compute_runtime_failed() {
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    return g_compute_runtime_failed;
#else
    return false;
#endif
}

uint32_t nds_gpu3d_compute_output_texture() {
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    auto* renderer = dynamic_cast<melonDS::ComputeRenderer*>(
        &g_nds.GPU.GPU3D.GetCurrentRenderer());
    return renderer ? renderer->GetLowResTexture() : 0u;
#else
    return 0u;
#endif
}

bool nds_gpu3d_set_internal_scale(uint8_t scale) {
    if (scale < 1u || scale > kMaxInternalScale) return false;
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    // The scale is baked into every compute shader and sizes every
    // framebuffer, so it cannot change under a live renderer.
    if (dynamic_cast<melonDS::ComputeRenderer*>(
            &g_nds.GPU.GPU3D.GetCurrentRenderer()) != nullptr)
        return false;
#endif
    g_internal_scale = scale;
    return true;
}

uint8_t nds_gpu3d_internal_scale() {
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    auto* renderer = dynamic_cast<melonDS::ComputeRenderer*>(
        &g_nds.GPU.GPU3D.GetCurrentRenderer());
    // Report what is actually rendering, not what was requested, so a
    // presenter can never sample a hi-res surface that does not exist.
    if (renderer)
        return static_cast<uint8_t>(renderer->GetScaleFactor());
    return 1u;
#else
    return 1u;
#endif
}

uint32_t nds_gpu3d_compute_output_texture_hires() {
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    auto* renderer = dynamic_cast<melonDS::ComputeRenderer*>(
        &g_nds.GPU.GPU3D.GetCurrentRenderer());
    if (!renderer || renderer->GetScaleFactor() <= 1) return 0u;
    return renderer->GetHiResTexture();
#else
    return 0u;
#endif
}

bool nds_gpu3d_use_compute_renderer() {
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    g_compute_shader_setup_failed = false;
    g_compute_runtime_failed = false;
    clear_compute_gl_errors();
    const uint32_t render_width = g_nds.GPU.GPU3D.GetRenderWidth();
    if (render_width < 256u || render_width > kComputeMaxWidth ||
        (render_width % 64u) != 0u) {
        std::fprintf(stderr,
                     "[gpu3d] compute render width %u is unsupported\n",
                     render_width);
        return false;
    }
    auto renderer = melonDS::ComputeRenderer::New();
    if (!renderer || compute_gl_stage_failed("initialization")) return false;
    // Width is a shader constant and determines every framebuffer/PBO
    // allocation. Establish it before settings allocate or compile anything.
    renderer->SetRenderWidth(render_width);
    // Internal-resolution scaling multiplies sample density only. The native
    // readback surface this bridge maps every frame is still RenderWidth x
    // 192 (the final pass point-samples it out of the scaled raster), so the
    // faithful 2D compositor and display capture see byte-identical input at
    // scale 1. Higher internal-resolution modes use melonDS's sub-native
    // vertex coordinates so the extra samples reduce polygon/texture wobble
    // rather than merely magnifying the native integer grid.
    renderer->SetRenderSettings(
        static_cast<int>(g_internal_scale), g_internal_scale > 1u);
    if (compute_gl_stage_failed("render settings")) return false;
    if (g_internal_scale > 1u)
        std::fprintf(stderr,
                     "[gpu3d] internal resolution %ux (%ux%u 3D raster)\n",
                     static_cast<unsigned>(g_internal_scale),
                     render_width * g_internal_scale,
                     192u * g_internal_scale);
    while (renderer->NeedsShaderCompile()) {
        int current = 0;
        int count = 0;
        renderer->ShaderCompileStep(current, count);
        std::fprintf(stderr, "[gpu3d] compute shader %d/%d\r",
                     current + 1, count);
        if (g_compute_shader_setup_failed ||
            compute_gl_stage_failed("shader setup")) {
            std::fprintf(stderr,
                         "[gpu3d] compute shader setup failed at %d/%d\n",
                         current + 1, count);
            return false;
        }
    }
    std::fprintf(stderr, "[gpu3d] compute shaders ready          \n");
    std::fprintf(stderr, "[gpu3d] compute readback overlap: %s\n",
                 compute_readback_overlap() ? "on" : "off");
    g_nds.GPU.GPU3D.SetCurrentRenderer(std::move(renderer));
    g_compute_rendered_frame = false;
    g_compute_readback_pending = false;
    g_compute_frame_ready = false;
    return true;
#else
    return false;
#endif
}

void nds_gpu3d_profile(NdsGpu3dProfile* out) {
    if (out) *out = g_gpu3d_profile;
}

void nds_gpu3d_state(NdsGxStateSnapshot* out) {
    if (!out) return;
    auto& g3 = g_nds.GPU.GPU3D;
    *out = {
        g3.GeometryEnabled ? 1u : 0u,
        g3.RenderingEnabled ? 1u : 0u,
        g3.GXStat,
        g3.CycleCount,
        g3.CmdFIFO.Level(),
        g3.CmdPIPE.Level(),
        g3.NumPolygons,
        g3.NumVertices,
        g3.FlushRequest,
        g3.NumCommands,
        g3.CurCommand,
        g3.ParamCount,
        g3.TotalParams,
        {g3.Viewport[0], g3.Viewport[1], g3.Viewport[2], g3.Viewport[3],
         g3.Viewport[4], g3.Viewport[5]},
        g3.GetRenderWidth(),
        g3.GetGuestWideProjection() ? 1u : 0u,
    };
}

uint32_t nds_gpu3d_render_polygon_count() {
    return g_nds.GPU.GPU3D.RenderNumPolygons;
}

bool nds_gpu3d_render_polygon(uint32_t index,
                              NdsGpu3dPolygonSnapshot* out) {
    if (!out || index >= g_nds.GPU.GPU3D.RenderNumPolygons)
        return false;
    const melonDS::Polygon* const polygon =
        g_nds.GPU.GPU3D.RenderPolygonRAM[index];
    if (!polygon || polygon->NumVertices == 0u)
        return false;
    const ptrdiff_t absolute_index =
        polygon - g_nds.GPU.GPU3D.PolygonRAM;
    out->submission_index =
        static_cast<uint32_t>(absolute_index) & 0x7FFu;
    out->vertex_count = polygon->NumVertices;
    out->attr = polygon->Attr;
    out->tex_param = polygon->TexParam;
    out->tex_palette = polygon->TexPalette;
    out->min_x = 0x7FFFFFFF;
    out->max_x = -0x7FFFFFFF;
    out->min_y = 0x7FFFFFFF;
    out->max_y = -0x7FFFFFFF;
    out->min_z = 0xFFFFFFFFu;
    out->max_z = 0u;
    for (uint32_t vertex = 0; vertex < polygon->NumVertices; ++vertex) {
        out->min_x = std::min(out->min_x,
                              polygon->Vertices[vertex]->FinalPosition[0]);
        out->max_x = std::max(out->max_x,
                              polygon->Vertices[vertex]->FinalPosition[0]);
        out->min_y = std::min(out->min_y,
                              polygon->Vertices[vertex]->FinalPosition[1]);
        out->max_y = std::max(out->max_y,
                              polygon->Vertices[vertex]->FinalPosition[1]);
        const uint32_t z =
            static_cast<uint32_t>(polygon->FinalZ[vertex]);
        out->min_z = std::min(out->min_z, z);
        out->max_z = std::max(out->max_z, z);
    }
    return true;
}

uint64_t nds_gpu3d_write_trace_count() { return g_gx_write_trace_count; }

bool nds_gpu3d_write_trace_get(uint64_t count, NdsGxWriteTraceEntry* out) {
    if (!out || count == 0) return false;
    const NdsGxWriteTraceEntry& e =
        g_gx_write_trace[(count - 1) % kGxWriteTraceSize];
    if (e.count != count) return false;
    *out = e;
    return true;
}

uint64_t nds_gpu3d_run_trace_count() { return g_gx_run_trace_count; }

bool nds_gpu3d_run_trace_get(uint64_t count, NdsGxRunTraceEntry* out) {
    if (!out || count == 0) return false;
    const NdsGxRunTraceEntry& e = g_gx_run_trace[(count - 1) % kGxRunTraceSize];
    if (e.count != count) return false;
    *out = e;
    return true;
}

void nds_gpu3d_reset() {
    g_nds.ARM9Timestamp = 0;
    g_nds.GPU.GPU3D.Reset();
    // Match the retail/melonDS POWCNT1 reset value 0x820F.
    g_nds.GPU.GPU3D.SetEnabled(true, true);
    std::memset(g_nds.GPU.VRAMFlat_Texture, 0, sizeof g_nds.GPU.VRAMFlat_Texture);
    std::memset(g_nds.GPU.VRAMFlat_TexPal, 0, sizeof g_nds.GPU.VRAMFlat_TexPal);
    g_texture_flat_gen = 0;
    g_texpal_flat_gen = 0;
    std::memset(g_gx_run_trace, 0, sizeof g_gx_run_trace);
    g_gx_run_trace_count = 0;
    std::memset(g_gx_write_trace, 0, sizeof g_gx_write_trace);
    g_gx_write_trace_count = 0;
    g_gpu3d_profile = NdsGpu3dProfile{};
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    g_compute_rendered_frame = false;
    g_compute_readback_pending = false;
    g_compute_frame_ready = false;
#endif
    nds_gxfifo_set_stall(false);
}

bool nds_gpu3d_reg_addr(uint32_t addr) {
    return (addr >= 0x04000060u && addr < 0x04000064u) ||
           (addr >= 0x04000320u && addr < 0x040006A4u);
}

uint32_t nds_gpu3d_read(uint32_t addr, uint32_t width) {
    // melonDS ARM9Timestamp is live during ARM9.Execute, and GXSTAT reads
    // sync the engine to it (GPU3D::Read32 case 0x600 calls Run()). These
    // register accesses only ever come from the ARM9's own slice, where
    // g_runtime_cycles is that live timestamp. A stale value here makes the
    // engine's busy bit linger a round longer than melonDS and desyncs
    // guest poll loops (found via the gx_run/gx_write ring diff, SM64DS
    // 3D init at insn9=55.8M).
    g_nds.ARM9Timestamp = g_runtime_cycles;
    switch (width) {
        case 1:  return g_nds.GPU.GPU3D.Read8(addr);
        case 2:  return g_nds.GPU.GPU3D.Read16(addr);
        default: return g_nds.GPU.GPU3D.Read32(addr);
    }
}

void nds_gpu3d_write(uint32_t addr, uint32_t value, uint32_t width) {
    // Keep the engine's view of ARM9 time live for mid-slice writes too
    // (GXFIFO stall/IRQ/DMA decisions inside the vendored write paths).
    g_nds.ARM9Timestamp = g_runtime_cycles;
    auto& g3 = g_nds.GPU.GPU3D;
    ++g_gx_write_trace_count;
    NdsGxWriteTraceEntry& e =
        g_gx_write_trace[(g_gx_write_trace_count - 1) % kGxWriteTraceSize];
    e = {
        g_gx_write_trace_count, g_runtime_cycles, addr, value, width * 8u,
        g3.GeometryEnabled ? 1u : 0u, g3.GXStat, g3.CmdPIPE.Level(),
        0u, 0u,
    };
    switch (width) {
        case 1:  g3.Write8(addr, static_cast<melonDS::u8>(value)); break;
        case 2:  g3.Write16(addr, static_cast<melonDS::u16>(value)); break;
        default: g3.Write32(addr, value); break;
    }
    e.gxstat_after = g3.GXStat;
    e.pipe_after = g3.CmdPIPE.Level();
}

void nds_gpu3d_set_power(uint16_t powcnt1) {
    g_nds.GPU.GPU3D.SetEnabled((powcnt1 & (1u << 3)) != 0,
                               (powcnt1 & (1u << 2)) != 0);
}

void nds_gpu3d_run(unsigned long long arm9_cycles) {
    g_nds.ARM9Timestamp = arm9_cycles;
    auto& g3 = g_nds.GPU.GPU3D;
    const uint32_t stat_before = g3.GXStat;
    const int32_t cc_before = g3.CycleCount;
    g3.Run();
    ++g_gx_run_trace_count;
    g_gx_run_trace[(g_gx_run_trace_count - 1) % kGxRunTraceSize] = {
        g_gx_run_trace_count, arm9_cycles,
        stat_before, g3.GXStat, cc_before, g3.CycleCount,
    };
}

int32_t nds_gpu3d_cycles_to_run() {
    return g_nds.GPU.GPU3D.CyclesToRunFor();
}

void nds_gpu3d_check_fifo_dma() {
    g_nds.GPU.GPU3D.CheckFIFODMA();
}

void nds_gpu3d_check_fifo_irq() {
    g_nds.GPU.GPU3D.CheckFIFOIRQ();
}

void nds_gpu3d_vcount144() {
    if (!profiling()) {
        g_nds.GPU.GPU3D.VCount144(g_nds.GPU);
        return;
    }
    const auto start = ProfileClock::now();
    g_nds.GPU.GPU3D.VCount144(g_nds.GPU);
    profile_add(g_gpu3d_profile.vcount144_ns, start);
    ++g_gpu3d_profile.vcount144_calls;
}

void nds_gpu3d_vblank() {
    g_nds.GPU.GPU3D.VBlank();
}

void nds_gpu3d_vcount215() {
    if (!profiling()) {
        g_nds.GPU.GPU3D.VCount215(g_nds.GPU);
#if defined(NDS_HAVE_COMPUTE_RENDERER)
        if (g_nds.GPU.GPU3D.IsRendererAccelerated())
            g_compute_rendered_frame = true;
        if (g_compute_rendered_frame && compute_readback_overlap() &&
            nds_gpu2d_requires_3d_readback())
            compute_submit_readback();
#endif
        return;
    }
    const auto start = ProfileClock::now();
    g_nds.GPU.GPU3D.VCount215(g_nds.GPU);
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    if (g_nds.GPU.GPU3D.IsRendererAccelerated())
        g_compute_rendered_frame = true;
#endif
    profile_add(g_gpu3d_profile.vcount215_ns, start);
    ++g_gpu3d_profile.vcount215_calls;
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    if (g_compute_rendered_frame && compute_readback_overlap() &&
        nds_gpu2d_requires_3d_readback())
        compute_submit_readback();
#endif
}

const uint32_t* nds_gpu3d_line(int line) {
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    if (g_nds.GPU.GPU3D.IsRendererAccelerated()) {
        const uint32_t width = g_nds.GPU.GPU3D.GetRenderWidth();
        if (!g_compute_frame_ready || g_nds.GPU.GPU3D.AbortFrame)
            return g_compute_zero_line + (width - 256u) / 2u;
        const uint32_t* raw = &g_compute_frame[width * line];
        const uint16_t xpos = g_nds.GPU.GPU3D.GetRenderXPos();
        if (xpos == 0) return raw + (width - 256u) / 2u;
        if (xpos & 0x100u) {
            int i = 0;
            int shift = 512 - xpos;
            if (shift > static_cast<int>(width))
                shift = static_cast<int>(width);
            for (; i < shift; ++i) g_compute_scrolled_line[i] = 0;
            for (int j = 0; i < static_cast<int>(width); ++i, ++j)
                g_compute_scrolled_line[i] = raw[j];
        } else {
            int i = 0;
            int j = xpos;
            for (; j < static_cast<int>(width); ++i, ++j)
                g_compute_scrolled_line[i] = raw[j];
            for (; i < static_cast<int>(width); ++i)
                g_compute_scrolled_line[i] = 0;
        }
        return g_compute_scrolled_line + (width - 256u) / 2u;
    }
#endif
    if (!profiling()) {
        const uint32_t* result = g_nds.GPU.GPU3D.GetLine(line);
        return result + (g_nds.GPU.GPU3D.GetRenderWidth() - 256u) / 2u;
    }
    const auto start = ProfileClock::now();
    const uint32_t* result = g_nds.GPU.GPU3D.GetLine(line);
    profile_add(g_gpu3d_profile.getline_ns, start);
    ++g_gpu3d_profile.getline_calls;
    return result + (g_nds.GPU.GPU3D.GetRenderWidth() - 256u) / 2u;
}

bool nds_gpu3d_set_output_width(uint16_t width) {
    if (width < 256u || width > 448u || (width & 1u)) return false;
    if (g_nds.GPU.GPU3D.IsRendererAccelerated() &&
        width != g_nds.GPU.GPU3D.GetRenderWidth())
        return false;
    g_nds.GPU.GPU3D.SetRenderWidth(width);
    return true;
}

uint16_t nds_gpu3d_output_width() {
    return static_cast<uint16_t>(g_nds.GPU.GPU3D.GetRenderWidth());
}

void nds_gpu3d_set_guest_wide_projection(bool enabled) {
    g_nds.GPU.GPU3D.SetGuestWideProjection(enabled);
}

bool nds_gpu3d_guest_wide_projection() {
    return g_nds.GPU.GPU3D.GetGuestWideProjection();
}

const uint32_t* nds_gpu3d_wide_line(int line) {
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    if (g_nds.GPU.GPU3D.IsRendererAccelerated()) {
        const uint32_t width = g_nds.GPU.GPU3D.GetRenderWidth();
        if (!g_compute_frame_ready || g_nds.GPU.GPU3D.AbortFrame)
            return g_compute_zero_line;
        const uint32_t* raw = &g_compute_frame[width * line];
        const uint16_t xpos = g_nds.GPU.GPU3D.GetRenderXPos();
        if (xpos == 0) return raw;
        if (xpos & 0x100u) {
            int i = 0;
            int shift = 512 - xpos;
            if (shift > static_cast<int>(width))
                shift = static_cast<int>(width);
            for (; i < shift; ++i) g_compute_scrolled_line[i] = 0;
            for (int j = 0; i < static_cast<int>(width); ++i, ++j)
                g_compute_scrolled_line[i] = raw[j];
        } else {
            int i = 0;
            int j = xpos;
            for (; j < static_cast<int>(width); ++i, ++j)
                g_compute_scrolled_line[i] = raw[j];
            for (; i < static_cast<int>(width); ++i)
                g_compute_scrolled_line[i] = 0;
        }
        return g_compute_scrolled_line;
    }
#endif
    return g_nds.GPU.GPU3D.GetLine(line);
}

const uint32_t* nds_gpu3d_wide_attr_line(int line) {
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    if (g_nds.GPU.GPU3D.IsRendererAccelerated()) {
        const uint32_t width = g_nds.GPU.GPU3D.GetRenderWidth();
        if (!g_compute_frame_ready || g_nds.GPU.GPU3D.AbortFrame)
            return g_compute_zero_line;
        const uint32_t* raw = &g_compute_attr_frame[width * line];
        const uint16_t xpos = g_nds.GPU.GPU3D.GetRenderXPos();
        if (xpos == 0) return raw;
        if (xpos & 0x100u) {
            int i = 0;
            int shift = 512 - xpos;
            if (shift > static_cast<int>(width))
                shift = static_cast<int>(width);
            for (; i < shift; ++i) g_compute_scrolled_attr_line[i] = 0;
            for (int j = 0; i < static_cast<int>(width); ++i, ++j)
                g_compute_scrolled_attr_line[i] = raw[j];
        } else {
            int i = 0;
            int j = xpos;
            for (; j < static_cast<int>(width); ++i, ++j)
                g_compute_scrolled_attr_line[i] = raw[j];
            for (; i < static_cast<int>(width); ++i)
                g_compute_scrolled_attr_line[i] = 0;
        }
        return g_compute_scrolled_attr_line;
    }
#endif
    return g_nds.GPU.GPU3D.GetAttrLine(line);
}

void nds_gpu3d_set_render_xpos(uint16_t value) {
    g_nds.GPU.GPU3D.SetRenderXPos(value);
}

uint16_t nds_gpu3d_render_xpos() {
    return g_nds.GPU.GPU3D.GetRenderXPos();
}

void nds_gpu3d_start_frame() {
    if (g_nds.GPU.GPU3D.AbortFrame) {
        g_nds.GPU.GPU3D.RestartFrame(g_nds.GPU);
        g_nds.GPU.GPU3D.AbortFrame = false;
    }
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    if (g_nds.GPU.GPU3D.IsRendererAccelerated()) {
        // Reference mode reproduces the original immediate submit+map here.
        // Forced overlap queues at VCount215 and pays only any unfinished
        // portion of the copy at the next frame boundary.
        if (g_compute_readback_pending) compute_finish_readback();
        if (g_compute_rendered_frame) {
            if (nds_gpu2d_requires_3d_readback())
                compute_submit_readback();
            else
                g_compute_rendered_frame = false;
        }
        if (g_compute_readback_pending) compute_finish_readback();
    }
#endif
}
