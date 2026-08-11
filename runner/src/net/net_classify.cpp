// net_classify.cpp -- see net_classify.h for the design rationale and hard
// constraints (observation only, metadata only, no new ring fields).
//
// Every decode function below takes the whole frame buffer plus its exact
// length and never reads past `len`; a malformed/truncated header simply
// aborts that classification attempt (no push), it never crashes and never
// guesses past the bytes actually present. Multi-byte header fields are
// network byte order (big-endian) and read byte-by-byte -- the frame
// buffer is not guaranteed to be aligned for a wider load.

#include "net_classify.h"

#include <cstdio>

#include "net_ring.h"

namespace {

// ---- unaligned big-endian reads -------------------------------------------

uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>((uint16_t{p[0]} << 8) | p[1]);
}

uint32_t rd32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) |
           (uint32_t{p[2]} << 8) | uint32_t{p[3]};
}

void ipv4_to_str(uint32_t ip, char* out, size_t out_size) {
    std::snprintf(out, out_size, "%u.%u.%u.%u", (ip >> 24) & 0xFFu,
                  (ip >> 16) & 0xFFu, (ip >> 8) & 0xFFu, ip & 0xFFu);
}

// ---- DNS name decode (RFC 1035 label sequence, with compression) ---------
//
// `msg_base` is the byte offset (within `frame`) of the start of the DNS
// message (i.e. right after the UDP header) -- compression pointers are
// offsets relative to that base, per RFC 1035 4.1.4, not to the start of
// the Ethernet frame. Returns false on any out-of-bounds read (truncated
// frame or corrupt pointer chain); on success, `*end_pos` is the absolute
// offset in `frame` immediately after the name AS IT APPEARS AT `pos`
// (i.e. before following any compression pointer -- the correct point for
// the caller to resume sequential parsing of the record that contained
// this name).
bool dns_read_name(const uint8_t* frame, size_t len, size_t msg_base,
                    size_t pos, char* out, size_t out_size,
                    size_t* end_pos) {
    size_t out_pos = 0;
    size_t real_end = pos;
    bool end_set = false;
    int jumps = 0;
    if (out_size) out[0] = '\0';
    for (;;) {
        if (pos >= len) return false;
        const uint8_t label_len = frame[pos];
        if (label_len == 0) {
            if (!end_set) { real_end = pos + 1; end_set = true; }
            break;
        }
        if ((label_len & 0xC0u) == 0xC0u) {
            if (pos + 1 >= len) return false;
            if (!end_set) { real_end = pos + 2; end_set = true; }
            if (++jumps > 20) return false;  // guard against pointer loops
            const size_t ptr =
                (static_cast<size_t>(label_len & 0x3Fu) << 8) | frame[pos + 1];
            pos = msg_base + ptr;
            continue;
        }
        pos += 1;
        if (pos + label_len > len) return false;
        if (out_pos != 0 && out_pos + 1 < out_size) out[out_pos++] = '.';
        for (uint8_t i = 0; i < label_len; ++i) {
            if (out_pos + 1 < out_size) out[out_pos++] = static_cast<char>(frame[pos + i]);
        }
        pos += label_len;
    }
    if (out_size) out[out_pos < out_size ? out_pos : out_size - 1] = '\0';
    if (end_pos) *end_pos = real_end;
    return true;
}

// ---- DHCP (BOOTP over UDP 67/68) -------------------------------------------
//
// Fixed BOOTP layout (RFC 951/2131), offsets relative to `udp_payload`:
//   0 op(1) 1 htype(1) 2 hlen(1) 3 hops(1) 4 xid(4) 8 secs(2) 10 flags(2)
//   12 ciaddr(4) 16 yiaddr(4) 20 siaddr(4) 24 giaddr(4) 28 chaddr(16)
//   44 sname(64) 108 file(128) 236 magic-cookie(4) 240.. options (TLV,
//   0xFF = End, 0x00 = Pad).
void classify_dhcp(const uint8_t* frame, size_t len, uint8_t direction,
                    const uint8_t* src_mac, const uint8_t* dst_mac,
                    uint32_t src_ipv4, uint32_t dst_ipv4,
                    uint16_t src_port, uint16_t dst_port,
                    size_t udp_payload, size_t udp_payload_len) {
    uint32_t yiaddr = 0;
    if (udp_payload + 20 <= len) yiaddr = rd32(frame + udp_payload + 16);

    uint8_t msg_type = 0;      // DHCP option 53; 0 if absent/unparsed
    uint32_t router_ip = 0;    // option 3
    uint32_t dns_ip = 0;       // option 6
    uint32_t server_id = 0;    // option 54
    bool have_router = false, have_dns = false;

    const size_t cookie = udp_payload + 236;
    if (cookie + 4 <= len && frame[cookie] == 0x63 && frame[cookie + 1] == 0x82 &&
        frame[cookie + 2] == 0x53 && frame[cookie + 3] == 0x63) {
        size_t p = cookie + 4;
        while (p < len) {
            const uint8_t code = frame[p];
            if (code == 0xFFu) break;         // End
            if (code == 0x00u) { ++p; continue; }  // Pad
            if (p + 1 >= len) break;
            const uint8_t opt_len = frame[p + 1];
            if (p + 2u + opt_len > len) break;
            const uint8_t* val = frame + p + 2;
            if (code == 53 && opt_len >= 1) msg_type = val[0];
            else if (code == 3 && opt_len >= 4) { router_ip = rd32(val); have_router = true; }
            else if (code == 6 && opt_len >= 4) { dns_ip = rd32(val); have_dns = true; }
            else if (code == 54 && opt_len >= 4) { server_id = rd32(val); }
            p += 2u + opt_len;
        }
    }

    net_ring_push(NDS_NET_EVENT_DHCP, direction, /*wifi_reg=*/0,
                  /*wifi_value=*/yiaddr, src_mac, dst_mac, src_ipv4, dst_ipv4,
                  src_port, dst_port, static_cast<uint16_t>(udp_payload_len),
                  /*aux=*/msg_type);

    // Operator-visible diagnostic for the operationally interesting facts
    // (assigned IP / gateway / DNS server) that don't fit the ring's fixed
    // numeric fields -- see net_classify.h's design note. Not a ring write,
    // not payload bytes: same house style as every other "[component] ..."
    // Platform::Log/fprintf line already in this codebase (e.g.
    // wifi_net.cpp's "[wifi_net] ..." lines, net_ring.cpp's dump function).
    if ((msg_type == 2 || msg_type == 5) && direction == 1) {
        char client[16], server[16], gw[16], dns[16];
        ipv4_to_str(yiaddr, client, sizeof(client));
        ipv4_to_str(server_id ? server_id : src_ipv4, server, sizeof(server));
        if (have_router) ipv4_to_str(router_ip, gw, sizeof(gw));
        else std::snprintf(gw, sizeof(gw), "?");
        if (have_dns) ipv4_to_str(dns_ip, dns, sizeof(dns));
        else std::snprintf(dns, sizeof(dns), "?");
        std::fprintf(stderr,
            "[net_classify] dhcp %s: client=%s server=%s gateway=%s dns=%s\n",
            msg_type == 2 ? "OFFER" : "ACK", client, server, gw, dns);
    }
}

// ---- DNS (UDP 53) -----------------------------------------------------------
//
// Header (RFC 1035 4.1.1), all fields relative to `udp_payload`:
//   0 id(2) 2 flags(2) 4 qdcount(2) 6 ancount(2) 8 nscount(2) 10 arcount(2)
//   12.. question section (qdcount entries): name, qtype(2), qclass(2)
// flags bit15 (top bit of the first flags byte) is QR (0=query,1=response);
// bits 0..3 of the low flags byte are RCODE.
void classify_dns(const uint8_t* frame, size_t len, uint8_t direction,
                   const uint8_t* src_mac, const uint8_t* dst_mac,
                   uint32_t src_ipv4, uint32_t dst_ipv4,
                   uint16_t src_port, uint16_t dst_port,
                   size_t udp_payload, size_t udp_payload_len) {
    if (udp_payload + 12 > len) return;  // truncated DNS header

    const uint16_t txn_id = rd16(frame + udp_payload + 0);
    const uint16_t flags = rd16(frame + udp_payload + 2);
    const uint16_t qdcount = rd16(frame + udp_payload + 4);
    const uint16_t ancount = rd16(frame + udp_payload + 6);
    const bool is_response = (flags & 0x8000u) != 0;
    const uint8_t rcode = static_cast<uint8_t>(flags & 0x000Fu);

    if (qdcount < 1) return;  // nothing to classify without a question

    char qname[256];
    size_t after_name = 0;
    if (!dns_read_name(frame, len, udp_payload, udp_payload + 12, qname,
                       sizeof(qname), &after_name)) {
        return;
    }
    if (after_name + 4 > len) return;  // qtype/qclass truncated
    const uint16_t qtype = rd16(frame + after_name);

    if (!is_response) {
        const uint64_t count = net_ring_push(
            NDS_NET_EVENT_DNS_QUERY, direction, /*wifi_reg=*/0,
            /*wifi_value=*/txn_id, src_mac, dst_mac, src_ipv4, dst_ipv4,
            src_port, dst_port, static_cast<uint16_t>(udp_payload_len),
            /*aux=*/qtype);
        if (qname[0] != '\0') net_ring_set_hostname(count, qname);
        return;
    }

    // Response: walk past the (single, already-decoded) question to the
    // answer section and record the first A-record (type=1, class=IN)
    // address found, if any -- "the answer addresses" the task asks for.
    // Additional questions beyond the first are skipped generically (name
    // + 4 bytes) without decoding, matching classify_dhcp's "best-effort,
    // never crash on a shape we don't special-case" convention.
    size_t pos = after_name + 4;
    for (uint16_t i = 1; i < qdcount; ++i) {
        size_t end = 0;
        if (!dns_read_name(frame, len, udp_payload, pos, nullptr, 0, &end))
            break;
        pos = end + 4;
    }

    uint32_t answer_ip = 0;
    bool have_answer = false;
    for (uint16_t i = 0; i < ancount && !have_answer; ++i) {
        size_t end = 0;
        char rr_name[256];
        if (!dns_read_name(frame, len, udp_payload, pos, rr_name,
                           sizeof(rr_name), &end)) {
            break;
        }
        if (end + 10 > len) break;
        const uint16_t rtype = rd16(frame + end);
        const uint16_t rclass = rd16(frame + end + 2);
        const uint16_t rdlength = rd16(frame + end + 8);
        const size_t rdata = end + 10;
        if (rdata + rdlength > len) break;
        if (rtype == 1 && rclass == 1 && rdlength == 4) {
            answer_ip = rd32(frame + rdata);
            have_answer = true;
        }
        pos = rdata + rdlength;
    }

    const uint64_t count = net_ring_push(
        NDS_NET_EVENT_DNS_RESPONSE, direction, /*wifi_reg=*/0,
        /*wifi_value=*/txn_id, src_mac, dst_mac, src_ipv4,
        have_answer ? answer_ip : dst_ipv4, src_port, dst_port,
        static_cast<uint16_t>(udp_payload_len),
        /*aux=*/static_cast<uint32_t>(rcode) | (uint32_t{ancount} << 8));
    if (qname[0] != '\0') net_ring_set_hostname(count, qname);

    if (have_answer) {
        char answer_str[16];
        ipv4_to_str(answer_ip, answer_str, sizeof(answer_str));
        std::fprintf(stderr, "[net_classify] dns response: %s -> %s (rcode=%u)\n",
                     qname, answer_str, rcode);
    } else {
        std::fprintf(stderr,
            "[net_classify] dns response: %s -> no A record (rcode=%u, ancount=%u)\n",
            qname, rcode, ancount);
    }
}

// ---- TLS/SSLv3 (passive record-header classification, labels only) --------
//
// See net_ring.h's NDS_NET_EVENT_TLS_RECORD doc comment for the full
// contract: this peeks at the leading bytes of a TCP segment's payload --
// bytes already resident in the `frame` buffer classify_tcp holds -- and,
// if they look like a plausible TLS/SSLv3 record header, pushes a
// classification label. It never reassembles the TCP stream, never stores
// payload bytes, and never affects what bytes actually flow (the frame is
// never mutated and this runs after the frame has already been
// sent/received -- see net_classify.h's call-site note). A segment that
// doesn't start with a record header (a mid-record continuation, or simply
// not TLS traffic) is silently skipped, matching every other classifier in
// this file's "best-effort, never guess past the bytes actually present"
// convention.
//
// TLS/SSLv3 record header (5 bytes, RFC 6101/8446):
//   0 content_type(1) 1-2 version(major,minor) 3-4 length(2, big-endian)
// followed by `length` bytes of record body. For a handshake record
// (content_type==22) whose header sits exactly at the start of this
// segment, the body's first byte is itself a handshake message type.
void classify_tls_record(const uint8_t* frame, size_t len, uint8_t direction,
                          const uint8_t* src_mac, const uint8_t* dst_mac,
                          uint32_t src_ipv4, uint32_t dst_ipv4,
                          uint16_t src_port, uint16_t dst_port,
                          size_t payload_off, size_t payload_len) {
    if (payload_len < 5 || payload_off + 5 > len) return;

    const uint8_t content_type = frame[payload_off + 0];
    const uint8_t version_major = frame[payload_off + 1];
    const uint8_t version_minor = frame[payload_off + 2];
    const uint16_t record_len = rd16(frame + payload_off + 3);

    const bool plausible_content_type =
        content_type == NDS_TLS_CONTENT_CHANGE_CIPHER_SPEC ||
        content_type == NDS_TLS_CONTENT_ALERT ||
        content_type == NDS_TLS_CONTENT_HANDSHAKE ||
        content_type == NDS_TLS_CONTENT_APPLICATION_DATA;
    // SSLv3 is {3,0}; TLS 1.0/1.1/1.2 are {3,1}/{3,2}/{3,3}. The task spec
    // says the DS speaks SSLv3, but classify by the whole major==3 family
    // rather than hardcoding {3,0} -- a wider net for "is this plausibly a
    // TLS/SSL record" without pretending to know the guest's exact minor
    // version from bytes alone.
    if (!plausible_content_type || version_major != 3) return;
    // Sanity-bound the declared record length against RFC 8446's own
    // maximum (2^14 bytes plus a small overhead allowance) so a random
    // non-TLS payload that happens to start with a matching content-type
    // byte and version isn't misclassified just because two bytes lined up.
    if (record_len == 0 || record_len > 18432) return;

    uint8_t handshake_type = NDS_TLS_HANDSHAKE_UNKNOWN;
    if (content_type == NDS_TLS_CONTENT_HANDSHAKE && payload_len >= 6 &&
        payload_off + 6 <= len) {
        handshake_type = frame[payload_off + 5];
    }

    const uint32_t aux = (static_cast<uint32_t>(content_type) << 16) |
                          (static_cast<uint32_t>(handshake_type) << 8) |
                          version_minor;
    net_ring_push(NDS_NET_EVENT_TLS_RECORD, direction, /*wifi_reg=*/0,
                  /*wifi_value=*/0, src_mac, dst_mac, src_ipv4, dst_ipv4,
                  src_port, dst_port, record_len, aux);
}

// ---- TCP (proto 6) ----------------------------------------------------------

void classify_tcp(const uint8_t* frame, size_t len, uint8_t direction,
                   const uint8_t* src_mac, const uint8_t* dst_mac,
                   uint32_t src_ipv4, uint32_t dst_ipv4,
                   size_t tcp_offset, size_t ip_payload_len) {
    if (tcp_offset + 20 > len) return;  // truncated fixed TCP header
    const uint16_t src_port = rd16(frame + tcp_offset + 0);
    const uint16_t dst_port = rd16(frame + tcp_offset + 2);
    const uint8_t data_offset_word = frame[tcp_offset + 12];
    const size_t data_offset = static_cast<size_t>(data_offset_word >> 4) * 4u;
    if (data_offset < 20 || tcp_offset + data_offset > len) return;
    const uint8_t flags = frame[tcp_offset + 13];
    constexpr uint8_t kFIN = 0x01, kSYN = 0x02, kRST = 0x04;

    const size_t payload_len =
        ip_payload_len > data_offset ? ip_payload_len - data_offset : 0;

    NdsNetEventKind kind;
    if (flags & kRST) kind = NDS_NET_EVENT_TCP_RESET;
    else if (flags & kSYN) kind = NDS_NET_EVENT_TCP_OPEN;
    else if (flags & kFIN) kind = NDS_NET_EVENT_TCP_CLOSE;
    else kind = NDS_NET_EVENT_TCP_PACKET;

    net_ring_push(kind, direction, /*wifi_reg=*/0, /*wifi_value=*/0, src_mac,
                  dst_mac, src_ipv4, dst_ipv4, src_port, dst_port,
                  static_cast<uint16_t>(payload_len), /*aux=*/flags);

    if (payload_len > 0) {
        const size_t payload_off = tcp_offset + data_offset;
        classify_tls_record(frame, len, direction, src_mac, dst_mac,
                             src_ipv4, dst_ipv4, src_port, dst_port,
                             payload_off, payload_len);
    }
}

}  // namespace

void net_classify_ethernet_frame(const uint8_t* frame, size_t len,
                                  uint8_t direction) {
    if (!frame || len < 14) return;

    const uint8_t* dst_mac = frame + 0;
    const uint8_t* src_mac = frame + 6;
    const uint16_t ethertype = rd16(frame + 12);

    if (ethertype == 0x0806) {  // ARP, IPv4-over-Ethernet shape (28-byte body)
        if (len < 14 + 28) return;
        const uint8_t* arp = frame + 14;
        const uint16_t opcode = rd16(arp + 6);
        const uint8_t* sender_mac = arp + 8;
        const uint32_t sender_ip = rd32(arp + 14);
        const uint8_t* target_mac = arp + 18;
        const uint32_t target_ip = rd32(arp + 24);
        net_ring_push(NDS_NET_EVENT_ARP, direction, /*wifi_reg=*/0,
                      /*wifi_value=*/0, sender_mac, target_mac, sender_ip,
                      target_ip, 0, 0, static_cast<uint16_t>(len - 14),
                      /*aux=*/opcode);
        return;
    }

    if (ethertype != 0x0800) return;  // only IPv4 is classified past here

    if (len < 14 + 20) return;
    const uint8_t* ip = frame + 14;
    const uint8_t version = static_cast<uint8_t>(ip[0] >> 4);
    const size_t ihl = static_cast<size_t>(ip[0] & 0x0Fu) * 4u;
    if (version != 4 || ihl < 20 || 14 + ihl > len) return;
    const uint8_t protocol = ip[9];
    const uint32_t src_ipv4 = rd32(ip + 12);
    const uint32_t dst_ipv4 = rd32(ip + 16);
    const uint16_t ip_total_len = rd16(ip + 2);
    const size_t ip_payload_off = 14 + ihl;
    // Prefer the header's declared total length (clamped to what's actually
    // present) over the raw capture length -- Ethernet padding on short
    // frames would otherwise be misread as extra UDP/TCP payload.
    const size_t declared_ip_len =
        ip_total_len > ihl ? static_cast<size_t>(ip_total_len) - ihl : 0;
    const size_t available_ip_len =
        len > ip_payload_off ? len - ip_payload_off : 0;
    const size_t ip_payload_len =
        declared_ip_len < available_ip_len ? declared_ip_len : available_ip_len;

    if (protocol == 17) {  // UDP
        if (ip_payload_off + 8 > len) return;
        const uint16_t src_port = rd16(frame + ip_payload_off + 0);
        const uint16_t dst_port = rd16(frame + ip_payload_off + 2);
        const uint16_t udp_len = rd16(frame + ip_payload_off + 4);
        const size_t udp_payload_off = ip_payload_off + 8;
        const size_t declared_udp_len = udp_len > 8 ? size_t{udp_len} - 8 : 0;
        const size_t available_udp_len =
            ip_payload_len > 8 ? ip_payload_len - 8 : 0;
        const size_t udp_payload_len =
            declared_udp_len < available_udp_len ? declared_udp_len
                                                  : available_udp_len;

        if (src_port == 67 || src_port == 68 || dst_port == 67 || dst_port == 68) {
            classify_dhcp(frame, len, direction, src_mac, dst_mac, src_ipv4,
                          dst_ipv4, src_port, dst_port, udp_payload_off,
                          udp_payload_len);
        } else if (src_port == 53 || dst_port == 53) {
            classify_dns(frame, len, direction, src_mac, dst_mac, src_ipv4,
                        dst_ipv4, src_port, dst_port, udp_payload_off,
                        udp_payload_len);
        } else {
            net_ring_push(NDS_NET_EVENT_UDP_PACKET, direction, /*wifi_reg=*/0,
                          /*wifi_value=*/0, src_mac, dst_mac, src_ipv4,
                          dst_ipv4, src_port, dst_port,
                          static_cast<uint16_t>(udp_payload_len), /*aux=*/0);
        }
        return;
    }

    if (protocol == 6) {  // TCP
        classify_tcp(frame, len, direction, src_mac, dst_mac, src_ipv4,
                    dst_ipv4, ip_payload_off, ip_payload_len);
        return;
    }

    // Other IP protocols (ICMP, etc.) are left to the generic ETHERNET_TX/RX
    // event only -- not named in the task's classification list.
}
