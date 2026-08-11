/*
    Copyright 2016-2024 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef NET_SLIRP_H
#define NET_SLIRP_H

#include "types.h"
#include "FIFO.h"
#include "Platform.h"
#include "NetDriver.h"

#include <atomic>

#include <libslirp.h>

#ifdef __WIN32__
    #include <ws2tcpip.h>
#else
    #include <poll.h>
#endif

struct Slirp;

namespace melonDS
{
class Net_Slirp : public NetDriver
{
public:
    // ndsrecomp: `nameserver_ipv4_host_order` (host byte order) optionally
    // overrides the DNS server address slirp hands the guest via DHCP
    // option 6. 0 (the default -- every pre-existing call site) preserves
    // upstream melonDS's own behavior exactly: the internal fake address
    // `kDNSIP`, whose DNS queries SendPacket() intercepts and answers
    // locally via HandleDNSFrame()'s getaddrinfo() synthesis, never
    // touching a real socket. A nonzero override is used for the
    // project's configurable WFC DNS provider (runner/src/wifi_net.cpp,
    // NdsNetworkOptions): once the guest is told to use a REAL external
    // address instead of the fake `kDNSIP`, SendPacket()'s
    // `dst == kDNSIP` special case no longer matches, so those DNS
    // packets fall through to the normal `slirp_input()` NAT path and are
    // genuinely forwarded over the host's network to that address --
    // exactly how a physical DS reaches a configured WFC DNS server. See
    // runner/vendor/melonds/patches/0006-net-slirp-configurable-nameserver.patch.
    explicit Net_Slirp(const Platform::SendPacketCallback& callback,
                        u32 nameserver_ipv4_host_order = 0) noexcept;
    Net_Slirp(const Net_Slirp&) = delete;
    Net_Slirp& operator=(const Net_Slirp&) = delete;
    Net_Slirp(Net_Slirp&& other) noexcept;
    Net_Slirp& operator=(Net_Slirp&& other) noexcept;
    ~Net_Slirp() noexcept override;

    int SendPacket(u8* data, int len) noexcept override;
    void RecvCheck() noexcept override;

    // ndsrecomp: the real host-socket polling logic, split out of
    // RecvCheck() (see Net_Slirp.cpp) so it can run on a dedicated worker
    // thread instead of whatever thread calls Net::RecvPacket (always the
    // emulation thread in this build). Not part of the NetDriver interface
    // -- callers need a concrete Net_Slirp* to reach it. Contract: must be
    // invoked from exactly one thread, and never concurrently with
    // SendPacket() on this same instance (both touch Ctx). See
    // runner/src/wifi_net.cpp's worker thread, the only caller, which also
    // serializes SendPacket() through the same thread so this contract
    // holds trivially. See
    // runner/vendor/melonds/patches/0005-net-slirp-worker-thread-poll.patch.
    void PollHostSockets() noexcept;

    // ndsrecomp: PollHostSockets() used to pass poll()'s return value
    // straight into slirp_pollfds_poll as a bare `res<0` bool and never
    // otherwise look at it -- a genuine host-socket poll() failure (e.g.
    // EBADF/EINTR on POSIX, WSAENOTSOCK on Windows) was therefore
    // completely invisible, both to a human reading logs and to this
    // project's own always-on network observability ring (net_ring.h).
    // TakePollErrorCount()/LastPollError() surface it: PollHostSockets()
    // (the dedicated worker thread, see that method's own comment)
    // increments the counter and records the OS error code on every
    // poll() < 0; the emulation thread (Net_RecvPacket,
    // runner/src/wifi_net.cpp) drains both once per RX tick and pushes
    // NDS_NET_EVENT_BACKEND_ERROR -- the SAME atomic-counter-handoff
    // idiom wifi_net.cpp already uses for rx_queue drops
    // (rx_dropped_since_last_report), and for the identical reason:
    // net_ring_push is not thread-safe by design and must only ever be
    // called from the emulation thread, never from this worker thread.
    uint32_t TakePollErrorCount() noexcept;
    int LastPollError() const noexcept;
private:
    static constexpr int PollListMax = 64;
    static const SlirpCb cb;
    static int SlirpCbGetREvents(int idx, void* opaque) noexcept;
    static int SlirpCbAddPoll(int fd, int events, void* opaque) noexcept;
    static ssize_t SlirpCbSendPacket(const void* buf, size_t len, void* opaque) noexcept;
    void HandleDNSFrame(u8* data, int len) noexcept;

    Platform::SendPacketCallback Callback;
    pollfd PollList[PollListMax] {};
    int PollListSize = 0;
    FIFO<u32, (0x8000 >> 2)> RXBuffer {};
    u32 IPv4ID = 0;
    Slirp* Ctx = nullptr;
    std::atomic<uint32_t> PollErrorCount{0};
    std::atomic<int> LastPollErrorCode{0};
};
}
#endif // NET_SLIRP_H
