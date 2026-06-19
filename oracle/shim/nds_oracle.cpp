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

#include "oracle_hooks.h"

#include <cstdio>
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

namespace melonDS
{
// Resolved at link time by the patched NDS::SetIRQ. Must live in namespace
// melonDS to match the unqualified extern declaration inside SetIRQ.
void Oracle_OnSetIRQ(NDS* /*nds*/, u32 cpu, u32 irq)
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
}
}

// ─────────────────────────────────────────────────────── the NDS subclass ───

// Counts IPC register traffic by observing the (virtual) IO-write entry points
// the ARM cores dispatch through. IPCSYNC is 0x04000180; IPC FIFO send is
// 0x04000188. ARM9 sends → fifo9to7, ARM7 sends → fifo7to9.
class OracleNDS : public NDS
{
public:
    explicit OracleNDS(NDSArgs&& args) : NDS(std::move(args)) {}

    void ARM9IOWrite16(u32 addr, u16 val) override
    {
        if (addr == 0x04000180) g_oracle_counts.ipcsync_w++;
        NDS::ARM9IOWrite16(addr, val);
    }
    void ARM9IOWrite32(u32 addr, u32 val) override
    {
        if (addr == 0x04000180) g_oracle_counts.ipcsync_w++;
        else if (addr == 0x04000188) g_oracle_counts.fifo9to7++;
        NDS::ARM9IOWrite32(addr, val);
    }
    void ARM7IOWrite16(u32 addr, u16 val) override
    {
        if (addr == 0x04000180) g_oracle_counts.ipcsync_w++;
        NDS::ARM7IOWrite16(addr, val);
    }
    void ARM7IOWrite32(u32 addr, u32 val) override
    {
        if (addr == 0x04000180) g_oracle_counts.ipcsync_w++;
        else if (addr == 0x04000188) g_oracle_counts.fifo7to9++;
        NDS::ARM7IOWrite32(addr, val);
    }
};

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
    return UINT64_MAX;  // unknown event
}

static std::string countsJson()
{
    const OracleCounters& c = g_oracle_counts;
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"vblank9\":%llu,\"vblank7\":%llu,\"ipcsync_w\":%llu,"
        "\"fifo9to7\":%llu,\"fifo7to9\":%llu,\"dma_done\":%llu,\"timer_ovf\":%llu}",
        (unsigned long long)c.vblank9, (unsigned long long)c.vblank7,
        (unsigned long long)c.ipcsync_w, (unsigned long long)c.fifo9to7,
        (unsigned long long)c.fifo7to9, (unsigned long long)c.dma_done,
        (unsigned long long)c.timer_ovf);
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
        return countsJson();

    if (cmd == "run_to_event")
    {
        std::string ev = jsonStr(line, "event");
        uint64_t target = jsonU64(line, "count", 0);
        if (eventValue(ev) == UINT64_MAX)
            return "{\"error\":\"unknown event\"}";
        uint64_t frames = 0;
        while (eventValue(ev) < target && frames < kMaxFrames)
        {
            nds->RunFrame();
            frames++;
        }
        bool reached = eventValue(ev) >= target;
        return std::string("{\"reached\":") + (reached ? "true" : "false")
             + ",\"counts\":" + countsJson() + "}";
    }

    if (cmd == "read_mem")
    {
        uint64_t cpu  = jsonU64(line, "cpu", 9);
        uint32_t addr = (uint32_t)jsonU64(line, "addr", 0);
        uint32_t len  = (uint32_t)jsonU64(line, "len", 0);
        std::string hex;
        std::vector<uint8_t> tmp(len);
        for (uint32_t i = 0; i < len; i++)
            tmp[i] = (cpu == 7) ? nds->ARM7Read8(addr + i) : nds->ARM9Read8(addr + i);
        appendHex(hex, tmp.data(), tmp.size());
        return "{\"hex\":\"" + hex + "\"}";
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

    serve(nds, port);
    return 0;
}
