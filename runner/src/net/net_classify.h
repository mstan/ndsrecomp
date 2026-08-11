// net_classify.h -- passive protocol classification for the always-on
// network event ring (Wiimmfi M3 task 1, beads work under the Wiimmfi meta
// epic). See net_ring.h for the ring itself; this header only adds a
// decoder that turns a raw Ethernet frame into the richer event kinds the
// ring already declares (ARP, DHCP, DNS_QUERY/RESPONSE, TCP_*, UDP_PACKET)
// but that nothing previously populated -- until this file, every frame
// only ever produced a generic ETHERNET_TX/RX entry (WifiAP.cpp's
// patch-0004 hooks), so a DHCP or DNS handshake was invisible in the ring
// even though the raw bytes were flowing.
//
// Call sites: runner/src/wifi_net.cpp's Net_SendPacket (direction=0,
// guest->host) and Net_RecvPacket (direction=1, host->guest) -- the
// project-owned bridge/backend boundary named in the task, NOT a vendored
// melonDS file. Both call sites already run exclusively on the emulation
// thread (see the design comment above WifiBridgeState in wifi_net.cpp),
// which is the same thread net_ring_push assumes as its sole writer, so no
// additional synchronization is needed here.
//
// Hard constraints (see net_ring.h's privacy note and the task spec):
//   - Observation only. This function never mutates `frame` and never
//     changes what gets sent/received; it only reads bytes already on the
//     wire and calls net_ring_push/net_ring_set_hostname.
//   - Metadata only: addresses, ports, lengths, and small protocol-specific
//     codes -- never a payload byte range. DNS hostnames are the one
//     documented exception and go through the existing hostname side pool
//     (net_ring_set_hostname), not a new field.
//   - Every field populated here already exists on NdsNetTraceEntry; this
//     file adds no new ring fields.

#pragma once

#include <cstddef>
#include <cstdint>

// Decodes `frame[0..len)` (a complete, on-the-wire Ethernet II frame -- 14
// byte header, no FCS) and pushes zero or more classified net_ring events
// for whatever protocol layers it recognizes:
//   - EtherType 0x0806 (ARP)              -> NDS_NET_EVENT_ARP
//   - EtherType 0x0800 (IPv4), proto UDP:
//       - src/dst port 67 or 68           -> NDS_NET_EVENT_DHCP
//       - src/dst port 53                 -> NDS_NET_EVENT_DNS_QUERY or
//                                             NDS_NET_EVENT_DNS_RESPONSE
//       - otherwise                       -> NDS_NET_EVENT_UDP_PACKET
//   - EtherType 0x0800 (IPv4), proto TCP  -> NDS_NET_EVENT_TCP_OPEN/
//                                             TCP_CLOSE/TCP_RESET/TCP_PACKET,
//                                             plus (Wiimmfi M5) a passive,
//                                             labels-only NDS_NET_EVENT_
//                                             TLS_RECORD when the segment's
//                                             payload starts with a
//                                             plausible TLS/SSLv3 record
//                                             header -- see
//                                             classify_tls_record in the
//                                             .cpp for the exact contract.
//                                             This never terminates or
//                                             alters the TLS handshake.
// Anything else (IPv6, other EtherTypes, malformed/truncated headers) is
// silently skipped -- the existing generic ETHERNET_TX/RX event already
// recorded by WifiAP.cpp's patch-0004 hooks remains the only ring entry
// for such frames, exactly as before this file existed.
//
// `direction` uses the same convention as NdsNetTraceEntry::direction: 0 =
// guest->host (TX/egress), 1 = host->guest (RX/ingress).
void net_classify_ethernet_frame(const uint8_t* frame, size_t len,
                                  uint8_t direction);
