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
#include "state.h"
#include "vram.h"

#include "NDS.h"
#include "GPU3D_Soft.h"
#if defined(NDS_HAVE_COMPUTE_RENDERER)
#include "GPU3D_Compute.h"
#endif

namespace {

melonDS::NDS g_nds;

int g_log_budget = 64;

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
#if defined(NDS_HAVE_COMPUTE_RENDERER)
bool g_compute_rendered_frame = false;
bool g_compute_frame_ready = false;
uint32_t g_compute_zero_line[256] = {};
#endif

bool profiling() {
    static const bool enabled = std::getenv("NDS_PROFILE_GPU") != nullptr;
    return enabled;
}

using ProfileClock = std::chrono::steady_clock;

void profile_add(uint64_t& dst, ProfileClock::time_point start) {
    dst += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            ProfileClock::now() - start).count());
}

}  // namespace

// ── melonDS::NDS shim methods ───────────────────────────────────────────

namespace melonDS {

void NDS::SetIRQ(u32 cpu, u32 irq) { nds_raise_irq(static_cast<int>(cpu), 1u << irq); }

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
    auto* renderer = dynamic_cast<melonDS::SoftRenderer*>(
        &g_nds.GPU.GPU3D.GetCurrentRenderer());
    if (renderer) renderer->SetThreaded(threaded, g_nds.GPU);
}

void nds_gpu3d_use_soft_renderer(bool threaded) {
    auto* renderer = dynamic_cast<melonDS::SoftRenderer*>(
        &g_nds.GPU.GPU3D.GetCurrentRenderer());
    if (!renderer) {
        auto replacement = std::make_unique<melonDS::SoftRenderer>();
        renderer = replacement.get();
        g_nds.GPU.GPU3D.SetCurrentRenderer(std::move(replacement));
#if defined(NDS_HAVE_COMPUTE_RENDERER)
        g_compute_rendered_frame = false;
        g_compute_frame_ready = false;
#endif
    }
    renderer->SetThreaded(threaded, g_nds.GPU);
}

bool nds_gpu3d_compute_renderer_built() {
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    return true;
#else
    return false;
#endif
}

bool nds_gpu3d_use_compute_renderer() {
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    auto renderer = melonDS::ComputeRenderer::New();
    if (!renderer) return false;
    renderer->SetRenderSettings(1, false);
    while (renderer->NeedsShaderCompile()) {
        int current = 0;
        int count = 0;
        renderer->ShaderCompileStep(current, count);
        std::fprintf(stderr, "[gpu3d] compute shader %d/%d\r",
                     current + 1, count);
    }
    std::fprintf(stderr, "[gpu3d] compute shaders ready          \n");
    g_nds.GPU.GPU3D.SetCurrentRenderer(std::move(renderer));
    g_compute_rendered_frame = false;
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
    };
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
    // nds_io_reset resets POWCNT1 to its LLE cold-boot value 0x0001: both 3D
    // engines start disabled until the guest powers them via 0x04000304.
    g_nds.GPU.GPU3D.SetEnabled(false, false);
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
}

const uint32_t* nds_gpu3d_line(int line) {
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    if (g_nds.GPU.GPU3D.IsRendererAccelerated() && !g_compute_frame_ready)
        return g_compute_zero_line;
#endif
    if (!profiling()) return g_nds.GPU.GPU3D.GetLine(line);
    const auto start = ProfileClock::now();
    const uint32_t* result = g_nds.GPU.GPU3D.GetLine(line);
    profile_add(g_gpu3d_profile.getline_ns, start);
    ++g_gpu3d_profile.getline_calls;
    return result;
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
    if (g_nds.GPU.GPU3D.IsRendererAccelerated() &&
        g_compute_rendered_frame) {
        g_nds.GPU.GPU3D.GetCurrentRenderer().PrepareCaptureFrame();
        g_compute_rendered_frame = false;
        g_compute_frame_ready = true;
    }
#endif
}
