// Pins the direct-boot sequence against melonDS NDS::SetupDirectBoot +
// FirmwareMem::SetupDirectBoot, which is the oracle we diff against.
//
// Direct boot is the one genuinely high-level step in the no-dump path, so it
// has no LLE run to be checked against from the inside -- if the constants
// drift, nothing else in the system notices until a game boots into garbage.
// Hence a recording machine and exact assertions on what gets written.

#include "direct_boot.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

struct Write { int cpu; uint32_t addr; uint32_t value; uint32_t width; };
struct Cp15 { uint32_t crn, crm, op2, value; };
struct CpuBoot { int cpu; uint32_t entry, sp, sp_irq, sp_svc; };

struct Recorder : NdsDirectBootMachine {
    std::vector<Write> writes;
    std::vector<Cp15> cp15;
    std::vector<CpuBoot> cpus;
    int wramcnt = -1;
    uint32_t bios_prot = 0;
    bool latches = false;
    uint32_t logo_bytes = 0;
    // Ordering matters: the latches statement must come after the copies.
    size_t writes_at_latches = 0;

    void write16(int cpu, uint32_t addr, uint16_t value) override {
        writes.push_back({cpu, addr, value, 16});
    }
    void write32(int cpu, uint32_t addr, uint32_t value) override {
        writes.push_back({cpu, addr, value, 32});
    }
    void cp15_write(uint32_t crn, uint32_t crm, uint32_t op2,
                    uint32_t value) override {
        cp15.push_back({crn, crm, op2, value});
    }
    void set_cpu_boot(int cpu, uint32_t entry, uint32_t sp, uint32_t sp_irq,
                      uint32_t sp_svc) override {
        cpus.push_back({cpu, entry, sp, sp_irq, sp_svc});
    }
    void set_wramcnt(uint8_t value) override { wramcnt = value; }
    void set_arm7_bios_prot(uint32_t value) override { bios_prot = value; }
    void set_post_boot_latches() override {
        latches = true;
        writes_at_latches = writes.size();
    }
    void copy_logo_into_arm9_bios(const uint8_t*, uint32_t size) override {
        logo_bytes = size;
    }

    bool wrote(int cpu, uint32_t addr, uint32_t value, uint32_t width) const {
        for (const Write& w : writes)
            if (w.cpu == cpu && w.addr == addr && w.value == value &&
                w.width == width)
                return true;
        return false;
    }
    int count_at(uint32_t addr) const {
        int n = 0;
        for (const Write& w : writes) if (w.addr == addr) ++n;
        return n;
    }
};

void put32(std::vector<uint8_t>& v, uint32_t off, uint32_t value) {
    v[off] = uint8_t(value);
    v[off + 1] = uint8_t(value >> 8);
    v[off + 2] = uint8_t(value >> 16);
    v[off + 3] = uint8_t(value >> 24);
}
void put16(std::vector<uint8_t>& v, uint32_t off, uint16_t value) {
    v[off] = uint8_t(value);
    v[off + 1] = uint8_t(value >> 8);
}

constexpr uint32_t kArm9Rom = 0x4000;   // secure-area cartridge
constexpr uint32_t kArm9Ram = 0x02000000;
constexpr uint32_t kArm9Size = 0x1000;
constexpr uint32_t kArm9Entry = 0x02000800;
constexpr uint32_t kArm7Rom = 0x8000;
constexpr uint32_t kArm7Ram = 0x037F8000;
constexpr uint32_t kArm7Size = 0x400;
constexpr uint32_t kArm7Entry = 0x037F8100;

std::vector<uint8_t> make_rom() {
    std::vector<uint8_t> rom(0x20000, 0);
    put32(rom, 0x20, kArm9Rom);
    put32(rom, 0x24, kArm9Entry);
    put32(rom, 0x28, kArm9Ram);
    put32(rom, 0x2C, kArm9Size);
    put32(rom, 0x30, kArm7Rom);
    put32(rom, 0x34, kArm7Entry);
    put32(rom, 0x38, kArm7Ram);
    put32(rom, 0x3C, kArm7Size);
    put16(rom, 0x6C, 0xBEEF);   // SecureAreaCRC16
    put16(rom, 0x15E, 0xC0DE);  // HeaderCRC16
    for (uint32_t i = 0; i < 156; ++i) rom[0xC0 + i] = uint8_t(0x40 + i);
    // Distinguishable binary payloads.
    for (uint32_t i = 0; i < kArm9Size; i += 4) put32(rom, kArm9Rom + i, 0xA9000000u | i);
    for (uint32_t i = 0; i < kArm7Size; i += 4) put32(rom, kArm7Rom + i, 0xA7000000u | i);
    return rom;
}

std::vector<uint8_t> make_firmware() {
    std::vector<uint8_t> fw(0x40000, 0);
    // UserSettingsOffset is stored >> 3.
    put16(fw, 0x20, uint16_t(0x3FE00 >> 3));
    put16(fw, 0x26, 0x1234);
    // GUIWifiCodeChecksum is the THIRD u16 of the firmware header (melonDS
    // FirmwareHeader), far from the user-settings fields.
    put16(fw, 0x04, 0x5678);
    for (uint32_t i = 0; i < 0x70; i += 4) put32(fw, 0x3FE00 + i, 0x5E000000u | i);
    return fw;
}

std::vector<uint8_t> make_secure() {
    std::vector<uint8_t> s(0x800, 0);
    for (uint32_t i = 0; i < 0x800; i += 4) put32(s, i, 0x5EC00000u | i);
    return s;
}

NdsDirectBootInputs base_inputs(const std::vector<uint8_t>& rom,
                                const std::vector<uint8_t>& fw,
                                const std::vector<uint8_t>& secure) {
    NdsDirectBootInputs in;
    in.rom = rom.data();
    in.rom_size = uint32_t(rom.size());
    in.firmware = fw.data();
    in.firmware_size = uint32_t(fw.size());
    in.secure_area = secure.data();
    in.arm9_bios_is_native = true;
    // Chip ID as the live card protocol would report it for a 64 MB image
    // (0xC2 | (MB-1)<<8) -- the mirror must carry the SAME value or guest
    // boot code treats the cartridge as removed.
    in.cart_chip_id = 0x00003FC2;
    return in;
}

void test_sequence_matches_melonds() {
    const auto rom = make_rom(), fw = make_firmware(), secure = make_secure();
    Recorder m;
    std::string error;
    check(nds_direct_boot(m, base_inputs(rom, fw, secure), &error),
          "direct boot succeeds on a well-formed cartridge");

    check(m.wramcnt == 3, "shared WRAM is assigned to the ARM7 (WRAMCNT 3)");
    check(m.bios_prot == 0x1204, "ARM7 BIOS protection is 0x1204");
    check(m.latches, "post-boot device latches are applied");
    check(m.logo_bytes == 0, "a native ARM9 BIOS keeps its own Nintendo logo");

    // Header mirror.
    check(m.wrote(0, 0x027FFE00, 0, 32), "header mirror starts at 0x027FFE00");
    check(m.wrote(0, 0x027FFE00 + 0x20, kArm9Rom, 32),
          "header mirror carries the ARM9 ROM offset");
    check(m.count_at(0x027FFE00 + 0x16C) == 1, "header mirror covers 0x170 bytes");

    // Cart ID / CRC block, both copies. The chip ID must be the live
    // protocol's value, never a constant.
    check(m.wrote(0, 0x027FF800, 0x3FC2, 32), "live chip ID at 0x027FF800");
    check(m.wrote(0, 0x027FFC00, 0x3FC2, 32), "live chip ID at 0x027FFC00");
    check(m.wrote(0, 0x027FF808, 0xC0DE, 16), "header CRC lands at 0x027FF808");
    check(m.wrote(0, 0x027FF80A, 0xBEEF, 16), "secure CRC lands at 0x027FF80A");
    check(m.wrote(0, 0x027FF850, 0x5835, 16), "0x5835 marker at 0x027FF850");
    check(m.wrote(0, 0x027FFC08, 0xC0DE, 16), "header CRC mirrored at 0x027FFC08");
    check(m.wrote(0, 0x027FFC30, 0xFFFF, 16), "0xFFFF at 0x027FFC30");
    check(m.wrote(0, 0x027FFC40, 0x0001, 16), "0x0001 at 0x027FFC40");

    // Secure area occupies the first 0x800 of the ARM9 image, then the rest of
    // the binary follows -- the seam is where an off-by-one would land.
    check(m.wrote(0, kArm9Ram, 0x5EC00000u, 32),
          "decrypted secure area lands at the ARM9 RAM address");
    check(m.wrote(0, kArm9Ram + 0x7FC, 0x5EC007FCu, 32),
          "secure area is copied through its last word");
    check(m.wrote(0, kArm9Ram + 0x800, 0xA9000800u, 32),
          "the ARM9 binary resumes right after the secure area");
    check(m.count_at(kArm9Ram) == 1,
          "the secure area is not overwritten by the plain copy");
    check(m.wrote(0, kArm9Ram + kArm9Size - 4, 0xA9000000u | (kArm9Size - 4), 32),
          "the ARM9 binary is copied through its last word");
    check(m.wrote(1, kArm7Ram, 0xA7000000u, 32),
          "the ARM7 binary is copied on the ARM7 bus");
    check(m.wrote(1, kArm7Ram + kArm7Size - 4, 0xA7000000u | (kArm7Size - 4), 32),
          "the ARM7 binary is copied through its last word");

    // Firmware user settings mirror.
    check(m.wrote(0, 0x027FF868, 0x3FE00, 32),
          "user-settings offset is un-shifted into 0x027FF868");
    check(m.wrote(0, 0x027FF874, 0x1234, 16), "data/gfx checksum mirrored");
    check(m.wrote(0, 0x027FF876, 0x5678, 16), "GUI/wifi checksum mirrored");
    check(m.wrote(0, 0x027FFC80, 0x5E000000u, 32), "user settings mirrored");
    check(m.wrote(0, 0x027FFC80 + 0x6C, 0x5E00006Cu, 32),
          "user settings mirrored through 0x70 bytes");

    // CP15: exact list, exact order.
    check(m.cp15.size() == 24, "twenty-four CP15 writes");
    if (m.cp15.size() == 24) {
        check(m.cp15[0].crn == 1 && m.cp15[0].crm == 0 && m.cp15[0].op2 == 0 &&
              m.cp15[0].value == 0x00012078, "CP15 control register first");
        check(m.cp15[20].crn == 6 && m.cp15[20].crm == 7 &&
              m.cp15[20].value == 0x027FF017, "MPU region 7 covers the mirror");
        check(m.cp15[22].crn == 9 && m.cp15[22].crm == 1 &&
              m.cp15[22].op2 == 0 && m.cp15[22].value == 0x0300000A,
              "DTCM placement");
        check(m.cp15[23].crn == 9 && m.cp15[23].crm == 1 &&
              m.cp15[23].op2 == 1 && m.cp15[23].value == 0x00000020,
              "ITCM placement");
    }

    // Entry state.
    check(m.cpus.size() == 2, "both cores are given entry state");
    if (m.cpus.size() == 2) {
        check(m.cpus[0].cpu == 0 && m.cpus[0].entry == kArm9Entry &&
              m.cpus[0].sp == 0x03002F7C && m.cpus[0].sp_irq == 0x03003F80 &&
              m.cpus[0].sp_svc == 0x03003FC0, "ARM9 entry and stacks");
        check(m.cpus[1].cpu == 1 && m.cpus[1].entry == kArm7Entry &&
              m.cpus[1].sp == 0x0380FD80 && m.cpus[1].sp_irq == 0x0380FF80 &&
              m.cpus[1].sp_svc == 0x0380FFC0, "ARM7 entry and stacks");
    }
    check(m.writes_at_latches == m.writes.size(),
          "the latches are the last thing applied");
}

void test_freebios_gets_the_logo() {
    const auto rom = make_rom(), fw = make_firmware(), secure = make_secure();
    Recorder m;
    NdsDirectBootInputs in = base_inputs(rom, fw, secure);
    in.arm9_bios_is_native = false;
    std::string error;
    check(nds_direct_boot(m, in, &error), "direct boot succeeds on FreeBIOS");
    check(m.logo_bytes == 156,
          "a reimplemented ARM9 BIOS is given the cartridge's Nintendo logo");
}

void test_cartridge_without_secure_area() {
    auto rom = make_rom();
    put32(rom, 0x20, 0x200);            // ARM9ROMOffset below 0x4000
    put32(rom, 0x2C, 0x100);            // and a small binary
    for (uint32_t i = 0; i < 0x100; i += 4) put32(rom, 0x200 + i, 0xB9000000u | i);
    const auto fw = make_firmware(), secure = make_secure();
    Recorder m;
    NdsDirectBootInputs in = base_inputs(rom, fw, secure);
    in.secure_area = nullptr;
    std::string error;
    check(nds_direct_boot(m, in, &error),
          "a cartridge with no secure area needs no plaintext");
    check(m.wrote(0, kArm9Ram, 0xB9000000u, 32),
          "the ARM9 binary is copied from offset 0");
}

// 32-bit accumulator — the table terms overflow 16 bits and their high bits
// shift back into range (matches the CRCs a real firmware dump stores).
uint16_t fw_crc16(const uint8_t* data, uint32_t length, uint32_t start) {
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

// melonDS mirrors the EFFECTIVE user-settings copy (GetEffectiveUserData):
// CRC-valid, higher update counter when both validate, ties keep copy 0,
// copy 0 again when neither validates (make_firmware's copies pin that
// fallback in test_sequence_matches_melonds).
void test_effective_user_settings() {
    const auto rom = make_rom(), secure = make_secure();
    auto fw = make_firmware();
    constexpr uint32_t kCopy0 = 0x3FE00, kCopy1 = 0x3FF00;
    for (uint32_t i = 0; i < 0x70; i += 4) put32(fw, kCopy1 + i, 0x5F000000u | i);

    auto seal = [&](uint32_t base, uint16_t counter) {
        put16(fw, base + 0x70, counter);
        put16(fw, base + 0x72, fw_crc16(fw.data() + base, 0x70, 0xFFFF));
    };

    {   // Copy 1 valid and newer: it is the one mirrored.
        seal(kCopy0, 5);
        seal(kCopy1, 6);
        Recorder m;
        std::string error;
        check(nds_direct_boot(m, base_inputs(rom, fw, secure), &error),
              "direct boot succeeds with two valid user-settings copies");
        check(m.wrote(0, 0x027FFC80, 0x5F000000u, 32),
              "the newer valid copy 1 is mirrored");
    }
    {   // Equal counters: copy 0 wins the tie.
        seal(kCopy0, 6);
        Recorder m;
        std::string error;
        nds_direct_boot(m, base_inputs(rom, fw, secure), &error);
        check(m.wrote(0, 0x027FFC80, 0x5E000000u, 32),
              "copy 0 wins an update-counter tie");
    }
    {   // Copy 1 newer but CRC-invalid: copy 0 is mirrored.
        seal(kCopy1, 7);
        fw[kCopy1 + 0x72] ^= 0xFF;
        Recorder m;
        std::string error;
        nds_direct_boot(m, base_inputs(rom, fw, secure), &error);
        check(m.wrote(0, 0x027FFC80, 0x5E000000u, 32),
              "a CRC-invalid copy is never mirrored");
    }
}

void test_refusals() {
    const auto rom = make_rom(), fw = make_firmware(), secure = make_secure();
    std::string error;

    {   // Missing plaintext for a secure-area cartridge.
        Recorder m;
        NdsDirectBootInputs in = base_inputs(rom, fw, secure);
        in.secure_area = nullptr;
        check(!nds_direct_boot(m, in, &error),
              "refuses a secure-area cartridge with no plaintext");
    }
    {   // ARM9 binary running past the image.
        auto bad = make_rom();
        put32(bad, 0x2C, 0x100000);
        Recorder m;
        NdsDirectBootInputs in = base_inputs(bad, fw, secure);
        in.rom = bad.data();
        in.rom_size = uint32_t(bad.size());
        check(!nds_direct_boot(m, in, &error),
              "refuses an ARM9 binary that runs past the image");
        check(m.writes.empty(), "refusal happens before any write");
    }
    {   // User-settings offset outside the firmware.
        auto badfw = make_firmware();
        put16(badfw, 0x20, 0xFFFF);
        Recorder m;
        NdsDirectBootInputs in = base_inputs(rom, badfw, secure);
        in.firmware = badfw.data();
        in.firmware_size = uint32_t(badfw.size());
        check(!nds_direct_boot(m, in, &error),
              "refuses a firmware whose user settings are out of range");
    }
    {   // Truncated inputs.
        Recorder m;
        NdsDirectBootInputs in = base_inputs(rom, fw, secure);
        in.rom_size = 0x100;
        check(!nds_direct_boot(m, in, &error), "refuses a truncated cartridge");
    }
}

}  // namespace

int main() {
    test_sequence_matches_melonds();
    test_freebios_gets_the_logo();
    test_cartridge_without_secure_area();
    test_effective_user_settings();
    test_refusals();
    return g_failures == 0 ? 0 : 1;
}
