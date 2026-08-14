#include "direct_boot.h"

#include <cstring>

// Sequence and constants follow melonDS NDS::SetupDirectBoot (NDS.cpp) and
// FirmwareMem::SetupDirectBoot (SPI.cpp) -- the oracle we diff against, so the
// values are deliberately reproduced rather than re-derived.

namespace {

constexpr uint32_t kHeaderMirrorSize = 0x170;
constexpr uint32_t kSecureAreaSize = 0x800;
constexpr uint32_t kUserSettingsSize = 0x70;

// NDS cartridge header offsets (GBATEK; field order cross-checked against
// melonDS NDS_Header.h).
constexpr uint32_t kOffArm9RomOffset = 0x20;
constexpr uint32_t kOffArm9Entry = 0x24;
constexpr uint32_t kOffArm9RamAddress = 0x28;
constexpr uint32_t kOffArm9Size = 0x2C;
constexpr uint32_t kOffArm7RomOffset = 0x30;
constexpr uint32_t kOffArm7Entry = 0x34;
constexpr uint32_t kOffArm7RamAddress = 0x38;
constexpr uint32_t kOffArm7Size = 0x3C;
constexpr uint32_t kOffSecureAreaCrc = 0x6C;
constexpr uint32_t kOffNintendoLogo = 0xC0;
constexpr uint32_t kOffHeaderCrc = 0x15E;
constexpr uint32_t kNintendoLogoSize = 156;

// Firmware header offsets (melonDS FirmwareHeader field order: the GUI/wifi
// code checksum is the THIRD u16 of the header, not near the user-settings
// block -- getting this wrong put 0xFFFF in the 0x027FF876 mirror and was
// caught by the boot-state diff against the oracle).
constexpr uint32_t kFwUserSettingsOffset = 0x20;
constexpr uint32_t kFwDataGfxChecksum = 0x26;
constexpr uint32_t kFwGuiWifiChecksum = 0x04;

uint16_t load16(const uint8_t* p) {
    return static_cast<uint16_t>(uint16_t{p[0]} | (uint16_t{p[1]} << 8));
}

uint32_t load32(const uint8_t* p) {
    return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) |
           (uint32_t{p[3]} << 24);
}

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

// Firmware CRC16 (start 0xFFFF), the checksum both user-settings copies carry.
uint16_t crc16(const uint8_t* data, uint32_t length, uint16_t start) {
    static constexpr uint16_t kPolynomial[8] = {
        0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001,
    };
    for (uint32_t i = 0; i < length; ++i) {
        start = static_cast<uint16_t>(start ^ data[i]);
        for (unsigned bit = 0; bit < 8; ++bit) {
            if (start & 1u) {
                start = static_cast<uint16_t>(
                    (start >> 1) ^ (kPolynomial[bit] << (7 - bit)));
            } else {
                start = static_cast<uint16_t>(start >> 1);
            }
        }
    }
    return start;
}

// melonDS Firmware::UserData::ChecksumValid: the base 0x70-byte block must
// match its CRC at +0x72, and when the DSi extended block is flagged in use
// (byte +0x74 == 1) that block must match its own CRC at +0xFE too.
bool user_data_checksum_valid(const uint8_t* copy) {
    const bool base_ok = load16(copy + 0x72) == crc16(copy, 0x70, 0xFFFF);
    const bool extended_ok =
        copy[0x74] != 1 ||
        load16(copy + 0xFE) == crc16(copy + 0x74, 0x8A, 0xFFFF);
    return base_ok && extended_ok;
}

// melonDS Firmware::GetEffectiveUserData: the CRC-valid copy, the higher
// update counter when both are valid (ties keep copy 0), copy 0 when
// neither validates.
const uint8_t* effective_user_data(const uint8_t* copy0) {
    const uint8_t* copy1 = copy0 + 0x100;
    const bool ok0 = user_data_checksum_valid(copy0);
    const bool ok1 = user_data_checksum_valid(copy1);
    if (ok0 && ok1)
        return load16(copy0 + 0x70) >= load16(copy1 + 0x70) ? copy0 : copy1;
    if (ok1) return copy1;
    return copy0;
}

// Post-firmware CP15 state, as (crn, crm, op2, value). The MPU regions and TCM
// placement here are what the firmware's own boot code would have programmed.
struct Cp15Setting {
    uint32_t crn, crm, op2, value;
};
constexpr Cp15Setting kCp15Settings[] = {
    {1, 0, 0, 0x00012078},
    {2, 0, 0, 0x00000042}, {2, 0, 1, 0x00000042},
    {3, 0, 0, 0x00000002},
    {5, 0, 2, 0x15111011}, {5, 0, 3, 0x05100011},
    {6, 0, 0, 0x04000033}, {6, 0, 1, 0x04000033},
    {6, 1, 0, 0x0200002B}, {6, 1, 1, 0x0200002B},
    {6, 2, 0, 0x00000000}, {6, 2, 1, 0x00000000},
    {6, 3, 0, 0x08000035}, {6, 3, 1, 0x08000035},
    {6, 4, 0, 0x0300001B}, {6, 4, 1, 0x0300001B},
    {6, 5, 0, 0x00000000}, {6, 5, 1, 0x00000000},
    {6, 6, 0, 0xFFFF001D}, {6, 6, 1, 0xFFFF001D},
    {6, 7, 0, 0x027FF017}, {6, 7, 1, 0x027FF017},
    {9, 1, 0, 0x0300000A}, {9, 1, 1, 0x00000020},
};

}  // namespace

bool nds_direct_boot(NdsDirectBootMachine& machine,
                     const NdsDirectBootInputs& inputs, std::string* error) {
    if (!inputs.rom || inputs.rom_size < 0x200)
        return fail(error, "cartridge image is missing or truncated");
    if (!inputs.firmware || inputs.firmware_size < 0x200)
        return fail(error, "firmware image is missing or truncated");

    const uint8_t* rom = inputs.rom;
    const uint32_t arm9_rom_offset = load32(rom + kOffArm9RomOffset);
    const uint32_t arm9_entry = load32(rom + kOffArm9Entry);
    const uint32_t arm9_ram = load32(rom + kOffArm9RamAddress);
    const uint32_t arm9_size = load32(rom + kOffArm9Size);
    const uint32_t arm7_rom_offset = load32(rom + kOffArm7RomOffset);
    const uint32_t arm7_entry = load32(rom + kOffArm7Entry);
    const uint32_t arm7_ram = load32(rom + kOffArm7RamAddress);
    const uint32_t arm7_size = load32(rom + kOffArm7Size);

    // Bounds-check both binaries before writing anything: a half-copied machine
    // is far harder to diagnose than a refusal.
    if (uint64_t{arm9_rom_offset} + arm9_size > inputs.rom_size)
        return fail(error, "cartridge ARM9 binary runs past the image");
    if (uint64_t{arm7_rom_offset} + arm7_size > inputs.rom_size)
        return fail(error, "cartridge ARM7 binary runs past the image");
    if ((arm9_size & 3u) || (arm7_size & 3u))
        return fail(error, "cartridge binary size is not word-aligned");

    machine.set_wramcnt(3);

    if (!inputs.arm9_bios_is_native) {
        machine.copy_logo_into_arm9_bios(rom + kOffNintendoLogo,
                                         kNintendoLogoSize);
    }

    // Header mirror the BIOS/firmware normally leaves in main RAM.
    for (uint32_t i = 0; i < kHeaderMirrorSize; i += 4)
        machine.write32(0, 0x027FFE00 + i, load32(rom + i));

    const uint16_t header_crc = load16(rom + kOffHeaderCrc);
    const uint16_t secure_crc = load16(rom + kOffSecureAreaCrc);
    // Chip ID exactly as the live card protocol reports it -- NitroSDK boot
    // code re-reads the chip ID and compares it against this mirror to
    // detect cartridge removal, so a fixed constant here sends the game
    // down its card-error path the moment the sizes disagree.
    const uint32_t cartid = inputs.cart_chip_id;

    machine.write32(0, 0x027FF800, cartid);
    machine.write32(0, 0x027FF804, cartid);
    machine.write16(0, 0x027FF808, header_crc);
    machine.write16(0, 0x027FF80A, secure_crc);
    machine.write16(0, 0x027FF850, 0x5835);

    machine.write32(0, 0x027FFC00, cartid);
    machine.write32(0, 0x027FFC04, cartid);
    machine.write16(0, 0x027FFC08, header_crc);
    machine.write16(0, 0x027FFC0A, secure_crc);
    machine.write16(0, 0x027FFC10, 0x5835);
    machine.write16(0, 0x027FFC30, 0xFFFF);
    machine.write16(0, 0x027FFC40, 0x0001);

    // The secure area is the first 0x800 bytes of the ARM9 binary when the
    // cartridge has one; it lands decrypted, then the rest copies verbatim.
    uint32_t arm9_start = 0;
    if (arm9_rom_offset >= 0x4000 && arm9_rom_offset < 0x8000) {
        if (!inputs.secure_area)
            return fail(error, "cartridge has a secure area but no plaintext");
        if (arm9_size < kSecureAreaSize)
            return fail(error, "cartridge ARM9 binary is shorter than its secure area");
        for (uint32_t i = 0; i < kSecureAreaSize; i += 4) {
            machine.write32(0, arm9_ram + i, load32(inputs.secure_area + i));
            arm9_start += 4;
        }
    }
    for (uint32_t i = arm9_start; i < arm9_size; i += 4)
        machine.write32(0, arm9_ram + i, load32(rom + arm9_rom_offset + i));
    for (uint32_t i = 0; i < arm7_size; i += 4)
        machine.write32(1, arm7_ram + i, load32(rom + arm7_rom_offset + i));

    machine.set_arm7_bios_prot(0x1204);

    // Firmware user settings the guest expects to find already mirrored in
    // RAM. The firmware keeps TWO 0x100-byte copies; the mirror carries the
    // EFFECTIVE one (melonDS Firmware::GetEffectiveUserData), so both full
    // copies must be in range even though only 0x70 bytes land in RAM.
    const uint32_t user_offset =
        uint32_t{load16(inputs.firmware + kFwUserSettingsOffset)} << 3;
    if (uint64_t{user_offset} + 0x200 > inputs.firmware_size)
        return fail(error, "firmware user-settings offset is out of range");
    const uint8_t* user_data =
        effective_user_data(inputs.firmware + user_offset);
    machine.write32(0, 0x027FF864, 0);
    machine.write32(0, 0x027FF868, user_offset);
    machine.write16(0, 0x027FF874, load16(inputs.firmware + kFwDataGfxChecksum));
    machine.write16(0, 0x027FF876, load16(inputs.firmware + kFwGuiWifiChecksum));
    for (uint32_t i = 0; i < kUserSettingsSize; i += 4)
        machine.write32(0, 0x027FFC80 + i, load32(user_data + i));

    for (const Cp15Setting& setting : kCp15Settings)
        machine.cp15_write(setting.crn, setting.crm, setting.op2, setting.value);

    machine.set_cpu_boot(0, arm9_entry, 0x03002F7C, 0x03003F80, 0x03003FC0);
    machine.set_cpu_boot(1, arm7_entry, 0x0380FD80, 0x0380FF80, 0x0380FFC0);

    machine.set_post_boot_latches();
    return true;
}
