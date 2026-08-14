#include "generated_firmware.h"

#include <cstring>

// Byte-for-byte mirror of melonDS Firmware::Firmware(0) — see the header.
// Constants and layout follow melonDS SPI_Firmware.{h,cpp}; offsets are the
// flattened positions of its FirmwareHeader / WifiAccessPoint / UserData
// struct fields, cross-checked against GBATEK's firmware chapters.

namespace {

// Wi-Fi calibration defaults (melonDS BBINIT / RFINIT / CHANDATA — hardware
// calibration data, identical across retail DS Lite units).
constexpr uint8_t kBbInit[0x69] = {
    0x03, 0x17, 0x40, 0x00, 0x1B, 0x6C, 0x48, 0x80, 0x38, 0x00, 0x35, 0x07,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB0, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xC7, 0xBB, 0x01, 0x24, 0x7F, 0x5A, 0x01, 0x3F, 0x01,
    0x3F, 0x36, 0x1D, 0x00, 0x78, 0x35, 0x55, 0x12, 0x34, 0x1C, 0x00, 0x01,
    0x0E, 0x38, 0x03, 0x70, 0xC5, 0x2A, 0x0A, 0x08, 0x04, 0x01, 0x00, 0x00,
    0x00, 0xFF, 0xFF, 0xFE, 0xFE, 0xFE, 0xFE, 0xFC, 0xFC, 0xFA, 0xFA, 0xFA,
    0xFA, 0xFA, 0xF8, 0xF8, 0xF6, 0x00, 0x12, 0x14, 0x12, 0x41, 0x23, 0x03,
    0x04, 0x70, 0x35, 0x0E, 0x2C, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x12, 0x28, 0x1C,
};

constexpr uint8_t kRfInit[0x29] = {
    0x31, 0x4C, 0x4F, 0x21, 0x00, 0x10, 0xB0, 0x08, 0xFA, 0x15, 0x26, 0xE6,
    0xC1, 0x01, 0x0E, 0x50, 0x05, 0x00, 0x6D, 0x12, 0x00, 0x00, 0x01, 0xFF,
    0x0E, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06, 0x00, 0x00, 0x00,
    0x18, 0x00, 0x02, 0x00, 0x00,
};

constexpr uint8_t kChanData[0x3C] = {
    0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E,
    0x0E, 0x0E, 0x16, 0x26, 0x1C, 0x1C, 0x1C, 0x1D, 0x1D, 0x1D, 0x1E, 0x1E,
    0x1E, 0x1E, 0x1F, 0x1E, 0x1F, 0x18, 0x01, 0x4B, 0x4B, 0x4B, 0x4B, 0x4C,
    0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4C, 0x4D, 0x4D, 0x4D, 0x02, 0x6C, 0x71,
    0x76, 0x5B, 0x40, 0x45, 0x4A, 0x2F, 0x34, 0x39, 0x3E, 0x03, 0x08, 0x14,
};

// The InitialValues block at header +0x44: sixteen u16s exactly as
// melonDS FirmwareHeader::FirmwareHeader(0) stores them.
constexpr uint16_t kInitialValues[16] = {
    0x0002, 0x0017, 0x0026, 0x1818, 0x0048, 0x4840, 0x0058, 0x0042,
    0x0146, 0x8064, 0xE6E6, 0x2443, 0x000E, 0x0001, 0x0001, 0x0402,
};

// Firmware CRC16 (melonDS SPI.cpp CRC16 == CRC-16/MODBUS). The accumulator
// MUST be 32-bit: the table terms overflow 16 bits and their high bits shift
// back into range — a uint16_t accumulator computes a DIFFERENT (wrong)
// checksum on most inputs, verified against the CRCs a real firmware dump
// stores.
uint16_t crc16(const uint8_t* data, uint32_t length, uint32_t start) {
    static constexpr uint16_t kPolynomial[8] = {
        0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001,
    };
    for (uint32_t i = 0; i < length; ++i) {
        start ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            if (start & 1u)
                start = (start >> 1) ^ (uint32_t{kPolynomial[bit]} << (7 - bit));
            else
                start >>= 1;
        }
    }
    return static_cast<uint16_t>(start);
}

void put16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
}

// One 0x100-byte user-settings copy (melonDS UserData::UserData()).
void write_user_data(uint8_t* u) {
    std::memset(u, 0, 0x100);
    put16(u + 0x00, 5);                     // Version
    u[0x03] = 1;                            // BirthdayMonth
    u[0x04] = 1;                            // BirthdayDay
    static constexpr char16_t kName[] = u"melonDS";
    for (unsigned i = 0; i < 7; ++i) put16(u + 0x06 + i * 2, kName[i]);
    put16(u + 0x1A, 7);                     // NameLength
    // Ideal host touch calibration, exactly as melonDS FirmwareMem::Reset
    // stamps into every firmware before the guest reads it.
    put16(u + 0x58, 0);                     // ADC1 X
    put16(u + 0x5A, 0);                     // ADC1 Y
    u[0x5C] = 0; u[0x5D] = 0;               // pixel 1
    put16(u + 0x5E, 255 << 4);              // ADC2 X
    put16(u + 0x60, 191 << 4);              // ADC2 Y
    u[0x62] = 255; u[0x63] = 191;           // pixel 2
    put16(u + 0x64, 0x01 | (3u << 4));      // English | BacklightLevel::Max
    put16(u + 0x72, crc16(u, 0x70, 0xFFFF));
}

// One 0x100-byte access-point slot, mirroring melonDS's WifiAccessPoint(int)
// (the configured slot) vs WifiAccessPoint() (empty) — EXCEPT the SSID: the
// runner's emulated AP identifies as "ndsrecomp" (vendor/melonds/patches/
// 0001-wifi-ap-identity.patch renames melonDS's "melonAP"), and the guest
// associates with the SSID written HERE, so the two must agree or every
// AP scan ends in error 51099 "no compatible access point in range".
void write_access_point(uint8_t* ap, bool configured) {
    std::memset(ap, 0, 0x100);
    if (configured) {
        static constexpr char kSsid[] = "ndsrecomp";
        std::memcpy(ap + 0x40, kSsid, sizeof(kSsid) - 1);  // SSID
        ap[0xE7] = 0x00;                    // Status: Normal
    } else {
        ap[0xE7] = 0xFF;                    // Status: NotConfigured
    }
    ap[0xEF] = 0x01;                        // ConnectionConfigured
    put16(ap + 0xFE, crc16(ap, 0xFE, 0x0000));
}

}  // namespace

std::vector<uint8_t> nds_generate_firmware(const uint8_t mac[6]) {
    std::vector<uint8_t> fw(kNdsGeneratedFirmwareSize, 0xFF);
    uint8_t* h = fw.data();

    // ── Header (0x000..0x1FF), melonDS FirmwareHeader(0) ────────────────
    std::memset(h, 0, 0x200);
    static constexpr uint8_t kIdentifier[4] = {'M', 'E', 'L', 'N'};
    std::memcpy(h + 0x08, kIdentifier, 4);
    h[0x1D] = 0x20;                          // ConsoleType: DS Lite
    // UserSettingsOffset, stored >> 3: (0x7FE00 & (size-1)) = 0x1FE00.
    const uint32_t user_offset = 0x7FE00u & (kNdsGeneratedFirmwareSize - 1u);
    put16(h + 0x20, static_cast<uint16_t>(user_offset >> 3));
    put16(h + 0x2C, 0x138);                  // WifiConfigLength
    h[0x2F] = 6;                             // WifiVersion: W006
    static constexpr uint8_t kUnused3[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    std::memcpy(h + 0x30, kUnused3, 6);
    std::memcpy(h + 0x36, mac, 6);           // MacAddr — per-install identity
    put16(h + 0x3C, 0x3FFE);                 // EnabledChannels
    h[0x3E] = 0xFF; h[0x3F] = 0xFF;          // Unknown2
    h[0x40] = 0x03;                          // RFChipType: Type3
    h[0x41] = 0x94;                          // RFBitsPerEntry
    h[0x42] = 0x29;                          // RFEntries
    h[0x43] = 0x02;                          // Unknown3
    for (unsigned i = 0; i < 16; ++i)
        put16(h + 0x44 + i * 2, kInitialValues[i]);
    std::memcpy(h + 0x64, kBbInit, sizeof(kBbInit));   // InitialBBValues
    std::memcpy(h + 0xCE, kRfInit, sizeof(kRfInit));   // Type3 RF values
    h[0xF7] = 0x02;                          // BBIndicesPerChannel
    std::memcpy(h + 0xF8, kChanData, sizeof(kChanData));
    std::memset(h + 0x134, 0xFF, 46);        // Type3Config.Unused0
    // WifiConfigChecksum over [0x2C, 0x2C + WifiConfigLength).
    put16(h + 0x2A, crc16(h + 0x2C, 0x138, 0x0000));

    fw[0x2FF] = 0x80;                        // boot0: NAND as stage2 medium

    // ── Access points at user settings - 0x400 ──────────────────────────
    write_access_point(fw.data() + user_offset - 0x400, true);
    write_access_point(fw.data() + user_offset - 0x300, false);
    write_access_point(fw.data() + user_offset - 0x200, false);

    // ── Two user-settings copies ─────────────────────────────────────────
    write_user_data(fw.data() + user_offset);
    write_user_data(fw.data() + user_offset + 0x100);

    return fw;
}
