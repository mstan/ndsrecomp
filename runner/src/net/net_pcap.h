// net_pcap.h -- minimal classic-pcap ("libpcap savefile") writer, the
// Wireshark-openable sibling output for NdsNetCaptureWriter (see
// net_capture.h's top comment for why this exists alongside the native
// NDSNETREPLAY1 format: pcap cannot carry an exact guest-cycle integer in
// its native per-packet timestamp field, so it is a diagnostic companion,
// never the format this project's own replay path reads back).
//
// Deliberately NOT pcapng: classic pcap's global header + flat per-packet
// record list is materially simpler to hand-write correctly (no block/TLV
// framing, no section headers) and is exactly as Wireshark-openable for
// this project's needs (a flat Ethernet capture, no interface metadata or
// comments worth pcapng's extra structure). If a future need arises for
// per-packet comments or multiple interfaces, upgrading to pcapng is a
// contained change to this one file, not a structural one.

#pragma once

#include <cstddef>
#include <cstdint>

// The ARM7 Wi-Fi timer's own base rate (Wifi::ScheduleTimer's
// 33513982 Hz-based formula, also assumed by NDS::ScheduleEvent's
// CurrentSystemTimestamp domain -- see runner/src/wifi_net.cpp). Used only
// to convert a `guest_cycle` into an approximate (seconds, microseconds)
// pcap timestamp for human/Wireshark relative-ordering; never read back by
// this project's own capture reader.
constexpr double kNdsNetPcapClockHz = 33513982.0;

struct NdsNetPcapWriterImpl;

// Opens `path`, truncating any existing file, and writes the pcap global
// header (LINKTYPE_ETHERNET). Returns nullptr on any open/write failure --
// treated as non-fatal by the caller (NdsNetCaptureWriter): losing this
// diagnostic sibling must never abort or corrupt the authoritative native
// capture.
NdsNetPcapWriterImpl* nds_net_pcap_open(const char* path);

// Appends one packet record. `direction` is accepted for call-site
// symmetry with the native writer but not stored -- classic pcap has no
// per-packet direction field for a single-interface capture; Wireshark
// infers TX/RX itself from the frame's own addresses.
void nds_net_pcap_write(NdsNetPcapWriterImpl* writer, uint64_t guest_cycle,
                        uint8_t direction, const uint8_t* data, size_t len);

void nds_net_pcap_close(NdsNetPcapWriterImpl* writer);
