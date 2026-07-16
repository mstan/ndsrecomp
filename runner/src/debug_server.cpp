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
#include "gpu2d.h"
#include "gpu3d.h"
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
    uint64_t v = std::strtoull(s.c_str() + p, &end, 0);
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
    if (cmd == "io_state") return io_state_json();
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
        const uint32_t* fb = nds_gpu2d_framebuffer(screen);
        if (!fb) return "{\"error\":\"framebuffer not ready\"}";
        std::string rgb;
        rgb.reserve(256u * 192u * 6u);
        for (size_t i = 0; i < 256u * 192u; ++i) {
            const uint32_t px = fb[i];
            const uint8_t c[3] = {
                static_cast<uint8_t>(px >> 16),
                static_cast<uint8_t>(px >> 8),
                static_cast<uint8_t>(px),
            };
            append_hex(rgb, c, sizeof(c));
        }
        return "{\"w\":256,\"h\":192,\"rgb\":\"" + rgb + "\"}";
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
