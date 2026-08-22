// wifi_net.cpp -- see wifi_net.h and wifi.h for the bridge's status/scope.
//
// This file is now the SOLE Wi-Fi device-model implementation on the live
// bus: it implements every declaration in wifi.h (previously implemented
// by the retired runner/src/wifi.cpp) against the vendored melonDS Wifi
// class, plus the melonDS::NDS single-slot scheduler shim (declared in
// runner/vendor/melonds/NDS.h) and the melonDS::Platform::{Mutex_*,MP_*,
// Net_SendPacket,Net_RecvPacket[,DynamicLibrary_*]} shim functions
// (declared in runner/vendor/melonds/Platform.h) that the vendored
// Wifi.cpp/WifiAP.cpp/net/*.cpp translation units call into.
//
// melonDS::NDS::SetIRQ/ClearIRQ/CheckDMAs/GXFIFOStall/GXFIFOUnstall and
// melonDS::Platform::Log/Thread_*/Semaphore_* are already implemented in
// gpu3d.cpp for the (same, shared) NDS.h/Platform.h -- this file only adds
// the members those did not already cover. gpu3d.cpp's NDS::SetIRQ also
// pushes the Wi-Fi IRQ-assertion net-ring event when irq == IRQ_Wifi (see
// that file); nothing here duplicates it.

#include "wifi_net.h"
#include "wifi.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <array>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <mswsock.h>
#include <ws2tcpip.h>
#endif

#if defined(NDS_ENABLE_PCAP_BACKEND) && defined(_WIN32)
#include <windows.h>
#endif

#include "io.h"
#include "state.h"        // g_nds_active, NDS_ARM9/NDS_ARM7, nds_reschedule_slice
#include "scheduler.h"    // scheduler_system_timestamp, scheduler_cpu_state (Wiimmfi M8)
#include "net/net_ring.h"
#include "net/net_classify.h"
#include "net/net_capture.h"
#include "net/net_replay.h"

// Explicit relative paths, not bare quote-includes: runner/src/wifi.h
// (the existing hand-written device model's header, kept as a thin bridge
// header -- see its own comment) collides case-insensitively with
// runner/vendor/melonds/Wifi.h on Windows, and quote-include search checks
// the including file's own directory (runner/src/) before any -I path, so
// a bare #include "Wifi.h" from a file in runner/src/ silently resolves to
// the WRONG header (confirmed by hitting exactly this: a "class
// melonDS::Wifi" forward-declared-but-never-completed error whose note
// pointed at wifi_net.h's forward decl, because "Wifi.h" had resolved to
// runner/src/wifi.h, which declares no such class at all).
#include "../vendor/melonds/NDS.h"
#include "../vendor/melonds/Wifi.h"
#include "../vendor/melonds/WifiAP.h"
#include "../vendor/melonds/net/Net.h"
#include "../vendor/melonds/net/NetDriver.h"
#include "../vendor/melonds/net/Net_Slirp.h"
#if defined(NDS_ENABLE_PCAP_BACKEND)
#include "../vendor/melonds/net/Net_PCap.h"
#endif

// ── melonDS::NDS single-slot scheduler shim ─────────────────────────────
// Wifi::Wifi()/~Wifi() (Wifi.cpp:92-104) register/unregister exactly one
// event func (USTimer) under exactly one id (Event_Wifi); ScheduleTimer/
// UpdatePowerOn only ever schedule/cancel that same id with FuncID 0. See
// NDS.h's declaration comment for why a single slot suffices and what is
// intentionally simplified (no CurCPU/ARM7Timestamp split -- Wifi is
// ARM7-only hardware, so real melonDS's ARM9 branch of ScheduleEvent's
// non-periodic case is unreachable for Event_Wifi; CurrentSystemTimestamp
// always plays the role of real melonDS's ARM7Timestamp here).

namespace melonDS {

void NDS::RegisterEventFuncs(u32 id, void* that,
                              const std::initializer_list<EventFunc>& funcs) {
    EventID = id;
    EventThat = that;
    EventCallback = funcs.size() ? *funcs.begin() : nullptr;
    EventParam = 0;
    EventScheduled = false;
}

void NDS::UnregisterEventFuncs(u32 id) {
    if (id != EventID) return;
    EventThat = nullptr;
    EventCallback = nullptr;
    EventScheduled = false;
    EventID = kNoEvent;
}

void NDS::ScheduleEvent(u32 id, bool periodic, s32 delay, u32 funcid,
                         u32 param) {
    (void)funcid;  // Wifi always passes 0, its only registered func.
    if (id != EventID) return;
    if (EventScheduled) {
        // Real melonDS logs "EVENT ALREADY SCHEDULED" and bails rather than
        // clobbering a pending deadline; preserve that invariant.
        Platform::Log(Platform::Debug,
                       "[wifi_net] event %u already scheduled\n", id);
        return;
    }
    if (periodic) {
        EventTimestamp += static_cast<u64>(delay);
    } else {
        // Matches real melonDS::NDS::ScheduleEvent's non-periodic branch
        // (NDS.cpp:1159-1177) with CurCPU fixed to the ARM7 case (see the
        // NDS.h comment on CurrentSystemTimestamp for why that's always
        // correct for Event_Wifi): the new deadline is "now" (in system
        // cycles) plus delay (also system cycles, per
        // Wifi::ScheduleTimer's 33513982 Hz-based formula).
        EventTimestamp = CurrentSystemTimestamp + static_cast<u64>(delay);
        // A device write may schedule an earlier system event than the
        // live dispatch slice's cap already accounts for -- shorten it,
        // exactly like every other device deadline (io.cpp's
        // nds_reschedule_slice call sites for DIV/SQRT/card/SPI). Only the
        // non-periodic ("first") path needs this: a periodic reschedule
        // happens from inside nds_wifi_run_events, between CPU dispatch
        // slices, never inside one.
        nds_reschedule_slice(EventTimestamp);
    }
    EventParam = param;
    EventScheduled = true;
}

void NDS::CancelEvent(u32 id) {
    if (id == EventID) EventScheduled = false;
}

void NDS::RunPendingEvent() {
    if (!EventScheduled || !EventCallback) return;
    if (CurrentSystemTimestamp < EventTimestamp) return;
    EventScheduled = false;
    EventCallback(EventThat, EventParam);
}

}  // namespace melonDS

// ── melonDS::Platform::Mutex_* ──────────────────────────────────────────
// Backs PacketDispatcher's queue lock (PacketDispatcher.h:45). Real
// primitive (std::mutex), matching the existing Semaphore_*/Thread_* shim
// convention in gpu3d.cpp -- correct under real contention even though
// nothing drives concurrent access from this vendoring pass.

namespace melonDS::Platform {

struct Mutex { std::mutex m; };

Mutex* Mutex_Create() { return new Mutex(); }
void Mutex_Free(Mutex* mutex) { delete mutex; }
void Mutex_Lock(Mutex* mutex) { mutex->m.lock(); }
void Mutex_Unlock(Mutex* mutex) { mutex->m.unlock(); }

// melonDS::Platform::MP_* (local wireless).
// Defined below, after WifiBridgeState. With local wireless disabled these
// hooks report no local peer, so infrastructure-mode TX/RX still falls through
// to WifiAP exactly as it did when the hooks were unconditional no-ops.

#if defined(NDS_ENABLE_PCAP_BACKEND)
// Backs Net_PCap's runtime (not link-time) load of wpcap.dll/Packet.dll.
// Only compiled when the off-by-default NDS_ENABLE_PCAP_BACKEND CMake
// option is set. LibPCap::New() (Net_PCap.cpp) probes several library
// names and tolerates load failure (Npcap not installed) by returning
// std::optional{}, so a null return here is an expected, handled case,
// not an error path we need to report.
struct DynamicLibrary { HMODULE handle; };

DynamicLibrary* DynamicLibrary_Load(const char* lib) {
    HMODULE handle = LoadLibraryA(lib);
    if (!handle) return nullptr;
    return new DynamicLibrary{handle};
}

void DynamicLibrary_Unload(DynamicLibrary* lib) {
    if (!lib) return;
    FreeLibrary(lib->handle);
    delete lib;
}

void* DynamicLibrary_LoadFunction(DynamicLibrary* lib, const char* name) {
    if (!lib) return nullptr;
    return reinterpret_cast<void*>(GetProcAddress(lib->handle, name));
}
#endif  // NDS_ENABLE_PCAP_BACKEND

}  // namespace melonDS::Platform

// ── host networking worker thread + bounded cross-thread queues ────────
//
// Design (see the task's requirement -- host packet reception must never
// run on the emulation thread, and only the emulation thread may touch
// guest-visible state):
//
//   * ONE worker thread (melonDS::Platform::Thread_Create, the same real
//     std::thread-backed shim already used for GPU3D's optional threaded
//     softrenderer -- see gpu3d.cpp) owns EVERY interaction with the
//     vendored Net_Slirp driver -- both SendPacket() (guest->host) and
//     PollHostSockets() (host->guest polling, ex-RecvCheck() body, see
//     Net_Slirp.cpp/.h) -- from the moment it starts until it is joined
//     at shutdown. Because it is the driver's ONLY caller, on a single
//     thread, in a single unsynchronized loop, Ctx/PollList are never
//     touched from two threads at once and need no mutex of their own.
//     This also fixes a real latent bug found while wiring this up: the
//     libslirp send_packet callback used to be a no-op lambda here, so
//     every host->guest packet libslirp ever produced (DHCP/ARP/TCP/UDP
//     replies -- DNS synthesis included, since HandleDNSFrame funnels
//     through the same Callback) was silently discarded before reaching
//     the guest at all. Real melonDS's own frontend
//     (src/frontend/qt_sdl/main.cpp) wires this callback straight to
//     `net.RXEnqueue(data, len)`; this bridge now does the equivalent,
//     via the RX queue below, quantized to the guest's own RX tick.
//   * TWO bounded, mutex-guarded single-producer/single-consumer queues
//     of *complete, independently-owned* packet buffers (std::vector,
//     moved -- never a shared/aliased pointer) are the only channel
//     between the worker thread and the emulation thread:
//       - tx_queue: producer = emulation thread (Net_SendPacket, guest
//         TX), consumer = worker thread (drains, then calls
//         Driver->SendPacket() -- this is also where a blocking
//         getaddrinfo() inside HandleDNSFrame's DNS synthesis now runs,
//         off the emulation thread, closing a second host-wall-clock
//         hazard beyond the poll() timeout patch 0002 already fixed).
//       - rx_queue: producer = worker thread (the driver's SendPacket
//         callback below, fired from inside PollHostSockets or from a
//         DNS-synthesis SendPacket call, both worker-thread-only),
//         consumer = emulation thread (Net_RecvPacket, guest RX tick).
//     Bounded at kWifiNetTxQueueCapacity/kWifiNetRxQueueCapacity entries;
//     a full queue drops the new packet and records it, never blocks and
//     never grows unbounded under a host flood.
//   * A drop on the tx_queue is detected on the emulation thread already
//     (that IS the producer), so it is net_ring_push'd right there. A
//     drop on the rx_queue is detected on the WORKER thread (the
//     producer) -- net_ring_push itself is NOT thread-safe (plain global
//     counters/array, no atomics, by design: every other ring in this
//     runner assumes a single emulation thread, see net_ring.cpp), so the
//     worker thread only increments an atomic counter; the emulation
//     thread's next Net_RecvPacket call drains that counter and is the
//     one that actually calls net_ring_push, keeping every ring write on
//     the emulation thread as the rest of the codebase requires.
//   * Delivery into the guest is entirely tick-quantized: Net_RecvPacket
//     is reached only from Wifi::USTimer's own periodic CheckRX(0) poll
//     (a guest-cycle-scheduled event), never from the worker thread or
//     from any host-wall-clock-timed callback. Packets can sit in
//     rx_queue for an arbitrary amount of *host* wall-clock time before
//     the next guest tick drains them -- that time is not guest-visible
//     (Wifi has no register that exposes "how long a packet waited in a
//     host-side queue"), so it does not violate "no host wall-clock may
//     influence any guest-visible transition": the transition itself
//     (the packet becoming visible to the guest) always lands exactly on
//     a guest tick, never earlier.
//   * Synchronization primitive: plain std::mutex (one per queue) rather
//     than melonDS::Platform::Mutex_* (defined above for PacketDispatcher,
//     a vendored class that only knows Platform::Mutex's opaque forward
//     declaration). This bridge is ordinary project C++ on both ends of
//     every queue operation, so a raw std::mutex + std::lock_guard avoids
//     an unnecessary heap-allocated indirection through the C-shaped
//     shim; nothing here is vendored code that only knows the Platform.h
//     forward declaration.
//   * Shutdown: WifiBridgeState's own destructor sets the stop flag,
//     wakes the worker, and Platform::Thread_Wait()s (joins) it -- BEFORE
//     any member (net/wifi, and therefore the Net_Slirp driver and its
//     libslirp Ctx) is destroyed, by ordinary C++ object-destruction
//     order (a class's own destructor body runs before its members are
//     destroyed). This guarantees no thread ever polls a destroyed slirp
//     context and the join never races the teardown it is waiting for.
//     g_bridge is a namespace-scope std::unique_ptr with static storage
//     duration, so this destructor also runs automatically at normal
//     process exit even if nds_wifi3d_detach() is never called explicitly
//     (it currently isn't, from anywhere in this build -- matching real
//     melonDS's own NDS::Reset() not reconstructing its Wifi/Net members
//     either).

namespace {

constexpr int kNetInstance = 0;

// Set once by nds_wifi_configure_network() before the first
// nds_wifi3d_attach() call (see wifi_net.h). Namespace-scope, not a
// WifiBridgeState member, because it must be readable/writable before
// WifiBridgeState even exists.
NdsWifiNetworkConfig g_network_config{};

// Mirrors Net_Slirp.cpp's own `len > 2048` rejection in SendPacket() and
// SlirpCbSendPacket() -- no packet on either queue can ever exceed this,
// so the worst-case memory bound below is exact, not a guess.
constexpr size_t kWifiNetMaxPacketBytes = 2048;
// Slirp tends to emit a small guest-specific stream, but pcap sees the live LAN
// and can enqueue many accepted broadcasts or peer frames before the guest's
// next Wifi::CheckRX(0) tick drains them. Keep the worst-case bound
// ((tx + rx) * kWifiNetMaxPacketBytes = 18 MiB) fixed regardless of host
// traffic volume -- a host flood still drops packets past this point instead
// of growing memory.
constexpr size_t kWifiNetTxQueueCapacity = 1024;
constexpr size_t kWifiNetRxQueueCapacity = 8192;

uint16_t ReadNet16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8u) | p[1]);
}

uint32_t ReadNet32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24u) |
           (static_cast<uint32_t>(p[1]) << 16u) |
           (static_cast<uint32_t>(p[2]) << 8u) |
           static_cast<uint32_t>(p[3]);
}

#if defined(NDS_ENABLE_PCAP_BACKEND)
uint16_t ReadBe16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8u) | p[1]);
}

uint32_t ReadBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24u) |
           (static_cast<uint32_t>(p[1]) << 16u) |
           (static_cast<uint32_t>(p[2]) << 8u) |
           static_cast<uint32_t>(p[3]);
}

void WriteBe16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value >> 8u);
    p[1] = static_cast<uint8_t>(value);
}

uint16_t InternetChecksum(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2)
        sum += ReadBe16(data + i);
    if (len & 1u) sum += static_cast<uint16_t>(data[len - 1] << 8u);
    while (sum >> 16u)
        sum = (sum & 0xFFFFu) + (sum >> 16u);
    return static_cast<uint16_t>(~sum);
}

bool RewriteDhcpDnsOption(std::vector<uint8_t>* packet,
                          uint32_t dns_ipv4_host_order) {
    if (!packet || dns_ipv4_host_order == 0u) return false;
    std::vector<uint8_t>& p = *packet;
    if (p.size() < 14u + 20u + 8u + 240u) return false;
    if (ReadBe16(&p[12]) != 0x0800u) return false;  // Ethernet: IPv4

    const size_t ip = 14u;
    const uint8_t version = static_cast<uint8_t>(p[ip] >> 4u);
    const size_t ihl = static_cast<size_t>(p[ip] & 0x0Fu) * 4u;
    if (version != 4u || ihl < 20u || p.size() < ip + ihl + 8u + 240u)
        return false;
    if (p[ip + 9u] != 17u) return false;  // UDP

    const size_t udp = ip + ihl;
    const uint16_t src_port = ReadBe16(&p[udp]);
    const uint16_t dst_port = ReadBe16(&p[udp + 2u]);
    if (src_port != 67u || dst_port != 68u) return false;
    const uint16_t udp_len = ReadBe16(&p[udp + 4u]);
    if (udp_len < 8u + 240u || p.size() < udp + udp_len) return false;

    const size_t dhcp = udp + 8u;
    if (p[dhcp + 236u] != 0x63u || p[dhcp + 237u] != 0x82u ||
        p[dhcp + 238u] != 0x53u || p[dhcp + 239u] != 0x63u) {
        return false;
    }

    const size_t options_begin = dhcp + 240u;
    const size_t options_end = udp + udp_len;
    for (size_t opt = options_begin; opt < options_end;) {
        const uint8_t code = p[opt++];
        if (code == 0u) continue;       // pad
        if (code == 255u) return false; // end
        if (opt >= options_end) return false;
        const uint8_t len = p[opt++];
        if (opt + len > options_end) return false;
        if (code == 6u && len >= 4u) {
            for (size_t i = 0; i + 3u < len; i += 4u) {
                p[opt + i + 0u] =
                    static_cast<uint8_t>(dns_ipv4_host_order >> 24u);
                p[opt + i + 1u] =
                    static_cast<uint8_t>(dns_ipv4_host_order >> 16u);
                p[opt + i + 2u] =
                    static_cast<uint8_t>(dns_ipv4_host_order >> 8u);
                p[opt + i + 3u] =
                    static_cast<uint8_t>(dns_ipv4_host_order);
            }

            WriteBe16(&p[ip + 10u], 0u);
            WriteBe16(&p[ip + 10u], InternetChecksum(&p[ip], ihl));
            // IPv4 UDP checksums are optional; clear rather than carrying a
            // checksum over a payload we intentionally changed.
            WriteBe16(&p[udp + 6u], 0u);
            return true;
        }
        opt += len;
    }
    return false;
}

bool PcapAdapterHasMac(const melonDS::AdapterData& adapter) {
    for (uint8_t b : adapter.MAC) {
        if (b != 0u) return true;
    }
    return false;
}

bool PcapAdapterHasIpv4(const melonDS::AdapterData& adapter) {
    for (uint8_t b : adapter.IP_v4) {
        if (b != 0u) return true;
    }
    return false;
}

std::string PcapAdapterIpv4String(const melonDS::AdapterData& adapter) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  adapter.IP_v4[0], adapter.IP_v4[1],
                  adapter.IP_v4[2], adapter.IP_v4[3]);
    return std::string(buf);
}

bool PcapAdapterNameMatches(const melonDS::AdapterData& adapter,
                            const std::string& requested) {
    return requested == adapter.DeviceName ||
           requested == adapter.FriendlyName ||
           requested == adapter.Description;
}

bool ContainsAsciiInsensitive(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) return false;
    for (const char* h = haystack; *h; ++h) {
        const char* hp = h;
        const char* np = needle;
        while (*hp && *np &&
               std::tolower(static_cast<unsigned char>(*hp)) ==
                   std::tolower(static_cast<unsigned char>(*np))) {
            ++hp;
            ++np;
        }
        if (!*np) return true;
    }
    return false;
}

bool IsPcapAdapterProbablyVirtual(const melonDS::AdapterData& adapter) {
    for (const char* needle : {
             "bluetooth",
             "hyper-v",
             "tap-windows",
             "twingate",
             "virtual",
             "vethernet",
             "vpn",
             "wsl",
         }) {
        if (ContainsAsciiInsensitive(adapter.FriendlyName, needle) ||
            ContainsAsciiInsensitive(adapter.Description, needle) ||
            ContainsAsciiInsensitive(adapter.DeviceName, needle)) {
            return true;
        }
    }
    return false;
}

bool MacEqual(const uint8_t* a, const std::array<uint8_t, 6>& b) {
    for (size_t i = 0; i < b.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

bool IsBroadcastMac(const uint8_t* mac) {
    for (size_t i = 0; i < 6u; ++i) {
        if (mac[i] != 0xFFu) return false;
    }
    return true;
}

bool IsPcapArpFrameForGuest(const uint8_t* data, int len,
                            uint32_t guest_ipv4,
                            bool guest_ipv4_known) {
    if (!guest_ipv4_known) return false;
    if (len < 14 + 28) return false;
    if (ReadBe16(data + 12u) != 0x0806u) return false;

    const size_t arp = 14u;
    if (ReadBe16(data + arp + 0u) != 1u) return false;       // Ethernet
    if (ReadBe16(data + arp + 2u) != 0x0800u) return false;  // IPv4
    if (data[arp + 4u] != 6u || data[arp + 5u] != 4u) return false;
    const uint32_t target_ip = ReadBe32(data + arp + 24u);
    return target_ip == guest_ipv4;
}

bool IsPcapDhcpFrameForGuest(const uint8_t* data, int len,
                             const std::array<uint8_t, 6>& guest_mac,
                             bool guest_mac_known) {
    if (len < 14 + 20 + 8 + 44) return false;
    if (ReadBe16(data + 12u) != 0x0800u) return false;

    const size_t ip = 14u;
    const uint8_t version = static_cast<uint8_t>(data[ip] >> 4u);
    const size_t ihl = static_cast<size_t>(data[ip] & 0x0Fu) * 4u;
    if (version != 4u || ihl < 20u ||
        static_cast<size_t>(len) < ip + ihl + 8u + 44u) {
        return false;
    }
    if (data[ip + 9u] != 17u) return false;  // UDP

    const size_t udp = ip + ihl;
    const uint16_t src_port = ReadBe16(data + udp);
    const uint16_t dst_port = ReadBe16(data + udp + 2u);
    if (!((src_port == 67u && dst_port == 68u) ||
          (src_port == 68u && dst_port == 67u))) {
        return false;
    }

    if (!guest_mac_known) return true;
    const size_t bootp = udp + 8u;
    const uint8_t hlen = data[bootp + 2u];
    if (hlen != 6u) return false;
    return MacEqual(data + bootp + 28u, guest_mac);
}

bool IsPcapGuestUnicastFrame(const uint8_t* data, int len,
                             const std::array<uint8_t, 6>& guest_mac,
                             bool guest_mac_known,
                             uint32_t guest_ipv4,
                             bool guest_ipv4_known) {
    const uint16_t ethertype = ReadBe16(data + 12u);
    if (ethertype == 0x0806u)
        return IsPcapArpFrameForGuest(data, len, guest_ipv4,
                                      guest_ipv4_known);
    if (ethertype != 0x0800u) return false;

    if (IsPcapDhcpFrameForGuest(data, len, guest_mac, guest_mac_known))
        return true;

    if (!guest_ipv4_known) return false;
    if (len < 14 + 20) return false;
    const size_t ipv4 = 14u;
    const uint8_t version = static_cast<uint8_t>(data[ipv4] >> 4u);
    const size_t ihl = static_cast<size_t>(data[ipv4] & 0x0Fu) * 4u;
    if (version != 4u || ihl < 20u ||
        static_cast<size_t>(len) < ipv4 + ihl) {
        return false;
    }
    return ReadBe32(data + ipv4 + 16u) == guest_ipv4;
}

bool IsPcapRequiredBroadcastFrame(const uint8_t* data, int len,
                                  const std::array<uint8_t, 6>& guest_mac,
                                  bool guest_mac_known,
                                  uint32_t guest_ipv4,
                                  bool guest_ipv4_known) {
    return IsPcapArpFrameForGuest(data, len, guest_ipv4,
                                  guest_ipv4_known) ||
           IsPcapDhcpFrameForGuest(data, len, guest_mac, guest_mac_known);
}

const melonDS::AdapterData* ChoosePcapAdapter(
    const std::vector<melonDS::AdapterData>& adapters,
    const std::string& requested) {
    if (!requested.empty()) {
        for (const melonDS::AdapterData& adapter : adapters) {
            if (PcapAdapterNameMatches(adapter, requested)) return &adapter;
        }
        return nullptr;
    }

    for (const melonDS::AdapterData& adapter : adapters) {
        const bool loopback = (adapter.Flags & PCAP_IF_LOOPBACK) != 0u;
        const bool up = (adapter.Flags & PCAP_IF_UP) != 0u;
        const bool running = (adapter.Flags & PCAP_IF_RUNNING) != 0u;
        if (!loopback && up && running && PcapAdapterHasMac(adapter) &&
            PcapAdapterHasIpv4(adapter) &&
            !IsPcapAdapterProbablyVirtual(adapter)) {
            return &adapter;
        }
    }
    return nullptr;
}

void LogPcapAdapters(const std::vector<melonDS::AdapterData>& adapters) {
    for (const melonDS::AdapterData& adapter : adapters) {
        melonDS::Platform::Log(melonDS::Platform::Info,
            "[wifi_net] pcap adapter: device='%s' friendly='%s' "
            "description='%s' ipv4=%s flags=0x%08x\n",
            adapter.DeviceName, adapter.FriendlyName, adapter.Description,
            PcapAdapterIpv4String(adapter).c_str(), adapter.Flags);
    }
}
#endif

// A bounded FIFO of complete, independently-allocated packet buffers.
// Ownership transfers cleanly across the thread boundary: TryPush moves
// the caller's buffer in (the caller's copy is left empty), TryPop moves
// it back out to the caller (the queue's copy is left empty) -- there is
// never a shared pointer or a live alias on both sides, so there is no
// use-after-free or torn-frame hazard: each byte range is owned by
// exactly one side at any given time, transferred, never copied-in-place.
// Single internal std::mutex; both queues in this file are used as
// strict single-producer/single-consumer, so contention is a non-issue.
class BoundedPacketQueue {
public:
    explicit BoundedPacketQueue(size_t capacity) : capacity_(capacity) {}

    bool TryPush(std::vector<uint8_t>&& packet) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= capacity_) return false;
        queue_.push_back(std::move(packet));
        return true;
    }

    bool TryPop(std::vector<uint8_t>* out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        *out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

private:
    std::mutex mutex_;
    std::deque<std::vector<uint8_t>> queue_;
    size_t capacity_;
};

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

uint64_t ReadLe64(const uint8_t* p) {
    return static_cast<uint64_t>(ReadLe32(p)) |
           (static_cast<uint64_t>(ReadLe32(p + 4u)) << 32u);
}

void WriteLe32(std::vector<uint8_t>* out, uint32_t value) {
    out->push_back(static_cast<uint8_t>(value));
    out->push_back(static_cast<uint8_t>(value >> 8u));
    out->push_back(static_cast<uint8_t>(value >> 16u));
    out->push_back(static_cast<uint8_t>(value >> 24u));
}

void WriteLe64(std::vector<uint8_t>* out, uint64_t value) {
    WriteLe32(out, static_cast<uint32_t>(value));
    WriteLe32(out, static_cast<uint32_t>(value >> 32u));
}

struct LocalMpFrame {
    uint32_t sender = 0;
    uint32_t type = 0;
    uint64_t timestamp = 0;
    std::vector<uint8_t> payload;
};

class LocalMpFrameQueue {
public:
    explicit LocalMpFrameQueue(size_t capacity) : capacity_(capacity) {}

    bool TryPush(LocalMpFrame&& frame) {
        if (queue_.size() >= capacity_) return false;
        queue_.push_back(std::move(frame));
        return true;
    }

    bool TryPop(LocalMpFrame* out) {
        if (queue_.empty()) return false;
        *out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void Clear() { queue_.clear(); }

private:
    std::deque<LocalMpFrame> queue_;
    size_t capacity_;
};

class LocalMpTransport {
public:
    void Configure(bool enabled, uint32_t instance, uint16_t base_port) {
        enabled_ = enabled && instance < kMaxInstances;
        instance_ = instance;
        base_port_ = base_port;
    }

    void Begin() {
        if (!enabled_) return;
#ifdef _WIN32
        if (socket_ != INVALID_SOCKET) return;

        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) {
            melonDS::Platform::Log(melonDS::Platform::Error,
                "[local_mp] socket failed: %d\n", WSAGetLastError());
            return;
        }

        BOOL reset_enabled = FALSE;
        DWORD bytes_returned = 0;
        WSAIoctl(socket_, SIO_UDP_CONNRESET, &reset_enabled,
                 sizeof(reset_enabled), nullptr, 0, &bytes_returned,
                 nullptr, nullptr);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<uint16_t>(base_port_ + instance_));
        if (::bind(socket_, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) == SOCKET_ERROR) {
            melonDS::Platform::Log(melonDS::Platform::Error,
                "[local_mp] bind 127.0.0.1:%u failed: %d\n",
                static_cast<unsigned>(base_port_ + instance_),
                WSAGetLastError());
            CloseSocket();
            return;
        }

        u_long nonblocking = 1;
        if (ioctlsocket(socket_, FIONBIO, &nonblocking) == SOCKET_ERROR) {
            melonDS::Platform::Log(melonDS::Platform::Error,
                "[local_mp] nonblocking setup failed: %d\n", WSAGetLastError());
            CloseSocket();
            return;
        }

        melonDS::Platform::Log(melonDS::Platform::Info,
            "[local_mp] enabled instance=%u port=%u\n",
            static_cast<unsigned>(instance_),
            static_cast<unsigned>(base_port_ + instance_));
#else
        melonDS::Platform::Log(melonDS::Platform::Warn,
            "[local_mp] localhost UDP transport is currently Windows-only\n");
#endif
    }

    void End() {
        packet_queue_.Clear();
        reply_queue_.Clear();
        last_host_id_ = -1;
#ifdef _WIN32
        CloseSocket();
#endif
    }

    int SendPacket(uint8_t* data, int len, uint64_t timestamp) {
        return SendGeneric(0, data, len, timestamp);
    }

    int RecvPacket(uint8_t* data, uint64_t* timestamp) {
        return RecvPacketGeneric(data, timestamp, false);
    }

    int SendCmd(uint8_t* data, int len, uint64_t timestamp) {
        return SendGeneric(1, data, len, timestamp);
    }

    int SendReply(uint8_t* data, int len, uint64_t timestamp, uint16_t aid) {
        return SendGeneric(2u | (static_cast<uint32_t>(aid) << 16u),
                           data, len, timestamp);
    }

    int SendAck(uint8_t* data, int len, uint64_t timestamp) {
        return SendGeneric(3, data, len, timestamp);
    }

    int RecvHostPacket(uint8_t* data, uint64_t* timestamp) {
        (void)last_host_id_;
        return RecvPacketGeneric(data, timestamp, true);
    }

    uint16_t RecvReplies(uint8_t* data, uint64_t timestamp, uint16_t aidmask) {
        uint16_t received = 0;
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::milliseconds(25);
        stats_recv_replies_calls_.fetch_add(1, std::memory_order_relaxed);
        for (;;) {
            DrainSocket();
            LocalMpFrame frame;
            if (!reply_queue_.TryPop(&frame)) {
                // Wait on the socket itself: a datagram arrival wakes this
                // thread immediately. Sleeping a fixed tick here instead
                // costs every MP cmd/reply exchange a millisecond-scale
                // round trip on BOTH peers (the client is symmetrically
                // sleep-polling in RecvPacketGeneric), and a video frame
                // runs many MP exchanges -- that is what capped local
                // wireless at ~20 FPS while infrastructure-mode Wiimmfi
                // (which never blocks here) held 60.
                if (!WaitForData(deadline)) {
                    stats_recv_replies_timeouts_.fetch_add(
                        1, std::memory_order_relaxed);
                    break;
                }
                continue;
            }
            if (frame.timestamp + 32u < timestamp) {
                stats_stale_reply_drops_.fetch_add(1,
                                                   std::memory_order_relaxed);
                continue;
            }
            const uint16_t aid = static_cast<uint16_t>(frame.type >> 16u);
            if (aid == 0 || aid > 15) continue;
            if ((aidmask & (1u << aid)) == 0) continue;
            if (!frame.payload.empty()) {
                std::memcpy(&data[(aid - 1u) * 1024u],
                            frame.payload.data(), frame.payload.size());
            }
            received |= static_cast<uint16_t>(1u << aid);
            if ((received & aidmask) == aidmask) break;
        }
        stats_recv_replies_wait_us_.fetch_add(
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count()),
            std::memory_order_relaxed);
        return received;
    }

    void SnapshotStats(NdsLocalMpStats* out) const {
        out->enabled = enabled_;
        out->frames_sent =
            stats_frames_sent_.load(std::memory_order_relaxed);
        out->frames_received =
            stats_frames_received_.load(std::memory_order_relaxed);
        out->recv_replies_calls =
            stats_recv_replies_calls_.load(std::memory_order_relaxed);
        out->recv_replies_timeouts =
            stats_recv_replies_timeouts_.load(std::memory_order_relaxed);
        out->recv_replies_wait_us =
            stats_recv_replies_wait_us_.load(std::memory_order_relaxed);
        out->recv_host_calls =
            stats_recv_host_calls_.load(std::memory_order_relaxed);
        out->recv_host_timeouts =
            stats_recv_host_timeouts_.load(std::memory_order_relaxed);
        out->recv_host_wait_us =
            stats_recv_host_wait_us_.load(std::memory_order_relaxed);
        out->stale_reply_drops =
            stats_stale_reply_drops_.load(std::memory_order_relaxed);
    }

private:
    static constexpr uint32_t kMagic = 0x4946494Eu;
    static constexpr uint32_t kMaxInstances = 16;
    static constexpr uint32_t kHeaderBytes = 24;
    static constexpr size_t kMaxFrameBytes = 0x948;
    static constexpr size_t kQueueCapacity = 1024;

    int RecvPacketGeneric(uint8_t* data, uint64_t* timestamp, bool block) {
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::milliseconds(25);
        if (block)
            stats_recv_host_calls_.fetch_add(1, std::memory_order_relaxed);
        for (;;) {
            DrainSocket();
            LocalMpFrame frame;
            if (!packet_queue_.TryPop(&frame)) {
                if (!block) return 0;
                // See RecvReplies: wake on datagram arrival, never on a
                // sleep tick. The client sits here at every NextSync point
                // waiting for the host's next MP frame, so this wait's
                // latency directly paces BOTH instances.
                if (WaitForData(deadline)) continue;
                stats_recv_host_timeouts_.fetch_add(
                    1, std::memory_order_relaxed);
                RecordHostWait(start);
                return 0;
            }
            if (timestamp) *timestamp = frame.timestamp;
            if (frame.type == 1u) last_host_id_ = static_cast<int>(frame.sender);
            if (!frame.payload.empty())
                std::memcpy(data, frame.payload.data(), frame.payload.size());
            if (block) RecordHostWait(start);
            return static_cast<int>(frame.payload.size());
        }
    }

    void RecordHostWait(std::chrono::steady_clock::time_point start) {
        stats_recv_host_wait_us_.fetch_add(
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count()),
            std::memory_order_relaxed);
    }

    // Blocks until the transport socket is readable or `deadline` passes.
    // True = readable (caller should DrainSocket and re-check its queue),
    // false = deadline reached. select() wakes on arrival with microsecond
    // latency; the timeout only bites when the peer is genuinely absent.
    bool WaitForData(std::chrono::steady_clock::time_point deadline) {
#ifdef _WIN32
        if (socket_ == INVALID_SOCKET) return false;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        const auto remaining =
            std::chrono::duration_cast<std::chrono::microseconds>(deadline -
                                                                  now);
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket_, &readable);
        timeval tv{};
        tv.tv_sec = static_cast<long>(remaining.count() / 1000000);
        tv.tv_usec = static_cast<long>(remaining.count() % 1000000);
        return ::select(0, &readable, nullptr, nullptr, &tv) > 0;
#else
        (void)deadline;
        return false;
#endif
    }

    int SendGeneric(uint32_t type, uint8_t* data, int len,
                    uint64_t timestamp) {
        if (!enabled_) return 0;
#ifdef _WIN32
        if (socket_ == INVALID_SOCKET) return 0;
        if (len < 0 || static_cast<size_t>(len) > kMaxFrameBytes) return 0;
        if (len > 0 && !data) return 0;

        std::vector<uint8_t> datagram;
        datagram.reserve(kHeaderBytes + static_cast<size_t>(len));
        WriteLe32(&datagram, kMagic);
        WriteLe32(&datagram, instance_);
        WriteLe32(&datagram, type);
        WriteLe32(&datagram, static_cast<uint32_t>(len));
        WriteLe64(&datagram, timestamp);
        if (len > 0)
            datagram.insert(datagram.end(), data, data + len);

        int sent = 0;
        for (uint32_t i = 0; i < kMaxInstances; ++i) {
            if (i == instance_) continue;
            sockaddr_in target{};
            target.sin_family = AF_INET;
            target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            target.sin_port = htons(static_cast<uint16_t>(base_port_ + i));
            if (::sendto(socket_,
                         reinterpret_cast<const char*>(datagram.data()),
                         static_cast<int>(datagram.size()), 0,
                         reinterpret_cast<sockaddr*>(&target),
                         sizeof(target)) != SOCKET_ERROR) {
                sent = len;
                stats_frames_sent_.fetch_add(1, std::memory_order_relaxed);
            } else if (send_error_log_counter_ < 8u) {
                ++send_error_log_counter_;
                melonDS::Platform::Log(melonDS::Platform::Warn,
                    "[local_mp] sendto 127.0.0.1:%u failed: %d\n",
                    static_cast<unsigned>(base_port_ + i),
                    WSAGetLastError());
            }
        }
        return sent;
#else
        (void)type;
        (void)data;
        (void)len;
        (void)timestamp;
        return 0;
#endif
    }

    void DrainSocket() {
#ifdef _WIN32
        if (!enabled_ || socket_ == INVALID_SOCKET) return;
        for (;;) {
            uint8_t datagram[kHeaderBytes + kMaxFrameBytes];
            sockaddr_in from{};
            int from_len = sizeof(from);
            const int got = ::recvfrom(
                socket_, reinterpret_cast<char*>(datagram), sizeof(datagram),
                0, reinterpret_cast<sockaddr*>(&from), &from_len);
            if (got == SOCKET_ERROR) {
                const int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK && err != WSAECONNRESET) {
                    melonDS::Platform::Log(melonDS::Platform::Warn,
                        "[local_mp] recvfrom failed: %d\n", err);
                }
                return;
            }
            if (got < static_cast<int>(kHeaderBytes)) continue;
            if (ReadLe32(datagram) != kMagic) continue;

            LocalMpFrame frame;
            frame.sender = ReadLe32(datagram + 4u);
            frame.type = ReadLe32(datagram + 8u);
            const uint32_t len = ReadLe32(datagram + 12u);
            frame.timestamp = ReadLe64(datagram + 16u);
            if (frame.sender == instance_) continue;
            if (frame.sender >= kMaxInstances) continue;
            if (len > kMaxFrameBytes) continue;
            if (kHeaderBytes + len != static_cast<uint32_t>(got)) continue;
            frame.payload.assign(datagram + kHeaderBytes,
                                 datagram + kHeaderBytes + len);

            stats_frames_received_.fetch_add(1, std::memory_order_relaxed);
            const uint32_t low_type = frame.type & 0xFFFFu;
            if (low_type == 2u)
                reply_queue_.TryPush(std::move(frame));
            else
                packet_queue_.TryPush(std::move(frame));
        }
#endif
    }

#ifdef _WIN32
    void CloseSocket() {
        if (socket_ == INVALID_SOCKET) return;
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
#endif

    bool enabled_ = false;
    uint32_t instance_ = 0;
    uint16_t base_port_ = 26710;
    LocalMpFrameQueue packet_queue_{kQueueCapacity};
    LocalMpFrameQueue reply_queue_{kQueueCapacity};
    int last_host_id_ = -1;
    uint32_t send_error_log_counter_ = 0;
    // Always-on wait/traffic counters (see NdsLocalMpStats in wifi_net.h).
    // Relaxed atomics: written on the emulation thread, snapshotted from
    // the debug-server thread.
    std::atomic<uint64_t> stats_frames_sent_{0};
    std::atomic<uint64_t> stats_frames_received_{0};
    std::atomic<uint64_t> stats_recv_replies_calls_{0};
    std::atomic<uint64_t> stats_recv_replies_timeouts_{0};
    std::atomic<uint64_t> stats_recv_replies_wait_us_{0};
    std::atomic<uint64_t> stats_recv_host_calls_{0};
    std::atomic<uint64_t> stats_recv_host_timeouts_{0};
    std::atomic<uint64_t> stats_recv_host_wait_us_{0};
    std::atomic<uint64_t> stats_stale_reply_drops_{0};
#ifdef _WIN32
    SOCKET socket_ = INVALID_SOCKET;
#endif
};

class LocalWfcPeerBridge {
public:
    void Configure(uint32_t instance) {
        enabled_ = instance < kMaxInstances;
        instance_ = instance;
    }

    void Begin() {
        if (!enabled_) return;
#ifdef _WIN32
        if (socket_ != INVALID_SOCKET) return;

        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) {
            melonDS::Platform::Log(melonDS::Platform::Error,
                "[wfc_peer] socket failed: %d\n", WSAGetLastError());
            return;
        }

        BOOL reset_enabled = FALSE;
        DWORD bytes_returned = 0;
        WSAIoctl(socket_, SIO_UDP_CONNRESET, &reset_enabled,
                 sizeof(reset_enabled), nullptr, 0, &bytes_returned,
                 nullptr, nullptr);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<uint16_t>(kBasePort + instance_));
        if (::bind(socket_, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) == SOCKET_ERROR) {
            melonDS::Platform::Log(melonDS::Platform::Error,
                "[wfc_peer] bind 127.0.0.1:%u failed: %d\n",
                static_cast<unsigned>(kBasePort + instance_),
                WSAGetLastError());
            CloseSocket();
            return;
        }

        u_long nonblocking = 1;
        if (ioctlsocket(socket_, FIONBIO, &nonblocking) == SOCKET_ERROR) {
            melonDS::Platform::Log(melonDS::Platform::Error,
                "[wfc_peer] nonblocking setup failed: %d\n", WSAGetLastError());
            CloseSocket();
            return;
        }

        melonDS::Platform::Log(melonDS::Platform::Info,
            "[wfc_peer] enabled instance=%u port=%u\n",
            static_cast<unsigned>(instance_),
            static_cast<unsigned>(kBasePort + instance_));
#else
        melonDS::Platform::Log(melonDS::Platform::Warn,
            "[wfc_peer] localhost UDP transport is currently Windows-only\n");
#endif
    }

    void End() {
#ifdef _WIN32
        CloseSocket();
#endif
    }

    bool ForwardIfPeerFrame(const uint8_t* data, int len) {
        if (!enabled_ || !data) return false;
        uint32_t target = 0;
        if (!IsPeerFrame(data, len, &target)) return false;
#ifdef _WIN32
        if (socket_ == INVALID_SOCKET) return false;
        if (len <= 0 || static_cast<size_t>(len) > kMaxFrameBytes) return true;

        std::vector<uint8_t> datagram;
        datagram.reserve(kHeaderBytes + static_cast<size_t>(len));
        WriteLe32(&datagram, kMagic);
        WriteLe32(&datagram, instance_);
        WriteLe32(&datagram, static_cast<uint32_t>(len));
        datagram.insert(datagram.end(), data, data + len);
        // The sender routed this frame to its local gateway MAC. The receiving
        // WifiAP filters on Ethernet destination before the IP stack sees the
        // packet, so make the localhost-carried copy a broadcast Ethernet
        // frame while leaving the IP/UDP payload untouched.
        for (size_t i = 0; i < 6u; ++i) datagram[kHeaderBytes + i] = 0xFFu;

        sockaddr_in peer{};
        peer.sin_family = AF_INET;
        peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        peer.sin_port = htons(static_cast<uint16_t>(kBasePort + target));
        if (::sendto(socket_, reinterpret_cast<const char*>(datagram.data()),
                     static_cast<int>(datagram.size()), 0,
                     reinterpret_cast<sockaddr*>(&peer),
                     sizeof(peer)) == SOCKET_ERROR) {
            if (send_error_log_counter_ < 8u) {
                ++send_error_log_counter_;
                melonDS::Platform::Log(melonDS::Platform::Warn,
                    "[wfc_peer] sendto 127.0.0.1:%u failed: %d\n",
                    static_cast<unsigned>(kBasePort + target),
                    WSAGetLastError());
            }
        }
#endif
        return true;
    }

    void DrainTo(BoundedPacketQueue* rx_queue,
                 std::atomic<uint32_t>* rx_drop_counter) {
#ifdef _WIN32
        if (!enabled_ || socket_ == INVALID_SOCKET || !rx_queue) return;
        for (;;) {
            uint8_t datagram[kHeaderBytes + kMaxFrameBytes];
            sockaddr_in from{};
            int from_len = sizeof(from);
            const int got = ::recvfrom(
                socket_, reinterpret_cast<char*>(datagram), sizeof(datagram),
                0, reinterpret_cast<sockaddr*>(&from), &from_len);
            if (got == SOCKET_ERROR) {
                const int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK && err != WSAECONNRESET) {
                    melonDS::Platform::Log(melonDS::Platform::Warn,
                        "[wfc_peer] recvfrom failed: %d\n", err);
                }
                return;
            }
            if (got < static_cast<int>(kHeaderBytes)) continue;
            if (ReadLe32(datagram) != kMagic) continue;
            const uint32_t sender = ReadLe32(datagram + 4u);
            const uint32_t len = ReadLe32(datagram + 8u);
            if (sender == instance_ || sender >= kMaxInstances) continue;
            if (len > kMaxFrameBytes) continue;
            if (kHeaderBytes + len != static_cast<uint32_t>(got)) continue;

            std::vector<uint8_t> packet(datagram + kHeaderBytes,
                                        datagram + kHeaderBytes + len);
            if (!rx_queue->TryPush(std::move(packet)) && rx_drop_counter) {
                rx_drop_counter->fetch_add(1, std::memory_order_relaxed);
            }
        }
#else
        (void)rx_queue;
        (void)rx_drop_counter;
#endif
    }

private:
    static constexpr uint32_t kMagic = 0x50434657u;
    static constexpr uint32_t kMaxInstances = 16;
    static constexpr uint32_t kHeaderBytes = 12;
    static constexpr size_t kMaxFrameBytes = kWifiNetMaxPacketBytes;
    static constexpr uint16_t kBasePort = 27610;

    bool IsPeerFrame(const uint8_t* data, int len, uint32_t* target) const {
        if (len < 14 + 20) return false;
        if (ReadNet16(data + 12u) != 0x0800u) return false;
        const size_t ip = 14u;
        const uint8_t version = static_cast<uint8_t>(data[ip] >> 4u);
        const size_t ihl = static_cast<size_t>(data[ip] & 0x0Fu) * 4u;
        if (version != 4u || ihl < 20u ||
            static_cast<size_t>(len) < ip + ihl) {
            return false;
        }
        const uint32_t dst = ReadNet32(data + ip + 16u);
        if ((dst & 0xFFFF00FFu) != 0x0A400010u) return false;
        const uint32_t peer = (dst >> 8u) & 0xFFu;
        if (peer >= kMaxInstances || peer == instance_) return false;
        if (target) *target = peer;
        return true;
    }

#ifdef _WIN32
    void CloseSocket() {
        if (socket_ == INVALID_SOCKET) return;
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
#endif

    bool enabled_ = false;
    uint32_t instance_ = 0;
    uint32_t send_error_log_counter_ = 0;
#ifdef _WIN32
    SOCKET socket_ = INVALID_SOCKET;
#endif
};

struct WifiBridgeState {
    melonDS::NDS nds;
    std::unique_ptr<melonDS::Wifi> wifi;
    melonDS::Net net;
    bool net_instance_registered = false;

    // Non-owning: live socket/adapter backends are owned by `net` and called
    // only from the worker thread. Slirp also keeps the concrete pointer below
    // so its extra poll-error counters remain observable.
    melonDS::NetDriver* live_driver = nullptr;

    // Non-owning: `net` (above) owns the actual melonDS::NetDriver via
    // unique_ptr. Kept separately because PollHostSockets() is not part
    // of the NetDriver virtual interface (NetDriver.h is not a file this
    // pass owns/may extend), so reaching it needs the concrete type.
    // Valid for exactly as long as `net`'s driver is set, which is from
    // the end of nds_wifi3d_attach() until this struct's destructor runs
    // (net's own destruction happens after StopWorker() below, so the
    // worker thread never dereferences this after it goes stale).
    melonDS::Net_Slirp* slirp_driver = nullptr;

    // Wiimmfi M8: mirrors slirp_driver above, but for the replay backend
    // (net/net_replay.h) -- mutually exclusive with slirp_driver (attach()
    // constructs exactly one of the two, per NdsWifiNetworkConfig::backend).
    // Non-owning for the same reason: `net` owns the concrete NetDriver via
    // unique_ptr; this is only kept separately because SetCurrentCycle/
    // SetCurrentPCs and the mismatch-query accessors are not part of the
    // NetDriver virtual interface, so reaching them needs the concrete
    // type. Never a worker-thread concern: unlike slirp_driver, this is
    // touched ONLY from the emulation thread (replay never starts a worker
    // thread at all -- see nds_wifi3d_attach()).
    melonDS::NetReplay* replay_driver = nullptr;

    // Wiimmfi M8: live packet capture, independent of which backend is
    // active (see NdsWifiNetworkConfig::capture_out_path). No-op (is_open()
    // == false) unless a capture_out_path was configured and its Open()
    // call actually succeeded.
    NdsNetCaptureWriter capture_writer;
    LocalMpTransport local_mp;
    LocalWfcPeerBridge local_wfc_peer;

    NdsWifiNetworkState network_state;

    BoundedPacketQueue tx_queue{kWifiNetTxQueueCapacity};  // guest -> host
    BoundedPacketQueue rx_queue{kWifiNetRxQueueCapacity};  // host -> guest

    std::mutex guest_mac_mutex;
    std::array<uint8_t, 6> guest_mac{};
    bool guest_mac_known = false;
    uint32_t guest_ipv4 = 0;
    bool guest_ipv4_known = false;

    // Incremented (worker thread) whenever rx_queue.TryPush fails.
    // net_ring_push is not thread-safe (see the design comment above), so
    // the actual ring write happens on the emulation thread's next
    // Net_RecvPacket call, which drains this counter via exchange(0).
    std::atomic<uint32_t> rx_dropped_since_last_report{0};
    // Pcap frames that match this guest but exceed the emulated RX buffer
    // limit use the same emulation-thread rendezvous, with a distinct aux.
    // Oversized unrelated LAN frames are rejected before this counter.
    std::atomic<uint32_t> pcap_rx_oversize_since_last_report{0};

    std::atomic<bool> worker_stop{false};
    std::mutex wake_mutex;
    std::condition_variable wake_cv;
    bool wake_pending = false;
    melonDS::Platform::Thread* worker_thread = nullptr;

    void StopWorker() {
        if (!worker_thread) return;
        worker_stop.store(true, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(wake_mutex);
            wake_pending = true;
        }
        wake_cv.notify_all();
        // Blocks until the worker thread's loop observes worker_stop and
        // returns -- bounded by the loop's own idle-wait granularity
        // (a few milliseconds, see WifiWorkerThreadMain), never by a
        // host-socket wait: PollHostSockets()'s poll() is unconditionally
        // non-blocking (patch 0002's clamp, retained in PollHostSockets),
        // so the worker is never stuck inside a long syscall when this
        // runs.
        melonDS::Platform::Thread_Wait(worker_thread);
        melonDS::Platform::Thread_Free(worker_thread);
        worker_thread = nullptr;
        melonDS::Platform::Log(melonDS::Platform::Info,
            "[wifi_net] host network worker thread stopped\n");
    }

    // Destructor body runs BEFORE member destruction (net, wifi, nds are
    // destroyed after this returns) -- see the design comment above for
    // why that ordering is exactly what makes shutdown safe.
    ~WifiBridgeState() { StopWorker(); }
};

std::unique_ptr<WifiBridgeState> g_bridge;

void LearnGuestMacFromTx(WifiBridgeState* state, const uint8_t* data,
                         int len) {
    if (!state || !data || len < 12) return;
    std::lock_guard<std::mutex> lock(state->guest_mac_mutex);
    for (size_t i = 0; i < state->guest_mac.size(); ++i)
        state->guest_mac[i] = data[6u + i];
    state->guest_mac_known = true;
}

#if defined(NDS_ENABLE_PCAP_BACKEND)
void LearnGuestIpv4FromTx(WifiBridgeState* state, const uint8_t* data,
                          int len) {
    if (!state || !data || len < 14) return;
    uint32_t ip = 0;
    const uint16_t ethertype = ReadBe16(data + 12u);
    if (ethertype == 0x0800u && len >= 14 + 20) {
        const size_t ipv4 = 14u;
        const uint8_t version = static_cast<uint8_t>(data[ipv4] >> 4u);
        const size_t ihl = static_cast<size_t>(data[ipv4] & 0x0Fu) * 4u;
        if (version == 4u && ihl >= 20u &&
            static_cast<size_t>(len) >= ipv4 + ihl) {
            ip = ReadBe32(data + ipv4 + 12u);
        }
    } else if (ethertype == 0x0806u && len >= 14 + 28) {
        const size_t arp = 14u;
        if (ReadBe16(data + arp + 0u) == 1u &&
            ReadBe16(data + arp + 2u) == 0x0800u &&
            data[arp + 4u] == 6u && data[arp + 5u] == 4u) {
            ip = ReadBe32(data + arp + 14u);
        }
    }
    if (ip == 0u || ip == 0xFFFFFFFFu) return;
    std::lock_guard<std::mutex> lock(state->guest_mac_mutex);
    state->guest_ipv4 = ip;
    state->guest_ipv4_known = true;
}

bool ShouldAcceptPcapRxFrame(WifiBridgeState* state, const uint8_t* data,
                             int len) {
    if (!state || !data || len < 14) return false;

    std::array<uint8_t, 6> guest_mac{};
    bool guest_mac_known = false;
    uint32_t guest_ipv4 = 0;
    bool guest_ipv4_known = false;
    {
        std::lock_guard<std::mutex> lock(state->guest_mac_mutex);
        guest_mac = state->guest_mac;
        guest_mac_known = state->guest_mac_known;
        guest_ipv4 = state->guest_ipv4;
        guest_ipv4_known = state->guest_ipv4_known;
    }

    const uint8_t* dst = data;
    const uint8_t* src = data + 6u;
    if (guest_mac_known && MacEqual(src, guest_mac)) return false;
    if (guest_mac_known && MacEqual(dst, guest_mac)) {
        return IsPcapGuestUnicastFrame(data, len, guest_mac, guest_mac_known,
                                       guest_ipv4, guest_ipv4_known);
    }
    if (IsBroadcastMac(dst)) {
        return IsPcapRequiredBroadcastFrame(
            data, len, guest_mac, guest_mac_known,
            guest_ipv4, guest_ipv4_known);
    }
    if (dst[0] & 1u) return false;
    return false;
}
#endif

// The worker thread's whole life: drain outbound packets to the driver,
// poll host sockets (non-blocking), wait briefly, repeat. Every access to
// state->slirp_driver here is safe by construction: this function is the
// ONLY caller of SendPacket()/PollHostSockets() on the concrete driver
// (see the class comments), and it never runs before the driver is
// constructed (the thread is created after slirp_driver is set, in
// nds_wifi3d_attach()) nor after it is destroyed (StopWorker() joins this
// thread before WifiBridgeState's members, including the driver, are torn
// down).
void WifiWorkerThreadMain(WifiBridgeState* state) {
    melonDS::Platform::Log(melonDS::Platform::Info,
        "[wifi_net] host network worker thread started\n");
    std::vector<uint8_t> pkt;
    while (!state->worker_stop.load(std::memory_order_relaxed)) {
        while (state->tx_queue.TryPop(&pkt)) {
            if (state->live_driver)
                state->live_driver->SendPacket(
                    pkt.data(), static_cast<int>(pkt.size()));
        }
        if (state->slirp_driver)
            state->slirp_driver->PollHostSockets();
        else if (state->live_driver)
            state->live_driver->RecvCheck();
        state->local_wfc_peer.DrainTo(&state->rx_queue,
                                      &state->rx_dropped_since_last_report);

        std::unique_lock<std::mutex> lock(state->wake_mutex);
        state->wake_cv.wait_for(
            lock, std::chrono::milliseconds(2),
            [state] {
                return state->wake_pending ||
                       state->worker_stop.load(std::memory_order_relaxed);
            });
        state->wake_pending = false;
    }
    melonDS::Platform::Log(melonDS::Platform::Info,
        "[wifi_net] host network worker thread loop exiting\n");
}

}  // namespace

namespace melonDS::Platform {

int Net_SendPacket(u8* data, int len, void* userdata) {
    (void)userdata;
    if (!g_bridge) return 0;
    if (len <= 0) return 0;
    // network.enabled=false: no backend was attached at all (see
    // nds_wifi3d_attach()) -- absorb the packet silently rather than
    // queueing it for a worker thread that will never exist to drain it.
    // Wiimmfi M8: a replay backend attaches replay_driver instead of a live
    // backend, so "no backend at all" must check both.
    if (!g_bridge->live_driver && !g_bridge->replay_driver) return len;
    if (static_cast<size_t>(len) > kWifiNetMaxPacketBytes) {
        // Net_Slirp::SendPacket would reject this anyway; fail fast on
        // the emulation thread instead of spending queue capacity on a
        // packet the worker thread would just drop.
        net_ring_push(NDS_NET_EVENT_BACKEND_DROP, /*direction=*/0, 0, 0,
                      nullptr, nullptr, 0, 0, 0, 0,
                      static_cast<uint16_t>(len), /*aux=*/2u);
        return 0;
    }
    LearnGuestMacFromTx(g_bridge.get(), data, len);
#if defined(NDS_ENABLE_PCAP_BACKEND)
    if (g_network_config.backend == NdsNetBackendKind::Pcap)
        LearnGuestIpv4FromTx(g_bridge.get(), data, len);
#endif
    // Passive protocol classification (Wiimmfi M3 task 1): read-only decode
    // of the exact bytes about to be queued -- never alters what gets sent.
    // See net_classify.h for the full design note. This is the guest->host
    // (TX/egress) side of the bridge/backend boundary named in the task.
    net_classify_ethernet_frame(data, static_cast<size_t>(len), /*direction=*/0);

    // Wiimmfi M8: live capture, independent of which backend is active.
    if (g_bridge->capture_writer.is_open()) {
        g_bridge->capture_writer.Write(scheduler_system_timestamp(),
                                        kNdsNetCaptureDirTx, data,
                                        static_cast<size_t>(len));
    }

    if (g_bridge->local_wfc_peer.ForwardIfPeerFrame(data, len))
        return len;

    if (g_bridge->replay_driver) {
        // Synchronous, in-memory comparison -- no host I/O, no queue, no
        // worker thread involved, so calling straight through on the
        // emulation thread is exactly as safe as calling any other pure
        // function here. See net_replay.h's class comment for the full
        // calling contract.
        g_bridge->replay_driver->SetCurrentCycle(scheduler_system_timestamp());
        g_bridge->replay_driver->SetCurrentPCs(scheduler_cpu_state(0).R[15],
                                                scheduler_cpu_state(1).R[15]);
        g_bridge->replay_driver->SendPacket(data, len);
        return len;
    }

    std::vector<uint8_t> pkt(data, data + len);
    if (!g_bridge->tx_queue.TryPush(std::move(pkt))) {
        // Detected on the emulation thread (this function's caller), so
        // the ring write is safe here directly -- see the design comment
        // above.
        net_ring_push(NDS_NET_EVENT_BACKEND_DROP, /*direction=*/0, 0, 0,
                      nullptr, nullptr, 0, 0, 0, 0,
                      static_cast<uint16_t>(len), /*aux=*/0u);
        return 0;
    }
    {
        std::lock_guard<std::mutex> lock(g_bridge->wake_mutex);
        g_bridge->wake_pending = true;
    }
    g_bridge->wake_cv.notify_all();
    return len;
}

int Net_RecvPacket(u8* data, void* userdata) {
    (void)userdata;
    if (!g_bridge) return 0;

    // Wiimmfi M8: give the replay driver the current guest cycle/PCs
    // before net.RecvPacket() below reaches its RecvCheck() -- see
    // net_replay.h's class comment for why this must happen on every call,
    // not just the first.
    if (g_bridge->replay_driver) {
        g_bridge->replay_driver->SetCurrentCycle(scheduler_system_timestamp());
        g_bridge->replay_driver->SetCurrentPCs(scheduler_cpu_state(0).R[15],
                                                scheduler_cpu_state(1).R[15]);
    }

    // The only place host-arrived packets become guest-visible: drain
    // every packet the worker thread queued since the last guest RX tick
    // into the vendored per-instance dispatch queue (Net::RXEnqueue --
    // the same entry point real melonDS's own frontend uses for this
    // exact callback, src/frontend/qt_sdl/main.cpp). This function is
    // reached only from Wifi::CheckRX(0)'s guest-cycle-scheduled poll
    // (via WifiAP::RecvPacket), so this drain is inherently quantized to
    // that tick, never to host wall-clock. (In replay mode, rx_queue is
    // never pushed to at all -- see nds_wifi3d_attach() -- so this loop is
    // a harmless no-op there; the replay driver's own due-frame delivery
    // happens inside net.RecvPacket() below, via RecvCheck().)
    std::vector<uint8_t> pkt;
    while (g_bridge->rx_queue.TryPop(&pkt)) {
        g_bridge->net.RXEnqueue(pkt.data(), static_cast<int>(pkt.size()));
    }

    // Surface any worker-thread rx_queue drops here -- net_ring_push must
    // run on the emulation thread (see the design comment above); this is
    // that rendezvous point.
    const uint32_t dropped = g_bridge->rx_dropped_since_last_report.exchange(
        0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < dropped; ++i) {
        net_ring_push(NDS_NET_EVENT_BACKEND_DROP, /*direction=*/1, 0, 0,
                      nullptr, nullptr, 0, 0, 0, 0, 0, /*aux=*/1u);
    }
    const uint32_t oversized =
        g_bridge->pcap_rx_oversize_since_last_report.exchange(
            0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < oversized; ++i) {
        net_ring_push(NDS_NET_EVENT_BACKEND_DROP, /*direction=*/1, 0, 0,
                      nullptr, nullptr, 0, 0, 0, 0,
                      static_cast<uint16_t>(kWifiNetMaxPacketBytes),
                      /*aux=*/3u);
    }

    // ndsrecomp: PollHostSockets() (worker thread, see Net_Slirp.h's
    // TakePollErrorCount() doc comment) records genuine host-socket
    // poll() failures via an atomic counter rather than silently
    // discarding them -- the return value used to feed straight into
    // slirp_pollfds_poll's boolean `error` flag and nowhere else.
    // Surface it here, the same rendezvous point already used for
    // rx_queue drops above, and for the identical reason (net_ring_push
    // must run on the emulation thread).
    if (g_bridge->slirp_driver) {
        const uint32_t poll_errors =
            g_bridge->slirp_driver->TakePollErrorCount();
        if (poll_errors) {
            net_ring_push(NDS_NET_EVENT_BACKEND_ERROR, /*direction=*/1, 0,
                          static_cast<uint32_t>(
                              g_bridge->slirp_driver->LastPollError()),
                          nullptr, nullptr, 0, 0, 0, 0, 0,
                          /*aux=*/poll_errors);
        }
    }

    const int recv_len = g_bridge->net.RecvPacket(data, kNetInstance);
    // Passive protocol classification (Wiimmfi M3 task 1), symmetric to the
    // TX hook above: read-only decode of the exact bytes just delivered to
    // the guest, after RecvPacket has already filled `data` -- this is the
    // host->guest (RX/ingress) side of the bridge/backend boundary.
    if (recv_len > 0) {
        net_classify_ethernet_frame(data, static_cast<size_t>(recv_len),
                                    /*direction=*/1);
        // Wiimmfi M8: live capture, independent of which backend is active.
        if (g_bridge->capture_writer.is_open()) {
            g_bridge->capture_writer.Write(scheduler_system_timestamp(),
                                            kNdsNetCaptureDirRx, data,
                                            static_cast<size_t>(recv_len));
        }
    }
    return recv_len;
}

void MP_Begin(void* userdata) {
    (void)userdata;
    if (g_bridge) g_bridge->local_mp.Begin();
}

void MP_End(void* userdata) {
    (void)userdata;
    if (g_bridge) g_bridge->local_mp.End();
}

int MP_SendPacket(u8* data, int len, u64 timestamp, void* userdata) {
    (void)userdata;
    if (!g_bridge) return 0;
    return g_bridge->local_mp.SendPacket(data, len, timestamp);
}

int MP_RecvPacket(u8* data, u64* timestamp, void* userdata) {
    (void)userdata;
    if (!g_bridge) return 0;
    return g_bridge->local_mp.RecvPacket(data, timestamp);
}

int MP_SendCmd(u8* data, int len, u64 timestamp, void* userdata) {
    (void)userdata;
    if (!g_bridge) return 0;
    return g_bridge->local_mp.SendCmd(data, len, timestamp);
}

int MP_SendReply(u8* data, int len, u64 timestamp, u16 aid,
                 void* userdata) {
    (void)userdata;
    if (!g_bridge) return 0;
    return g_bridge->local_mp.SendReply(data, len, timestamp, aid);
}

int MP_SendAck(u8* data, int len, u64 timestamp, void* userdata) {
    (void)userdata;
    if (!g_bridge) return 0;
    return g_bridge->local_mp.SendAck(data, len, timestamp);
}

int MP_RecvHostPacket(u8* data, u64* timestamp, void* userdata) {
    (void)userdata;
    if (!g_bridge) return 0;
    return g_bridge->local_mp.RecvHostPacket(data, timestamp);
}

u16 MP_RecvReplies(u8* data, u64 timestamp, u16 aidmask, void* userdata) {
    (void)userdata;
    if (!g_bridge) return 0;
    return g_bridge->local_mp.RecvReplies(data, timestamp, aidmask);
}

}  // namespace melonDS::Platform

// ── process-wide Winsock lifecycle ──────────────────────────────────────
// See wifi_net.h's declaration comment for the full rationale (fixes
// main.cpp's boot()-starts-a-Winsock-using-worker-thread-before-anyone-
// called-WSAStartup bug). WSAStartup is reference-counted by the OS, so
// this pair coexisting with debug_server.cpp's own pre-existing
// WSAStartup/WSACleanup calls is intentional and harmless -- each success
// increments a per-process count that the matching cleanup call
// decrements; Winsock itself only tears down once the count reaches zero.

bool nds_net_platform_init() {
#ifdef _WIN32
    WSADATA wsa;
    const int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        std::fprintf(stderr,
            "[wifi_net] WSAStartup failed (error %d) -- host networking is "
            "unavailable this run\n", rc);
        return false;
    }
    std::fprintf(stderr,
        "[wifi_net] Winsock initialized (WSAStartup 2.2) before boot()\n");
    return true;
#else
    return true;
#endif
}

void nds_net_platform_shutdown() {
#ifdef _WIN32
    WSACleanup();
#endif
}

// ── device-model attach seam ────────────────────────────────────────────

melonDS::Wifi* nds_wifi3d_attach() {
    if (g_bridge) return g_bridge->wifi.get();

    g_bridge = std::make_unique<WifiBridgeState>();
    g_bridge->network_state.attached = true;
    g_bridge->network_state.network_enabled = g_network_config.enabled;
    g_bridge->network_state.backend = g_network_config.backend;
    g_bridge->network_state.wfc_enabled = g_network_config.wfc_enabled;
    g_bridge->network_state.wfc_dns_ipv4 = g_network_config.wfc_dns_ipv4;
    g_bridge->network_state.pcap_adapter_requested =
        g_network_config.pcap_adapter;
    g_bridge->local_mp.Configure(
        g_network_config.local_wireless_enabled,
        g_network_config.local_wireless_instance,
        g_network_config.local_wireless_base_port);
    g_bridge->local_wfc_peer.Configure(
        g_network_config.slirp_virtual_network_instance);
    if (g_network_config.local_wireless_enabled) {
        melonDS::Platform::Log(melonDS::Platform::Info,
            "[local_mp] configured instance=%u base_port=%u\n",
            static_cast<unsigned>(g_network_config.local_wireless_instance),
            static_cast<unsigned>(g_network_config.local_wireless_base_port));
    }
    g_bridge->nds.SPI.SetFirmwareSource(nds_firmware_bytes(),
                                         nds_firmware_size());

    g_bridge->net.RegisterInstance(kNetInstance);
    g_bridge->net_instance_registered = true;

    // network.enabled=false (--network off): the guest-visible Wi-Fi
    // device model above is still fully constructed (the hardware exists
    // and the guest can still scan/associate with the virtual AP), but no
    // host networking backend is attached at all -- slirp_driver/
    // worker_thread stay null, and Net_SendPacket/Net_RecvPacket below
    // become unconditional no-ops the moment they see that. This is the
    // "no host networking backend" mode named in the task, distinct from
    // wfc_enabled=false (which just means "use upstream melonDS's own
    // local-getaddrinfo DNS synthesis instead of a configured WFC
    // provider" -- a fully-networked default).
    if (g_network_config.enabled) {
        // Wiimmfi M8: live capture, independent of which backend gets
        // attached below -- open it first so even the very first frame
        // either backend produces is captured.
        if (!g_network_config.capture_out_path.empty()) {
            const bool opened = g_bridge->capture_writer.Open(
                g_network_config.capture_out_path,
                g_network_config.capture_sanitize,
                g_network_config.capture_write_pcap,
                g_network_config.capture_scenario_tag,
                g_network_config.rom_sha1);
            melonDS::Platform::Log(
                opened ? melonDS::Platform::Info : melonDS::Platform::Error,
                "[wifi_net] capture-out %s '%s' (sanitize=%d, pcap=%d)\n",
                opened ? "opened" : "FAILED to open",
                g_network_config.capture_out_path.c_str(),
                g_network_config.capture_sanitize ? 1 : 0,
                g_network_config.capture_write_pcap ? 1 : 0);
        }

        if (g_network_config.backend == NdsNetBackendKind::Replay) {
            // Wiimmfi M8: host Internet is disabled entirely for this
            // backend -- no sockets, no libslirp, no worker thread (see
            // below). `replay_records` was already loaded and fully
            // validated by main.cpp's CLI handling (NdsNetCaptureReader::
            // ReadAll), so this constructor never itself parses a file.
            auto replay = std::make_unique<melonDS::NetReplay>(
                std::move(g_network_config.replay_records),
                [](const melonDS::u8* data, int len) noexcept {
                    if (!g_bridge || len <= 0) return;
                    // Synchronous: RecvCheck() (which invokes this
                    // callback) is itself only ever called from
                    // Net::RecvPacket on the emulation thread, at a guest
                    // RX tick -- see Net_RecvPacket above -- so enqueueing
                    // straight into the Dispatcher here needs no cross-
                    // thread queue at all (unlike the live slirp path's
                    // worker-thread callback, which DOES need the
                    // rx_queue hop below).
                    g_bridge->net.RXEnqueue(data, len);
                },
                g_network_config.replay_sanitized);
            g_bridge->replay_driver = replay.get();
            const uint64_t tx_total = g_bridge->replay_driver->TxTotalCount();
            const uint64_t rx_total = g_bridge->replay_driver->RxTotalCount();
            g_bridge->net.SetDriver(std::move(replay));
            melonDS::Platform::Log(melonDS::Platform::Info,
                "[wifi_net] network backend attached: REPLAY (%llu TX, "
                "%llu RX recorded frame(s); host networking fully "
                "disabled)\n",
                (unsigned long long)tx_total, (unsigned long long)rx_total);
        } else if (g_network_config.backend == NdsNetBackendKind::Pcap) {
#if defined(NDS_ENABLE_PCAP_BACKEND)
            auto pcap_lib = melonDS::LibPCap::New();
            if (!pcap_lib) {
                std::fprintf(stderr,
                    "[wifi_net] --network-backend pcap requested, but no "
                    "Npcap/WinPcap runtime library could be loaded\n");
                std::exit(2);
            }

            std::vector<melonDS::AdapterData> adapters =
                pcap_lib->GetAdapters();
            const melonDS::AdapterData* selected =
                ChoosePcapAdapter(adapters, g_network_config.pcap_adapter);
            if (!selected) {
                std::fprintf(stderr,
                    "[wifi_net] --network-backend pcap could not select an "
                    "adapter%s%s%s\n",
                    g_network_config.pcap_adapter.empty() ? "" :
                        " matching '",
                    g_network_config.pcap_adapter.empty() ? "" :
                        g_network_config.pcap_adapter.c_str(),
                    g_network_config.pcap_adapter.empty() ? "" : "'");
                LogPcapAdapters(adapters);
                std::exit(2);
            }

            auto pcap = pcap_lib->Open(*selected,
                [](const melonDS::u8* data, int len) noexcept {
                    if (!g_bridge || len <= 0) return;
                    if (!ShouldAcceptPcapRxFrame(g_bridge.get(), data, len))
                        return;
                    if (static_cast<size_t>(len) > kWifiNetMaxPacketBytes) {
                        g_bridge->pcap_rx_oversize_since_last_report.fetch_add(
                            1, std::memory_order_relaxed);
                        return;
                    }
                    std::vector<uint8_t> pkt(
                        data, data + static_cast<size_t>(len));
                    if (g_network_config.wfc_enabled &&
                        g_network_config.wfc_dns_ipv4 != 0u) {
                        RewriteDhcpDnsOption(&pkt,
                                             g_network_config.wfc_dns_ipv4);
                    }
                    if (!g_bridge->rx_queue.TryPush(std::move(pkt))) {
                        g_bridge->rx_dropped_since_last_report.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                });
            if (!pcap) {
                std::fprintf(stderr,
                    "[wifi_net] --network-backend pcap failed to open "
                    "adapter '%s'\n", selected->DeviceName);
                std::exit(2);
            }

            g_bridge->live_driver = pcap.get();
            g_bridge->net.SetDriver(std::move(pcap));
            g_bridge->network_state.live_backend_active = true;
            g_bridge->network_state.pcap_adapter_selected = true;
            g_bridge->network_state.pcap_device_name = selected->DeviceName;
            g_bridge->network_state.pcap_friendly_name =
                selected->FriendlyName;
            g_bridge->network_state.pcap_description =
                selected->Description;
            g_bridge->network_state.pcap_ipv4 =
                PcapAdapterIpv4String(*selected);
            melonDS::Platform::Log(melonDS::Platform::Info,
                "[wifi_net] network backend attached: PCAP device='%s' "
                "friendly='%s' ipv4=%s%s\n",
                selected->DeviceName, selected->FriendlyName,
                PcapAdapterIpv4String(*selected).c_str(),
                (g_network_config.wfc_enabled &&
                 g_network_config.wfc_dns_ipv4 != 0u)
                    ? " dhcp_dns_override=yes"
                    : " dhcp_dns_override=no");
#else
            std::fprintf(stderr,
                "[wifi_net] internal error: pcap backend selected in a build "
                "without NDS_ENABLE_PCAP_BACKEND\n");
            std::exit(2);
#endif
        } else {
            // Wiimmfi M4: when a WFC provider is configured, this overrides
            // the DHCP-advertised DNS server address so the guest's own DNS
            // queries are genuinely NAT-forwarded to that real address
            // instead of answered locally -- see the constructor's doc
            // comment in Net_Slirp.h and
            // patches/0006-net-slirp-configurable-nameserver.patch. 0 (the
            // default, wfc_enabled=false) preserves upstream's own behavior
            // exactly, which is how "ordinary DNS" is proven end-to-end
            // without this project's provider config at all.
            const melonDS::u32 nameserver_override =
                (g_network_config.wfc_enabled && g_network_config.wfc_dns_ipv4)
                    ? static_cast<melonDS::u32>(g_network_config.wfc_dns_ipv4)
                    : 0u;
            const melonDS::u32 virtual_subnet =
                0x0A400000u |
                ((g_network_config.slirp_virtual_network_instance & 0xFFu)
                 << 8u);

            // The libslirp send_packet callback: fires only from inside
            // Net_Slirp::PollHostSockets() or Net_Slirp::SendPacket()'s DNS
            // synthesis (HandleDNSFrame) -- both exclusively worker-thread
            // calls once the thread below is running (and, transiently during
            // this very construction, only from this thread, before the
            // worker starts). Never touches guest state directly: it only
            // ever hands the packet to the bounded rx_queue, which the
            // emulation thread later drains in Net_RecvPacket above.
            auto slirp = std::make_unique<melonDS::Net_Slirp>(
                [](const melonDS::u8* data, int len) noexcept {
                    if (!g_bridge || len <= 0) return;
                    std::vector<uint8_t> pkt(
                        data, data + static_cast<size_t>(len));
                    if (!g_bridge->rx_queue.TryPush(std::move(pkt))) {
                        g_bridge->rx_dropped_since_last_report.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                },
                nameserver_override,
                virtual_subnet);
            g_bridge->slirp_driver = slirp.get();
            g_bridge->live_driver = slirp.get();
            g_bridge->net.SetDriver(std::move(slirp));
            g_bridge->network_state.live_backend_active = true;
            g_bridge->local_wfc_peer.Begin();

            melonDS::Platform::Log(melonDS::Platform::Info,
                "[wifi_net] network backend attached (wfc_enabled=%d, "
                "nameserver_override=%s, slirp_subnet=10.64.%u.0/24)\n",
                g_network_config.wfc_enabled ? 1 : 0,
                nameserver_override ? "yes" : "no (upstream default)",
                static_cast<unsigned>(
                    g_network_config.slirp_virtual_network_instance & 0xFFu));
        }
    } else {
        melonDS::Platform::Log(melonDS::Platform::Info,
            "[wifi_net] network disabled (--network off): Wi-Fi device is "
            "live but no host networking backend is attached\n");
    }

    g_bridge->wifi = std::make_unique<melonDS::Wifi>(g_bridge->nds);

    // Start the host networking worker thread last, once live_driver and
    // wifi are both fully constructed -- std::thread's own construction
    // establishes a happens-before edge, so the new thread is guaranteed
    // to observe every write above. Skipped entirely when no backend was
    // attached above, AND skipped for the replay backend (Wiimmfi M8):
    // replay never touches a host socket or adapter, so there is nothing
    // for a background thread to poll -- see net_replay.h's class comment.
    if (g_bridge->live_driver) {
        g_bridge->worker_thread = melonDS::Platform::Thread_Create(
            [state = g_bridge.get()] { WifiWorkerThreadMain(state); });
        g_bridge->network_state.worker_active =
            g_bridge->worker_thread != nullptr;
    }

    return g_bridge->wifi.get();
}

bool nds_wifi_network_state(NdsWifiNetworkState* out) {
    if (!out || !g_bridge) return false;
    *out = g_bridge->network_state;
    out->live_backend_active = g_bridge->live_driver != nullptr;
    out->replay_backend_active = g_bridge->replay_driver != nullptr;
    out->worker_active = g_bridge->worker_thread != nullptr;
    return true;
}

bool nds_wifi_replay_status(NdsNetReplayStatus* out) {
    if (!out || !g_bridge || !g_bridge->replay_driver) return false;
    melonDS::NetReplay* r = g_bridge->replay_driver;
    out->active = true;
    out->mismatch = r->HasMismatch();
    out->tx_matched = r->TxMatchedCount();
    out->tx_total = r->TxTotalCount();
    out->rx_delivered = r->RxDeliveredCount();
    out->rx_total = r->RxTotalCount();
    if (out->mismatch) {
        const melonDS::NdsNetReplayMismatch& m = r->Mismatch();
        out->mismatch_tx_frame_index = m.tx_frame_index;
        out->mismatch_guest_cycle = m.guest_cycle;
        out->mismatch_arm9_pc = m.arm9_pc;
        out->mismatch_arm7_pc = m.arm7_pc;
        out->mismatch_reason = m.reason;
    }
    return true;
}

void nds_wifi_configure_network(const NdsWifiNetworkConfig& config) {
    g_network_config = config;
}

bool nds_wifi_local_mp_stats(NdsLocalMpStats* out) {
    if (!g_bridge) return false;
    g_bridge->local_mp.SnapshotStats(out);
    return true;
}

void nds_wifi3d_detach() {
    if (!g_bridge) return;
    if (g_bridge->net_instance_registered)
        g_bridge->net.UnregisterInstance(kNetInstance);
    // ~WifiBridgeState() stops and joins the worker thread (StopWorker())
    // before net/wifi -- and therefore the Net_Slirp driver and its
    // libslirp Ctx -- are destroyed. See the design comment above.
    g_bridge.reset();
}

// ── bus-facing implementation of wifi.h (retired wifi.cpp's role) ──────

namespace {

constexpr uint32_t kWifiBase = 0x04800000u;
constexpr uint32_t kWifiEnd  = 0x04810000u;

// Wi-Fi is ARM7-only hardware (nds_wifi_address below gates cpu==7), so
// every path that reaches the vendored Wifi object through a bus access is
// driven by the live ARM7 timeline. Mirrors the retired wifi.cpp's
// active_system_timestamp(): g_runtime_cycles is the CURRENTLY LOADED
// CPU's live, mid-slice cycle counter (scheduler.cpp's run_slice), already
// in system(1x)/ARM7-clock units when that CPU is ARM7, or in the 2x/ARM9
// domain (needing >>1) when it is ARM9 -- defensive only, since the
// cpu==7 gate above every call site here means the ARM9 branch should
// never actually execute in practice.
uint64_t wifi_current_system_timestamp() {
    return g_nds_active == NDS_ARM9 ? (g_runtime_cycles >> 1u) : g_runtime_cycles;
}

// Register-access ring hooks live here, in the bridge, rather than as a
// vendored patch to Wifi::Read/Write: nds_wifi_read/write (below) are
// ALREADY the single funnel every bus-level Wi-Fi access passes through
// (bus.cpp calls nothing else), so wrapping the call here captures every
// access uniformly, with the real before/after register value, without
// touching vendored source at all. Direction convention: 0 = guest->host
// (the guest is driving/writing the device, analogous to TX/egress), 1 =
// host->guest (the guest is observing/reading the device, analogous to
// RX/ingress) -- see the comment on NdsNetTraceEntry::direction.
uint16_t wifi_reg_read16(melonDS::Wifi* wifi, uint32_t addr) {
    g_bridge->nds.CurrentSystemTimestamp = wifi_current_system_timestamp();
    const uint16_t value = wifi->Read(addr);
    net_ring_push(NDS_NET_EVENT_WIFI_REG_READ, /*direction=*/1,
                  static_cast<uint16_t>(addr & 0xFFFFu), value,
                  nullptr, nullptr, 0, 0, 0, 0, 0, 0);
    return value;
}

void wifi_reg_write16(melonDS::Wifi* wifi, uint32_t addr, uint16_t value) {
    g_bridge->nds.CurrentSystemTimestamp = wifi_current_system_timestamp();
    net_ring_push(NDS_NET_EVENT_WIFI_REG_WRITE, /*direction=*/0,
                  static_cast<uint16_t>(addr & 0xFFFFu), value,
                  nullptr, nullptr, 0, 0, 0, 0, 0, 0);
    wifi->Write(addr, value);
}

}  // namespace

bool nds_wifi_address(int cpu, uint32_t addr) {
    return cpu == 7 && addr >= kWifiBase && addr < kWifiEnd;
}

void nds_wifi_reset() {
    melonDS::Wifi* wifi = nds_wifi3d_attach();
    if (wifi) wifi->Reset();
}

void nds_wifi_load_firmware(const uint8_t* data, uint32_t size) {
    // Boot order (main.cpp:681-682): nds_io_reset() -- which calls
    // nds_wifi_reset() above -- runs BEFORE nds_io_load_firmware()
    // populates the firmware buffer. Real melonDS's Wifi::Reset() reads RF
    // chip type and the per-channel RF calibration table straight out of
    // an ALREADY-loaded firmware image (NDS.SPI.GetFirmware(),
    // Wifi.cpp:147-186); our first Reset() above therefore ran against an
    // empty view (SPI.h's defensive all-zero/DS-default fallback, not a
    // crash, but not the real calibration data either). Rebind the SPI
    // firmware view to the fresh buffer here and re-Reset() so the
    // guest-visible state once firmware/BIOS code starts executing
    // reflects the real firmware image. This is safe: nothing has touched
    // a Wi-Fi register yet at this point in boot (the BIOS/firmware
    // haven't started running), so a second full Reset() is not
    // observable as a guest-visible glitch -- it is strictly more correct
    // than the single early Reset() alone.
    melonDS::Wifi* wifi = nds_wifi3d_attach();
    if (!g_bridge) return;
    g_bridge->nds.SPI.SetFirmwareSource(data, size);
    if (wifi) wifi->Reset();
}

void nds_wifi_rebind_firmware(const uint8_t* data, uint32_t size) {
    if (!g_bridge) return;
    g_bridge->nds.SPI.SetFirmwareSource(data, size);
}

void nds_wifi_set_power_control(bool enabled, uint64_t timestamp) {
    melonDS::Wifi* wifi = nds_wifi3d_attach();
    if (!wifi || !g_bridge) return;
    g_bridge->nds.CurrentSystemTimestamp = timestamp;
    // Wifi::SetPowerCnt(u32 val) reads the raw POWCNT2 bit (val & (1<<1));
    // io.cpp's caller has already reduced the register to that one bit
    // (nds_wifi_set_power_control's `enabled` parameter), so synthesize a
    // value carrying just that bit rather than changing this function's
    // long-lived signature.
    wifi->SetPowerCnt(enabled ? 0x0002u : 0u);
}

uint64_t nds_wifi_next_event_time() {
    if (!g_bridge) return UINT64_MAX;
    return g_bridge->nds.HasPendingEvent() ? g_bridge->nds.PendingEventTime()
                                            : UINT64_MAX;
}

void nds_wifi_run_events(uint64_t timestamp) {
    if (!g_bridge || !g_bridge->wifi) return;
    g_bridge->nds.CurrentSystemTimestamp = timestamp;
    // Drains every event due at or before this guest-cycle rendezvous,
    // exactly mirroring the retired wifi.cpp's
    // `while (g_power_on && g_timer_deadline <= timestamp)` loop: Wifi's
    // own USTimer reschedules itself periodically (Wifi.cpp:1934,
    // ScheduleTimer(false)) every call, and UpdatePowerOn's CancelEvent
    // naturally empties HasPendingEvent() when the device powers off, so
    // no separate "is it powered on" gate is needed here -- the schedule
    // state already encodes it.
    while (g_bridge->nds.HasPendingEvent() &&
           g_bridge->nds.PendingEventTime() <= timestamp) {
        g_bridge->nds.RunPendingEvent();
    }
}

uint16_t nds_wifi_debug_if() {
    // W_IF (0x010) has no read-side special case in Wifi::Read -- it falls
    // straight through to the generic `return IOPORT(addr&0xFFF);` at the
    // end, so calling Read() directly here is side-effect-free. This is a
    // diagnostic accessor for the IRQ trace ring (io.cpp's
    // nds_note_irq_accept), not a bus transaction, so it intentionally
    // bypasses nds_wifi_read's ARM7/POWCNT2 gating and does not push a
    // net_ring event -- logging it there would conflate driver-visible
    // register traffic with an internal trace-annotation read.
    if (!g_bridge || !g_bridge->wifi) return 0u;
    return g_bridge->wifi->Read(melonDS::Wifi::W_IF);
}

uint16_t nds_wifi_debug_ie() {
    if (!g_bridge || !g_bridge->wifi) return 0u;
    return g_bridge->wifi->Read(melonDS::Wifi::W_IE);
}

uint32_t nds_wifi_read(uint32_t addr, uint32_t width, bool powered) {
    // Bus-level semantics preserved exactly as documented in wifi.h and
    // implemented by the retired wifi.cpp: the ARM7-only aperture and the
    // POWCNT2 gate are enforced HERE, not inside the vendored
    // Wifi::Read/Write -- melonDS's own NDS.cpp enforces them at its
    // ARM7Read8/16/32 call sites (case 0x04800000), not inside the Wifi
    // class itself, so the bridge must keep doing exactly that.
    if (!powered || addr < kWifiBase || addr >= kWifiEnd) return 0u;
    melonDS::Wifi* wifi = nds_wifi3d_attach();
    if (!wifi) return 0u;
    if (width == 1u) {
        // The device is 16-bit; a byte read selects a byte out of the
        // aligned halfword the device actually returns.
        const uint16_t value = wifi_reg_read16(wifi, addr & ~1u);
        return (value >> ((addr & 1u) * 8u)) & 0xFFu;
    }
    if (width == 2u) return wifi_reg_read16(wifi, addr);
    if (width == 4u)
        // 32-bit accesses split into two halfwords, low word first.
        return static_cast<uint32_t>(wifi_reg_read16(wifi, addr)) |
               (static_cast<uint32_t>(wifi_reg_read16(wifi, addr + 2u)) << 16u);
    return 0u;
}

void nds_wifi_write(uint32_t addr, uint32_t value, uint32_t width, bool powered) {
    if (!powered || addr < kWifiBase || addr >= kWifiEnd) return;
    melonDS::Wifi* wifi = nds_wifi3d_attach();
    if (!wifi) return;
    if (width == 2u) {
        wifi_reg_write16(wifi, addr, static_cast<uint16_t>(value));
    } else if (width == 4u) {
        wifi_reg_write16(wifi, addr, static_cast<uint16_t>(value));
        wifi_reg_write16(wifi, addr + 2u, static_cast<uint16_t>(value >> 16u));
    }
    // ARM7 byte writes do not route to Wifi::Write in melonDS/NDS.cpp.
}
