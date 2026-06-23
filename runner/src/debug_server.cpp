#include "debug_server.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "scheduler.h"
#include "state.h"
#include "io.h"
#include "runtime_arm.h"
#include "tier3.h"

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socket_t = SOCKET;
#  define CLOSESOCK closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_t = int;
#  define INVALID_SOCKET (-1)
#  define CLOSESOCK ::close
#endif

namespace {

const char HEX[] = "0123456789abcdef";

void append_hex(std::string& s, const uint8_t* data, size_t len) {
    s.reserve(s.size() + len * 2);
    for (size_t i = 0; i < len; ++i) {
        s.push_back(HEX[data[i] >> 4]);
        s.push_back(HEX[data[i] & 0xF]);
    }
}

bool json_find(const std::string& s, const std::string& key, size_t& pos) {
    std::string pat = "\"" + key + "\"";
    size_t p = s.find(pat);
    if (p == std::string::npos) return false;
    p += pat.size();
    while (p < s.size() && (s[p] == ' ' || s[p] == ':' || s[p] == '\t')) ++p;
    pos = p;
    return true;
}

std::string json_str(const std::string& s, const std::string& key,
                     const std::string& def = "") {
    size_t p;
    if (!json_find(s, key, p) || p >= s.size() || s[p] != '"') return def;
    ++p;
    std::string out;
    while (p < s.size() && s[p] != '"') out.push_back(s[p++]);
    return out;
}

uint64_t json_u64(const std::string& s, const std::string& key,
                  uint64_t def = 0) {
    size_t p;
    if (!json_find(s, key, p)) return def;
    if (p < s.size() && s[p] == '"') ++p;
    char* end = nullptr;
    uint64_t v = std::strtoull(s.c_str() + p, &end, 10);
    return (end == s.c_str() + p) ? def : v;
}

std::string counts_json() {
    const NdsEventCounts& c = nds_event_counts();
    char buf[448];
    std::snprintf(buf, sizeof(buf),
        "{\"vblank9\":%llu,\"vblank7\":%llu,\"ipcsync_w\":%llu,"
        "\"fifo9to7\":%llu,\"fifo7to9\":%llu,\"dma_done\":%llu,\"timer_ovf\":%llu,"
        "\"soundbias_w\":%llu,\"soundbias_first\":%u,\"soundbias_last\":%u}",
        (unsigned long long)c.vblank9, (unsigned long long)c.vblank7,
        (unsigned long long)c.ipcsync_w, (unsigned long long)c.fifo9to7,
        (unsigned long long)c.fifo7to9, (unsigned long long)c.dma_done,
        (unsigned long long)c.timer_ovf,
        (unsigned long long)c.soundbias_w, c.soundbias_first, c.soundbias_last);
    return buf;
}

std::string io_state_json() {
    char buf[768];
    std::snprintf(buf, sizeof(buf),
        "{\"cpu9\":{\"ime\":%u,\"ie\":%u,\"if\":%u,\"postflg\":%u,\"ipcsync\":%u},"
        "\"cpu7\":{\"ime\":%u,\"ie\":%u,\"if\":%u,\"postflg\":%u,\"ipcsync\":%u},"
        "\"cpu_stop\":0,\"num_frames\":%llu,\"counts\":%s}",
        nds_io_debug_read(9, 0x04000208, 32),
        nds_io_debug_read(9, 0x04000210, 32),
        nds_io_debug_read(9, 0x04000214, 32),
        nds_io_debug_read(9, 0x04000300, 8),
        nds_io_debug_read(9, 0x04000180, 16),
        nds_io_debug_read(7, 0x04000208, 32),
        nds_io_debug_read(7, 0x04000210, 32),
        nds_io_debug_read(7, 0x04000214, 32),
        nds_io_debug_read(7, 0x04000300, 8),
        nds_io_debug_read(7, 0x04000180, 16),
        (unsigned long long)nds_event_counts().vblank9,
        counts_json().c_str());
    return buf;
}

uint32_t spsr_for_mode(const ArmCpuState& c, uint32_t mode) {
    switch (mode) {
        case 0x11: return c.banked_spsr[ARM_BANK_FIQ];
        case 0x12: return c.banked_spsr[ARM_BANK_IRQ];
        case 0x13: return c.banked_spsr[ARM_BANK_SUPERVISOR];
        case 0x17: return c.banked_spsr[ARM_BANK_ABORT];
        case 0x1B: return c.banked_spsr[ARM_BANK_UNDEFINED];
        default: return 0;
    }
}

std::string handle(const std::string& line) {
    std::string cmd = json_str(line, "cmd");

    if (cmd == "ping") return "{\"pong\":true}";

    if (cmd == "regs") {
        uint64_t cpu = json_u64(line, "cpu", 9);
        const ArmCpuState& c = scheduler_cpu_state(cpu == 7 ? 1 : 0);
        std::string out = "{\"r\":[";
        for (int i = 0; i < 16; ++i) {
            char b[20];
            std::snprintf(b, sizeof(b), "%s%u", i ? "," : "", c.R[i]);
            out += b;
        }
        uint32_t mode = c.cpsr & 0x1F;
        char tail[128];
        std::snprintf(tail, sizeof(tail),
            "],\"cpsr\":%u,\"spsr\":%u,\"mode\":%u}",
            c.cpsr, spsr_for_mode(c, mode), mode);
        return out + tail;
    }

    if (cmd == "event_counts") return counts_json();
    if (cmd == "io_state") return io_state_json();

    if (cmd == "run_to_event") {
        std::string ev = json_str(line, "event");
        uint64_t target = json_u64(line, "count", 0);
        if (nds_event_value(ev.c_str()) == UINT64_MAX)
            return "{\"error\":\"unknown event\"}";

        uint64_t rounds = 0;
        constexpr uint64_t kMaxRounds = 5000000;
        while (nds_event_value(ev.c_str()) < target && rounds < kMaxRounds) {
            scheduler_run_round();
            ++rounds;
        }
        bool reached = nds_event_value(ev.c_str()) >= target;
        return std::string("{\"reached\":") + (reached ? "true" : "false") +
            ",\"counts\":" + counts_json() + "}";
    }

    if (cmd == "run_cycles") {
        uint64_t target = json_u64(line, "arm9", 0);
        uint64_t rounds = 0;
        constexpr uint64_t kMaxRounds = 5000000;
        while (scheduler_cpu_cycles(0) < target && rounds < kMaxRounds) {
            uint64_t before = scheduler_cpu_cycles(0);
            scheduler_run_round();
            ++rounds;
            if (scheduler_cpu_cycles(0) == before)
                break;
        }
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "{\"reached\":%s,\"cycles\":[%llu,%llu],\"counts\":%s}",
            scheduler_cpu_cycles(0) >= target ? "true" : "false",
            (unsigned long long)scheduler_cpu_cycles(0),
            (unsigned long long)scheduler_cpu_cycles(1),
            counts_json().c_str());
        return buf;
    }

    if (cmd == "run_rounds") {
        uint64_t count = json_u64(line, "count", 0);
        constexpr uint64_t kMaxRounds = 5000000;
        if (count > kMaxRounds) count = kMaxRounds;
        for (uint64_t i = 0; i < count; ++i)
            scheduler_run_round();
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "{\"rounds\":%llu,\"cycles\":[%llu,%llu],\"counts\":%s}",
            (unsigned long long)count,
            (unsigned long long)scheduler_cpu_cycles(0),
            (unsigned long long)scheduler_cpu_cycles(1),
            counts_json().c_str());
        return buf;
    }

    if (cmd == "read_region") {
        BusRegion reg{};
        if (!bus_get_region(json_str(line, "region").c_str(), &reg))
            return "{\"error\":\"unknown or absent region\"}";
        std::string hex;
        append_hex(hex, reg.ptr, reg.len);
        return "{\"hex\":\"" + hex + "\"}";
    }

    if (cmd == "read_mem") {
        uint64_t cpu = json_u64(line, "cpu", 9);
        uint32_t addr = (uint32_t)json_u64(line, "addr", 0);
        uint32_t len = (uint32_t)json_u64(line, "len", 0);
        std::vector<uint8_t> tmp(len);
        for (uint32_t i = 0; i < len; ++i)
            tmp[i] = bus_debug_read8(cpu == 7 ? 7 : 9, addr + i);
        std::string hex;
        append_hex(hex, tmp.data(), tmp.size());
        return "{\"hex\":\"" + hex + "\"}";
    }

    if (cmd == "read_io") {
        uint64_t cpu = json_u64(line, "cpu", 9);
        uint32_t addr = (uint32_t)json_u64(line, "addr", 0);
        uint32_t width = (uint32_t)json_u64(line, "width", 32);
        if (width != 8 && width != 16 && width != 32)
            return "{\"error\":\"width must be 8, 16, or 32\"}";
        char buf[64];
        std::snprintf(buf, sizeof(buf), "{\"value\":%u}",
            nds_io_debug_read(cpu == 7 ? 7 : 9, addr, width));
        return buf;
    }

    if (cmd == "watch") {
        uint32_t max = (uint32_t)json_u64(line, "max", 128);
        if (max > 512) max = 512;
        std::vector<BusWatchEvent> ev(max);
        uint32_t count = bus_debug_watch_copy(ev.data(), max);
        std::string out = "{\"events\":[";
        for (uint32_t i = 0; i < count; ++i) {
            char b[160];
            std::snprintf(b, sizeof(b),
                "%s{\"seq\":%llu,\"cpu\":%u,\"width\":%u,"
                "\"pc\":%u,\"addr\":%u,\"value\":%u}",
                i ? "," : "",
                (unsigned long long)ev[i].seq, ev[i].cpu, ev[i].width,
                ev[i].pc, ev[i].addr, ev[i].value);
            out += b;
        }
        out += "]}";
        return out;
    }

    if (cmd == "tier3_trace") {
        uint32_t max = (uint32_t)json_u64(line, "max", 128);
        if (max > 4096) max = 4096;
        std::vector<Tier3TraceEvent> ev(max);
        uint32_t count = tier3_debug_trace_copy(ev.data(), max);
        std::string out = "{\"events\":[";
        for (uint32_t i = 0; i < count; ++i) {
            char b[360];
            std::snprintf(b, sizeof(b),
                "%s{\"seq\":%llu,\"cpu\":%u,\"thumb\":%u,\"phase\":%u,"
                "\"result\":%u,\"pc\":%u,\"raw\":%u,\"next_pc\":%u,"
                "\"cpsr\":%u,\"r0\":%u,\"r1\":%u,\"r2\":%u,\"r3\":%u,"
                "\"r12\":%u,\"sp\":%u,\"lr\":%u,\"cycles\":%llu}",
                i ? "," : "",
                (unsigned long long)ev[i].seq, ev[i].cpu, ev[i].thumb,
                ev[i].phase, ev[i].result, ev[i].pc, ev[i].raw,
                ev[i].next_pc, ev[i].cpsr, ev[i].r0, ev[i].r1, ev[i].r2,
                ev[i].r3, ev[i].r12, ev[i].sp, ev[i].lr,
                (unsigned long long)ev[i].cycles);
            out += b;
        }
        out += "]}";
        return out;
    }

    if (cmd == "runtime_trace") {
        uint32_t max = (uint32_t)json_u64(line, "max", 128);
        if (max > 4096) max = 4096;
        std::vector<RuntimeTraceEntry> ev(max);
        uint32_t count = runtime_trace_copy_recent(ev.data(), max);
        std::string out = "{\"events\":[";
        for (uint32_t i = 0; i < count; ++i) {
            char b[420];
            std::snprintf(b, sizeof(b),
                "%s{\"seq\":%u,\"cycles\":%llu,\"kind\":%u,\"pc\":%u,"
                "\"cpsr\":%u,\"addr\":%u,\"value\":%u,\"aux\":%u,"
                "\"r0\":%u,\"r1\":%u,\"r2\":%u,\"r3\":%u,\"r4\":%u,"
                "\"r5\":%u,\"r12\":%u,\"sp\":%u,\"lr\":%u}",
                i ? "," : "", ev[i].seq,
                (unsigned long long)ev[i].cycles, ev[i].kind, ev[i].pc,
                ev[i].cpsr, ev[i].addr, ev[i].value, ev[i].aux,
                ev[i].r0, ev[i].r1, ev[i].r2, ev[i].r3, ev[i].r4,
                ev[i].r5, ev[i].r12, ev[i].r13, ev[i].r14);
            out += b;
        }
        out += "]}";
        return out;
    }

    if (cmd == "framebuffer")
        return "{\"error\":\"framebuffer unavailable\"}";
    if (cmd == "touch" || cmd == "keys")
        return "{\"ok\":true}";

    return "{\"error\":\"unknown cmd\"}";
}

bool send_all(socket_t s, const char* data, size_t len) {
    while (len > 0) {
        int n = send(s, data, (int)len, 0);
        if (n <= 0) return false;
        data += n;
        len -= (size_t)n;
    }
    return true;
}

}  // namespace

void debug_serve(uint16_t port) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "[debug] WSAStartup failed\n");
        return;
    }
#endif

    socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) {
        std::fprintf(stderr, "[debug] socket() failed\n");
        return;
    }
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) != 0) {
        std::fprintf(stderr, "[debug] bind() failed on port %u\n", port);
        return;
    }
    if (listen(listener, 1) != 0) {
        std::fprintf(stderr, "[debug] listen() failed\n");
        return;
    }
    std::fprintf(stderr, "[debug] listening on 127.0.0.1:%u\n", port);

    for (;;) {
        socket_t client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;

        std::string buf;
        char chunk[65536];
        bool open = true;
        while (open) {
            int n = recv(client, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            buf.append(chunk, (size_t)n);
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string req = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                std::string resp = handle(req);
                resp.push_back('\n');
                if (!send_all(client, resp.data(), resp.size())) {
                    open = false;
                    break;
                }
            }
        }
        CLOSESOCK(client);
    }
}
