// sha1.cpp — straight FIPS 180-4 SHA-1.

#include "sha1.h"

#include <cstdio>
#include <cstring>

namespace gba {

namespace {

inline uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

void process_block(uint32_t h[5], const uint8_t* block) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4 + 0]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) <<  8) |
               (static_cast<uint32_t>(block[i * 4 + 3])      );
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
        uint32_t t = rotl32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl32(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

}  // namespace

std::string Sha1Digest::hex() const {
    char buf[41];
    for (int i = 0; i < 20; ++i) {
        std::snprintf(buf + i * 2, 3, "%02x", bytes[i]);
    }
    buf[40] = '\0';
    return std::string(buf);
}

Sha1Digest sha1(const void* data, std::size_t len) {
    uint32_t h[5] = {
        0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u,
    };

    const uint8_t* p = static_cast<const uint8_t*>(data);
    std::size_t full_blocks = len / 64;
    for (std::size_t i = 0; i < full_blocks; ++i) {
        process_block(h, p + i * 64);
    }

    // Pad final block(s).
    uint8_t tail[128] = {0};
    std::size_t rem = len - full_blocks * 64;
    std::memcpy(tail, p + full_blocks * 64, rem);
    tail[rem] = 0x80;
    std::size_t pad_len = (rem < 56) ? 64 : 128;
    uint64_t bit_len = static_cast<uint64_t>(len) * 8;
    for (int i = 0; i < 8; ++i) {
        tail[pad_len - 1 - i] = static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFF);
    }
    process_block(h, tail);
    if (pad_len == 128) process_block(h, tail + 64);

    Sha1Digest out{};
    for (int i = 0; i < 5; ++i) {
        out.bytes[i * 4 + 0] = static_cast<uint8_t>((h[i] >> 24) & 0xFF);
        out.bytes[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xFF);
        out.bytes[i * 4 + 2] = static_cast<uint8_t>((h[i] >>  8) & 0xFF);
        out.bytes[i * 4 + 3] = static_cast<uint8_t>((h[i]      ) & 0xFF);
    }
    return out;
}

}  // namespace gba
