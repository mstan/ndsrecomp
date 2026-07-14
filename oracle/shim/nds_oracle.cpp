// nds_oracle.cpp — headless melonDS reference ("oracle") for ndsrecomp.
//
// Boots the REAL ARM9/ARM7 BIOS + firmware to the interactive menu (firmware
// mode — never SetupDirectBoot) and serves the line-delimited JSON debug
// protocol in ../../TCP.md on 127.0.0.1:19843, so the diff harness can sync
// the native runtime against melonDS on counted HARDWARE EVENTS (never frame
// indices) and pull register/memory/framebuffer state at each checkpoint.
//
// melonDS is GPLv3 and stays a SEPARATE BINARY — this links the melonDS `core`
// static lib but is never linked into the native runner. Tool-use, not
// distribution-as-part-of-the-recompiler.

#include "NDS.h"
#include "Args.h"
#include "SPI.h"
#include "SPI_Firmware.h"
#include "RTC.h"
#include "GPU.h"
#include "ARM.h"
#include "SPU.h"

#include "oracle_hooks.h"

#include <algorithm>
#include <cstdio>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <memory>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socket_t = SOCKET;
#  define CLOSESOCK closesocket
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
   using socket_t = int;
#  define INVALID_SOCKET (-1)
#  define CLOSESOCK ::close
#endif

using namespace melonDS;

// ───────────────────────────────────────────── event counters + IRQ hook ───

// The single always-on event ring. NDS::SetIRQ (patched, guarded by
// MELONDS_ORACLE_HOOKS) bumps the IRQ-derived counters through Oracle_OnSetIRQ;
// the OracleNDS IO-write overrides bump the register-write counters.
OracleCounters g_oracle_counts;

struct OracleSpiTraceEntry {
    uint64_t count;
    uint64_t sys;
    uint64_t cyc9;
    uint64_t cyc7;
    uint64_t insn7;
    uint32_t pc;
    uint32_t value;
};
static constexpr uint32_t kSpiTraceSize = 2048;
static OracleSpiTraceEntry g_spi_trace[kSpiTraceSize] = {};

struct OracleIrqTraceEntry {
    uint64_t count;
    uint64_t sys;
    uint64_t cyc9;
    uint64_t cyc7;
    uint64_t insn;
    uint32_t return_address;
    uint32_t pending;
    uint16_t wifi_if;
    uint16_t wifi_ie;
};
static constexpr uint32_t kIrqTraceSize = 256;
static OracleIrqTraceEntry g_irq_trace[2][kIrqTraceSize] = {};

struct OracleInsnTraceEntry {
    uint64_t count;
    uint64_t sys;
    uint64_t cycles;
    uint32_t pc;
    uint32_t cpsr;
    uint32_t pending;
    uint32_t r[15];
};
// Match the native observer: one full firmware frame must remain queryable so
// the first producer predating a VBlank checkpoint is not evicted.
static constexpr uint32_t kInsnTraceSize = 262144;
static OracleInsnTraceEntry g_insn_trace[2][kInsnTraceSize] = {};

struct OracleFifoTraceEntry {
    uint64_t count;
    uint64_t sys;
    uint64_t cyc9;
    uint64_t cyc7;
    uint64_t insn9;
    uint64_t insn7;
    uint32_t value;
};
static constexpr uint32_t kFifoTraceSize = 64;
static OracleFifoTraceEntry g_fifo_trace[2][kFifoTraceSize] = {};

struct OracleWatchEntry {
    uint64_t seq;
    uint64_t cycles;
    uint64_t insn;
    uint32_t pc;
    uint32_t addr;
    uint32_t value;
    uint8_t cpu;
    uint8_t write;
    uint8_t width;
};
static constexpr uint32_t kWatchSize = 512;
static OracleWatchEntry g_watch[kWatchSize] = {};
static uint32_t g_watch_w = 0;
static uint32_t g_watch_count = 0;
static uint64_t g_watch_seq = 0;

static bool oracleWatchRange(u32 addr, u32 width, u32 lo, u32 hi)
{
    const u32 bytes = width >> 3;
    return addr < hi && addr + bytes > lo;
}

// ── sub-frame event break ───────────────────────────────────────────────
// run_to_event arms a watched counter + target here; the event hooks call
// oracle_brk_check() right after each bump, and the patched RunFrame loop
// (MELONDS_ORACLE_HOOKS) polls Oracle_BreakRequested() and bails mid-frame.
// Without this, RunFrame only stops at a frame boundary (~560k cycles), so a
// sub-frame event (IPCSYNC/FIFO, dozens per frame) overshoots by a whole frame
// and a clean native-vs-oracle state diff at the Nth event is impossible.
static const uint64_t* g_brk_ptr    = nullptr;
static uint64_t        g_brk_target = 0;
static bool            g_brk_hit    = false;
static bool            g_frame_in_progress = false;
static inline void oracle_brk_check(melonDS::NDS* nds)
{
    if (g_brk_ptr && *g_brk_ptr >= g_brk_target && !g_brk_hit)
    {
        g_brk_hit = true;
        // Tight stop: truncate both CPUs' execution targets to "now" so the
        // interpreter loops exit after the *current* instruction rather than
        // running to the end of the scheduler chunk (which can be thousands of
        // cycles, overshooting the Nth event well into a later routine). The
        // next RunFrame reloads the targets from NextTarget(), so resume is
        // clean and emulated time stays continuous.
        nds->ARM9Target = nds->ARM9Timestamp;
        nds->ARM7Target = nds->ARM7Timestamp;
    }
}

namespace melonDS
{
// Polled by the patched RunFrame loop. External linkage in namespace melonDS
// to match the `extern bool Oracle_BreakRequested();` declaration in NDS.cpp.
bool Oracle_BreakRequested() { return g_brk_hit; }
bool Oracle_FrameInProgress() { return g_frame_in_progress; }
void Oracle_SetFrameInProgress(bool inProgress) { g_frame_in_progress = inProgress; }

// Resolved at link time by the patched NDS::SetIRQ. Must live in namespace
// melonDS to match the unqualified extern declaration inside SetIRQ.
void Oracle_OnSetIRQ(NDS* nds, u32 cpu, u32 irq)
{
    switch (irq)
    {
        case IRQ_VBlank: (cpu == 0 ? g_oracle_counts.vblank9 : g_oracle_counts.vblank7)++; break;
        case IRQ_DMA0: case IRQ_DMA1: case IRQ_DMA2: case IRQ_DMA3:
            g_oracle_counts.dma_done++; break;
        case IRQ_Timer0: case IRQ_Timer1: case IRQ_Timer2: case IRQ_Timer3:
            g_oracle_counts.timer_ovf++; break;
        default: break;
    }
    oracle_brk_check(nds);
}

// Always-on register-write ring. Called from the base NDS::ARMxIOWriteN
// (patched, guarded by MELONDS_ORACLE_HOOKS) so it sees writes regardless of
// the `NDS::`-qualified (non-virtual) call sites that bypass the OracleNDS
// vtable overrides. The width rules dedupe melonDS's internal width-forwarding:
//   0x180 IPCSYNC : 16-bit handler is canonical (32-bit forwards down to it).
//   0x188 FIFOsend: 32-bit handler is canonical (16-bit forwards up to it).
//   0x504 SOUNDBIAS: every width is handled directly (no forwarding).
// cpu: 0 = ARM9, 1 = ARM7. Symmetric with the native NdsEventCounts.
void Oracle_OnIOWrite(NDS* nds, u32 cpu, u32 addr, u32 val, u32 width)
{
    if (addr == 0x04000180) {
        if (width == 16) g_oracle_counts.ipcsync_w++;
    } else if (addr == 0x04000188) {
        if (width == 32) {
            uint64_t& count = cpu == 0 ? g_oracle_counts.fifo9to7
                                        : g_oracle_counts.fifo7to9;
            ++count;
            g_fifo_trace[cpu][(count - 1) % kFifoTraceSize] = {
                count,
                nds->OracleSysTimestamp(),
                nds->ARM9Timestamp,
                nds->ARM7Timestamp,
                g_oracle_counts.insn9 + (cpu == 0 ? 1u : 0u),
                g_oracle_counts.insn7 + (cpu == 1 ? 1u : 0u),
                val,
            };
        }
    } else if (addr == 0x040001C2 && cpu == 1 && (width == 8 || width == 16)) {
        g_oracle_counts.spi_w++;
        g_spi_trace[(g_oracle_counts.spi_w - 1) % kSpiTraceSize] = {
            g_oracle_counts.spi_w,
            nds->OracleSysTimestamp(),
            nds->ARM9Timestamp,
            nds->ARM7Timestamp,
            g_oracle_counts.insn7,
            nds->ARM7.R[15],
            val & 0xFFu,
        };
    } else if (addr == 0x04000504 && cpu == 1) {
        u32 v = val & 0x3FF;
        if (g_oracle_counts.soundbias_w == 0) g_oracle_counts.soundbias_first = v;
        g_oracle_counts.soundbias_w++;
        g_oracle_counts.soundbias_last = v;
    }
    oracle_brk_check(nds);
}

// Always-on firmware settings control-block write history. The hook sits in
// the base ARMxWriteN paths, so it remains independent of the CPU execution
// backend and cannot be armed after the fact.
void Oracle_OnMemWrite(NDS* nds, u32 cpu, u32 addr, u32 val, u32 width)
{
    if (!oracleWatchRange(addr, width, 0x02356000u, 0x02356200u)) return;
    OracleWatchEntry& e = g_watch[g_watch_w];
    e.seq = ++g_watch_seq;
    e.cycles = cpu == 0 ? nds->ARM9Timestamp : nds->ARM7Timestamp;
    e.insn = cpu == 0 ? g_oracle_counts.insn9 : g_oracle_counts.insn7;
    ARM* arm = cpu == 0 ? static_cast<ARM*>(&nds->ARM9)
                        : static_cast<ARM*>(&nds->ARM7);
    const bool thumb = (arm->CPSR & 0x20u) != 0u;
    e.pc = arm->R[15] - (thumb ? 4u : 8u);
    e.addr = addr;
    e.value = val;
    e.cpu = static_cast<uint8_t>(cpu);
    e.write = 1u;
    e.width = static_cast<uint8_t>(width >> 3);
    g_watch_w = (g_watch_w + 1u) % kWatchSize;
    if (g_watch_count < kWatchSize) ++g_watch_count;
}

// Always-on per-CPU retired-instruction ring. Called once per architectural
// guest instruction from the patched ARMv5::Execute (ARM9) / ARMv4::Execute
// (ARM7) — see ../patches/0004-nds-insn-retire-hook.patch for the exact hook
// site and the Thumb-BL-long-branch counting rationale (it retires as two
// instructions here, matching real hardware: Thumb has no native 32-bit
// encoding, BL is architecturally a prefix+suffix halfword pair).
// cpu: 0 = ARM9, 1 = ARM7 (same convention as Oracle_OnSetIRQ/Oracle_OnIOWrite).
void Oracle_OnInsnRetire(NDS* nds, u32 cpu)
{
    (cpu == 0 ? g_oracle_counts.insn9 : g_oracle_counts.insn7)++;
    oracle_brk_check(nds);
}

void Oracle_OnInsnBegin(NDS* nds, u32 cpu, u32 pc, u32 cpsr)
{
    const uint64_t count = (cpu == 0 ? g_oracle_counts.insn9
                                      : g_oracle_counts.insn7) + 1u;
    OracleInsnTraceEntry& e =
        g_insn_trace[cpu][(count - 1) % kInsnTraceSize];
    e = {
        count,
        nds->OracleSysTimestamp(),
        cpu == 0 ? nds->ARM9Timestamp : nds->ARM7Timestamp,
        pc,
        cpsr,
        static_cast<uint32_t>(cpu == 0 ? nds->ARM9.Cycles : nds->ARM7.Cycles),
    };
    const u32* regs = cpu == 0 ? nds->ARM9.R : nds->ARM7.R;
    std::memcpy(e.r, regs, sizeof(e.r));
}

// Called by ARM::TriggerIRQ after the mask check and before JumpTo adds the
// exception refill. `instruction_cycles` is the just-retired instruction's
// pending cost, which melonDS commits after TriggerIRQ returns. Including it
// makes this timestamp directly comparable to native runtime_irq entry.
void Oracle_OnIRQAccept(NDS* nds, u32 cpu, u32 return_address,
                        u32 instruction_cycles)
{
    uint64_t& count = cpu == 0 ? g_oracle_counts.irq9 : g_oracle_counts.irq7;
    ++count;
    g_irq_trace[cpu][(count - 1) % kIrqTraceSize] = {
        count,
        nds->OracleSysTimestamp(),
        nds->ARM9Timestamp + (cpu == 0 ? instruction_cycles : 0),
        nds->ARM7Timestamp + (cpu == 1 ? instruction_cycles : 0),
        cpu == 0 ? g_oracle_counts.insn9 : g_oracle_counts.insn7,
        return_address,
        nds->IE[cpu] & nds->IF[cpu],
        cpu == 1 ? nds->Wifi.Read(0x04800010u) : 0u,
        cpu == 1 ? nds->Wifi.Read(0x04800012u) : 0u,
    };
}
}

// ─────────────────────────────────────────────────────── the NDS subclass ───

// Adds a width-dispatched debug read used by the TCP protocol. Register-write
// event counting is NOT done here: melonDS's bus calls the IO-write methods
// with explicit `NDS::` qualification (non-virtual), so subclass overrides are
// never invoked. Counting lives in Oracle_OnIOWrite, called from the patched
// base methods (see above).
class OracleNDS : public NDS
{
public:
    explicit OracleNDS(NDSArgs&& args) : NDS(std::move(args)) {}

    u8 DebugMemRead(u32 cpu, u32 addr)
    {
        if (cpu == 9)
        {
            // ARM9's TCMs are private CPU memories and never reach the NDS
            // bus accessors.  Mirror ARMv5's debugger view here so TCP memory
            // inspection sees the same bytes as guest data loads.
            if (addr < ARM9.ITCMSize)
                return ARM9.ITCM[addr & (ITCMPhysicalSize - 1)];
            if ((addr & ARM9.DTCMMask) == ARM9.DTCMBase)
                return ARM9.DTCM[addr & (DTCMPhysicalSize - 1)];
            return ARM9Read8(addr);
        }
        return ARM7Read8(addr);
    }

    u32 DebugIORead(u32 cpu, u32 addr, u32 width)
    {
        if (cpu == 7)
        {
            if (width == 8) return ARM7IORead8(addr);
            if (width == 16) return ARM7IORead16(addr);
            if (width == 32) return ARM7IORead32(addr);
        }
        else
        {
            if (width == 8) return ARM9IORead8(addr);
            if (width == 16) return ARM9IORead16(addr);
            if (width == 32) return ARM9IORead32(addr);
        }
        return 0;
    }
};

// The public melonDS SPU output FIFO is destructive and only receives a block
// when a physical frame finishes. Drain it after every RunFrame call into a
// separate ordinal trace so debug reads neither perturb nor lose audio.
static constexpr uint32_t kAudioTraceFrames = 1048576;
static std::array<int16_t, kAudioTraceFrames * 2> g_audio_trace{};
static uint64_t g_audio_produced = 0;

static void resetAudioTrace()
{
    g_audio_produced = 0;
}

static uint64_t oldestAudioOrdinal()
{
    return g_audio_produced > kAudioTraceFrames
        ? g_audio_produced - kAudioTraceFrames : 0;
}

static void pushAudioTrace(const int16_t* samples, uint32_t frames)
{
    for (uint32_t i = 0; i < frames; ++i)
    {
        const uint32_t pos = static_cast<uint32_t>(
            g_audio_produced % kAudioTraceFrames);
        g_audio_trace[pos * 2] = samples[i * 2];
        g_audio_trace[pos * 2 + 1] = samples[i * 2 + 1];
        ++g_audio_produced;
    }
}

static void drainOracleAudio(OracleNDS* nds)
{
    int16_t samples[1024 * 2];
    for (;;)
    {
        const int frames = nds->SPU.ReadOutput(samples, 1024);
        if (frames <= 0) break;
        pushAudioTrace(samples, static_cast<uint32_t>(frames));
    }
}

static uint32_t copyAudioTrace(uint64_t start, int16_t* samples,
                               uint32_t frames)
{
    if (!samples || start < oldestAudioOrdinal() || start >= g_audio_produced)
        return 0;
    const uint32_t take = static_cast<uint32_t>(std::min<uint64_t>(
        frames, g_audio_produced - start));
    for (uint32_t i = 0; i < take; ++i)
    {
        const uint32_t pos = static_cast<uint32_t>(
            (start + i) % kAudioTraceFrames);
        samples[i * 2] = g_audio_trace[pos * 2];
        samples[i * 2 + 1] = g_audio_trace[pos * 2 + 1];
    }
    return take;
}

// ──────────────────────────────────────────────────────────── small utils ───

static bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff len = f.tellg();
    f.seekg(0, std::ios::beg);
    if (len <= 0) return false;
    out.resize((size_t)len);
    f.read(reinterpret_cast<char*>(out.data()), len);
    return (bool)f;
}

static const char HEX[] = "0123456789abcdef";
static void appendHex(std::string& s, const uint8_t* data, size_t len)
{
    s.reserve(s.size() + len * 2);
    for (size_t i = 0; i < len; i++)
    {
        s.push_back(HEX[data[i] >> 4]);
        s.push_back(HEX[data[i] & 0xF]);
    }
}

// Minimal flat-object JSON field extractors (requests are scalar-only).
static bool jsonFind(const std::string& s, const std::string& key, size_t& pos)
{
    std::string pat = "\"" + key + "\"";
    size_t p = s.find(pat);
    if (p == std::string::npos) return false;
    p += pat.size();
    while (p < s.size() && (s[p] == ' ' || s[p] == ':' || s[p] == '\t')) p++;
    pos = p;
    return true;
}
static std::string jsonStr(const std::string& s, const std::string& key, const std::string& def = "")
{
    size_t p;
    if (!jsonFind(s, key, p) || p >= s.size() || s[p] != '"') return def;
    p++;
    std::string out;
    while (p < s.size() && s[p] != '"') out.push_back(s[p++]);
    return out;
}
static uint64_t jsonU64(const std::string& s, const std::string& key, uint64_t def = 0)
{
    size_t p;
    if (!jsonFind(s, key, p)) return def;
    if (p < s.size() && s[p] == '"') p++;  // tolerate quoted numbers
    char* end = nullptr;
    uint64_t v = strtoull(s.c_str() + p, &end, 10);
    return (end == s.c_str() + p) ? def : v;
}
static bool jsonBool(const std::string& s, const std::string& key, bool def = false)
{
    size_t p;
    if (!jsonFind(s, key, p)) return def;
    if (s.compare(p, 4, "true") == 0)  return true;
    if (s.compare(p, 5, "false") == 0) return false;
    if (s[p] == '1') return true;
    if (s[p] == '0') return false;
    return def;
}

// ───────────────────────────────────────────────────── region resolution ───

struct Region { const uint8_t* ptr; size_t len; };

static Region resolveRegion(NDS* nds, const std::string& r)
{
    GPU& g = nds->GPU;
    if (r == "mainram")    return { nds->MainRAM,    (size_t)nds->MainRAMMask + 1 };
    if (r == "wram7")      return { nds->ARM7WRAM,   0x10000 };
    if (r == "wramshared") return { nds->SharedWRAM, 0x8000 };
    if (r == "vramA")      return { g.VRAM[0], 0x20000 };
    if (r == "vramB")      return { g.VRAM[1], 0x20000 };
    if (r == "vramC")      return { g.VRAM[2], 0x20000 };
    if (r == "vramD")      return { g.VRAM[3], 0x20000 };
    if (r == "vramE")      return { g.VRAM[4], 0x10000 };
    if (r == "vramF")      return { g.VRAM[5], 0x4000 };
    if (r == "vramG")      return { g.VRAM[6], 0x4000 };
    if (r == "vramH")      return { g.VRAM[7], 0x8000 };
    if (r == "vramI")      return { g.VRAM[8], 0x4000 };
    if (r == "palA")       return { g.Palette,         0x400 };
    if (r == "palB")       return { g.Palette + 0x400, 0x400 };
    if (r == "oam")        return { g.OAM, 0x800 };
    if (r == "itcm")       return { nds->ARM9.ITCM, sizeof(nds->ARM9.ITCM) };
    if (r == "dtcm")       return { nds->ARM9.DTCM, 0x4000 };
    return { nullptr, 0 };
}

static uint64_t eventValue(const std::string& ev)
{
    const OracleCounters& c = g_oracle_counts;
    if (ev == "vblank9")   return c.vblank9;
    if (ev == "vblank7")   return c.vblank7;
    if (ev == "ipcsync_w") return c.ipcsync_w;
    if (ev == "fifo9to7")  return c.fifo9to7;
    if (ev == "fifo7to9")  return c.fifo7to9;
    if (ev == "dma_done")  return c.dma_done;
    if (ev == "timer_ovf") return c.timer_ovf;
    if (ev == "spi_w")     return c.spi_w;
    if (ev == "soundbias_w") return c.soundbias_w;
    if (ev == "insn9")     return c.insn9;
    if (ev == "insn7")     return c.insn7;
    return UINT64_MAX;  // unknown event
}

// Pointer to the counter named by `ev`, for arming the sub-frame break.
// Mirrors eventValue; nullptr for an unknown event (break stays disarmed).
static const uint64_t* eventPtr(const std::string& ev)
{
    OracleCounters& c = g_oracle_counts;
    if (ev == "vblank9")     return &c.vblank9;
    if (ev == "vblank7")     return &c.vblank7;
    if (ev == "ipcsync_w")   return &c.ipcsync_w;
    if (ev == "fifo9to7")    return &c.fifo9to7;
    if (ev == "fifo7to9")    return &c.fifo7to9;
    if (ev == "dma_done")    return &c.dma_done;
    if (ev == "timer_ovf")   return &c.timer_ovf;
    if (ev == "spi_w")       return &c.spi_w;
    if (ev == "soundbias_w") return &c.soundbias_w;
    if (ev == "insn9")       return &c.insn9;
    if (ev == "insn7")       return &c.insn7;
    return nullptr;
}

static std::string countsJson(OracleNDS* nds)
{
    const OracleCounters& c = g_oracle_counts;
    // cyc9/cyc7 = ARM9/ARM7 timestamps in each core's own cycle units (ARM9 =
    // 2x ARM7). Lets the fp-stream microscope compare timestamps at equal
    // retired-instruction indices vs native's scheduler_cpu_cycles, to separate
    // pure interleave-order divergence from cycle-accounting drift.
    char buf[640];
    snprintf(buf, sizeof(buf),
        "{\"vblank9\":%llu,\"vblank7\":%llu,\"ipcsync_w\":%llu,"
        "\"fifo9to7\":%llu,\"fifo7to9\":%llu,\"dma_done\":%llu,\"timer_ovf\":%llu,"
        "\"spi_w\":%llu,\"irq9\":%llu,\"irq7\":%llu,"
        "\"soundbias_w\":%llu,\"soundbias_first\":%u,\"soundbias_last\":%u,"
        "\"insn9\":%llu,\"insn7\":%llu,\"cyc9\":%llu,\"cyc7\":%llu}",
        (unsigned long long)c.vblank9, (unsigned long long)c.vblank7,
        (unsigned long long)c.ipcsync_w, (unsigned long long)c.fifo9to7,
        (unsigned long long)c.fifo7to9, (unsigned long long)c.dma_done,
        (unsigned long long)c.timer_ovf, (unsigned long long)c.spi_w,
        (unsigned long long)c.irq9, (unsigned long long)c.irq7,
        (unsigned long long)c.soundbias_w, c.soundbias_first, c.soundbias_last,
        (unsigned long long)c.insn9, (unsigned long long)c.insn7,
        (unsigned long long)nds->ARM9Timestamp,
        (unsigned long long)nds->ARM7Timestamp);
    return buf;
}

static std::string ioStateJson(OracleNDS* nds)
{
    char buf[768];
    snprintf(buf, sizeof(buf),
        "{\"cpu9\":{\"ime\":%u,\"ie\":%u,\"if\":%u,\"postflg\":%u,\"ipcsync\":%u},"
        "\"cpu7\":{\"ime\":%u,\"ie\":%u,\"if\":%u,\"postflg\":%u,\"ipcsync\":%u},"
        "\"cpu_stop\":%u,\"num_frames\":%u,\"counts\":%s}",
        nds->IME[0], nds->IE[0], nds->IF[0],
        nds->DebugIORead(9, 0x04000300, 8),
        nds->DebugIORead(9, 0x04000180, 16),
        nds->IME[1], nds->IE[1], nds->IF[1],
        nds->DebugIORead(7, 0x04000300, 8),
        nds->DebugIORead(7, 0x04000180, 16),
        nds->CPUStop, nds->NumFrames, countsJson(nds).c_str());
    return buf;
}

// ───────────────────────────────────────────────────────── command dispatch ─

// Cap so run_to_event can't spin forever if the target event never advances
// (e.g. a diverged boot that never IPCs). One RunFrame == one VBlank/CPU.
static constexpr uint64_t kMaxFrames = 200000;

static std::string handle(OracleNDS* nds, const std::string& line)
{
    std::string cmd = jsonStr(line, "cmd");

    if (cmd == "ping")
        return "{\"pong\":true}";

    if (cmd == "reset")
    {
        // Full power-on re-init, identical to main()'s sequence, so the
        // bisector can compare fresh-from-reset at each event count.
        nds->Reset();
        nds->SPI.GetPowerMan()->SetBatteryLevelOkay(true);
        nds->RTC.SetDateTime(2024, 1, 1, 12, 0, 0);
        nds->SetLidClosed(false);
        nds->Start();
        resetAudioTrace();
        g_oracle_counts = OracleCounters{};   // clear the event ring
        std::memset(g_spi_trace, 0, sizeof(g_spi_trace));
        std::memset(g_irq_trace, 0, sizeof(g_irq_trace));
        std::memset(g_insn_trace, 0, sizeof(g_insn_trace));
        std::memset(g_fifo_trace, 0, sizeof(g_fifo_trace));
        std::memset(g_watch, 0, sizeof(g_watch));
        g_watch_w = 0; g_watch_count = 0; g_watch_seq = 0;
        g_brk_ptr = nullptr; g_brk_target = 0; g_brk_hit = false;
        g_frame_in_progress = false;
        return "{\"ok\":true}";
    }

    if (cmd == "regs")
    {
        uint64_t cpu = jsonU64(line, "cpu", 9);
        ARM* arm = (cpu == 7) ? (ARM*)&nds->ARM7 : (ARM*)&nds->ARM9;
        std::string out = "{\"r\":[";
        for (int i = 0; i < 16; i++)
        {
            char b[16];
            snprintf(b, sizeof(b), "%s%u", i ? "," : "", arm->R[i]);
            out += b;
        }
        // SPSR for the current mode (banked); 0 in USR/SYS where none exists.
        uint32_t mode = arm->CPSR & 0x1F;
        uint32_t spsr = 0;
        switch (mode)
        {
            case 0x11: spsr = arm->R_FIQ[7]; break;  // FIQ
            case 0x12: spsr = arm->R_IRQ[2]; break;  // IRQ
            case 0x13: spsr = arm->R_SVC[2]; break;  // SVC
            case 0x17: spsr = arm->R_ABT[2]; break;  // ABT
            case 0x1B: spsr = arm->R_UND[2]; break;  // UND
            default: break;
        }
        char tail[96];
        snprintf(tail, sizeof(tail), "],\"cpsr\":%u,\"spsr\":%u,\"mode\":%u}",
                 arm->CPSR, spsr, mode);
        return out + tail;
    }

    if (cmd == "event_counts")
        return countsJson(nds);

    if (cmd == "audio_samples")
    {
        drainOracleAudio(nds);
        const uint64_t start = jsonU64(line, "start", 0);
        uint32_t count = static_cast<uint32_t>(jsonU64(line, "count", 1024));
        if (count > 4096) count = 4096;
        const uint64_t oldest = oldestAudioOrdinal();
        if (start < oldest)
            return "{\"error\":\"audio start is no longer retained\"}";
        std::vector<int16_t> samples(count * 2);
        const uint32_t copied = copyAudioTrace(start, samples.data(), count);
        std::string pcm;
        pcm.reserve(copied * 8);
        for (uint32_t i = 0; i < copied * 2; ++i)
        {
            const uint16_t value = static_cast<uint16_t>(samples[i]);
            const uint8_t bytes[2] = {
                static_cast<uint8_t>(value),
                static_cast<uint8_t>(value >> 8),
            };
            appendHex(pcm, bytes, sizeof(bytes));
        }
        return "{\"start\":" + std::to_string(start)
             + ",\"count\":" + std::to_string(copied)
             + ",\"oldest\":" + std::to_string(oldest)
             + ",\"produced\":" + std::to_string(g_audio_produced)
             + ",\"pcm_s16le\":\"" + pcm + "\"}";
    }

    if (cmd == "rtc_state")
    {
        RTC::StateData s{};
        nds->RTC.GetState(s);
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"io\":%u,\"status1\":%u,\"status2\":%u,"
            "\"datetime\":[%u,%u,%u,%u,%u,%u,%u]}",
            nds->RTC.Read(), s.StatusReg1, s.StatusReg2,
            s.DateTime[0], s.DateTime[1], s.DateTime[2], s.DateTime[3],
            s.DateTime[4], s.DateTime[5], s.DateTime[6]);
        return buf;
    }

    if (cmd == "spi_sample")
    {
        uint64_t count = jsonU64(line, "count", 0);
        if (count == 0) return "{\"found\":false}";
        const OracleSpiTraceEntry& e = g_spi_trace[(count - 1) % kSpiTraceSize];
        if (e.count != count) return "{\"found\":false}";
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"found\":true,\"count\":%llu,\"sys\":%llu,\"cyc9\":%llu,"
            "\"cyc7\":%llu,\"insn7\":%llu,\"pc\":%u,\"value\":%u}",
            (unsigned long long)e.count, (unsigned long long)e.sys,
            (unsigned long long)e.cyc9, (unsigned long long)e.cyc7,
            (unsigned long long)e.insn7, e.pc, e.value);
        return buf;
    }

    if (cmd == "irq_sample")
    {
        uint32_t cpu = jsonU64(line, "cpu", 9) == 7 ? 1u : 0u;
        uint64_t count = jsonU64(line, "count", 0);
        if (count == 0) return "{\"found\":false}";
        const OracleIrqTraceEntry& e = g_irq_trace[cpu][(count - 1) % kIrqTraceSize];
        if (e.count != count) return "{\"found\":false}";
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"found\":true,\"count\":%llu,\"sys\":%llu,\"cyc9\":%llu,"
            "\"cyc7\":%llu,\"insn\":%llu,\"return_address\":%u,"
            "\"pending\":%u,\"wifi_if\":%u,\"wifi_ie\":%u}",
            (unsigned long long)e.count, (unsigned long long)e.sys,
            (unsigned long long)e.cyc9, (unsigned long long)e.cyc7,
            (unsigned long long)e.insn, e.return_address, e.pending,
            e.wifi_if, e.wifi_ie);
        return buf;
    }

    if (cmd == "insn_sample")
    {
        uint32_t cpu = jsonU64(line, "cpu", 9) == 7 ? 1u : 0u;
        uint64_t count = jsonU64(line, "count", 0);
        if (count == 0) return "{\"found\":false}";
        const OracleInsnTraceEntry& e = g_insn_trace[cpu][(count - 1) % kInsnTraceSize];
        if (e.count != count) return "{\"found\":false}";
        char buf[320];
        snprintf(buf, sizeof(buf),
            "{\"found\":true,\"count\":%llu,\"sys\":%llu,\"cycles\":%llu,"
            "\"pc\":%u,\"cpsr\":%u,\"pending\":%u,\"r\":[",
            (unsigned long long)e.count, (unsigned long long)e.sys,
            (unsigned long long)e.cycles, e.pc, e.cpsr, e.pending);
        std::string out = buf;
        for (int i = 0; i < 15; i++)
        {
            if (i) out += ',';
            out += std::to_string(e.r[i]);
        }
        return out + "]}";
    }

    if (cmd == "fifo_sample")
    {
        uint32_t cpu = jsonU64(line, "cpu", 9) == 7 ? 1u : 0u;
        uint64_t count = jsonU64(line, "count", 0);
        if (count == 0) return "{\"found\":false}";
        const OracleFifoTraceEntry& e = g_fifo_trace[cpu][(count - 1) % kFifoTraceSize];
        if (e.count != count) return "{\"found\":false}";
        char buf[320];
        snprintf(buf, sizeof(buf),
            "{\"found\":true,\"count\":%llu,\"sys\":%llu,\"cyc9\":%llu,"
            "\"cyc7\":%llu,\"insn9\":%llu,\"insn7\":%llu,\"value\":%u}",
            (unsigned long long)e.count, (unsigned long long)e.sys,
            (unsigned long long)e.cyc9, (unsigned long long)e.cyc7,
            (unsigned long long)e.insn9, (unsigned long long)e.insn7, e.value);
        return buf;
    }

    if (cmd == "io_state")
        return ioStateJson(nds);

    if (cmd == "sched_state")
    {
        std::string out = "{\"sys\":" + std::to_string(nds->OracleSysTimestamp())
            + ",\"arm9\":" + std::to_string(nds->ARM9Timestamp)
            + ",\"arm7\":" + std::to_string(nds->ARM7Timestamp)
            + ",\"mask\":" + std::to_string(nds->OracleSchedListMask())
            + ",\"events\":[";
        for (uint32_t i = 0; i < Event_MAX; ++i)
        {
            if (i) out += ',';
            out += std::to_string(nds->SchedList[i].Timestamp);
        }
        out += "]}";
        return out;
    }

    if (cmd == "run_to_event")
    {
        std::string ev = jsonStr(line, "event");
        uint64_t target = jsonU64(line, "count", 0);
        if (eventValue(ev) == UINT64_MAX)
            return "{\"error\":\"unknown event\"}";
        // Arm the sub-frame break so RunFrame stops AT the Nth event, not at
        // the next frame boundary. The loop condition (checked before each
        // frame) guarantees we never re-enter RunFrame once the target is hit.
        g_brk_ptr = eventPtr(ev); g_brk_target = target; g_brk_hit = false;
        // No-progress early-out: if the watched counter has not advanced for
        // this many consecutive frames the boot has stalled / plateaued (e.g.
        // reached the menu and stopped IPC-ing), so bail with reached=false
        // instead of grinding to kMaxFrames. Sized far above the largest
        // legitimate inter-event gap during boot; overridable via "stall".
        uint64_t stallLimit = jsonU64(line, "stall", 2000);
        uint64_t lastVal = eventValue(ev);
        uint64_t stale = 0, frames = 0;
        bool stalled = false;
        while (eventValue(ev) < target && frames < kMaxFrames)
        {
            g_brk_hit = false;
            nds->RunFrame();
            drainOracleAudio(nds);
            frames++;
            uint64_t v = eventValue(ev);
            if (v > lastVal) { lastVal = v; stale = 0; }
            else if (++stale >= stallLimit) { stalled = true; break; }
        }
        g_brk_ptr = nullptr; g_brk_hit = false;  // disarm
        bool reached = eventValue(ev) >= target;
        return std::string("{\"reached\":") + (reached ? "true" : "false")
             + ",\"stalled\":" + (stalled ? "true" : "false")
             + ",\"frames\":" + std::to_string(frames)
             + ",\"counts\":" + countsJson(nds) + "}";
    }

    if (cmd == "run_frames")
    {
        // Advance exactly N full frames (no break). Lets the harness step the
        // oracle by raw frames — useful for characterizing per-frame progress
        // without relying on vblank9 (which counts IRQs-delivered, not frames).
        uint64_t count = jsonU64(line, "count", 1);
        if (count > kMaxFrames) count = kMaxFrames;
        g_brk_ptr = nullptr; g_brk_hit = false;
        for (uint64_t i = 0; i < count; i++)
        {
            nds->RunFrame();
            drainOracleAudio(nds);
        }
        return std::string("{\"frames\":") + std::to_string(count)
             + ",\"counts\":" + countsJson(nds) + "}";
    }

    if (cmd == "read_mem")
    {
        uint64_t cpu  = jsonU64(line, "cpu", 9);
        uint32_t addr = (uint32_t)jsonU64(line, "addr", 0);
        uint32_t len  = (uint32_t)jsonU64(line, "len", 0);
        std::string hex;
        std::vector<uint8_t> tmp(len);
        for (uint32_t i = 0; i < len; i++)
            tmp[i] = nds->DebugMemRead(static_cast<uint32_t>(cpu), addr + i);
        appendHex(hex, tmp.data(), tmp.size());
        return "{\"hex\":\"" + hex + "\"}";
    }

    if (cmd == "read_io")
    {
        uint64_t cpu = jsonU64(line, "cpu", 9);
        uint32_t addr = (uint32_t)jsonU64(line, "addr", 0);
        uint32_t width = (uint32_t)jsonU64(line, "width", 32);
        if (width != 8 && width != 16 && width != 32)
            return "{\"error\":\"width must be 8, 16, or 32\"}";
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"value\":%u}",
                 nds->DebugIORead(cpu == 7 ? 7 : 9, addr, width));
        return buf;
    }

    if (cmd == "read_region")
    {
        Region reg = resolveRegion(nds, jsonStr(line, "region"));
        if (!reg.ptr)
            return "{\"error\":\"unknown or absent region\"}";
        std::string hex;
        appendHex(hex, reg.ptr, reg.len);
        return "{\"hex\":\"" + hex + "\"}";
    }

    if (cmd == "watch")
    {
        uint32_t max = (uint32_t)jsonU64(line, "max", 128);
        if (max > kWatchSize) max = kWatchSize;
        uint32_t count = g_watch_count < max ? g_watch_count : max;
        uint32_t start = (g_watch_w + kWatchSize - count) % kWatchSize;
        std::string out = "{\"events\":[";
        for (uint32_t i = 0; i < count; ++i)
        {
            const OracleWatchEntry& e = g_watch[(start + i) % kWatchSize];
            char b[240];
            std::snprintf(b, sizeof(b),
                "%s{\"seq\":%llu,\"cycles\":%llu,\"insn\":%llu,"
                "\"cpu\":%u,\"write\":%u,\"width\":%u,"
                "\"pc\":%u,\"addr\":%u,\"value\":%u}",
                i ? "," : "", (unsigned long long)e.seq,
                (unsigned long long)e.cycles, (unsigned long long)e.insn,
                e.cpu, e.write, e.width, e.pc, e.addr, e.value);
            out += b;
        }
        return out + "]}";
    }

    if (cmd == "framebuffer")
    {
        std::string eng = jsonStr(line, "engine", "A");
        int screen = (eng == "B" || eng == "b") ? 1 : 0;  // A=top(0), B=bottom(1)
        const uint32_t* fb = nds->GPU.Framebuffer[nds->GPU.FrontBuffer][screen].get();
        if (!fb)
            return "{\"error\":\"framebuffer not ready\"}";
        std::string rgb;
        rgb.reserve(256 * 192 * 6);
        for (int i = 0; i < 256 * 192; i++)
        {
            uint32_t px = fb[i];  // 0xXXRRGGBB (melonDS Format_RGB32)
            uint8_t c[3] = { (uint8_t)(px >> 16), (uint8_t)(px >> 8), (uint8_t)px };
            appendHex(rgb, c, 3);
        }
        return "{\"w\":256,\"h\":192,\"rgb\":\"" + rgb + "\"}";
    }

    if (cmd == "touch")
    {
        uint32_t x = (uint32_t)jsonU64(line, "x", 0);
        uint32_t y = (uint32_t)jsonU64(line, "y", 0);
        if (jsonBool(line, "down", true)) nds->TouchScreen((u16)x, (u16)y);
        else                              nds->ReleaseScreen();
        return "{\"ok\":true}";
    }

    if (cmd == "keys")
    {
        nds->SetKeyMask((u32)jsonU64(line, "mask", 0x3FF));
        return "{\"ok\":true}";
    }

    return "{\"error\":\"unknown cmd\"}";
}

// ──────────────────────────────────────────────────────────── TCP server ───

static bool sendAll(socket_t s, const char* data, size_t len)
{
    while (len > 0)
    {
        int n = send(s, data, (int)len, 0);
        if (n <= 0) return false;
        data += n;
        len  -= (size_t)n;
    }
    return true;
}

static void serve(OracleNDS* nds, uint16_t port)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "WSAStartup failed\n"); return; }
#endif
    socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) { fprintf(stderr, "socket() failed\n"); return; }
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 only
    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) != 0) { fprintf(stderr, "bind() failed on port %u\n", port); return; }
    if (listen(listener, 1) != 0) { fprintf(stderr, "listen() failed\n"); return; }
    fprintf(stderr, "nds_oracle: listening on 127.0.0.1:%u\n", port);

    for (;;)
    {
        socket_t client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;

        std::string buf;
        char chunk[65536];
        bool open = true;
        while (open)
        {
            int n = recv(client, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            buf.append(chunk, (size_t)n);
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos)
            {
                std::string reqline = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                std::string resp = handle(nds, reqline);
                resp.push_back('\n');
                if (!sendAll(client, resp.data(), resp.size())) { open = false; break; }
            }
        }
        CLOSESOCK(client);
    }
}

// ─────────────────────────────────────────────────────────────────── main ───

static void usage(const char* argv0)
{
    fprintf(stderr,
        "usage: %s --bios9 <f> --bios7 <f> --firmware <f> "
        "[--boot firmware] [--port 19843]\n", argv0);
}

int main(int argc, char** argv)
{
    std::string bios9, bios7, firmware, boot = "firmware";
    uint16_t port = 19843;

    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if      (a == "--bios9")    bios9 = next();
        else if (a == "--bios7")    bios7 = next();
        else if (a == "--firmware") firmware = next();
        else if (a == "--boot")     boot = next();
        else if (a == "--port")     port = (uint16_t)strtoul(next().c_str(), nullptr, 10);
        else { fprintf(stderr, "unknown arg: %s\n", a.c_str()); usage(argv[0]); return 2; }
    }
    if (bios9.empty() || bios7.empty() || firmware.empty()) { usage(argv[0]); return 2; }
    if (boot != "firmware")
    {
        // The firmware-menu gate forbids direct boot: we only ever run the real
        // BIOS+firmware path. Refuse anything else rather than silently HLE it.
        fprintf(stderr, "nds_oracle: only --boot firmware is supported\n");
        return 2;
    }

    std::vector<uint8_t> b9, b7, fw;
    if (!readFile(bios9, b9))    { fprintf(stderr, "cannot read bios9: %s\n", bios9.c_str()); return 1; }
    if (!readFile(bios7, b7))    { fprintf(stderr, "cannot read bios7: %s\n", bios7.c_str()); return 1; }
    if (!readFile(firmware, fw)) { fprintf(stderr, "cannot read firmware: %s\n", firmware.c_str()); return 1; }
    if (b9.size() != ARM9BIOSSize) { fprintf(stderr, "bios9 wrong size %zu (want %u)\n", b9.size(), ARM9BIOSSize); return 1; }
    if (b7.size() != ARM7BIOSSize) { fprintf(stderr, "bios7 wrong size %zu (want %u)\n", b7.size(), ARM7BIOSSize); return 1; }

    NDSArgs args;
    args.ARM9BIOS = std::make_unique<ARM9BIOSImage>();
    args.ARM7BIOS = std::make_unique<ARM7BIOSImage>();
    std::memcpy(args.ARM9BIOS->data(), b9.data(), ARM9BIOSSize);
    std::memcpy(args.ARM7BIOS->data(), b7.data(), ARM7BIOSSize);
    {
        // The ARM7 BIOS validates firmware CRCs during its boot-load; stale
        // checksums make it refuse the hand-off (ARM7 stalls pre-POSTFLG and
        // never advances the IPCSYNC handshake). The frontend does the same
        // via customizeFirmware() before install.
        Firmware fwobj(fw.data(), (u32)fw.size());
        fwobj.UpdateChecksums();
        args.Firmware = std::move(fwobj);
    }
    args.JIT = std::nullopt;  // deterministic interpreter; no JIT in the oracle

    OracleNDS* nds = new OracleNDS(std::move(args));
    // Compare the original DS/DS Lite DAC, not melonDS's optional enhanced
    // 16-bit host output. The native runtime models the hardware's 10-bit
    // degradation before producing signed stereo samples.
    nds->SPU.SetDegrade10Bit(AudioBitDepth::_10Bit);
    if (nds->NeedsDirectBoot())
    {
        // A bootable retail firmware (256 KB) drives the menu from BIOS reset.
        // 128 KB / non-bootable firmware would force direct boot — not allowed.
        fprintf(stderr, "nds_oracle: firmware is not bootable (would require direct boot)\n");
        return 1;
    }
    nds->Reset();   // begins at the real BIOS reset vectors (firmware boot, LLE)

    // The firmware's boot path polls power-management battery status and the
    // RTC; without these the ARM7 BIOS stalls before the IPC handoff and never
    // reaches the menu. The qt_sdl frontend does the same after Reset().
    // A FIXED date keeps the oracle deterministic — the native runtime must
    // seed the same constant so the diff harness compares like for like.
    nds->SPI.GetPowerMan()->SetBatteryLevelOkay(true);
    nds->RTC.SetDateTime(2024, 1, 1, 12, 0, 0);
    // Lid must be OPEN, or the firmware sees the hinge sensor closed and enters
    // sleep (ARM7 writes HALTCNT=0xC0) immediately at boot — a dead wait.
    nds->SetLidClosed(false);

    nds->Start();
    resetAudioTrace();

    serve(nds, port);
    return 0;
}
