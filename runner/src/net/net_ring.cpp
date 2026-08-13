// net_ring.cpp — storage + write API + query surface for the always-on
// network event ring. See net_ring.h for the design rationale; this file
// only implements it. No call site exists yet (Wi-Fi device / AP / bridge /
// backend are later Wiimmfi-plan phases), so every push function here is
// currently unreachable from any production code path — the ring is empty
// by construction until those land, and this file's job is to make sure
// storage, eviction, and the query surface are correct and inert in the
// meantime.

#include "net_ring.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

#include "io.h"          // g_insn_count[2]
#include "scheduler.h"   // scheduler_system_timestamp/cpu_cycles/cpu_state
#include "state.h"       // full ArmCpuState definition (R[15]) — scheduler.h
                         // only forward-declares it, same as debug_server.cpp

namespace {

// Same three-counter shape as the gamecard trace (io.cpp:534-536): `w` is
// the next slot to write, `count` is the number of live (unevicted) entries
// clamped to capacity, `seq` is the unbounded absolute ordinal assigned to
// the most recent push (== the `count` field stored in that entry).
NdsNetTraceEntry g_net_trace[kNetTraceSize] = {};
struct NetHostnameSlot {
    uint64_t owner_count;
    char hostname[kNetHostnameMaxLen];
};
static_assert(kNetHostnameSlots <= 65536u,
              "NdsNetTraceEntry::hostname_ref is uint16_t");
NetHostnameSlot g_net_hostname_pool[kNetHostnameSlots] = {};
uint32_t g_net_trace_w = 0;
uint32_t g_net_trace_retained = 0;
uint64_t g_net_trace_seq = 0;
uint16_t g_net_hostname_w = 0;

const char* const kNetEventKindNames[NDS_NET_EVENT_KIND_COUNT] = {
    "wifi_reg_read",
    "wifi_reg_write",
    "wifi_irq",
    "wifi_tx_begin",
    "wifi_tx_frame",
    "wifi_rx_frame",
    "wifi_association",
    "wifi_state_change",
    "ethernet_tx",
    "ethernet_rx",
    "arp",
    "dhcp",
    "dns_query",
    "dns_response",
    "tcp_open",
    "tcp_close",
    "tcp_reset",
    "tcp_packet",
    "udp_packet",
    "backend_drop",
    "backend_error",
    "tls_record",
};

// Case-insensitive ASCII compare; the CLI/JSON inputs this parses are never
// intended to be locale-sensitive.
bool iequals(const char* a, const char* b) {
    while (*a && *b) {
        const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(*a)));
        const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(*b)));
        if (ca != cb) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

}  // namespace

const char* nds_net_event_kind_name(uint8_t kind) {
    if (kind < NDS_NET_EVENT_KIND_COUNT) return kNetEventKindNames[kind];
    return "unknown";
}

bool nds_net_event_kind_parse(const char* name, uint8_t* out) {
    if (!name || !out) return false;
    if (iequals(name, "all")) {
        *out = static_cast<uint8_t>(NDS_NET_EVENT_KIND_COUNT);
        return true;
    }
    for (uint8_t i = 0; i < NDS_NET_EVENT_KIND_COUNT; ++i) {
        if (iequals(name, kNetEventKindNames[i])) {
            *out = i;
            return true;
        }
    }
    return false;
}

uint64_t net_ring_push(NdsNetEventKind kind, uint8_t direction,
                       uint16_t wifi_reg, uint32_t wifi_value,
                       const uint8_t* src_mac, const uint8_t* dst_mac,
                       uint32_t src_ipv4, uint32_t dst_ipv4,
                       uint16_t src_port, uint16_t dst_port,
                       uint16_t payload_len, uint32_t aux) {
    const uint32_t slot = g_net_trace_w;
    NdsNetTraceEntry& e = g_net_trace[slot];
    e = {};
    e.count = ++g_net_trace_seq;
    e.sys = scheduler_system_timestamp();
    e.cyc9 = scheduler_cpu_cycles(0);
    e.cyc7 = scheduler_cpu_cycles(1);
    e.insn9 = g_insn_count[0];
    e.insn7 = g_insn_count[1];
    e.arm9_pc = scheduler_cpu_state(0).R[15];
    e.arm7_pc = scheduler_cpu_state(1).R[15];
    e.kind = static_cast<uint8_t>(kind);
    e.direction = direction;
    e.wifi_reg = wifi_reg;
    e.wifi_value = wifi_value;
    if (src_mac) std::memcpy(e.src_mac, src_mac, sizeof(e.src_mac));
    if (dst_mac) std::memcpy(e.dst_mac, dst_mac, sizeof(e.dst_mac));
    e.src_ipv4 = src_ipv4;
    e.dst_ipv4 = dst_ipv4;
    e.src_port = src_port;
    e.dst_port = dst_port;
    e.payload_len = payload_len;
    e.has_hostname = 0;
    e.hostname_ref = 0;
    e.aux = aux;
    g_net_trace_w = (slot + 1u) % kNetTraceSize;
    if (g_net_trace_retained < kNetTraceSize) ++g_net_trace_retained;
    return e.count;
}

void net_ring_set_hostname(uint64_t count, const char* hostname) {
    if (!hostname || count == 0) return;
    const uint32_t idx = static_cast<uint32_t>((count - 1) % kNetTraceSize);
    NdsNetTraceEntry& e = g_net_trace[idx];
    if (e.count != count) return;  // already evicted; drop silently
    const uint16_t hostname_ref = g_net_hostname_w;
    NetHostnameSlot& slot = g_net_hostname_pool[hostname_ref];
    slot.owner_count = count;
    std::snprintf(slot.hostname, kNetHostnameMaxLen, "%s", hostname);
    g_net_hostname_w =
        static_cast<uint16_t>((g_net_hostname_w + 1u) % kNetHostnameSlots);
    e.has_hostname = 1;
    e.hostname_ref = hostname_ref;
}

uint64_t net_ring_latest() { return g_net_trace_seq; }

bool net_ring_get(uint64_t count, NdsNetTraceEntry* out) {
    if (!out || count == 0 || count > g_net_trace_seq) return false;
    const uint32_t idx = static_cast<uint32_t>((count - 1) % kNetTraceSize);
    const NdsNetTraceEntry& e = g_net_trace[idx];
    if (e.count != count) return false;  // evicted
    *out = e;
    return true;
}

bool net_ring_get_hostname(uint64_t count, char* buf, size_t buf_size) {
    if (!buf || buf_size == 0 || count == 0 || count > g_net_trace_seq)
        return false;
    const uint32_t idx = static_cast<uint32_t>((count - 1) % kNetTraceSize);
    const NdsNetTraceEntry& e = g_net_trace[idx];
    if (e.count != count || !e.has_hostname) return false;
    if (e.hostname_ref >= kNetHostnameSlots) return false;
    const NetHostnameSlot& slot = g_net_hostname_pool[e.hostname_ref];
    if (slot.owner_count != count) return false;
    std::snprintf(buf, buf_size, "%s", slot.hostname);
    return true;
}

uint32_t net_ring_copy_recent(NdsNetTraceEntry* out, uint32_t max_entries,
                              uint8_t filter_kind) {
    if (!out || max_entries == 0) return 0;
    const uint32_t retained = g_net_trace_retained;
    uint32_t found = 0;
    // Walk backward from the most recently written slot across the whole
    // retained window, collecting up to max_entries matches (newest-first),
    // then reverse in place to the ring's usual oldest-first convention.
    // Scanning the full retained window (not just the last max_entries raw
    // slots) means a filtered query isn't starved by unrelated recent
    // traffic — e.g. "give me the last 32 TCP events" should look past a
    // burst of unrelated Wi-Fi register accesses to find them.
    for (uint32_t i = 0; i < retained && found < max_entries; ++i) {
        const uint32_t idx =
            (g_net_trace_w + kNetTraceSize - 1u - i) % kNetTraceSize;
        const NdsNetTraceEntry& e = g_net_trace[idx];
        if (filter_kind == NDS_NET_EVENT_KIND_COUNT || e.kind == filter_kind)
            out[found++] = e;
    }
    for (uint32_t a = 0, b = found; a < b / 2; ++a)
        std::swap(out[a], out[b - 1u - a]);
    return found;
}

void net_ring_debug_state(NdsNetRingState* out) {
    if (!out) return;
    out->produced = g_net_trace_seq;
    out->capacity = kNetTraceSize;
    if (g_net_trace_retained) {
        const uint32_t first = (g_net_trace_w + kNetTraceSize -
                                g_net_trace_retained) % kNetTraceSize;
        out->oldest = g_net_trace[first].count;
    } else {
        out->oldest = g_net_trace_seq + 1u;
    }
}

void net_ring_reset() {
    g_net_trace_w = 0;
    g_net_trace_retained = 0;
    g_net_trace_seq = 0;
    g_net_hostname_w = 0;
    // Entries/hostnames are left as-is (matching every other ring's reset —
    // e.g. card_trace at io.cpp:1905 only rewinds the cursors); a stale slot
    // is unreachable because net_ring_get's stored.count == count check will
    // reject it the moment `seq` restarts from 0, and net_ring_push clears
    // the hostname slot it is about to reuse before the first real write.
}

void net_ring_dump_recent(uint32_t max_entries, uint8_t filter_kind) {
    if (max_entries > kNetTraceSize) max_entries = kNetTraceSize;
    std::vector<NdsNetTraceEntry> ev(max_entries ? max_entries : 1);
    const uint32_t count =
        max_entries ? net_ring_copy_recent(ev.data(), max_entries, filter_kind)
                    : 0;
    if (filter_kind == NDS_NET_EVENT_KIND_COUNT) {
        std::fprintf(stderr, "[net_ring] last %u event(s):\n", count);
    } else {
        std::fprintf(stderr, "[net_ring] last %u event(s) (kind=%s):\n", count,
                    nds_net_event_kind_name(filter_kind));
    }
    for (uint32_t i = 0; i < count; ++i) {
        const NdsNetTraceEntry& e = ev[i];
        char hostname[kNetHostnameMaxLen] = {};
        const bool have_host =
            e.has_hostname && net_ring_get_hostname(e.count, hostname, sizeof(hostname));
        std::fprintf(stderr,
            "  #%llu %-18s dir=%u sys=%llu arm9_pc=0x%08X arm7_pc=0x%08X "
            "wifi_reg=0x%04X wifi_value=0x%08X "
            "src_ip=0x%08X:%u dst_ip=0x%08X:%u len=%u aux=0x%08X%s%s\n",
            (unsigned long long)e.count, nds_net_event_kind_name(e.kind),
            e.direction, (unsigned long long)e.sys, e.arm9_pc, e.arm7_pc,
            e.wifi_reg, e.wifi_value, e.src_ipv4, e.src_port, e.dst_ipv4,
            e.dst_port, e.payload_len, e.aux,
            have_host ? " host=" : "", have_host ? hostname : "");
    }
}
