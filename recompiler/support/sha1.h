// sha1.h — minimal SHA-1 (FIPS 180-4). Used by the BIOS loader and
// ROM hash verifier. We don't depend on OpenSSL/CryptoAPI; this keeps
// the recompiler tools portable across Windows/Linux/Mac.
//
// Matches the same in-tree pattern used by psxrecomp's main_bios.cpp.

#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <string>

namespace gba {

struct Sha1Digest {
    std::array<uint8_t, 20> bytes{};
    std::string hex() const;
};

// Hash a byte buffer.
Sha1Digest sha1(const void* data, std::size_t len);

}  // namespace gba
