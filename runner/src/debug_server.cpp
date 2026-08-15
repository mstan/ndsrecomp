#include "debug_server.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "scheduler.h"
#include "state.h"
#include "io.h"
#include "frontend.h"
#include "gpu2d.h"
#include "gpu3d.h"
#include "hle_profile.h"
#include "dispatch_stats.h"
#include "mem_timing_profile.h"
#include "net/net_ring.h"
#include "wifi_net.h"
#include "runtime_arm.h"
#include "spu.h"
#include "tier3.h"

extern "C" uint32_t g_runtime_break_pc;

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

std::function<void()> g_reset_fn;

// Play-mode flag: set by debug_pump_start(). Execution-driving commands are
// rejected while the SDL frontend owns execution (psxrecomp model — query
// the always-on rings instead of advancing the machine from a handler).
bool g_play_mode = false;

const char HEX[] = "0123456789abcdef";

void append_hex(std::string& s, const uint8_t* data, size_t len) {
    s.reserve(s.size() + len * 2);
    for (size_t i = 0; i < len; ++i) {
        s.push_back(HEX[data[i] >> 4]);
        s.push_back(HEX[data[i] & 0xF]);
    }
}

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool parse_hex_bytes(const std::string& hex, std::vector<uint8_t>& out) {
    if ((hex.size() & 1u) != 0u)
        return false;
    out.clear();
    out.reserve(hex.size() / 2u);
    for (size_t i = 0; i < hex.size(); i += 2u) {
        const int hi = hex_value(hex[i]);
        const int lo = hex_value(hex[i + 1u]);
        if (hi < 0 || lo < 0)
            return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

bool json_find(const std::string& s, const std::string& key, size_t& pos) {
    std::string pat = "\"" + key + "\"";
    size_t start = 0;
    while (true) {
        size_t p = s.find(pat, start);
        if (p == std::string::npos) return false;
        p += pat.size();
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
        if (p < s.size() && s[p] == ':') {
            ++p;
            while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
            pos = p;
            return true;
        }
        start = p;
    }
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

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(ch);
            }
            break;
        }
    }
    return out;
}

const char* net_backend_name(NdsNetBackendKind backend) {
    switch (backend) {
    case NdsNetBackendKind::Slirp: return "slirp";
    case NdsNetBackendKind::Replay: return "replay";
    case NdsNetBackendKind::Pcap: return "pcap";
    }
    return "unknown";
}

std::string ipv4_host_order_string(uint32_t value) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  static_cast<unsigned>((value >> 24u) & 0xFFu),
                  static_cast<unsigned>((value >> 16u) & 0xFFu),
                  static_cast<unsigned>((value >> 8u) & 0xFFu),
                  static_cast<unsigned>(value & 0xFFu));
    return std::string(buf);
}

uint64_t json_u64(const std::string& s, const std::string& key,
                  uint64_t def = 0) {
    size_t p;
    if (!json_find(s, key, p)) return def;
    if (p < s.size() && s[p] == '"') ++p;
    char* end = nullptr;
    uint64_t v = std::strtoull(s.c_str() + p, &end, 0);
    return (end == s.c_str() + p) ? def : v;
}

int64_t json_i64(const std::string& s, const std::string& key,
                 int64_t def = 0) {
    size_t p;
    if (!json_find(s, key, p)) return def;
    if (p < s.size() && s[p] == '"') ++p;
    char* end = nullptr;
    int64_t v = std::strtoll(s.c_str() + p, &end, 0);
    return (end == s.c_str() + p) ? def : v;
}

bool json_bool(const std::string& s, const std::string& key, bool def = false) {
    size_t p;
    if (!json_find(s, key, p)) return def;
    if (s.compare(p, 4, "true") == 0) return true;
    if (s.compare(p, 5, "false") == 0) return false;
    if (p < s.size() && s[p] == '"') ++p;
    char* end = nullptr;
    const unsigned long value = std::strtoul(s.c_str() + p, &end, 10);
    return end == s.c_str() + p ? def : value != 0;
}

uint8_t mouse_button_from_string(const std::string& value) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) {
                       if (c == '_' || c == '-') return ' ';
                       return static_cast<char>(std::tolower(c));
                   });
    while (!normalized.empty() && normalized.front() == ' ')
        normalized.erase(normalized.begin());
    while (!normalized.empty() && normalized.back() == ' ')
        normalized.pop_back();
    if (normalized.empty()) return 0;
    if (normalized == "left" || normalized == "mouse left" ||
        normalized == "left mouse") {
        return 1;
    }
    if (normalized == "middle" || normalized == "mouse middle" ||
        normalized == "middle mouse") {
        return 2;
    }
    if (normalized == "right" || normalized == "mouse right" ||
        normalized == "right mouse") {
        return 3;
    }
    if (normalized == "mouse 4" || normalized == "x1")
        return 4;
    if (normalized == "mouse 5" || normalized == "x2")
        return 5;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(normalized.c_str(), &end, 10);
    if (end == normalized.c_str() || parsed == 0 || parsed > 255)
        return 0;
    return static_cast<uint8_t>(parsed);
}

std::string counts_json() {
    const NdsEventCounts& c = nds_event_counts();
    char buf[640];
    std::snprintf(buf, sizeof(buf),
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
        (unsigned long long)g_insn_count[0], (unsigned long long)g_insn_count[1],
        (unsigned long long)scheduler_cpu_cycles(0),
        (unsigned long long)scheduler_cpu_cycles(1));
    return buf;
}

std::string direct_class_json(const uint64_t* values) {
    std::string result = "{";
    for (uint32_t index = 0; index < NDS_GPU2D_DIRECT_CLASS_COUNT; ++index) {
        if (index != 0u) result += ",";
        result += "\"";
        result += nds_gpu2d_direct_class_name(index);
        result += "\":";
        result += std::to_string(values[index]);
    }
    result += "}";
    return result;
}

std::string indexed_profile_json(const uint64_t* values, uint32_t count) {
    std::string result = "{";
    for (uint32_t index = 0; index < count; ++index) {
        if (index != 0u) result += ",";
        result += "\"" + std::to_string(index) + "\":" +
                  std::to_string(values[index]);
    }
    result += "}";
    return result;
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

const char* card_event_name(uint8_t kind) {
    switch (kind) {
        case NDS_CARD_TRACE_ROMCTRL: return "romctrl";
        case NDS_CARD_TRACE_COMMAND: return "command";
        case NDS_CARD_TRACE_DATA_READY: return "data_ready";
        case NDS_CARD_TRACE_COMPLETE: return "complete";
        default: return "unknown";
    }
}

const char* card_phase_name(uint32_t mode) {
    switch (mode) {
        case 0: return "raw";
        case 1: return "key1";
        case 2: return "normal";
        default: return "unknown";
    }
}

std::string cartridge_json(uint32_t max_entries) {
    NdsCardDebugState state{};
    nds_card_debug_state(&state);
    if (max_entries > state.capacity) max_entries = state.capacity;
    std::vector<NdsCardTraceEntry> events(max_entries);
    const uint32_t count = nds_card_debug_trace_copy(
        events.data(), max_entries);

    std::string command;
    append_hex(command, state.command, sizeof(state.command));
    std::string game_code(reinterpret_cast<const char*>(state.game_code), 4);
    char header[1024];
    std::snprintf(header, sizeof(header),
        "{\"present\":%s,\"cart_type\":%u,\"chip_id\":%u,"
        "\"rom_size\":%u,\"game_code\":\"%s\",\"owner\":%u,"
        "\"auxspicnt\":%u,\"romctrl\":%u,\"busy\":%s,"
        "\"data_ready\":%s,\"command\":\"%s\","
        "\"transfer_pos\":%u,\"transfer_len\":%u,\"transfer_dir\":%u,"
        "\"command_phase\":\"%s\",\"command_phase_id\":%u,"
        "\"data_phase_id\":%u,\"produced\":%llu,\"oldest\":%llu,"
        "\"capacity\":%u,\"events\":[",
        state.present ? "true" : "false", state.present ? 257u : 0u,
        state.chip_id, state.rom_size, game_code.c_str(),
        state.owner ? 7u : 9u, state.auxspicnt, state.romctrl,
        (state.romctrl & 0x80000000u) ? "true" : "false",
        (state.romctrl & 0x00800000u) ? "true" : "false",
        command.c_str(), state.transfer_pos, state.transfer_len,
        state.transfer_dir, card_phase_name(state.command_mode),
        state.command_mode, state.data_mode,
        (unsigned long long)state.produced,
        (unsigned long long)state.oldest, state.capacity);
    std::string out = header;

    for (uint32_t i = 0; i < count; ++i) {
        const NdsCardTraceEntry& e = events[i];
        std::string event_command;
        append_hex(event_command, e.command, sizeof(e.command));
        char item[1024];
        std::snprintf(item, sizeof(item),
            "%s{\"seq\":%llu,\"kind\":\"%s\",\"sys\":%llu,"
            "\"cyc9\":%llu,\"cyc7\":%llu,\"insn9\":%llu,"
            "\"insn7\":%llu,\"owner\":%u,\"command\":\"%s\","
            "\"requested_romctrl\":%u,\"romctrl\":%u,\"auxspicnt\":%u,"
            "\"transfer_pos\":%u,\"transfer_len\":%u,\"transfer_dir\":%u,"
            "\"word\":%u,\"start\":%s,"
            "\"command_phase_before\":\"%s\","
            "\"command_phase_after\":\"%s\","
            "\"command_phase_before_id\":%u,\"command_phase_after_id\":%u,"
            "\"data_phase_before_id\":%u,\"data_phase_after_id\":%u}",
            i ? "," : "", (unsigned long long)e.seq,
            card_event_name(e.kind), (unsigned long long)e.sys,
            (unsigned long long)e.cyc9, (unsigned long long)e.cyc7,
            (unsigned long long)e.insn9, (unsigned long long)e.insn7,
            e.owner ? 7u : 9u, event_command.c_str(),
            e.requested_romctrl, e.romctrl, e.auxspicnt,
            e.transfer_pos, e.transfer_len, e.transfer_dir, e.word,
            e.start ? "true" : "false",
            card_phase_name(e.command_mode_before),
            card_phase_name(e.command_mode_after),
            e.command_mode_before, e.command_mode_after,
            e.data_mode_before, e.data_mode_after);
        out += item;
    }
    return out + "]}";
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

    if (cmd == "reset") {
        if (!g_reset_fn) return "{\"error\":\"reset unsupported\"}";
        g_reset_fn();   // full power-on re-init (both cores from reset vectors)
        return "{\"ok\":true}";
    }

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
    if (cmd == "cartridge") {
        uint32_t max = static_cast<uint32_t>(json_u64(line, "max", 128));
        return cartridge_json(max);
    }
    if (cmd == "audio_samples") {
        const uint64_t start = json_u64(line, "start", 0);
        uint32_t count = static_cast<uint32_t>(json_u64(line, "count", 1024));
        if (count > 4096u) count = 4096u;
        const uint64_t produced = nds_spu_debug_output_produced();
        const uint64_t oldest = nds_spu_debug_output_oldest();
        if (start < oldest)
            return "{\"error\":\"audio start is no longer retained\"}";
        std::vector<int16_t> samples(count * 2u);
        const uint32_t copied = nds_spu_debug_copy_output(
            start, samples.data(), count);
        std::string pcm;
        pcm.reserve(copied * 8u);
        for (uint32_t i = 0; i < copied * 2u; ++i) {
            const uint16_t value = static_cast<uint16_t>(samples[i]);
            const uint8_t bytes[2] = {
                static_cast<uint8_t>(value),
                static_cast<uint8_t>(value >> 8),
            };
            append_hex(pcm, bytes, sizeof(bytes));
        }
        return "{\"start\":" + std::to_string(start) +
            ",\"count\":" + std::to_string(copied) +
            ",\"oldest\":" + std::to_string(oldest) +
            ",\"produced\":" + std::to_string(produced) +
            ",\"pcm_s16le\":\"" + pcm + "\"}";
    }
    if (cmd == "static_coverage") {
        const Tier3Stats s = tier3_stats();
        char buf[320];
        std::snprintf(buf, sizeof(buf),
            "{\"tier3_entries9\":%llu,\"tier3_entries7\":%llu,"
            "\"tier3_insns9\":%llu,\"tier3_insns7\":%llu,"
            "\"clean_ram_rejects9\":%llu,\"clean_ram_rejects7\":%llu}",
            (unsigned long long)s.entries[0],
            (unsigned long long)s.entries[1],
            (unsigned long long)s.instructions[0],
            (unsigned long long)s.instructions[1],
            (unsigned long long)s.clean_ram_rejects[0],
            (unsigned long long)s.clean_ram_rejects[1]);
        return buf;
    }
    if (cmd == "exec_provenance") {
        const int cpu = json_u64(line, "cpu", 9) == 7 ? 7 : 9;
        const uint32_t addr = static_cast<uint32_t>(json_u64(line, "addr", 0));
        const BusExecProvenance p = bus_debug_exec_provenance(cpu, addr);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
            "{\"cpu\":%d,\"addr\":%u,\"writable\":%s,"
            "\"written\":%s,\"generation\":%u}",
            cpu, addr, p.writable ? "true" : "false",
            p.written ? "true" : "false", p.generation);
        return buf;
    }
    if (cmd == "tier3_coverage") {
        uint32_t max = static_cast<uint32_t>(json_u64(line, "max", 65536));
        if (max > 262144u) max = 262144u;
        std::vector<Tier3CoverageEntry> entries(max);
        const uint32_t count = tier3_coverage_copy(entries.data(), max);
        std::string out = "{\"entries\":[";
        for (uint32_t i = 0; i < count; ++i) {
            char b[192];
            std::snprintf(b, sizeof(b),
                "%s{\"cpu\":%u,\"pc\":%u,\"thumb\":%u,"
                "\"kind\":%u,\"caller\":%u,\"hits\":%llu}",
                i ? "," : "", entries[i].cpu ? 7u : 9u,
                entries[i].pc, entries[i].thumb, entries[i].kind,
                entries[i].caller, (unsigned long long)entries[i].hits);
            out += b;
        }
        out += "]}";
        return out;
    }
    if (cmd == "rtc_state") {
        NdsRtcDebugState s{};
        nds_rtc_debug_state(&s);
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "{\"io\":%u,\"status1\":%u,\"status2\":%u,"
            "\"datetime\":[%u,%u,%u,%u,%u,%u,%u]}",
            s.io, s.status1, s.status2,
            s.datetime[0], s.datetime[1], s.datetime[2], s.datetime[3],
            s.datetime[4], s.datetime[5], s.datetime[6]);
        return buf;
    }
    if (cmd == "spi_sample") {
        NdsSpiTraceEntry e{};
        const uint64_t count = json_u64(line, "count", 0);
        if (!nds_spi_trace_get(count, &e)) return "{\"found\":false}";
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "{\"found\":true,\"count\":%llu,\"sys\":%llu,\"cyc9\":%llu,"
            "\"cyc7\":%llu,\"insn7\":%llu,\"pc\":%u,\"value\":%u}",
            (unsigned long long)e.count, (unsigned long long)e.sys,
            (unsigned long long)e.cyc9, (unsigned long long)e.cyc7,
            (unsigned long long)e.insn7, e.pc, e.value);
        return buf;
    }
    if (cmd == "irq_sample") {
        const int cpu = json_u64(line, "cpu", 9) == 7 ? 1 : 0;
        const uint64_t count = json_u64(line, "count", 0);
        NdsIrqTraceEntry e{};
        if (!nds_irq_trace_get(cpu, count, &e)) return "{\"found\":false}";
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "{\"found\":true,\"count\":%llu,\"sys\":%llu,\"cyc9\":%llu,"
            "\"cyc7\":%llu,\"insn\":%llu,\"return_address\":%u,"
            "\"pending\":%u,\"wifi_if\":%u,\"wifi_ie\":%u}",
            (unsigned long long)e.count, (unsigned long long)e.sys,
            (unsigned long long)e.cyc9, (unsigned long long)e.cyc7,
            (unsigned long long)e.insn, e.return_address, e.pending,
            e.wifi_if, e.wifi_ie);
        return buf;
    }
    if (cmd == "gx_state") {
        NdsGxStateSnapshot s{};
        nds_gpu3d_state(&s);
        char buf[448];
        std::snprintf(buf, sizeof(buf),
            "{\"geometry_enabled\":%u,\"rendering_enabled\":%u,"
            "\"gxstat\":%u,\"cycle_count\":%d,\"fifo_level\":%u,"
            "\"pipe_level\":%u,\"num_polygons\":%u,\"num_vertices\":%u,"
            "\"flush_request\":%u,\"num_commands\":%u,\"cur_command\":%u,"
            "\"param_count\":%u,\"total_params\":%u}",
            s.geometry_enabled, s.rendering_enabled, s.gxstat,
            s.cycle_count, s.fifo_level, s.pipe_level,
            s.num_polygons, s.num_vertices, s.flush_request,
            s.num_commands, s.cur_command, s.param_count, s.total_params);
        return buf;
    }
    if (cmd == "gx_polygon") {
        const uint64_t index = json_u64(line, "index", UINT64_MAX);
        NdsGpu3dPolygonSnapshot polygon{};
        if (index > UINT32_MAX ||
            !nds_gpu3d_render_polygon(static_cast<uint32_t>(index),
                                      &polygon)) {
            return "{\"found\":false,\"count\":" +
                std::to_string(nds_gpu3d_render_polygon_count()) + "}";
        }
        char result[512];
        std::snprintf(
            result, sizeof(result),
            "{\"found\":true,\"count\":%u,\"index\":%llu,"
            "\"submission_index\":%u,\"vertex_count\":%u,"
            "\"attr\":%u,\"tex_param\":%u,\"tex_palette\":%u,"
            "\"min_x\":%d,\"max_x\":%d,\"min_y\":%d,\"max_y\":%d,"
            "\"min_z\":%u,\"max_z\":%u}",
            nds_gpu3d_render_polygon_count(),
            static_cast<unsigned long long>(index),
            polygon.submission_index, polygon.vertex_count,
            polygon.attr, polygon.tex_param, polygon.tex_palette,
            polygon.min_x, polygon.max_x, polygon.min_y, polygon.max_y,
            polygon.min_z, polygon.max_z);
        return result;
    }
    if (cmd == "gx_polygons") {
        const uint32_t count = nds_gpu3d_render_polygon_count();
        std::string result =
            "{\"count\":" + std::to_string(count) + ",\"polygons\":[";
        for (uint32_t index = 0; index < count; ++index) {
            NdsGpu3dPolygonSnapshot polygon{};
            if (!nds_gpu3d_render_polygon(index, &polygon)) continue;
            if (result.back() != '[') result += ',';
            result +=
                "{\"index\":" + std::to_string(index) +
                ",\"submission_index\":" +
                    std::to_string(polygon.submission_index) +
                ",\"vertex_count\":" +
                    std::to_string(polygon.vertex_count) +
                ",\"attr\":" + std::to_string(polygon.attr) +
                ",\"tex_param\":" + std::to_string(polygon.tex_param) +
                ",\"tex_palette\":" +
                    std::to_string(polygon.tex_palette) +
                ",\"min_x\":" + std::to_string(polygon.min_x) +
                ",\"max_x\":" + std::to_string(polygon.max_x) +
                ",\"min_y\":" + std::to_string(polygon.min_y) +
                ",\"max_y\":" + std::to_string(polygon.max_y) +
                ",\"min_z\":" + std::to_string(polygon.min_z) +
                ",\"max_z\":" + std::to_string(polygon.max_z) + "}";
        }
        result += "]}";
        return result;
    }
    if (cmd == "gx_write_sample") {
        const uint64_t count = json_u64(line, "count", 0);
        if (count == 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "{\"latest\":%llu}",
                (unsigned long long)nds_gpu3d_write_trace_count());
            return buf;
        }
        NdsGxWriteTraceEntry e{};
        if (!nds_gpu3d_write_trace_get(count, &e)) return "{\"found\":false}";
        char buf[384];
        std::snprintf(buf, sizeof(buf),
            "{\"found\":true,\"count\":%llu,\"arm9\":%llu,\"addr\":%u,"
            "\"val\":%u,\"width\":%u,\"geometry_enabled\":%u,\"gxstat\":%u,"
            "\"pipe_level\":%u,\"gxstat_after\":%u,\"pipe_after\":%u}",
            (unsigned long long)e.count, (unsigned long long)e.arm9_cycles,
            e.addr, e.val, e.width, e.geometry_enabled, e.gxstat,
            e.pipe_level, e.gxstat_after, e.pipe_after);
        return buf;
    }
    if (cmd == "gx_run_sample") {
        const uint64_t count = json_u64(line, "count", 0);
        if (count == 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "{\"latest\":%llu}",
                (unsigned long long)nds_gpu3d_run_trace_count());
            return buf;
        }
        NdsGxRunTraceEntry e{};
        if (!nds_gpu3d_run_trace_get(count, &e)) return "{\"found\":false}";
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "{\"found\":true,\"count\":%llu,\"arm9\":%llu,"
            "\"stat_before\":%u,\"stat_after\":%u,\"cc_before\":%d,"
            "\"cc_after\":%d}",
            (unsigned long long)e.count, (unsigned long long)e.arm9_cycles,
            e.gxstat_before, e.gxstat_after,
            e.cycle_count_before, e.cycle_count_after);
        return buf;
    }
    if (cmd == "dma_sample") {
        const uint64_t count = json_u64(line, "count", 0);
        if (count == 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "{\"latest\":%llu}",
                (unsigned long long)nds_dma_trace_count());
            return buf;
        }
        NdsDmaTraceEntry e{};
        if (!nds_dma_trace_get(count, &e)) return "{\"found\":false}";
        char buf[320];
        std::snprintf(buf, sizeof(buf),
            "{\"found\":true,\"count\":%llu,\"sys\":%llu,\"cyc\":%llu,"
            "\"insn\":%llu,\"cpu\":%u,\"ch\":%u,\"cnt\":%u,\"src\":%u,"
            "\"dst\":%u,\"start_mode\":%u}",
            (unsigned long long)e.count, (unsigned long long)e.sys,
            (unsigned long long)e.cyc, (unsigned long long)e.insn,
            e.cpu, e.ch, e.cnt, e.src, e.dst, e.start_mode);
        return buf;
    }
    if (cmd == "insn_sample") {
        const int cpu = json_u64(line, "cpu", 9) == 7 ? 1 : 0;
        const uint64_t count = json_u64(line, "count", 0);
        NdsInsnTraceEntry e{};
        if (!nds_insn_trace_get(cpu, count, &e)) return "{\"found\":false}";
        char buf[320];
        std::snprintf(buf, sizeof(buf),
            "{\"found\":true,\"count\":%llu,\"sys\":%llu,\"cycles\":%llu,"
            "\"pc\":%u,\"cpsr\":%u,\"pending\":%u,\"r\":[",
            (unsigned long long)e.count, (unsigned long long)e.sys,
            (unsigned long long)e.cycles, e.pc, e.cpsr, e.pending);
        std::string out = buf;
        for (int i = 0; i < 15; ++i) {
            if (i) out += ',';
            out += std::to_string(e.r[i]);
        }
        return out + "]}";
    }
    if (cmd == "fifo_sample") {
        const int cpu = json_u64(line, "cpu", 9) == 7 ? 1 : 0;
        const uint64_t count = json_u64(line, "count", 0);
        NdsFifoTraceEntry e{};
        if (!nds_fifo_trace_get(cpu, count, &e)) return "{\"found\":false}";
        char buf[320];
        std::snprintf(buf, sizeof(buf),
            "{\"found\":true,\"count\":%llu,\"sys\":%llu,\"cyc9\":%llu,"
            "\"cyc7\":%llu,\"insn9\":%llu,\"insn7\":%llu,\"value\":%u}",
            (unsigned long long)e.count, (unsigned long long)e.sys,
            (unsigned long long)e.cyc9, (unsigned long long)e.cyc7,
            (unsigned long long)e.insn9, (unsigned long long)e.insn7, e.value);
        return buf;
    }
    // ── Network event ring (Wiimmfi M0) ─────────────────────────────────
    // Read-only ring queries, same "by ordinal" / "most recent N" idioms as
    // the rest of this group above. No call site pushes into the ring yet
    // (Wi-Fi device/AP/bridge/backend are later phases), so a normal run
    // will see an empty ring here — {"latest":0}, {"found":false} for any
    // nonzero count, and zero entries from net_ring_dump. That is the
    // correct, expected shape of an inert-but-present query surface, not an
    // error. None of these three commands advance execution, so none of
    // them belong in the play-mode execution guard below.
    if (cmd == "net_sample") {
        const uint64_t count = json_u64(line, "count", 0);
        if (count == 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "{\"latest\":%llu}",
                (unsigned long long)net_ring_latest());
            return buf;
        }
        NdsNetTraceEntry e{};
        if (!net_ring_get(count, &e)) return "{\"found\":false}";
        std::string src_mac_hex, dst_mac_hex;
        append_hex(src_mac_hex, e.src_mac, sizeof(e.src_mac));
        append_hex(dst_mac_hex, e.dst_mac, sizeof(e.dst_mac));
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "{\"found\":true,\"count\":%llu,\"sys\":%llu,\"cyc9\":%llu,"
            "\"cyc7\":%llu,\"insn9\":%llu,\"insn7\":%llu,\"arm9_pc\":%u,"
            "\"arm7_pc\":%u,\"kind\":\"%s\",\"direction\":%u,"
            "\"wifi_reg\":%u,\"wifi_value\":%u,\"src_mac\":\"%s\","
            "\"dst_mac\":\"%s\",\"src_ipv4\":%u,\"dst_ipv4\":%u,"
            "\"src_port\":%u,\"dst_port\":%u,\"payload_len\":%u,\"aux\":%u",
            (unsigned long long)e.count, (unsigned long long)e.sys,
            (unsigned long long)e.cyc9, (unsigned long long)e.cyc7,
            (unsigned long long)e.insn9, (unsigned long long)e.insn7,
            e.arm9_pc, e.arm7_pc, nds_net_event_kind_name(e.kind),
            e.direction, e.wifi_reg, e.wifi_value, src_mac_hex.c_str(),
            dst_mac_hex.c_str(), e.src_ipv4, e.dst_ipv4, e.src_port,
            e.dst_port, e.payload_len, e.aux);
        std::string out = buf;
        if (e.has_hostname) {
            char hostname[kNetHostnameMaxLen] = {};
            if (net_ring_get_hostname(e.count, hostname, sizeof(hostname)))
                out += ",\"hostname\":\"" + std::string(hostname) + "\"";
        }
        out += "}";
        return out;
    }
    if (cmd == "net_ring_dump") {
        uint32_t max = static_cast<uint32_t>(json_u64(line, "max", 128));
        if (max > 4096) max = 4096;
        const std::string filter_str = json_str(line, "filter", "all");
        uint8_t filter = NDS_NET_EVENT_KIND_COUNT;
        if (!nds_net_event_kind_parse(filter_str.c_str(), &filter))
            return "{\"error\":\"unknown filter '" + filter_str + "'\"}";
        std::vector<NdsNetTraceEntry> ev(max ? max : 1);
        const uint32_t count =
            max ? net_ring_copy_recent(ev.data(), max, filter) : 0;
        std::string out = "{\"events\":[";
        for (uint32_t i = 0; i < count; ++i) {
            const NdsNetTraceEntry& e = ev[i];
            std::string src_mac_hex, dst_mac_hex;
            append_hex(src_mac_hex, e.src_mac, sizeof(e.src_mac));
            append_hex(dst_mac_hex, e.dst_mac, sizeof(e.dst_mac));
            char b[512];
            std::snprintf(b, sizeof(b),
                "%s{\"count\":%llu,\"sys\":%llu,\"cyc9\":%llu,\"cyc7\":%llu,"
                "\"insn9\":%llu,\"insn7\":%llu,\"arm9_pc\":%u,\"arm7_pc\":%u,"
                "\"kind\":\"%s\",\"direction\":%u,\"wifi_reg\":%u,"
                "\"wifi_value\":%u,\"src_mac\":\"%s\",\"dst_mac\":\"%s\","
                "\"src_ipv4\":%u,\"dst_ipv4\":%u,\"src_port\":%u,"
                "\"dst_port\":%u,\"payload_len\":%u,\"aux\":%u",
                i ? "," : "", (unsigned long long)e.count,
                (unsigned long long)e.sys, (unsigned long long)e.cyc9,
                (unsigned long long)e.cyc7, (unsigned long long)e.insn9,
                (unsigned long long)e.insn7, e.arm9_pc, e.arm7_pc,
                nds_net_event_kind_name(e.kind), e.direction, e.wifi_reg,
                e.wifi_value, src_mac_hex.c_str(), dst_mac_hex.c_str(),
                e.src_ipv4, e.dst_ipv4, e.src_port, e.dst_port,
                e.payload_len, e.aux);
            out += b;
            // Same convention as net_sample: attach the hostname (DNS
            // events only) as an extra JSON field rather than a raw C
            // buffer append, so a filtered dns_query/dns_response dump is
            // actually useful without a second net_sample round trip per
            // entry.
            if (e.has_hostname) {
                char hostname[kNetHostnameMaxLen] = {};
                if (net_ring_get_hostname(e.count, hostname, sizeof(hostname)))
                    out += ",\"hostname\":\"" + std::string(hostname) + "\"";
            }
            out += "}";
        }
        out += "]}";
        return out;
    }
    if (cmd == "net_state") {
        NdsNetRingState st{};
        net_ring_debug_state(&st);
        NdsWifiNetworkState net{};
        const bool has_net = nds_wifi_network_state(&net);
        const std::string wfc_dns_ip =
            has_net ? ipv4_host_order_string(net.wfc_dns_ipv4) : "0.0.0.0";
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "{\"produced\":%llu,\"oldest\":%llu,\"capacity\":%u,"
            "\"wifi_attached\":%s,\"network_enabled\":%s,"
            "\"backend\":\"%s\",\"live_backend_active\":%s,"
            "\"replay_backend_active\":%s,\"worker_active\":%s,"
            "\"wfc_enabled\":%s,\"wfc_dns_ipv4\":%u,"
            "\"wfc_dns_ip\":\"%s\","
            "\"pcap_adapter_selected\":%s",
            (unsigned long long)st.produced, (unsigned long long)st.oldest,
            st.capacity,
            has_net && net.attached ? "true" : "false",
            has_net && net.network_enabled ? "true" : "false",
            has_net ? net_backend_name(net.backend) : "none",
            has_net && net.live_backend_active ? "true" : "false",
            has_net && net.replay_backend_active ? "true" : "false",
            has_net && net.worker_active ? "true" : "false",
            has_net && net.wfc_enabled ? "true" : "false",
            has_net ? net.wfc_dns_ipv4 : 0u,
            wfc_dns_ip.c_str(),
            has_net && net.pcap_adapter_selected ? "true" : "false");
        std::string out = buf;
        if (has_net) {
            out += ",\"pcap_adapter_requested\":\"" +
                   json_escape(net.pcap_adapter_requested) + "\"";
            out += ",\"pcap_device_name\":\"" +
                   json_escape(net.pcap_device_name) + "\"";
            out += ",\"pcap_friendly_name\":\"" +
                   json_escape(net.pcap_friendly_name) + "\"";
            out += ",\"pcap_description\":\"" +
                   json_escape(net.pcap_description) + "\"";
            out += ",\"pcap_ipv4\":\"" +
                   json_escape(net.pcap_ipv4) + "\"";
        }
        out += "}";
        return out;
    }
    if (cmd == "net_replay_status") {
        // Wiimmfi M8: live query, mid-session -- never wait for the
        // process to exit to learn a replay's outcome (this project's own
        // always-on-ring philosophy, DEBUG.md: query the live state, don't
        // arm-then-capture). Does not advance execution, so it does not
        // belong in the play-mode execution guard either.
        NdsNetReplayStatus st{};
        if (!nds_wifi_replay_status(&st)) return "{\"active\":false}";
        char buf[320];
        std::snprintf(buf, sizeof(buf),
            "{\"active\":true,\"mismatch\":%s,\"tx_matched\":%llu,"
            "\"tx_total\":%llu,\"rx_delivered\":%llu,\"rx_total\":%llu",
            st.mismatch ? "true" : "false",
            (unsigned long long)st.tx_matched, (unsigned long long)st.tx_total,
            (unsigned long long)st.rx_delivered, (unsigned long long)st.rx_total);
        std::string out = buf;
        if (st.mismatch) {
            char mbuf[384];
            std::snprintf(mbuf, sizeof(mbuf),
                ",\"mismatch_tx_frame_index\":%llu,\"mismatch_guest_cycle\":%llu,"
                "\"mismatch_arm9_pc\":%u,\"mismatch_arm7_pc\":%u,"
                "\"mismatch_reason\":\"%s\"",
                (unsigned long long)st.mismatch_tx_frame_index,
                (unsigned long long)st.mismatch_guest_cycle,
                st.mismatch_arm9_pc, st.mismatch_arm7_pc,
                st.mismatch_reason.c_str());
            out += mbuf;
        }
        out += "}";
        return out;
    }
    if (cmd == "firmware_dump") {
        // Read-only export of the private in-memory firmware image. This is
        // used by WFC setup flows that intentionally end by powering the DS
        // off after saving connection settings. The caller owns where the
        // bytes are written; do not publish dumps containing real console
        // identity or service configuration.
        const uint8_t* data = nds_firmware_bytes();
        const uint32_t size = nds_firmware_size();
        std::string hex;
        if (data && size) append_hex(hex, data, size);
        return "{\"size\":" + std::to_string(size) +
               ",\"hex\":\"" + hex + "\"}";
    }
    if (cmd == "firmware_replace") {
        // Local-owner operation for M7 prepared-save launches: boot the
        // firmware menu from the pristine dump, then install the guest-written
        // WFC identity bytes before the game reads firmware over SPI.
        std::vector<uint8_t> data;
        if (!parse_hex_bytes(json_str(line, "hex"), data))
            return "{\"error\":\"invalid firmware hex\"}";
        if (!nds_io_replace_firmware(data.data(),
                                     static_cast<uint32_t>(data.size())))
            return "{\"error\":\"firmware image must be exactly 262144 bytes\"}";
        return "{\"ok\":true,\"size\":" + std::to_string(data.size()) + "}";
    }
    if (cmd == "io_state") return io_state_json();
    if (cmd == "frontend_stats") {
        // Cumulative frontend counters; sample twice and diff for fps /
        // phase shares over the window. active=0 → headless (all zeros).
        NdsFrontendLiveStats s{};
        nds_frontend_live_stats(&s);
        return "{\"active\":" + std::to_string(s.active) +
               ",\"frames\":" + std::to_string(s.frames) +
               ",\"emu_ticks\":" + std::to_string(s.emu_ticks) +
               ",\"present_ticks\":" + std::to_string(s.present_ticks) +
               ",\"adaptive_ticks\":" + std::to_string(s.adaptive_ticks) +
               ",\"upload_ticks\":" + std::to_string(s.upload_ticks) +
               ",\"draw_ticks\":" + std::to_string(s.draw_ticks) +
               ",\"swap_ticks\":" + std::to_string(s.swap_ticks) +
               ",\"drain_ticks\":" + std::to_string(s.drain_ticks) +
               ",\"now_ticks\":" + std::to_string(s.now_ticks) +
               ",\"freq\":" + std::to_string(s.freq) +
               ",\"underruns\":" + std::to_string(s.underruns) + "}";
    }
    if (cmd == "frontend_input_stats") {
        NdsFrontendInputDebugState s{};
        nds_frontend_input_debug_state(&s);
        return "{\"active\":" + std::to_string(s.active) +
               ",\"mph_prime_controls_available\":" +
               std::to_string(s.mph_prime_controls_available) +
               ",\"mph_prime_controls_active\":" +
               std::to_string(s.mph_prime_controls_active) +
               ",\"relative_mouse_captured\":" +
               std::to_string(s.relative_mouse_captured) +
               ",\"keyboard_pressed\":" +
               std::to_string(s.keyboard_pressed) +
               ",\"mouse_pressed\":" + std::to_string(s.mouse_pressed) +
               ",\"mph_prime_pressed\":" +
               std::to_string(s.mph_prime_pressed) +
               ",\"stick_pressed\":" + std::to_string(s.stick_pressed) +
               ",\"pad_engaged\":" + std::to_string(s.pad_engaged) +
               ",\"pad_aim_writes\":" + std::to_string(s.pad_aim_writes) +
               ",\"published_key_mask\":" +
               std::to_string(s.published_key_mask) +
               ",\"relative_direct_writes\":" +
               std::to_string(s.relative_direct_writes) +
               ",\"mph_prime_key_downs\":" +
               std::to_string(s.mph_prime_key_downs) +
               ",\"mph_prime_mouse_downs\":" +
               std::to_string(s.mph_prime_mouse_downs) +
               ",\"debug_key_events\":" +
               std::to_string(s.debug_key_events) +
               ",\"debug_mouse_button_events\":" +
               std::to_string(s.debug_mouse_button_events) +
               ",\"debug_mouse_motion_events\":" +
               std::to_string(s.debug_mouse_motion_events) +
               ",\"debug_touch_events\":" +
               std::to_string(s.debug_touch_events) +
               ",\"debug_capture_events\":" +
               std::to_string(s.debug_capture_events) +
               ",\"debug_release_events\":" +
               std::to_string(s.debug_release_events) +
               ",\"debug_event_errors\":" +
               std::to_string(s.debug_event_errors) +
               ",\"debug_last_key_scancode\":" +
               std::to_string(s.debug_last_key_scancode) +
               ",\"virtual_stylus_x\":" +
               std::to_string(s.virtual_stylus_x) +
               ",\"virtual_stylus_y\":" +
               std::to_string(s.virtual_stylus_y) +
               ",\"top_window_id\":" + std::to_string(s.top_window_id) +
               ",\"bottom_window_id\":" +
               std::to_string(s.bottom_window_id) +
               ",\"bottom_content_left\":" +
               std::to_string(s.bottom_content_left) +
               ",\"separate\":" + std::to_string(s.separate) + "}";
    }
    if (cmd == "frontend_input") {
        const std::string action = json_str(line, "action");
        bool ok = false;
        if (action == "capture") {
            ok = nds_frontend_debug_capture_mouse();
        } else if (action == "release") {
            ok = nds_frontend_debug_release_mouse();
        } else if (action == "key") {
            ok = nds_frontend_debug_key(
                json_str(line, "key").c_str(),
                json_bool(line, "down", true));
        } else if (action == "mouse") {
            const std::string named = json_str(line, "button");
            uint8_t button = mouse_button_from_string(named);
            if (button == 0)
                button = static_cast<uint8_t>(json_u64(line, "button", 0));
            ok = nds_frontend_debug_mouse_button(
                button, json_bool(line, "down", true));
        } else if (action == "motion") {
            ok = nds_frontend_debug_mouse_motion(
                static_cast<int>(json_i64(line, "dx", 0)),
                static_cast<int>(json_i64(line, "dy", 0)));
        } else if (action == "touch") {
            ok = nds_frontend_debug_touch(
                static_cast<uint16_t>(json_u64(line, "x", 0)),
                static_cast<uint16_t>(json_u64(line, "y", 0)),
                json_bool(line, "down", true));
        } else {
            return "{\"error\":\"unknown frontend_input action\"}";
        }
        return "{\"ok\":" + std::string(ok ? "true" : "false") + "}";
    }
    if (cmd == "frontend_exit") {
        const bool requested = nds_frontend_request_exit();
        return "{\"requested\":" +
               std::to_string(requested ? 1 : 0) + "}";
    }
    if (cmd == "black_band_scan") {
        const bool enabled = json_bool(line, "on", true);
        const bool reset = json_bool(line, "reset", false);
        nds_frontend_black_band_scan(enabled, reset);
        return "{\"enabled\":" + std::to_string(enabled ? 1 : 0) + "}";
    }
    if (cmd == "black_band_capture") {
        NdsFrontendBlackBandCapture capture{};
        nds_frontend_black_band_capture(&capture);
        std::string rgb;
        if (capture.has_capture) {
            rgb.reserve(256u * 192u * 6u);
            for (size_t i = 0; i < 256u * 192u; ++i) {
                const uint32_t px = capture.top_pixels[i];
                const uint8_t c[3] = {
                    static_cast<uint8_t>(px >> 16),
                    static_cast<uint8_t>(px >> 8),
                    static_cast<uint8_t>(px),
                };
                append_hex(rgb, c, sizeof(c));
            }
        }
        return "{\"enabled\":" + std::to_string(capture.enabled) +
               ",\"has_capture\":" + std::to_string(capture.has_capture) +
               ",\"scanned_frames\":" +
               std::to_string(capture.scanned_frames) +
               ",\"band_frames\":" + std::to_string(capture.band_frames) +
               ",\"worst_frame\":" + std::to_string(capture.worst_frame) +
               ",\"worst_system_timestamp\":" +
               std::to_string(capture.worst_system_timestamp) +
               ",\"worst_start_row\":" +
               std::to_string(capture.worst_start_row) +
               ",\"worst_row_count\":" +
               std::to_string(capture.worst_row_count) +
               ",\"w\":256,\"h\":192,\"rgb\":\"" + rgb + "\"}";
    }
    if (cmd == "framebuffer_sync") {
        NdsFrontendLiveStats stats{};
        nds_frontend_live_stats(&stats);
        if (stats.active) nds_gpu2d_force_cpu_frames(2);
        return "{\"active\":" + std::to_string(stats.active) +
               ",\"frames\":" + std::to_string(stats.frames) + "}";
    }
    if (cmd == "hle_heat") return nds_hle_profile_json();
    if (cmd == "mem_timing_profile") return nds_mem_timing_profile_json();
    if (cmd == "dispatch_stats") return nds_dispatch_stats_json();
    if (cmd == "cart_save_info") {
        const uint8_t* data = nullptr;
        uint32_t size = 0;
        bool dirty = false;
        if (!nds_io_cartridge_save_snapshot(&data, &size, &dirty))
            return "{\"error\":\"no cartridge save is present\"}";
        (void)data;
        return "{\"size\":" + std::to_string(size) +
            ",\"dirty\":" + std::to_string(dirty ? 1 : 0) + "}";
    }
    if (cmd == "cart_save") {
        const uint8_t* data = nullptr;
        uint32_t size = 0;
        bool dirty = false;
        if (!nds_io_cartridge_save_snapshot(&data, &size, &dirty))
            return "{\"error\":\"no cartridge save is present\"}";
        std::string hex;
        append_hex(hex, data, size);
        return "{\"size\":" + std::to_string(size) +
            ",\"dirty\":" + std::to_string(dirty ? 1 : 0) +
            ",\"hex\":\"" + hex + "\"}";
    }
    if (cmd == "cart_save_flush")
        return std::string("{\"ok\":") +
            (nds_io_flush_cartridge_save() ? "true}" : "false}");
    if (cmd == "profile") {
        // Raw NDS_PROFILE_GPU / NDS_PROFILE_SCHED accumulators (zero unless
        // the corresponding env var armed sampling at process start).
        NdsGpu2dProfile gpu{};
        nds_gpu2d_profile(&gpu);
        NdsGpu3dProfile gpu3d{};
        nds_gpu3d_profile(&gpu3d);
        NdsSchedulerProfile sched{};
        scheduler_profile(&sched);
        const std::string direct_class_frames =
            direct_class_json(gpu.direct_class_frames);
        const std::string direct_class_engine_a_ns =
            direct_class_json(gpu.direct_class_engine_a_ns);
        const std::string direct_extra_bg_mask_frames =
            indexed_profile_json(gpu.direct_extra_bg_mask_frames,
                                 NDS_GPU2D_DIRECT_BG_MASK_COUNT);
        const std::string direct_extra_bg_mask_engine_a_ns =
            indexed_profile_json(gpu.direct_extra_bg_mask_engine_a_ns,
                                 NDS_GPU2D_DIRECT_BG_MASK_COUNT);
        const std::string direct_extra_bg_mode_frames =
            indexed_profile_json(gpu.direct_extra_bg_mode_frames,
                                 NDS_GPU2D_DIRECT_BG_MODE_COUNT);
        const std::string direct_extra_effect_frames =
            indexed_profile_json(gpu.direct_extra_effect_frames,
                                 NDS_GPU2D_DIRECT_EFFECT_MODE_COUNT);
        const std::string direct_extra_master_bright_frames =
            indexed_profile_json(gpu.direct_extra_master_bright_frames,
                                 NDS_GPU2D_DIRECT_EFFECT_MODE_COUNT);
        return "{\"gpu2d\":{\"render_ns\":" + std::to_string(gpu.render_ns) +
               ",\"engine_a_ns\":" + std::to_string(gpu.engine_ns[0]) +
               ",\"engine_b_ns\":" + std::to_string(gpu.engine_ns[1]) +
               ",\"obj_ns\":" + std::to_string(gpu.obj_ns) +
               ",\"direct_overlay_ns\":" +
               std::to_string(gpu.direct_overlay_ns) +
               ",\"direct_frames\":" +
               std::to_string(gpu.direct_frames) +
               ",\"direct_class_frames\":" + direct_class_frames +
               ",\"direct_class_engine_a_ns\":" +
               direct_class_engine_a_ns +
               ",\"direct_class_transitions\":" +
               std::to_string(gpu.direct_class_transitions) +
               ",\"direct_extra_bg_mask_frames\":" +
               direct_extra_bg_mask_frames +
               ",\"direct_extra_bg_mask_engine_a_ns\":" +
               direct_extra_bg_mask_engine_a_ns +
               ",\"direct_extra_bg_mode_frames\":" +
               direct_extra_bg_mode_frames +
               ",\"direct_extra_effect_frames\":" +
               direct_extra_effect_frames +
               ",\"direct_extra_master_bright_frames\":" +
               direct_extra_master_bright_frames +
               ",\"scanlines\":" + std::to_string(gpu.scanlines) +
               "},\"gpu3d\":{\"vcount215_ns\":" +
               std::to_string(gpu3d.vcount215_ns) +
               ",\"vcount215_calls\":" +
               std::to_string(gpu3d.vcount215_calls) +
               ",\"getline_ns\":" + std::to_string(gpu3d.getline_ns) +
               ",\"getline_calls\":" + std::to_string(gpu3d.getline_calls) +
               ",\"vcount144_ns\":" + std::to_string(gpu3d.vcount144_ns) +
               ",\"vcount144_calls\":" +
               std::to_string(gpu3d.vcount144_calls) +
               ",\"compute_sync_ns\":" +
               std::to_string(gpu3d.compute_sync_ns) +
               ",\"compute_sync_calls\":" +
               std::to_string(gpu3d.compute_sync_calls) +
               ",\"compute_submit_ns\":" +
               std::to_string(gpu3d.compute_submit_ns) +
               ",\"compute_submit_calls\":" +
               std::to_string(gpu3d.compute_submit_calls) +
               ",\"compute_map_ns\":" +
               std::to_string(gpu3d.compute_map_ns) +
               ",\"compute_map_calls\":" +
               std::to_string(gpu3d.compute_map_calls) +
               "},\"sched\":{\"sampled_rounds\":" +
               std::to_string(sched.sampled_rounds) +
               ",\"rounds\":" + std::to_string(sched.rounds) +
               ",\"sampled_round_ns\":" + std::to_string(sched.sampled_round_ns) +
               ",\"next_event_ns\":" + std::to_string(sched.next_event_ns) +
               ",\"arm9_ns\":" + std::to_string(sched.arm9_ns) +
               ",\"arm7_ns\":" + std::to_string(sched.arm7_ns) +
               ",\"devices_ns\":" + std::to_string(sched.devices_ns) +
               ",\"display_ns\":" + std::to_string(sched.display_ns) +
               ",\"spu_ns\":" + std::to_string(sched.spu_ns) +
               ",\"wifi_ns\":" + std::to_string(sched.wifi_ns) +
               ",\"rtc_ns\":" + std::to_string(sched.rtc_ns) +
               ",\"sysev_ns\":" + std::to_string(sched.sysev_ns) +
               ",\"switch_ns\":" + std::to_string(sched.switch_ns) +
               ",\"switches\":" + std::to_string(sched.switches) +
               ",\"crs_words\":" + std::to_string(sched.crs_words) + "}}";
    }
    if (cmd == "deep_trace") {
        // Live toggle for the per-access payload policy (bus ring, mem_r/w
        // events, per-insn register images + the B3 inline bus fast path,
        // which engages while this is off). Play mode has a query surface,
        // so the payloads are armable on demand.
        runtime_set_deep_trace(
            static_cast<uint32_t>(json_u64(line, "on", 1)));
        return "{\"deep_trace\":" + std::to_string(g_runtime_deep_trace) + "}";
    }
    if (cmd == "sched_state") {
        char buf[768];
        std::snprintf(buf, sizeof(buf),
            "{\"sys\":%llu,\"arm9\":%llu,\"arm7\":%llu,"
            "\"next\":%llu,\"spi\":%llu,\"card\":%llu,"
            "\"terminal9\":%s,\"terminal7\":%s,"
            "\"reason9\":\"%s\",\"reason7\":\"%s\"}",
            (unsigned long long)scheduler_system_timestamp(),
            (unsigned long long)scheduler_cpu_cycles(0),
            (unsigned long long)scheduler_cpu_cycles(1),
            (unsigned long long)scheduler_next_event_timestamp(),
            (unsigned long long)nds_debug_spi_deadline(),
            (unsigned long long)nds_debug_card_deadline(),
            scheduler_cpu_terminal_halted(0) ? "true" : "false",
            scheduler_cpu_terminal_halted(1) ? "true" : "false",
            scheduler_cpu_halt_reason(0), scheduler_cpu_halt_reason(1));
        return buf;
    }
    // Execution-driving commands are serve-mode only: in play mode the
    // frontend owns execution; a handler advancing the machine here would
    // fight the frame loop. Query the always-on rings / event_counts and
    // sample twice instead.
    if (g_play_mode && (cmd == "run_to_pc" || cmd == "run_to_event" ||
                        cmd == "run_cycles" || cmd == "run_rounds"))
        return "{\"error\":\"" + cmd + " unavailable in play mode: the "
               "frontend owns execution; query rings/event_counts and sample "
               "twice\"}";
    if (cmd == "run_to_pc") {
        const uint32_t pc = static_cast<uint32_t>(json_u64(line, "pc", 0));
        uint64_t max_rounds = json_u64(line, "max_rounds", 50000000);
        if (!pc) return "{\"error\":\"pc must be nonzero\"}";
        if (max_rounds > 100000000u) max_rounds = 100000000u;
        g_runtime_break_pc = pc;
        uint64_t rounds = 0;
        while (!scheduler_cpu_terminal_halted(0) &&
               !scheduler_cpu_terminal_halted(1) && rounds < max_rounds) {
            scheduler_run_round();
            ++rounds;
        }
        g_runtime_break_pc = 0;
        const bool reached = ((g_cpu.R[15] & ~1u) == (pc & ~1u)) &&
            (scheduler_cpu_terminal_halted(0) || scheduler_cpu_terminal_halted(1));
        return std::string("{\"reached\":") + (reached ? "true" : "false") +
            ",\"rounds\":" + std::to_string(rounds) +
            ",\"reason9\":\"" + scheduler_cpu_halt_reason(0) + "\"" +
            ",\"reason7\":\"" + scheduler_cpu_halt_reason(1) + "\"" +
            ",\"counts\":" + counts_json() + "}";
    }
    if (cmd == "cp15_state") {
        std::string out = "{\"control\":" + std::to_string(g_cp15.control) +
                          ",\"regions\":[";
        for (unsigned i = 0; i < 8; ++i) {
            if (i) out += ',';
            out += std::to_string(cp15_debug_mpu_region(i));
        }
        out += "],\"cache_cfg\":[";
        for (unsigned i = 0; i < 8; ++i) {
            if (i) out += ',';
            out += std::to_string(cp15_debug_cache_cfg(i));
        }
        out += "]}";
        return out;
    }

    if (cmd == "run_to_event") {
        std::string ev = json_str(line, "event");
        uint64_t target = json_u64(line, "count", 0);
        if (nds_event_value(ev.c_str()) == UINT64_MAX)
            return "{\"error\":\"unknown event\"}";

        // Arm the sub-event break so the slice stops AT the Nth event, not at
        // the next round boundary. The loop condition (checked each round)
        // guarantees we exit once the target is reached.
        nds_event_break_arm(ev.c_str(), target);
        uint64_t rounds = 0;
        // Long firmware-menu waits eventually exceed the original fixed 5M
        // scheduler-round ceiling even though both CPUs and devices are still
        // progressing. Keep a defensive cap, but make it an explicit protocol
        // control so a failed run can never masquerade as a CPU divergence.
        uint64_t max_rounds = json_u64(line, "max_rounds", 50000000);
        if (max_rounds > 100000000u) max_rounds = 100000000u;
        // No-progress early-out: if the watched counter has not advanced for
        // this many consecutive rounds the boot has stalled (diverged into an
        // idle loop / halt that never reaches the Nth event), so bail with
        // reached=false instead of grinding to kMaxRounds (minutes). Sized far
        // above the largest legitimate inter-event gap during boot (a few
        // VBlank waits ~ hundreds of rounds); overridable via "stall".
        uint64_t stall_limit = json_u64(line, "stall", 300000);
        uint64_t last_val = nds_event_value(ev.c_str());
        uint64_t last_sys = scheduler_system_timestamp();
        uint64_t last_cyc9 = scheduler_cpu_cycles(0);
        uint64_t last_cyc7 = scheduler_cpu_cycles(1);
        uint64_t stale = 0;
        bool stalled = false;
        bool terminal = false;
        while (nds_event_value(ev.c_str()) < target && rounds < max_rounds) {
            scheduler_run_round();
            ++rounds;
            uint64_t v = nds_event_value(ev.c_str());
            const uint64_t sys = scheduler_system_timestamp();
            const uint64_t cyc9 = scheduler_cpu_cycles(0);
            const uint64_t cyc7 = scheduler_cpu_cycles(1);
            if (scheduler_cpu_terminal_halted(0) ||
                scheduler_cpu_terminal_halted(1)) {
                terminal = true;
                stalled = true;
                break;
            }
            if (v > last_val || sys != last_sys ||
                cyc9 != last_cyc9 || cyc7 != last_cyc7) {
                last_val = v;
                last_sys = sys;
                last_cyc9 = cyc9;
                last_cyc7 = cyc7;
                stale = 0;
            } else if (++stale >= stall_limit) {
                stalled = true;
                break;
            }
        }
        nds_event_break_disarm();
        bool reached = nds_event_value(ev.c_str()) >= target;
        bool exhausted = !reached && !stalled && rounds >= max_rounds;
        return std::string("{\"reached\":") + (reached ? "true" : "false") +
            ",\"stalled\":" + (stalled ? "true" : "false") +
            ",\"terminal\":" + (terminal ? "true" : "false") +
            ",\"reason9\":\"" + scheduler_cpu_halt_reason(0) + "\"" +
            ",\"reason7\":\"" + scheduler_cpu_halt_reason(1) + "\"" +
            ",\"exhausted\":" + (exhausted ? "true" : "false") +
            ",\"rounds\":" + std::to_string(rounds) +
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
        char buf[768];
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
        char buf[768];
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
            char b[240];
            std::snprintf(b, sizeof(b),
                "%s{\"seq\":%llu,\"cycles\":%llu,\"insn\":%llu,"
                "\"cpu\":%u,\"write\":%u,\"width\":%u,"
                "\"pc\":%u,\"addr\":%u,\"value\":%u}",
                i ? "," : "",
                (unsigned long long)ev[i].seq,
                (unsigned long long)ev[i].cycles,
                (unsigned long long)ev[i].insn,
                ev[i].cpu, ev[i].write, ev[i].width,
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

    if (cmd == "framebuffer") {
        const std::string engine = json_str(line, "engine", "A");
        const int screen = (engine == "B" || engine == "b") ? 1 : 0;
        uint16_t width = 256;
        const bool adaptive = json_bool(line, "adaptive", false);
        const uint32_t* fb = adaptive
            ? nds_gpu2d_adaptive_framebuffer(screen, &width)
            : nds_gpu2d_framebuffer(screen);
        if (!fb) return "{\"error\":\"framebuffer not ready\"}";
        std::string rgb;
        rgb.reserve(size_t{width} * 192u * 6u);
        for (size_t i = 0; i < size_t{width} * 192u; ++i) {
            const uint32_t px = fb[i];
            const uint8_t c[3] = {
                static_cast<uint8_t>(px >> 16),
                static_cast<uint8_t>(px >> 8),
                static_cast<uint8_t>(px),
            };
            append_hex(rgb, c, sizeof(c));
        }
        return "{\"w\":" + std::to_string(width) +
            ",\"h\":192,\"rgb\":\"" + rgb + "\"}";
    }
    if (cmd == "touch") {
        const uint16_t x = static_cast<uint16_t>(json_u64(line, "x", 0));
        const uint16_t y = static_cast<uint16_t>(json_u64(line, "y", 0));
        nds_set_touch(x, y, json_bool(line, "down", true));
        return "{\"ok\":true}";
    }
    if (cmd == "keys") {
        nds_set_key_mask(static_cast<uint32_t>(json_u64(line, "mask", 0x3FFu)));
        return "{\"ok\":true}";
    }

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

void debug_set_reset_fn(std::function<void()> fn) { g_reset_fn = std::move(fn); }

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

    bool fatal_backend_failure = false;
    while (!fatal_backend_failure) {
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
                const bool request_failed =
                    nds_gpu3d_compute_runtime_failed();
                resp.push_back('\n');
                if (!send_all(client, resp.data(), resp.size())) {
                    fatal_backend_failure = request_failed;
                    open = false;
                    break;
                }
                // Preserve the response that observed the terminal failure,
                // then unwind serve mode so forced-compute automation receives
                // a nonzero process status from main.
                if (request_failed) {
                    fatal_backend_failure = true;
                    open = false;
                    break;
                }
            }
        }
        CLOSESOCK(client);
    }
    CLOSESOCK(listener);
#ifdef _WIN32
    WSACleanup();
#endif
}

// ── Play-mode pump (psxrecomp handoff model) ────────────────────────────
// A dedicated I/O thread owns accept/recv/send on the same line-JSON
// protocol; each complete request line is handed to the frontend thread,
// which executes it inside debug_pump() between frames — the emu state is
// only ever touched by its owning thread, so no emulator locking exists.
// The mutex/condvar below protect ONLY the request/response handoff pair.

namespace {

enum class PumpIo { Idle, Req, Resp };
std::thread g_pump_thread;
std::mutex g_pump_mutex;
std::condition_variable g_pump_resp_cv;
PumpIo g_pump_state = PumpIo::Idle;
std::string g_pump_req;
std::string g_pump_resp;
std::atomic<bool> g_pump_shutdown{false};
socket_t g_pump_listener = INVALID_SOCKET;
std::atomic<socket_t> g_pump_client{INVALID_SOCKET};

void pump_io_thread() {
    while (!g_pump_shutdown.load(std::memory_order_relaxed)) {
        socket_t client = accept(g_pump_listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (g_pump_shutdown.load(std::memory_order_relaxed)) break;
            continue;
        }
        g_pump_client.store(client, std::memory_order_relaxed);
        std::string buf;
        char chunk[65536];
        bool open = true;
        while (open && !g_pump_shutdown.load(std::memory_order_relaxed)) {
            int n = recv(client, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            buf.append(chunk, (size_t)n);
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string req = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                std::string resp;
                if (json_str(req, "cmd") == "ping") {
                    // Liveness fast path answered on the I/O thread: ping
                    // works even while the frontend thread is buried in a
                    // slow frame (freeze diagnosis).
                    resp = "{\"pong\":true}";
                } else {
                    std::unique_lock<std::mutex> lock(g_pump_mutex);
                    g_pump_req = req;
                    g_pump_state = PumpIo::Req;
                    if (g_pump_resp_cv.wait_for(
                            lock, std::chrono::seconds(30),
                            [] { return g_pump_state == PumpIo::Resp; })) {
                        resp = g_pump_resp;
                    } else {
                        resp = "{\"error\":\"frontend busy or frozen\"}";
                    }
                    g_pump_state = PumpIo::Idle;
                }
                resp.push_back('\n');
                if (!send_all(client, resp.data(), resp.size())) {
                    open = false;
                    break;
                }
            }
        }
        // Exchange-then-close so shutdown and this thread never both close
        // the same handle.
        const socket_t mine =
            g_pump_client.exchange(INVALID_SOCKET, std::memory_order_relaxed);
        if (mine != INVALID_SOCKET) CLOSESOCK(mine);
    }
}

}  // namespace

bool debug_pump_start(uint16_t port) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "[debug] WSAStartup failed\n");
        return false;
    }
#endif
    g_pump_listener = socket(AF_INET, SOCK_STREAM, 0);
    if (g_pump_listener == INVALID_SOCKET) {
        std::fprintf(stderr, "[debug] socket() failed\n");
        return false;
    }
    int yes = 1;
    setsockopt(g_pump_listener, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_pump_listener, (sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(g_pump_listener, 1) != 0) {
        std::fprintf(stderr,
                     "[debug] play-mode surface unavailable (port %u busy)\n",
                     port);
        CLOSESOCK(g_pump_listener);
        g_pump_listener = INVALID_SOCKET;
        return false;
    }
    g_play_mode = true;
    g_pump_shutdown.store(false, std::memory_order_relaxed);
    g_pump_thread = std::thread(pump_io_thread);
    std::fprintf(stderr, "[debug] play-mode surface on 127.0.0.1:%u\n", port);
    return true;
}

void debug_pump() {
    if (g_pump_listener == INVALID_SOCKET) return;
    std::unique_lock<std::mutex> lock(g_pump_mutex, std::try_to_lock);
    if (!lock.owns_lock() || g_pump_state != PumpIo::Req) return;
    const std::string req = g_pump_req;
    lock.unlock();
    std::string resp = handle(req);   // frontend thread: the safe point
    lock.lock();
    g_pump_resp = std::move(resp);
    g_pump_state = PumpIo::Resp;
    lock.unlock();
    g_pump_resp_cv.notify_one();
}

void debug_pump_stop() {
    if (g_pump_listener == INVALID_SOCKET) return;
    g_pump_shutdown.store(true, std::memory_order_relaxed);
    CLOSESOCK(g_pump_listener);   // unblocks accept()
    g_pump_listener = INVALID_SOCKET;
    const socket_t client =
        g_pump_client.exchange(INVALID_SOCKET, std::memory_order_relaxed);
    if (client != INVALID_SOCKET) CLOSESOCK(client);  // unblocks recv()
    if (g_pump_thread.joinable()) g_pump_thread.join();
    g_play_mode = false;
}
