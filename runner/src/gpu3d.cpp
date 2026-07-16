// Bridge between the runner and the vendored melonDS GPU3D device model.
// Owns the melonDS::NDS shim instance and implements the shim interfaces
// declared in runner/vendor/melonds/{NDS.h, GPU.h, Platform.h} in terms of
// the runner's own device models (io.cpp IRQ/DMA/stall, vram.cpp texture
// slots). The vendored translation units are unmodified melonDS 1.0rc.

#include "gpu3d.h"

#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "io.h"
#include "vram.h"

#include "NDS.h"
#include "GPU3D_Soft.h"

namespace {

melonDS::NDS g_nds;

int g_log_budget = 64;

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
    NonStupidBitField<512*1024/VRAMDirtyGranularity>&) noexcept {
    static u8 fresh[512*1024];
    nds_vram_copy_texture(fresh);
    if (std::memcmp(fresh, VRAMFlat_Texture, sizeof fresh) == 0) return false;
    std::memcpy(VRAMFlat_Texture, fresh, sizeof fresh);
    return true;
}

bool GPU::MakeVRAMFlat_TexPalCoherent(
    NonStupidBitField<128*1024/VRAMDirtyGranularity>&) noexcept {
    static u8 fresh[128*1024];
    nds_vram_copy_texpal(fresh);
    if (std::memcmp(fresh, VRAMFlat_TexPal, sizeof fresh) == 0) return false;
    std::memcpy(VRAMFlat_TexPal, fresh, sizeof fresh);
    return true;
}

}  // namespace melonDS

// ── melonDS::Platform shim ──────────────────────────────────────────────
// Real primitives so every vendored code path is sound, but the runner
// never calls SoftRenderer::SetThreaded(true, ...): deterministic,
// oracle-comparable execution requires the single-threaded render path.

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

void nds_gpu3d_reset() {
    g_nds.ARM9Timestamp = 0;
    g_nds.GPU.GPU3D.Reset();
    // nds_io_reset resets POWCNT1 to its LLE cold-boot value 0x0001: both 3D
    // engines start disabled until the guest powers them via 0x04000304.
    g_nds.GPU.GPU3D.SetEnabled(false, false);
    std::memset(g_nds.GPU.VRAMFlat_Texture, 0, sizeof g_nds.GPU.VRAMFlat_Texture);
    std::memset(g_nds.GPU.VRAMFlat_TexPal, 0, sizeof g_nds.GPU.VRAMFlat_TexPal);
    nds_gxfifo_set_stall(false);
}

bool nds_gpu3d_reg_addr(uint32_t addr) {
    return (addr >= 0x04000060u && addr < 0x04000064u) ||
           (addr >= 0x04000320u && addr < 0x040006A4u);
}

uint32_t nds_gpu3d_read(uint32_t addr, uint32_t width) {
    switch (width) {
        case 1:  return g_nds.GPU.GPU3D.Read8(addr);
        case 2:  return g_nds.GPU.GPU3D.Read16(addr);
        default: return g_nds.GPU.GPU3D.Read32(addr);
    }
}

void nds_gpu3d_write(uint32_t addr, uint32_t value, uint32_t width) {
    switch (width) {
        case 1:  g_nds.GPU.GPU3D.Write8(addr, static_cast<melonDS::u8>(value)); break;
        case 2:  g_nds.GPU.GPU3D.Write16(addr, static_cast<melonDS::u16>(value)); break;
        default: g_nds.GPU.GPU3D.Write32(addr, value); break;
    }
}

void nds_gpu3d_set_power(uint16_t powcnt1) {
    g_nds.GPU.GPU3D.SetEnabled((powcnt1 & (1u << 3)) != 0,
                               (powcnt1 & (1u << 2)) != 0);
}
