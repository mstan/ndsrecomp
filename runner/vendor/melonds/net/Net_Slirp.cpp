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

#include <stdio.h>
#include <string.h>
#include <cerrno>
#include "Net.h"
#include "Net_Slirp.h"
#include "FIFO.h"
#include "Platform.h"

#include <libslirp.h>

#ifdef __WIN32__
	#include <ws2tcpip.h>
#else
	#include <sys/socket.h>
	#include <netdb.h>
	#include <poll.h>
	#include <time.h>
#endif

namespace melonDS
{

using Platform::Log;
using Platform::LogLevel;

const u32 kDefaultSubnet = 0x0A400000;
const u32 kHostOffset = 0x01;
const u32 kDNSOffset = 0x02;
const u32 kClientOffset = 0x10;

const u8 kServerMAC[6] = {0x00, 0xAB, 0x33, 0x28, 0x99, 0x44};

#ifdef __WIN32__

#define poll WSAPoll

// https://stackoverflow.com/questions/5404277/porting-clock-gettime-to-windows

struct timespec { long tv_sec; long tv_nsec; };
#define CLOCK_MONOTONIC 1312

int clock_gettime(int, struct timespec *spec)
{
    __int64 wintime;
    GetSystemTimeAsFileTime((FILETIME*)&wintime);
    wintime -=116444736000000000LL;                 //1jan1601 to 1jan1970
    spec->tv_sec  = wintime / 10000000LL;           //seconds
    spec->tv_nsec = wintime % 10000000LL * 100;     //nano-seconds
    return 0;
}

#endif // __WIN32__


ssize_t Net_Slirp::SlirpCbSendPacket(const void* buf, size_t len, void* opaque) noexcept
{
    if (len > 2048)
    {
        Log(LogLevel::Warn, "slirp: packet too big (%zu)\n", len);
        return 0;
    }

    Log(LogLevel::Debug, "slirp: response packet of %zu bytes, type %04X\n", len, ntohs(((u16*)buf)[6]));

    Net_Slirp& self = *static_cast<Net_Slirp*>(opaque);
    if (self.Callback)
    {
        self.Callback((const u8*)buf, len);
    }

    return len;
}

void SlirpCbGuestError(const char* msg, void* opaque)
{
    Log(LogLevel::Error, "SLIRP: error: %s\n", msg);
}

int64_t SlirpCbClockGetNS(void* opaque)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void* SlirpCbTimerNew(SlirpTimerCb cb, void* cb_opaque, void* opaque)
{
    return nullptr;
}

void SlirpCbTimerFree(void* timer, void* opaque)
{
}

void SlirpCbTimerMod(void* timer, int64_t expire_time, void* opaque)
{
}

void SlirpCbRegisterPollFD(int fd, void* opaque)
{
    Log(LogLevel::Debug, "Slirp: register poll FD %d\n", fd);
}

void SlirpCbUnregisterPollFD(int fd, void* opaque)
{
    Log(LogLevel::Debug, "Slirp: unregister poll FD %d\n", fd);
}

void SlirpCbNotify(void* opaque)
{
    Log(LogLevel::Debug, "Slirp: notify???\n");
}

const SlirpCb Net_Slirp::cb =
{
    .send_packet = SlirpCbSendPacket,
    .guest_error = SlirpCbGuestError,
    .clock_get_ns = SlirpCbClockGetNS,
    .timer_new = SlirpCbTimerNew,
    .timer_free = SlirpCbTimerFree,
    .timer_mod = SlirpCbTimerMod,
    .register_poll_fd = SlirpCbRegisterPollFD,
    .unregister_poll_fd = SlirpCbUnregisterPollFD,
    .notify = SlirpCbNotify
};

Net_Slirp::Net_Slirp(const Platform::SendPacketCallback& callback,
                     u32 nameserver_ipv4_host_order,
                     u32 virtual_subnet_ipv4_host_order) noexcept : Callback(callback)
{
    SlirpConfig cfg {};
    memset(&cfg, 0, sizeof(cfg));
    // ndsrecomp: version bumped 1 -> 3 so libslirp actually honors
    // `disable_dns` below (libslirp.h: "Fields introduced in SlirpConfig
    // version 3 begin" covers disable_dns; a lower version number tells
    // slirp to ignore fields past that version's boundary regardless of
    // what's stored in them). Every version-2 field (outbound_addr/
    // outbound_addr6) stays null/default via the memset above, so this is
    // a pure additive capability bump, not a behavior change on its own.
    cfg.version = 3;

    const u32 subnet =
        virtual_subnet_ipv4_host_order
            ? (virtual_subnet_ipv4_host_order & 0xFFFFFF00u)
            : kDefaultSubnet;
    const u32 server_ip = subnet | kHostOffset;
    const u32 dns_ip = subnet | kDNSOffset;
    const u32 client_ip = subnet | kClientOffset;
    InternalDNSIP = dns_ip;

    cfg.in_enabled = true;
    *(u32*)&cfg.vnetwork = htonl(subnet);
    *(u32*)&cfg.vnetmask = htonl(0xFFFFFF00);
    *(u32*)&cfg.vhost = htonl(server_ip);
    cfg.vhostname = "melonServer";
    *(u32*)&cfg.vdhcp_start = htonl(client_ip);
    // ndsrecomp: configurable DHCP-advertised nameserver -- see the
    // constructor declaration comment in Net_Slirp.h and
    // patches/0006-net-slirp-configurable-nameserver.patch. 0 preserves
    // upstream's own kDNSIP (local getaddrinfo-backed DNS synthesis).
    *(u32*)&cfg.vnameserver =
        htonl(nameserver_ipv4_host_order ? nameserver_ipv4_host_order : dns_ip);
    // ndsrecomp: without this, libslirp's OWN built-in DNS proxy (separate
    // from and in addition to melonDS's HandleDNSFrame() interception
    // above SendPacket()) silently re-intercepts ANY UDP packet destined
    // to port 53 -- REGARDLESS of destination address -- and answers it
    // using the HOST's own configured resolver, discarding the packet's
    // real destination entirely. Confirmed by direct measurement: with
    // disable_dns left false, a guest DNS query sent to a configured WFC
    // provider (nameserver_ipv4_host_order override, a real external
    // address, NOT kDNSIP) never actually reached that address -- it came
    // back answered by whatever DNS server this host's own network stack
    // uses, with a visibly different (larger, multi-record) response than
    // querying the real provider address directly from this host. That
    // defeated the entire point of a configurable WFC DNS provider: the
    // guest was silently resolving through the host's ordinary internet
    // DNS no matter what nameserver we told it to use via DHCP. Setting
    // this unconditionally (not just when nameserver_ipv4_host_order != 0)
    // is safe for the unmodified default path too: when no override is
    // configured, HandleDNSFrame's own `dst == kDNSIP` check upstream in
    // SendPacket() already intercepts and answers the packet BEFORE it
    // would ever reach slirp_input()/this proxy layer, so disable_dns is
    // simply never consulted in that case.
    cfg.disable_dns = true;

    Ctx = slirp_new(&cfg, &cb, this);
}

Net_Slirp::~Net_Slirp() noexcept
{
    if (Ctx)
    {
        slirp_cleanup(Ctx);
        Ctx = nullptr;
    }
}

void FinishUDPFrame(u8* data, int len)
{
    u8* ipheader = &data[0xE];
    u8* udpheader = &data[0x22];

    // lengths
    *(u16*)&ipheader[2] = htons(len - 0xE);
    *(u16*)&udpheader[4] = htons(len - (0xE + 0x14));

    // IP checksum
    u32 tmp = 0;

    for (int i = 0; i < 20; i += 2)
        tmp += ntohs(*(u16*)&ipheader[i]);
    while (tmp >> 16)
        tmp = (tmp & 0xFFFF) + (tmp >> 16);
    tmp ^= 0xFFFF;
    *(u16*)&ipheader[10] = htons(tmp);

    // UDP checksum
    // (note: normally not mandatory, but some older sgIP versions require it)
    tmp = 0;
    tmp += ntohs(*(u16*)&ipheader[12]);
    tmp += ntohs(*(u16*)&ipheader[14]);
    tmp += ntohs(*(u16*)&ipheader[16]);
    tmp += ntohs(*(u16*)&ipheader[18]);
    tmp += ntohs(0x1100);
    tmp += (len-0x22);
    for (u8* i = udpheader; i < &udpheader[len-0x23]; i += 2)
        tmp += ntohs(*(u16*)i);
    if (len & 1)
        tmp += ntohs((u_short)udpheader[len-0x23]);
    while (tmp >> 16)
        tmp = (tmp & 0xFFFF) + (tmp >> 16);
    tmp ^= 0xFFFF;
    if (tmp == 0) tmp = 0xFFFF;
    *(u16*)&udpheader[6] = htons(tmp);
}

void Net_Slirp::HandleDNSFrame(u8* data, int len) noexcept
{
    u8* ipheader = &data[0xE];
    u8* udpheader = &data[0x22];
    u8* dnsbody = &data[0x2A];

    u32 srcip = ntohl(*(u32*)&ipheader[12]);
    u16 srcport = ntohs(*(u16*)&udpheader[0]);

    u16 id = ntohs(*(u16*)&dnsbody[0]);
    u16 flags = ntohs(*(u16*)&dnsbody[2]);
    u16 numquestions = ntohs(*(u16*)&dnsbody[4]);
    u16 numanswers = ntohs(*(u16*)&dnsbody[6]);
    u16 numauth = ntohs(*(u16*)&dnsbody[8]);
    u16 numadd = ntohs(*(u16*)&dnsbody[10]);

    Log(LogLevel::Debug, "DNS: ID=%04X, flags=%04X, Q=%d, A=%d, auth=%d, add=%d\n",
           id, flags, numquestions, numanswers, numauth, numadd);

    // for now we only take 'simple' DNS requests
    if (flags & 0x8000) return;
    if (numquestions != 1 || numanswers != 0) return;

    u8 resp[1024];
    u8* out = &resp[0];

    // ethernet
    memcpy(out, &data[6], 6); out += 6;
    memcpy(out, kServerMAC, 6); out += 6;
    *(u16*)out = htons(0x0800); out += 2;

    // IP
    u8* resp_ipheader = out;
    *out++ = 0x45;
    *out++ = 0x00;
    *(u16*)out = 0; out += 2; // total length
    *(u16*)out = htons(IPv4ID); out += 2; IPv4ID++;
    *out++ = 0x00;
    *out++ = 0x00;
    *out++ = 0x80; // TTL
    *out++ = 0x11; // protocol (UDP)
    *(u16*)out = 0; out += 2; // checksum
    *(u32*)out = htonl(InternalDNSIP); out += 4; // source IP
    *(u32*)out = htonl(srcip); out += 4; // destination IP

    // UDP
    u8* resp_udpheader = out;
    *(u16*)out = htons(53); out += 2; // source port
    *(u16*)out = htons(srcport); out += 2; // destination port
    *(u16*)out = 0; out += 2; // length
    *(u16*)out = 0; out += 2; // checksum

    // DNS
    u8* resp_body = out;
    *(u16*)out = htons(id); out += 2; // ID
    *(u16*)out = htons(0x8000); out += 2; // flags
    *(u16*)out = htons(numquestions); out += 2; // num questions
    *(u16*)out = htons(numquestions); out += 2; // num answers
    *(u16*)out = 0; out += 2; // num authority
    *(u16*)out = 0; out += 2; // num additional

    u32 curoffset = 12;
    for (u16 i = 0; i < numquestions; i++)
    {
        if (curoffset >= (len-0x2A)) return;

        u8 bitlength = 0;
        while ((bitlength = dnsbody[curoffset++]) != 0)
            curoffset += bitlength;

        curoffset += 4;
    }

    u32 qlen = curoffset-12;
    if (qlen > 512) return;
    memcpy(out, &dnsbody[12], qlen); out += qlen;

    curoffset = 12;
	for (u16 i = 0; i < numquestions; i++)
	{
		// assemble the requested domain name
		u8 bitlength = 0;
		char domainname[256] = ""; int o = 0;
		while ((bitlength = dnsbody[curoffset++]) != 0)
		{
		    if ((o+bitlength) >= 255)
            {
                // welp. atleast try not to explode.
                domainname[o++] = '\0';
                break;
            }

			strncpy(&domainname[o], (const char *)&dnsbody[curoffset], bitlength);
			o += bitlength;

			curoffset += bitlength;
			if (dnsbody[curoffset] != 0)
				domainname[o++] = '.';
            else
                domainname[o++] = '\0';
		}

		u16 type = ntohs(*(u16*)&dnsbody[curoffset]);
		u16 cls = ntohs(*(u16*)&dnsbody[curoffset+2]);

		printf("- q%d: %04X %04X %s", i, type, cls, domainname);

		// get answer
		struct addrinfo dns_hint;
		struct addrinfo* dns_res;
		u32 addr_res;

		memset(&dns_hint, 0, sizeof(dns_hint));
		dns_hint.ai_family = AF_INET; // TODO: other address types (INET6, etc)
		if (getaddrinfo(domainname, "0", &dns_hint, &dns_res) == 0)
        {
            struct addrinfo* p = dns_res;
            while (p)
            {
                struct sockaddr_in* addr = (struct sockaddr_in*)p->ai_addr;
                addr_res = *(u32*)&addr->sin_addr;

                printf(" -> %d.%d.%d.%d",
                       addr_res & 0xFF, (addr_res >> 8) & 0xFF,
                       (addr_res >> 16) & 0xFF, addr_res >> 24);

                break;
                p = p->ai_next;
            }
        }
        else
        {
            printf(" shat itself :(");
            addr_res = 0;
        }

		printf("\n");
		curoffset += 4;

		// TODO: betterer support
		// (under which conditions does the C00C marker work?)
		*(u16*)out = htons(0xC00C); out += 2;
		*(u16*)out = htons(type); out += 2;
		*(u16*)out = htons(cls); out += 2;
		*(u32*)out = htonl(3600); out += 4; // TTL (hardcoded for now)
		*(u16*)out = htons(4); out += 2; // address length
		*(u32*)out = addr_res; out += 4; // address
    }

    u32 framelen = (u32)(out - &resp[0]);
    if (framelen & 1) { *out++ = 0; framelen++; }
    FinishUDPFrame(resp, framelen);

    if (Callback)
        Callback(resp, framelen);
}

int Net_Slirp::SendPacket(u8* data, int len) noexcept
{
    if (!Ctx) return 0;

    if (len > 2048)
    {
        Log(LogLevel::Error, "Net_SendPacket: error: packet too long (%d)\n", len);
        return 0;
    }

    u16 ethertype = ntohs(*(u16*)&data[0xC]);

    if (ethertype == 0x800)
    {
        u8 protocol = data[0x17];
        if (protocol == 0x11) // UDP
        {
            u16 dstport = ntohs(*(u16*)&data[0x24]);
            if (dstport == 53 && htonl(*(u32*)&data[0x1E]) == InternalDNSIP) // DNS
            {
                HandleDNSFrame(data, len);
                return len;
            }
        }
    }

    slirp_input(Ctx, data, len);
    return len;
}

int Net_Slirp::SlirpCbAddPoll(int fd, int events, void* opaque) noexcept
{
    Net_Slirp& self = *static_cast<Net_Slirp*>(opaque);

    if (self.PollListSize >= PollListMax)
    {
        Log(LogLevel::Error, "slirp: POLL LIST FULL\n");
        return -1;
    }

    int idx = self.PollListSize++;

    u16 evt = 0;

    if (events & SLIRP_POLL_IN) evt |= POLLIN;
    if (events & SLIRP_POLL_OUT) evt |= POLLWRNORM;

#ifndef __WIN32__
    // CHECKME
    if (events & SLIRP_POLL_PRI) evt |= POLLPRI;
    if (events & SLIRP_POLL_ERR) evt |= POLLERR;
    if (events & SLIRP_POLL_HUP) evt |= POLLHUP;
#endif // !__WIN32__

    self.PollList[idx].fd = fd;
    self.PollList[idx].events = evt;

    return idx;
}

int Net_Slirp::SlirpCbGetREvents(int idx, void* opaque) noexcept
{
    Net_Slirp& self = *static_cast<Net_Slirp*>(opaque);

    if (idx < 0 || idx >= self.PollListSize)
        return 0;

    u16 evt = self.PollList[idx].revents;
    int ret = 0;

    if (evt & POLLIN) ret |= SLIRP_POLL_IN;
    if (evt & POLLWRNORM) ret |= SLIRP_POLL_OUT;
    if (evt & POLLPRI) ret |= SLIRP_POLL_PRI;
    if (evt & POLLERR) ret |= SLIRP_POLL_ERR;
    if (evt & POLLHUP) ret |= SLIRP_POLL_HUP;

    return ret;
}

void Net_Slirp::RecvCheck() noexcept
{
    // ndsrecomp: intentionally empty. Net::RecvPacket (vendored,
    // net/Net.cpp:61) calls this virtual unconditionally on whatever
    // thread calls Net::RecvPacket -- in this build that is always the
    // emulation thread (Platform::Net_RecvPacket's only caller chain is
    // Wifi::USTimer -> CheckRX -> WifiAP::RecvPacket -> Net::RecvPacket,
    // runner/src/wifi_net.cpp). All host-socket polling now happens
    // exclusively on a dedicated worker thread via PollHostSockets()
    // below, invoked only from runner/src/wifi_net.cpp's host networking
    // thread. Making this a true no-op -- rather than the non-blocking
    // poll it ran after 0002-net-slirp-nonblocking-poll.patch -- removes
    // every trace of host-socket-syscall latency/jitter from the
    // emulation thread's path, per project plan §18/§19. See
    // runner/vendor/melonds/patches/0005-net-slirp-worker-thread-poll.patch
    // and docs/adr-melonds-wifi-vendoring.md §8 Finding 1.
}

void Net_Slirp::PollHostSockets() noexcept
{
    // ndsrecomp: this is the body RecvCheck() used to run directly on the
    // emulation thread, unchanged except for the comments. See this
    // class's declaration for the single-caller-thread contract that
    // makes the unsynchronized access to Ctx/PollList below safe: only
    // runner/src/wifi_net.cpp's worker thread ever calls this, and that
    // same thread is also the only caller of SendPacket(), so the two
    // never interleave.
    if (!Ctx) return;

    //if (PollListSize > 0)
    {
        u32 timeout = 0;
        PollListSize = 0;
        slirp_pollfds_fill(Ctx, &timeout, SlirpCbAddPoll, this);
        // ndsrecomp: upstream let libslirp's own internal timer deadlines
        // (via the `timeout` out-parameter above) decide how long poll()
        // may block. Keep forcing non-blocking regardless of what libslirp
        // asked for (0002-net-slirp-nonblocking-poll.patch's mandatory
        // clamp, retained here even though this now runs on a dedicated
        // worker thread rather than the emulation thread): a bounded,
        // fast, always-non-blocking poll() keeps this loop's own shutdown
        // latency small and predictable -- a stop request is never stuck
        // behind an in-flight blocking poll() with an arbitrary
        // libslirp-chosen timeout. See runner/src/wifi_net.cpp's worker
        // loop and runner/vendor/melonds/patches/0002-net-slirp-nonblocking-poll.patch.
        timeout = 0;
        // ndsrecomp: only issue the syscall when libslirp actually handed
        // us descriptors. Upstream's own `if (PollListSize > 0)` guard
        // just above is commented out, and PollListSize is reset to 0
        // immediately before slirp_pollfds_fill, so with no active guest
        // socket this reached poll() with nfds == 0 -- and Windows'
        // WSAPoll(), which this platform's poll() resolves to, is
        // documented to fail with WSAEINVAL when nfds is zero. That turned
        // 0007's BACKEND_ERROR channel into pure noise: a measured 8170
        // WSAEINVAL "failures" during one MKDS WFC login, 8094 of them
        // coalesced into a single ring event, all benign, and all of them
        // stopping the instant the guest opened its first socket (the last
        // one landed at guest cycle 4113489534, the first DNS query at
        // 4113966067). A genuine poll() failure would have been
        // indistinguishable from that flood, which defeats the whole point
        // of the channel. libslirp still needs slirp_pollfds_poll() called
        // unconditionally so its internal timers keep advancing, so only
        // the syscall itself is skipped -- res stays 0, meaning "no
        // error", for the no-descriptor case.
        int res = 0;
        if (PollListSize > 0)
        {
            res = poll(PollList, PollListSize, timeout);
            if (res < 0)
            {
                // ndsrecomp: surface the failure via the atomic handoff
                // pair declared in Net_Slirp.h -- see that declaration's
                // comment for why this can't just call net_ring_push
                // directly from here (this runs on the dedicated worker
                // thread; net_ring_push may only ever be called from the
                // emulation thread).
#ifdef __WIN32__
                LastPollErrorCode.store(WSAGetLastError(), std::memory_order_relaxed);
#else
                LastPollErrorCode.store(errno, std::memory_order_relaxed);
#endif
                PollErrorCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
        slirp_pollfds_poll(Ctx, res<0, SlirpCbGetREvents, this);
    }
}

uint32_t Net_Slirp::TakePollErrorCount() noexcept
{
    return PollErrorCount.exchange(0, std::memory_order_relaxed);
}

int Net_Slirp::LastPollError() const noexcept
{
    return LastPollErrorCode.load(std::memory_order_relaxed);
}

}
