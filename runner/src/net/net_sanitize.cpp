// net_sanitize.cpp -- see net_sanitize.h for the design rationale
// (identity-based: rewrite exactly one learned MAC, never "every MAC-
// shaped field"). Every decode here mirrors net_classify.cpp's own "never
// read/write past len, best-effort skip on anything that doesn't parse"
// convention -- this file additionally WRITES bytes back into the buffer
// (net_classify.cpp never does), so it is more conservative about
// bounds-checking before any write, not less.

#include "net_sanitize.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>((uint16_t{p[0]} << 8) | p[1]);
}

uint32_t rd32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) |
           (uint32_t{p[2]} << 8) | uint32_t{p[3]};
}

void wr16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

// FNV-1a 64-bit -- simple, dependency-free, and trivial to re-implement
// bit-for-bit in Python (tools/net_capture_tool.py), which is exactly what
// this module needs for its "deterministic across implementations" bonus
// property (see net_sanitize.h's top comment).
uint64_t Fnv1a64(const uint8_t* data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

// RFC 768 Internet checksum over the UDP pseudo-header + UDP header +
// payload. `udp` points at the start of the UDP header (8 bytes) followed
// immediately by `udp_len - 8` payload bytes; the checksum field itself
// (udp[6..8)) is treated as zero for the purposes of this computation,
// regardless of whatever value is currently sitting there. Returns 0xFFFF
// (never the all-zero "no checksum" sentinel) in the vanishingly unlikely
// case the raw computation is exactly zero, per RFC 768.
uint16_t ComputeUdpChecksum(uint32_t src_ipv4, uint32_t dst_ipv4,
                             const uint8_t* udp, size_t udp_len) {
    uint32_t sum = 0;
    sum += (src_ipv4 >> 16) & 0xFFFFu;
    sum += src_ipv4 & 0xFFFFu;
    sum += (dst_ipv4 >> 16) & 0xFFFFu;
    sum += dst_ipv4 & 0xFFFFu;
    sum += 17u;  // protocol = UDP
    sum += static_cast<uint32_t>(udp_len);

    size_t i = 0;
    for (; i + 1 < udp_len; i += 2) {
        if (i == 6) continue;  // checksum field itself: treated as zero
        sum += (uint32_t{udp[i]} << 8) | udp[i + 1];
    }
    if (i < udp_len) sum += uint32_t{udp[i]} << 8;  // trailing odd byte

    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    const uint16_t result = static_cast<uint16_t>(~sum & 0xFFFFu);
    return result == 0 ? 0xFFFFu : result;
}

}  // namespace

uint64_t NdsNetSanitizeState::PackMac(const uint8_t mac[6]) {
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v = (v << 8) | mac[i];
    return v;
}

void NdsNetSanitizeState::LearnIdentityFromTxSource(const uint8_t eth_src[6]) {
    if (has_identity_) return;
    std::memcpy(identity_mac_.data(), eth_src, 6);
    has_identity_ = true;
}

std::array<uint8_t, 6> NdsNetSanitizeState::MapMac(const uint8_t mac[6]) {
    const uint64_t key = PackMac(mac);
    auto it = mac_cache_.find(key);
    if (it != mac_cache_.end()) return it->second;

    const uint64_t h = Fnv1a64(mac, 6);
    std::array<uint8_t, 6> out{};
    out[0] = 0x02;  // locally-administered + unicast
    for (int i = 1; i < 6; ++i)
        out[static_cast<size_t>(i)] =
            static_cast<uint8_t>((h >> (8 * (i - 1))) & 0xFFu);
    mac_cache_.emplace(key, out);
    return out;
}

std::vector<uint8_t> NdsNetSanitizeState::MapHostname(const uint8_t* data,
                                                       size_t len) {
    const std::string key(reinterpret_cast<const char*>(data), len);
    auto it = hostname_cache_.find(key);
    if (it != hostname_cache_.end()) return it->second;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "ndsguest-%08x",
                  static_cast<unsigned>(Fnv1a64(data, len) & 0xFFFFFFFFu));
    const size_t buf_len = std::strlen(buf);
    std::vector<uint8_t> out(len);
    for (size_t i = 0; i < len; ++i)
        out[i] = static_cast<uint8_t>(buf[i % buf_len]);
    hostname_cache_.emplace(key, out);
    return out;
}

bool net_sanitize_ethernet_frame(uint8_t* frame, size_t len, uint8_t direction,
                                  NdsNetSanitizeState& state) {
    if (!frame || len < 14) return false;
    bool changed = false;

    // Learn the console's identity from THIS frame's own Ethernet source,
    // if this is a TX (guest->host) frame and no identity is known yet --
    // see net_sanitize.h's design note for why this is always safe (the
    // Ethernet source of an outgoing frame is, by construction, always
    // the sender's own address).
    if (direction == kNdsNetSanitizeDirTx && !state.HasIdentity()) {
        state.LearnIdentityFromTxSource(frame + 6);
    }
    if (!state.HasIdentity()) {
        // Nothing yet known to be sensitive -- an RX frame arrived before
        // any TX ever did (not expected for this project's DHCP-first
        // flows). Still handle the hostname option below (needs no
        // identity), but no MAC-shaped field can be safely rewritten yet.
    }

    const std::array<uint8_t, 6> identity = state.IdentityMac();
    const bool have_identity = state.HasIdentity();
    // Computed lazily (only meaningful once have_identity is true) but
    // cheap (memoized by MapMac's own cache) to compute unconditionally
    // here for simplicity.
    const std::array<uint8_t, 6> synthetic =
        have_identity ? state.MapMac(identity.data()) : std::array<uint8_t, 6>{};

    auto rewrite_if_identity = [&](uint8_t* p) {
        if (!have_identity) return;
        if (std::memcmp(p, identity.data(), 6) != 0) return;
        if (std::memcmp(p, synthetic.data(), 6) != 0) {
            std::memcpy(p, synthetic.data(), 6);
            changed = true;
        }
    };

    rewrite_if_identity(frame + 0);  // Ethernet dst
    rewrite_if_identity(frame + 6);  // Ethernet src

    const uint16_t ethertype = rd16(frame + 12);

    if (ethertype == 0x0806) {  // ARP, IPv4-over-Ethernet shape (28-byte body)
        if (len >= 14 + 28) {
            uint8_t* arp = frame + 14;
            rewrite_if_identity(arp + 8);   // sender hardware address
            rewrite_if_identity(arp + 18);  // target hardware address
        }
        return changed;
    }

    if (ethertype != 0x0800 || len < 14 + 20) return changed;

    const uint8_t* ip = frame + 14;
    const uint8_t version = static_cast<uint8_t>(ip[0] >> 4);
    const size_t ihl = static_cast<size_t>(ip[0] & 0x0Fu) * 4u;
    if (version != 4 || ihl < 20 || 14 + ihl > len) return changed;
    if (ip[9] != 17) return changed;  // only UDP/BOOTP carries chaddr/options

    const uint32_t src_ipv4 = rd32(ip + 12);
    const uint32_t dst_ipv4 = rd32(ip + 16);
    const size_t udp_off = 14 + ihl;
    if (udp_off + 8 > len) return changed;

    const uint16_t src_port = rd16(frame + udp_off + 0);
    const uint16_t dst_port = rd16(frame + udp_off + 2);
    if (!(src_port == 67 || src_port == 68 || dst_port == 67 ||
          dst_port == 68))
        return changed;  // not DHCP/BOOTP

    const uint16_t udp_declared_len = rd16(frame + udp_off + 4);
    const size_t udp_avail = len - udp_off;
    const size_t udp_len =
        std::min<size_t>(udp_declared_len, udp_avail);
    if (udp_len < 8) return changed;

    const size_t bootp_off = udp_off + 8;
    const size_t bootp_len = udp_len - 8;
    bool payload_changed = false;

    // chaddr (client hardware address), offset 28, length 16 -- only the
    // first 6 bytes are a MAC when htype==1 (Ethernet) && hlen==6, the
    // overwhelmingly common real-world case and the only one this project
    // itself ever produces.
    if (bootp_len >= 34 && have_identity) {
        const uint8_t htype = frame[bootp_off + 1];
        const uint8_t hlen = frame[bootp_off + 2];
        if (htype == 1 && hlen == 6) {
            uint8_t* chaddr = frame + bootp_off + 28;
            if (std::memcmp(chaddr, identity.data(), 6) == 0 &&
                std::memcmp(chaddr, synthetic.data(), 6) != 0) {
                std::memcpy(chaddr, synthetic.data(), 6);
                payload_changed = true;
            }
        }
    }

    // Options (magic cookie at +236 relative to bootp_off, RFC 2131).
    const size_t cookie = bootp_off + 236;
    const size_t bootp_end = std::min(bootp_off + bootp_len, len);
    if (cookie + 4 <= bootp_end && frame[cookie] == 0x63 &&
        frame[cookie + 1] == 0x82 && frame[cookie + 2] == 0x53 &&
        frame[cookie + 3] == 0x63) {
        size_t p = cookie + 4;
        while (p < bootp_end) {
            const uint8_t code = frame[p];
            if (code == 0xFFu) break;             // End
            if (code == 0x00u) { ++p; continue; }  // Pad
            if (p + 1 >= bootp_end) break;
            const uint8_t opt_len = frame[p + 1];
            if (p + 2u + opt_len > bootp_end) break;
            uint8_t* val = frame + p + 2;

            if (have_identity && code == 61 && opt_len == 7 && val[0] == 1 &&
                std::memcmp(val + 1, identity.data(), 6) == 0) {
                // Standard "htype=1(Ethernet) + 6-byte MAC" client-id
                // shape, and it IS the learned identity -- remap through
                // the SAME synthetic value as every other occurrence.
                if (std::memcmp(val + 1, synthetic.data(), 6) != 0) {
                    std::memcpy(val + 1, synthetic.data(), 6);
                    payload_changed = true;
                }
            } else if (code == 12 && opt_len > 0) {
                const std::vector<uint8_t> repl =
                    state.MapHostname(val, opt_len);
                if (std::memcmp(val, repl.data(), opt_len) != 0) {
                    std::memcpy(val, repl.data(), opt_len);
                    payload_changed = true;
                }
            }
            // Any other option-61 shape (not the standard MAC client-id)
            // is deliberately left untouched: this project's own DHCP
            // client only ever produces the standard shape (confirmed by
            // direct capture), and guessing at which bytes of an
            // arbitrary opaque identifier are "the sensitive part"
            // without the MAC-shape structure risks exactly the same
            // class of protocol-breaking mis-rewrite this redesign
            // exists to eliminate -- see net_sanitize.h's design note.
            p += 2u + opt_len;
        }
    }

    if (payload_changed) {
        changed = true;
        const uint16_t orig_checksum = rd16(frame + udp_off + 6);
        // A zero checksum means "checksum disabled" (RFC 768, legal for
        // unicast IPv4 UDP) -- preserve that, don't synthesize one.
        if (orig_checksum != 0) {
            const uint16_t new_checksum =
                ComputeUdpChecksum(src_ipv4, dst_ipv4, frame + udp_off, udp_len);
            wr16(frame + udp_off + 6, new_checksum);
        }
    }
    return changed;
}

void net_desanitize_ethernet_frame_for_replay(uint8_t* frame, size_t len,
                                                const uint8_t synthetic_mac[6],
                                                const uint8_t real_mac[6]) {
    if (!frame || len < 14) return;

    // Same field-position walk as net_sanitize_ethernet_frame above, but
    // the rewrite rule is a plain "if these 6 bytes are exactly
    // synthetic_mac, replace with real_mac" substitution instead of a
    // stateful hash-derived mapping -- see this function's doc comment in
    // net_sanitize.h for why that is enough (only one identity per
    // session, already known). Since the forward function now only ever
    // rewrites occurrences of the ONE learned identity MAC, no other MAC
    // in a replayed frame (broadcast, multicast, the AP, the virtual DHCP
    // server) can ever equal synthetic_mac, so this substitution is safe
    // without any additional broadcast/multicast special-casing.
    auto restore_mac = [&](uint8_t* p) {
        if (std::memcmp(p, synthetic_mac, 6) == 0)
            std::memcpy(p, real_mac, 6);
    };

    restore_mac(frame + 0);  // Ethernet dst
    restore_mac(frame + 6);  // Ethernet src

    const uint16_t ethertype = rd16(frame + 12);

    if (ethertype == 0x0806) {
        if (len >= 14 + 28) {
            uint8_t* arp = frame + 14;
            restore_mac(arp + 8);
            restore_mac(arp + 18);
        }
        return;
    }

    if (ethertype != 0x0800 || len < 14 + 20) return;

    const uint8_t* ip = frame + 14;
    const uint8_t version = static_cast<uint8_t>(ip[0] >> 4);
    const size_t ihl = static_cast<size_t>(ip[0] & 0x0Fu) * 4u;
    if (version != 4 || ihl < 20 || 14 + ihl > len) return;
    if (ip[9] != 17) return;

    const uint32_t src_ipv4 = rd32(ip + 12);
    const uint32_t dst_ipv4 = rd32(ip + 16);
    const size_t udp_off = 14 + ihl;
    if (udp_off + 8 > len) return;

    const uint16_t src_port = rd16(frame + udp_off + 0);
    const uint16_t dst_port = rd16(frame + udp_off + 2);
    if (!(src_port == 67 || src_port == 68 || dst_port == 67 || dst_port == 68))
        return;

    const uint16_t udp_declared_len = rd16(frame + udp_off + 4);
    const size_t udp_avail = len - udp_off;
    const size_t udp_len = std::min<size_t>(udp_declared_len, udp_avail);
    if (udp_len < 8) return;

    const size_t bootp_off = udp_off + 8;
    const size_t bootp_len = udp_len - 8;
    bool payload_changed = false;

    if (bootp_len >= 34) {
        const uint8_t htype = frame[bootp_off + 1];
        const uint8_t hlen = frame[bootp_off + 2];
        if (htype == 1 && hlen == 6) {
            uint8_t* chaddr = frame + bootp_off + 28;
            if (std::memcmp(chaddr, synthetic_mac, 6) == 0) {
                std::memcpy(chaddr, real_mac, 6);
                payload_changed = true;
            }
        }
    }

    const size_t cookie = bootp_off + 236;
    const size_t bootp_end = std::min(bootp_off + bootp_len, len);
    if (cookie + 4 <= bootp_end && frame[cookie] == 0x63 &&
        frame[cookie + 1] == 0x82 && frame[cookie + 2] == 0x53 &&
        frame[cookie + 3] == 0x63) {
        size_t p = cookie + 4;
        while (p < bootp_end) {
            const uint8_t code = frame[p];
            if (code == 0xFFu) break;
            if (code == 0x00u) { ++p; continue; }
            if (p + 1 >= bootp_end) break;
            const uint8_t opt_len = frame[p + 1];
            if (p + 2u + opt_len > bootp_end) break;
            uint8_t* val = frame + p + 2;

            if (code == 61 && opt_len == 7 && val[0] == 1 &&
                std::memcmp(val + 1, synthetic_mac, 6) == 0) {
                std::memcpy(val + 1, real_mac, 6);
                payload_changed = true;
            }
            p += 2u + opt_len;
        }
    }

    if (payload_changed) {
        const uint16_t orig_checksum = rd16(frame + udp_off + 6);
        if (orig_checksum != 0) {
            const uint16_t new_checksum =
                ComputeUdpChecksum(src_ipv4, dst_ipv4, frame + udp_off, udp_len);
            wr16(frame + udp_off + 6, new_checksum);
        }
    }
}
