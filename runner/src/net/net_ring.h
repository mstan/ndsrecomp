// net_ring.h — always-on Wi-Fi/network event ring (M0 observability floor).
//
// Modeled directly on the gamecard trace (NdsCardTraceEntry/card_trace_push,
// io.h:147-169, io.cpp:530-579): one ring, several event *kinds*, a small
// fixed-size header payload inline, no unbounded data in the entry itself.
// Same idioms as every other always-on ring in this runner (DEBUG.md):
// unconditional writes from the producing call site (no arm/disarm step),
// absolute-ordinal addressing via a self-describing `count` field (a reader
// that queried by ordinal K detects eviction by checking stored.count == K,
// exactly like nds_gpu3d_write_trace_get / nds_gpu3d_run_trace_get), plus a
// "most recent N" copy-out for a client that hasn't learned `latest` yet.
//
// This header/impl pair is storage + write API + query surface ONLY. No
// call site exists yet — the Wi-Fi device (wifi.cpp), a future virtual AP
// (wifi_ap.cpp), an 802.11<->Ethernet bridge (eth_bridge.cpp), and a host
// network backend (backend_slirp.cpp) are later phases of the Wiimmfi plan
// (docs/networking-observability-plan.md, section (d)). Until they land,
// the ring is empty by construction: that is the expected, correct state,
// not a bug. Nothing here perturbs execution — every function below either
// self-timestamps a push (same as card_trace_push) or performs a read-only
// copy-out; none of it can be reached from a code path that doesn't already
// call it explicitly.
//
// Privacy (plan section 28): this struct carries only headers, addresses,
// lengths, and register values — never payload bytes. There is no payload
// pool and no capture-arming command in this milestone, so "metadata only"
// is not a toggle a caller could get wrong; it is the only thing this ring
// is capable of recording. A future full-payload capture path (plan (g))
// must add its own explicit opt-in gate alongside whatever storage it adds
// — do not grow payload bytes into this struct to skip that step.

#pragma once

#include <cstddef>
#include <cstdint>

// One enumerator per network-observable event class, 1:1 with the call
// sites named in the observability plan section (d). Unscoped + explicit
// uint8_t underlying type, matching NdsCardTraceKind (io.h:138-143): stored
// straight into NdsNetTraceEntry::kind with no cast, and the debug-server
// JSON builder can switch on it directly.
enum NdsNetEventKind : uint8_t {
    NDS_NET_EVENT_WIFI_REG_READ = 0,
    NDS_NET_EVENT_WIFI_REG_WRITE = 1,
    NDS_NET_EVENT_WIFI_IRQ = 2,
    NDS_NET_EVENT_WIFI_TX_BEGIN = 3,
    NDS_NET_EVENT_WIFI_TX_FRAME = 4,
    NDS_NET_EVENT_WIFI_RX_FRAME = 5,
    NDS_NET_EVENT_WIFI_ASSOCIATION = 6,
    NDS_NET_EVENT_WIFI_STATE_CHANGE = 7,
    NDS_NET_EVENT_ETHERNET_TX = 8,
    NDS_NET_EVENT_ETHERNET_RX = 9,
    NDS_NET_EVENT_ARP = 10,
    NDS_NET_EVENT_DHCP = 11,
    NDS_NET_EVENT_DNS_QUERY = 12,
    NDS_NET_EVENT_DNS_RESPONSE = 13,
    NDS_NET_EVENT_TCP_OPEN = 14,
    NDS_NET_EVENT_TCP_CLOSE = 15,
    NDS_NET_EVENT_TCP_RESET = 16,
    NDS_NET_EVENT_TCP_PACKET = 17,
    NDS_NET_EVENT_UDP_PACKET = 18,
    NDS_NET_EVENT_BACKEND_DROP = 19,
    NDS_NET_EVENT_BACKEND_ERROR = 20,
    // Wiimmfi M5: passive TLS record classification (labels only -- see
    // net_classify.cpp's classify_tls_record). Pushed from inside
    // classify_tcp() when a TCP segment's payload begins with a
    // syntactically plausible TLS/SSLv3 record header (content type byte in
    // {20,21,22,23}, version major byte == 3). This NEVER terminates or
    // alters the handshake -- it only reads header bytes already present in
    // the frame buffer that classify_tcp already holds, exactly the same
    // "peek at bytes already on the wire" idiom classify_dns uses to read a
    // DNS question name. `aux` packs (content_type << 16) |
    // (handshake_type << 8) | version_minor; handshake_type is 0xFF when
    // content_type != 22 (handshake) or the segment is a mid-record
    // continuation whose first byte isn't a handshake message header (this
    // classifier does not reassemble the TCP stream, so it can only label
    // what a single segment's leading bytes show -- best-effort, same
    // caveat classify_dns already documents for DNS-over-TCP). payload_len
    // is the TLS record's OWN declared length from the record header (not
    // the TCP segment length), clamped the same way classify_dns clamps a
    // DNS response's declared vs. available length.
    NDS_NET_EVENT_TLS_RECORD = 21,
    // Not a real event; sentinel meaning "no filter" for the copy/dump/query
    // surface below (net_ring_copy_recent, net_ring_dump_recent, the
    // debug-server `net_ring_dump` command's optional `filter`).
    NDS_NET_EVENT_KIND_COUNT = 22,
};

// TLS/SSLv3 content type byte values, exposed here so a reader of `aux` on a
// NDS_NET_EVENT_TLS_RECORD entry doesn't have to hardcode RFC 6101/8446
// magic numbers. Matches the wire values exactly (these are on-the-wire
// bytes, not a project-invented enum).
enum NdsTlsContentType : uint8_t {
    NDS_TLS_CONTENT_CHANGE_CIPHER_SPEC = 20,
    NDS_TLS_CONTENT_ALERT = 21,
    NDS_TLS_CONTENT_HANDSHAKE = 22,
    NDS_TLS_CONTENT_APPLICATION_DATA = 23,
};

// TLS handshake message type byte (only meaningful when content_type ==
// NDS_TLS_CONTENT_HANDSHAKE and the segment starts exactly at a handshake
// message boundary). 0xFF is this project's own "unknown/not observable
// from this segment" sentinel, not a wire value.
enum NdsTlsHandshakeType : uint8_t {
    NDS_TLS_HANDSHAKE_CLIENT_HELLO = 1,
    NDS_TLS_HANDSHAKE_SERVER_HELLO = 2,
    NDS_TLS_HANDSHAKE_CERTIFICATE = 11,
    NDS_TLS_HANDSHAKE_SERVER_HELLO_DONE = 14,
    NDS_TLS_HANDSHAKE_CLIENT_KEY_EXCHANGE = 16,
    NDS_TLS_HANDSHAKE_FINISHED = 20,
    NDS_TLS_HANDSHAKE_UNKNOWN = 0xFF,
};

// Name used by the CLI --net-ring-filter flag, the debug-server
// `net_ring_dump` command's `filter` argument, and JSON serialization of
// the `kind` field. Returns "unknown" for an out-of-range value rather than
// guessing — same defensive default as card_event_name/card_phase_name in
// debug_server.cpp.
const char* nds_net_event_kind_name(uint8_t kind);

// Case-insensitive parse of a kind name, or "all" for
// NDS_NET_EVENT_KIND_COUNT (the "no filter" sentinel). Returns false and
// leaves *out untouched for an unrecognized name — callers (main.cpp CLI
// validation, debug_server.cpp) must reject rather than silently fall back.
bool nds_net_event_kind_parse(const char* name, uint8_t* out);

// Absolute-ordinal-addressed entry. `count` is self-describing exactly like
// NdsCardTraceEntry::seq / every other ring's ordinal field — a reader that
// queried by ordinal K compares stored.count == K to detect eviction rather
// than trusting the modulo-indexed slot blindly.
//
// sys/cyc9/cyc7/insn9/insn7 mirror the timestamp block every existing
// cross-subsystem ring carries (NdsCardTraceEntry, NdsFifoTraceEntry,
// io.h:147-169 and io.h:127-135) so a probe can anchor a network event
// against the IPC ring, the IRQ ring, or a VBlank count without a second
// round trip. arm9_pc/arm7_pc are split rather than a single generic PC
// pair (PRINCIPLES.md's dual-CPU rule): Wi-Fi is ARM7-owned hardware, but
// association/DHCP/DNS state is often driven by ARM9-side library code
// relayed over IPC, so both are useful and neither should be inferred from
// "whichever CPU is active" the way BusEvent::cpu is. Both are populated
// unconditionally by net_ring_push (self-read from scheduler_cpu_state, the
// same accessor the `regs` debug-server command uses) — a caller never
// passes a PC in.
struct NdsNetTraceEntry {
    uint64_t count;
    uint64_t sys;
    uint64_t cyc9;
    uint64_t cyc7;
    uint64_t insn9;
    uint64_t insn7;
    uint32_t arm9_pc;       // always recorded (see comment above)
    uint32_t arm7_pc;       // always recorded; ARM7 owns the Wi-Fi device
    uint8_t  kind;          // NdsNetEventKind
    uint8_t  direction;     // 0 = guest->host (TX/egress), 1 = host->guest (RX/ingress)
    uint16_t wifi_reg;      // MMIO offset for WifiReg{Read,Write}; 0 otherwise
    uint32_t wifi_value;    // register value, or protocol-specific aux
    uint8_t  src_mac[6];
    uint8_t  dst_mac[6];
    uint32_t src_ipv4;
    uint32_t dst_ipv4;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t payload_len;   // on-wire length; NEVER the captured length —
                             // this ring never stores payload bytes (see
                             // the privacy note at the top of this file)
    uint8_t  has_hostname;  // 1 if this entry owns a hostname side-table ref
    uint16_t hostname_ref;  // compact index into the hostname side table
    uint32_t aux;           // event-kind-specific (TCP flags, DHCP opcode,
                             // association result code, ...)
};

// One step above the gamecard/DMA/GX-write rings (8192, chosen there to
// "span two maximum-size blocks" / "a full frame of them", io.cpp:524-532):
// a Wi-Fi association + DHCP + DNS + a burst of gameplay UDP packets in one
// frame window can plausibly exceed a gamecard transfer's event count, and
// a network regression probe (Wiimmfi plan phase 21) wants to retain a
// whole multi-second connection sequence (scan -> associate -> DHCP -> DNS
// -> auth) without evicting the start before the end is even reached.
// 16384 entries x sizeof(NdsNetTraceEntry) is on the order of 1 MiB — small
// next to the existing budget (g_insn_trace[2] alone is already tens of MB).
//
// Wiimmfi M5 (beads-yjp.1.6): measured DIRECTLY, not guessed -- a single
// login-attempt scenario run that includes one AP-association retry window
// (guest driver busy-polling Wi-Fi status registers while waiting on a
// stalled/erroring network step) pushed 85,097 total events by the time the
// WFC connection test alone settled, of which >99% were WIFI_REG_READ/WRITE
// (the guest's own driver polling cadence, not backend traffic). At the
// original 16384-entry capacity, that register-poll volume evicted the
// DHCP/DNS/TCP events from EARLIER in the exact same connection sequence
// before a query even ran -- i.e. the always-on ring silently failed to
// retain "a whole multi-second connection sequence" (this constant's own
// design goal, stated above) once a real network stall triggered guest-side
// retry polling. Bumped 16x (to 262144) so a full scenario run -- AP scan
// through the actual NAS login attempt, including at least one such
// register-poll retry window -- fits inside the retained window without
// evicting the DNS/TCP/TLS evidence a query needs. Still small in absolute
// terms: ~25-30 MiB at this entry size, well under the existing g_insn_trace
// budget referenced above. This is a capacity extension of the existing
// always-on ring (DEBUG.md's "extend the ring buffer" remedy for
// insufficient retention), not a new arm/disarm capture mechanism.
constexpr uint32_t kNetTraceSize = 262144;

// DNS hostnames are variable-length (up to 253 bytes) and don't belong
// inline in a fixed-size entry. Only DNS_QUERY/DNS_RESPONSE events attach
// names, so hostname storage is a smaller side ring keyed by
// NdsNetTraceEntry::hostname_ref instead of a kNetTraceSize-sized pool.
// A 4096-name retained window is much larger than observed login/match-menu
// DNS volume while removing the old 66 MiB BSS reservation.
constexpr uint32_t kNetHostnameSlots = 4096;
constexpr uint32_t kNetHostnameMaxLen = 254;  // 253 + NUL

// ── Write API ────────────────────────────────────────────────────────────
// Call sites (none exist yet; this is the API they will call):
//   - wifi.cpp (device register model, once it grows association state):
//     WIFI_REG_READ/WRITE, WIFI_IRQ, WIFI_STATE_CHANGE on the existing
//     nds_wifi_read/write entry points.
//   - a future wifi_ap.cpp (virtual AP): WIFI_ASSOCIATION,
//     WIFI_TX_BEGIN/WIFI_TX_FRAME/WIFI_RX_FRAME.
//   - a future eth_bridge.cpp (802.11<->Ethernet bridge): ETHERNET_TX/RX,
//     ARP.
//   - a future backend_slirp.cpp: DHCP, DNS_QUERY/DNS_RESPONSE,
//     TCP_OPEN/CLOSE/RESET/PACKET, UDP_PACKET, BACKEND_DROP/ERROR.
//
// One function, every field a parameter; self-timestamps internally
// (scheduler_system_timestamp/scheduler_cpu_cycles/g_insn_count/both CPUs'
// R[15] via scheduler_cpu_state) exactly as card_trace_push does — a caller
// never passes cycle counts or PCs. Pass nullptr for src_mac/dst_mac when
// the event has no MAC (e.g. a pure register access); they are zero-filled.
// Returns the assigned absolute ordinal (`count`) so a caller that also has
// a hostname (DNS events) can attach it via net_ring_set_hostname without a
// second timestamp round trip.
uint64_t net_ring_push(NdsNetEventKind kind, uint8_t direction,
                       uint16_t wifi_reg, uint32_t wifi_value,
                       const uint8_t* src_mac, const uint8_t* dst_mac,
                       uint32_t src_ipv4, uint32_t dst_ipv4,
                       uint16_t src_port, uint16_t dst_port,
                       uint16_t payload_len, uint32_t aux);

// Attaches a hostname to the entry most recently returned by net_ring_push.
// `count` must be exactly the ordinal that call returned. Silently a no-op
// if that entry has already been evicted by the time this is called (same
// "evicted queries/writes are not errors" convention as every other ring).
void net_ring_set_hostname(uint64_t count, const char* hostname);

// ── Query surface (debug-server + CLI; read-only, never advances execution) ─

// The newest produced ordinal (0 = nothing pushed yet). Same "query latest
// first" idiom as nds_gpu3d_write_trace_count/nds_gpu3d_run_trace_count.
uint64_t net_ring_latest();

// stored.count == count check, same idiom as nds_gpu3d_write_trace_get /
// nds_gpu3d_run_trace_get (gpu3d.cpp:525-542): false if `count` was never
// produced or has since been evicted.
bool net_ring_get(uint64_t count, NdsNetTraceEntry* out);

// Copies the hostname associated with an entry obtained via net_ring_get /
// net_ring_copy_recent, if any. False (buffer left untouched) if the entry
// never carried a hostname or the smaller hostname side ring has evicted it.
bool net_ring_get_hostname(uint64_t count, char* buf, size_t buf_size);

// "Most recent N" copy-out, oldest-first — same result shape as
// bus_debug_watch_copy / tier3_debug_trace_copy. `filter_kind` of
// NDS_NET_EVENT_KIND_COUNT means "no filter" (copy every kind). When a
// filter is given, the scan walks back through the whole retained window
// looking for up to `max_entries` matches (not just the last `max_entries`
// raw entries), so a filtered query is not starved by unrelated event
// traffic that happens to be more recent.
uint32_t net_ring_copy_recent(NdsNetTraceEntry* out, uint32_t max_entries,
                              uint8_t filter_kind);

// Live (non-historized) snapshot: ring produced/oldest ordinals, mirroring
// NdsCardDebugState::produced/oldest/capacity (io.h:185-187) — the thing a
// client calls once before deciding what window of net_ring_get /
// net_ring_copy_recent to pull.
struct NdsNetRingState {
    uint64_t produced;   // == net_ring_latest(); 0 if nothing pushed yet
    uint64_t oldest;      // oldest ordinal still retained; produced+1 if empty
    uint32_t capacity;    // == kNetTraceSize
};
void net_ring_debug_state(NdsNetRingState* out);

// Reset all ring/pool state to empty (called from the same full power-on
// re-init path as every other ring — see the `reset` debug-server command
// and card_trace's reset block at io.cpp:1905).
void net_ring_reset();

// ── Stderr dump (CLI --net-ring-dump / --net-ring-last / --net-ring-filter)─
// Batch-mode, zero-server-required text dump — same destination and idiom
// as runtime_trace_dump_recent (runtime_arm.cpp:728-740). `filter_kind` of
// NDS_NET_EVENT_KIND_COUNT means "no filter".
void net_ring_dump_recent(uint32_t max_entries, uint8_t filter_kind);
