#include "firmware_user_settings.h"

#include <cstring>

namespace {

// The firmware header stores the user-settings offset >> 3 at 0x20 (GBATek
// "DS Firmware Header" / melonDS FirmwareHeader::UserSettingsOffset). Two
// redundant 0x100-byte copies live there; the console picks the one with the
// higher Version counter, so EVERY patch below must touch BOTH or the guest
// can silently read the un-patched one.
bool user_settings_base(const std::vector<uint8_t>& fw, size_t* out) {
    if (fw.size() < 0x22u) return false;
    const size_t user = size_t{fw[0x20]} << 3u | size_t{fw[0x21]} << 11u;
    if (user + 0x200u > fw.size()) return false;
    *out = user;
    return true;
}

void put16(std::vector<uint8_t>& fw, size_t off, uint16_t value) {
    fw[off] = static_cast<uint8_t>(value);
    fw[off + 1u] = static_cast<uint8_t>(value >> 8u);
}

uint16_t get16(const std::vector<uint8_t>& fw, size_t off) {
    return static_cast<uint16_t>(uint16_t{fw[off]} |
                                 (uint16_t{fw[off + 1u]} << 8u));
}

// Nickname layout inside one user-settings copy.
constexpr size_t kNameOffset = 0x06u;    // 10 UTF-16LE code units
constexpr size_t kNameLenOffset = 0x1Au;  // length in CHARACTERS
constexpr size_t kChecksumOffset = 0x72u;
constexpr size_t kChecksumLength = 0x70u;
constexpr size_t kMaxNameChars = 10u;

}  // namespace

uint16_t nds_firmware_crc16(const uint8_t* data, size_t len, uint32_t start) {
    static constexpr uint16_t kPoly[8] = {
        0xC0C1u, 0xC181u, 0xC301u, 0xC601u,
        0xCC01u, 0xD801u, 0xF001u, 0xA001u,
    };
    for (size_t i = 0; i < len; ++i) {
        start ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            if (start & 1u)
                start = (start >> 1u) ^ (uint32_t{kPoly[bit]} << (7u - bit));
            else
                start >>= 1u;
        }
    }
    return static_cast<uint16_t>(start);
}

// melonDS exposes host screen pixels through a deterministic, ideal DS touch
// calibration. It applies this to both redundant user-settings blocks before
// the firmware CPU reads them. Mirror that in our private in-memory image so
// an input coordinate means the same thing on both sides; never alter the dump
// on disk (main.cpp's SHA-1 check always verifies the original bytes first).
bool nds_normalize_touch_calibration(std::vector<uint8_t>& fw) {
    size_t user = 0;
    if (!user_settings_base(fw, &user)) return false;
    for (unsigned copy = 0; copy < 2; ++copy) {
        const size_t base = user + size_t{copy} * 0x100u;
        put16(fw, base + 0x58u, 0u);          // ADC1 X
        put16(fw, base + 0x5Au, 0u);          // ADC1 Y
        fw[base + 0x5Cu] = 0u;                // pixel1 X
        fw[base + 0x5Du] = 0u;                // pixel1 Y
        put16(fw, base + 0x5Eu, 255u << 4u);  // ADC2 X
        put16(fw, base + 0x60u, 191u << 4u);  // ADC2 Y
        fw[base + 0x62u] = 255u;              // pixel2 X
        fw[base + 0x63u] = 191u;              // pixel2 Y
        put16(fw, base + kChecksumOffset,
              nds_firmware_crc16(fw.data() + base, kChecksumLength, 0xFFFFu));
    }
    return true;
}

bool nds_apply_startup_mode(std::vector<uint8_t>& fw, NdsStartupMode mode) {
    if (mode == NdsStartupMode::Preserve) return true;
    size_t user = 0;
    if (!user_settings_base(fw, &user)) return false;
    for (unsigned copy = 0; copy < 2; ++copy) {
        const size_t base = user + size_t{copy} * 0x100u;
        uint16_t language_and_flags = get16(fw, base + 0x64u);
        if (mode == NdsStartupMode::Automatic)
            language_and_flags |= 1u << 6u;
        else
            language_and_flags &= ~(1u << 6u);
        put16(fw, base + 0x64u, language_and_flags);
        put16(fw, base + kChecksumOffset,
              nds_firmware_crc16(fw.data() + base, kChecksumLength, 0xFFFFu));
    }
    return true;
}

// Wiimmfi (beads-yjp.1.11): two ndsrecomp instances booted from the SAME
// firmware dump present the SAME console MAC, so Wiimmfi identity and DS
// friend codes -- both derived from it -- collide: two such clients look
// like one console appearing twice and cannot match with each other
// online. This mirrors melonDS's own proven fix for exactly this problem
// (multi-instance local testing) byte-for-byte -- see
// ndsref/third_party/melonDS/src/frontend/qt_sdl/EmuInstance.cpp:1739-1752
// -- rather than inventing a new perturbation scheme: add the instance
// index into MAC bytes 3/4/5 (melonDS's own wrap-mod-256 u8 arithmetic,
// reproduced here with explicit truncating casts), then mask byte 0 so
// the result can never be a broadcast/multicast address.
//
// Instance 0 is a deliberate, total no-op (early return): the owner
// chose LLE-faithful as the default, so the guest reads its REAL MAC off
// the real firmware dump over its own ordinary SPI path, unperturbed,
// exactly like a physical console. Only a nonzero --instance-index
// perturbs anything.
//
// The MAC lives inside the firmware HEADER's Wi-Fi calibration block
// (GBATek "DS Firmware Header" / melonDS's SPI_Firmware.h
// FirmwareHeader::MacAddr), a completely different region with a
// DIFFERENT checksum than the per-boot user-settings block the other
// patches touch (UserData::Checksum, CRC16 start 0xFFFF over 0x70 bytes).
// The header's own WifiConfigChecksum (offset 0x2A) instead covers
// WifiConfigLength bytes (a value stored IN the header, read here rather
// than hardcoded -- default firmware ships 0x138) starting at 0x2C (the
// length field itself is inside its own checksummed range), CRC16 start
// 0x0000 -- confirmed against melonDS's own verification call,
// SPI.cpp:99: `VerifyCRC16(0x0000, 0x2C, *(u16*)&Buffer[0x2C], 0x2A)`.
bool nds_apply_instance_mac(std::vector<uint8_t>& fw, uint32_t instance_index) {
    if (instance_index == 0) return true;  // instance 0: LLE-faithful no-op
    constexpr size_t kMacOffset = 0x36u;
    constexpr size_t kWifiConfigChecksumOffset = 0x2Au;
    constexpr size_t kWifiConfigLenOffset = 0x2Cu;
    if (fw.size() < kWifiConfigLenOffset + 2u) return false;
    const uint16_t wifi_config_length = get16(fw, kWifiConfigLenOffset);
    if (kWifiConfigLenOffset + wifi_config_length > fw.size()) return false;
    if (kMacOffset + 6u > kWifiConfigLenOffset + wifi_config_length)
        return false;  // MacAddr must fall inside the checksummed region

    uint8_t mac[6];
    std::memcpy(mac, fw.data() + kMacOffset, 6u);
    // Exact melonDS perturbation (EmuInstance.cpp:1739-1752): u8 arithmetic
    // wraps mod 256 on both sides, reproduced here with explicit truncating
    // casts since these are plain uint8_t.
    mac[3] = static_cast<uint8_t>(mac[3] + instance_index);
    mac[4] = static_cast<uint8_t>(mac[4] + instance_index * 0x44u);
    mac[5] = static_cast<uint8_t>(mac[5] + instance_index * 0x10u);
    mac[0] &= 0xFCu;  // never a broadcast/multicast address
    std::memcpy(fw.data() + kMacOffset, mac, 6u);

    const uint16_t crc = nds_firmware_crc16(
        fw.data() + kWifiConfigLenOffset, wifi_config_length, 0u);
    fw[kWifiConfigChecksumOffset] = static_cast<uint8_t>(crc);
    fw[kWifiConfigChecksumOffset + 1u] = static_cast<uint8_t>(crc >> 8u);
    return true;
}

bool nds_validate_player_name(const std::string& name, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (name.empty())
        return fail("player name must not be empty");
    if (name.size() > kMaxNameChars)
        return fail("player name must be at most 10 characters "
                    "(the DS firmware nickname limit)");
    // Deliberately a SUBSET of printable ASCII. The DS stores UTF-16, but the
    // name has to survive a command line, an .ini round trip, and a peer's
    // font: shell metacharacters and quoting characters are rejected rather
    // than escaped, and non-ASCII is rejected rather than transliterated.
    // Nothing here ever truncates -- an unrepresentable name is an error,
    // because the name a player typed is the name their peers must see.
    static const char kExtraAllowed[] = " -_.,!?'()[]{}+=@#&*:;/";
    for (const char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool alnum = (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') ||
                           (u >= 'a' && u <= 'z');
        if (alnum) continue;
        if (std::strchr(kExtraAllowed, c) != nullptr && c != '\0') continue;
        return fail("player name may contain only letters, digits, spaces, "
                    "and the punctuation -_.,!?'()[]{}+=@#&*:;/");
    }
    if (name.front() == ' ' || name.back() == ' ')
        return fail("player name must not start or end with a space");
    return true;
}

bool nds_apply_player_name(std::vector<uint8_t>& fw, const std::string& name) {
    // Unset means "leave the firmware's own name alone": a retail dump keeps
    // the real console's nickname (LLE-faithful), a generated image keeps its
    // "ndsrecomp" default (generated_firmware.cpp).
    if (name.empty()) return true;
    if (!nds_validate_player_name(name, nullptr)) return false;
    size_t user = 0;
    if (!user_settings_base(fw, &user)) return false;
    for (unsigned copy = 0; copy < 2; ++copy) {
        const size_t base = user + size_t{copy} * 0x100u;
        // Zero-pad the whole 10-slot field: a shorter new name must not leave
        // trailing code units of the old one behind for anything that reads
        // the array without honouring NameLength.
        for (size_t i = 0; i < kMaxNameChars; ++i) {
            const uint16_t unit = i < name.size()
                ? static_cast<uint16_t>(static_cast<unsigned char>(name[i]))
                : uint16_t{0};
            put16(fw, base + kNameOffset + i * 2u, unit);
        }
        put16(fw, base + kNameLenOffset, static_cast<uint16_t>(name.size()));
        put16(fw, base + kChecksumOffset,
              nds_firmware_crc16(fw.data() + base, kChecksumLength, 0xFFFFu));
    }
    return true;
}

bool nds_read_player_name(const std::vector<uint8_t>& fw, unsigned copy,
                          std::string* out) {
    if (!out || copy > 1u) return false;
    size_t user = 0;
    if (!user_settings_base(fw, &user)) return false;
    const size_t base = user + size_t{copy} * 0x100u;
    const uint16_t length = get16(fw, base + kNameLenOffset);
    if (length > kMaxNameChars) return false;
    std::string text;
    text.reserve(length);
    for (uint16_t i = 0; i < length; ++i) {
        const uint16_t unit = get16(fw, base + kNameOffset + size_t{i} * 2u);
        if (unit < 0x20u || unit > 0x7Eu) return false;  // not ASCII-writable
        text.push_back(static_cast<char>(unit));
    }
    *out = text;
    return true;
}
