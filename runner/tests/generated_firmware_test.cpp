// Pins the generated firmware image against melonDS Firmware::Firmware(0),
// the oracle it must match byte for byte (the ndsref debug server's
// firmware_dump command produces the golden image; the harness diff ran at
// landing time and the structural facts it validated are pinned here).
//
// Invoked with an argument, writes the image (melonDS default MAC) to that
// path instead, so the oracle byte-diff can be re-run at any time:
//   generated_firmware_test out.bin
//   ndsref --generated-firmware --boot direct ... -> firmware_dump
//   fc /b out.bin oracle.bin

#include "generated_firmware.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// 32-bit accumulator — the table terms overflow 16 bits and their high bits
// shift back into range (matches the CRCs a real firmware dump stores).
uint16_t crc16(const uint8_t* data, uint32_t length, uint32_t start) {
    static constexpr uint16_t kPolynomial[8] = {
        0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001,
    };
    for (uint32_t i = 0; i < length; ++i) {
        start ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            start = (start & 1u)
                ? (start >> 1) ^ (uint32_t{kPolynomial[bit]} << (7 - bit))
                : start >> 1;
        }
    }
    return uint16_t(start);
}

uint16_t load16(const uint8_t* p) {
    return uint16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}

// melonDS's default MAC — used here ONLY to reproduce the oracle image for
// diffing. The runner never ships it as an identity (beads-yjp.1.11).
constexpr uint8_t kOracleMac[6] = {0x00, 0x09, 0xBF, 0x11, 0x22, 0x33};

}  // namespace

int main(int argc, char** argv) {
    const auto fw = nds_generate_firmware(kOracleMac);

    if (argc > 1) {
        std::FILE* f = std::fopen(argv[1], "wb");
        if (!f) return 2;
        std::fwrite(fw.data(), 1, fw.size(), f);
        std::fclose(f);
        return 0;
    }

    check(fw.size() == 0x20000, "image is 128 KB");
    check(std::memcmp(fw.data() + 0x08, "MELN", 4) == 0,
          "carries the generated-firmware identifier (non-bootable)");
    check(fw[0x1D] == 0x20, "console type DS Lite");
    check(load16(fw.data() + 0x20) == (0x1FE00 >> 3),
          "user settings live at 0x1FE00");
    check(std::memcmp(fw.data() + 0x36, kOracleMac, 6) == 0,
          "the caller's MAC lands in the header");
    check(load16(fw.data() + 0x2A) == crc16(fw.data() + 0x2C, 0x138, 0x0000),
          "wifi config checksum is valid");
    check(fw[0x2FF] == 0x80, "boot0 selects NAND as the stage2 medium");

    for (uint32_t copy = 0; copy < 2; ++copy) {
        const uint8_t* u = fw.data() + 0x1FE00 + copy * 0x100;
        check(load16(u + 0x72) == crc16(u, 0x70, 0xFFFF),
              "user settings checksum is valid");
        check(load16(u + 0x00) == 5, "user settings version 5");
        check(load16(u + 0x1A) == 7, "nickname length 7");
        check(load16(u + 0x64) == 0x31, "English, max backlight");
    }

    const uint8_t* ap = fw.data() + 0x1FA00;
    check(std::memcmp(ap + 0x40, "melonAP", 7) == 0, "AP1 SSID");
    check(ap[0xE7] == 0x00, "AP1 configured");
    for (uint32_t slot = 0; slot < 3; ++slot) {
        const uint8_t* a = fw.data() + 0x1FA00 + slot * 0x100;
        check(load16(a + 0xFE) == crc16(a, 0xFE, 0x0000),
              "access point checksum is valid");
        check(a[0xEF] == 0x01, "connection configured");
    }
    check(fw[0x1FA00 + 0x1E7] == 0xFF && fw[0x1FA00 + 0x2E7] == 0xFF,
          "AP2/AP3 unconfigured");

    // Unwritten space keeps the erased-flash fill.
    check(fw[0x300] == 0xFF && fw[0x10000] == 0xFF && fw[0x1F9FF] == 0xFF,
          "unwritten regions stay 0xFF");

    // A different MAC changes exactly the header identity + its checksum.
    constexpr uint8_t other_mac[6] = {0x00, 0x09, 0xBF, 0xAA, 0xBB, 0xCC};
    const auto fw2 = nds_generate_firmware(other_mac);
    check(std::memcmp(fw2.data() + 0x36, other_mac, 6) == 0,
          "identity MAC is caller-controlled");
    check(std::memcmp(fw.data() + 0x1FA00, fw2.data() + 0x1FA00, 0x600) == 0,
          "MAC does not leak into AP or user settings blocks");

    return g_failures == 0 ? 0 : 1;
}
