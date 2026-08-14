// Pins the AUXSPI backup-chip protocol, and in particular the write-enable
// latch's lifetime across transfers.
//
// The regression this exists for: the latch must survive from its own WREN
// transfer until the write that consumes it, across anything in between. Save
// drivers poll the status register in that gap, and a rule that cleared the
// latch at the end of every transfer consumed it on that poll -- so every write
// was dropped, silently, because reads kept working and the dirty flag never
// rose. That shipped for two weeks and meant no game could write a save at all.
// test_status_poll_between_wren_and_write is the case that actually failed.

#include "cart_backup.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

NdsCartBackup make_chip(NdsCartridgeSaveType type, uint32_t size) {
    NdsCartBackup chip;
    chip.config.type = type;
    chip.config.size = size;
    chip.sram.assign(size, 0xFFu);
    return chip;
}

// Drives one complete chip-select transfer: `last` is set on the final byte.
void transfer(NdsCartBackup& chip, const std::vector<uint8_t>& bytes) {
    for (size_t i = 0; i < bytes.size(); ++i)
        nds_cart_backup_spi_write(chip, bytes[i], static_cast<uint32_t>(i),
                                  i + 1 == bytes.size());
}

uint8_t read_byte(NdsCartBackup& chip, uint32_t address) {
    // 0x03 read, 3-byte address for a 256 KiB flash, then one data byte.
    nds_cart_backup_spi_write(chip, 0x03u, 0, false);
    nds_cart_backup_spi_write(chip, uint8_t(address >> 16), 1, false);
    nds_cart_backup_spi_write(chip, uint8_t(address >> 8), 2, false);
    nds_cart_backup_spi_write(chip, uint8_t(address), 3, false);
    return nds_cart_backup_spi_write(chip, 0u, 4, true);
}

void write_enable(NdsCartBackup& chip) {
    transfer(chip, {0x06u});  // WREN is a single-byte transfer of its own
}

void page_write(NdsCartBackup& chip, uint32_t address,
                const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> bytes{0x0Au, uint8_t(address >> 16),
                               uint8_t(address >> 8), uint8_t(address)};
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    transfer(chip, bytes);
}

void test_flash_write_survives_separate_wren_transfer() {
    NdsCartBackup chip = make_chip(NdsCartridgeSaveType::Flash, 262144u);

    // The regression: WREN in one transfer, the write in the next.
    write_enable(chip);
    check((chip.status & 0x02u) != 0u, "WREN sets the write-enable latch");
    page_write(chip, 0x1234u, {0xAAu, 0xBBu});
    check(chip.dirty, "a write after WREN marks the image dirty");
    check(read_byte(chip, 0x1234u) == 0xAAu, "first written byte lands");
    check(read_byte(chip, 0x1235u) == 0xBBu, "second written byte lands");

    // The latch is consumed by the write, so an unenabled write is ignored.
    check((chip.status & 0x02u) == 0u, "the write clears the latch");
    page_write(chip, 0x2000u, {0x77u});
    check(read_byte(chip, 0x2000u) == 0xFFu,
          "a write without WREN is dropped");

    // WRDI cancels a pending enable.
    write_enable(chip);
    transfer(chip, {0x04u});
    check((chip.status & 0x02u) == 0u, "WRDI clears the latch");
    page_write(chip, 0x3000u, {0x55u});
    check(read_byte(chip, 0x3000u) == 0xFFu, "a write after WRDI is dropped");
}

uint8_t read_status(NdsCartBackup& chip) {
    // 0x05 read-status is a two-byte transfer: command, then the returned byte.
    nds_cart_backup_spi_write(chip, 0x05u, 0, false);
    return nds_cart_backup_spi_write(chip, 0x00u, 1, true);
}

// The sequence that actually broke: a status poll sits between WREN and the
// program, and must not consume the latch.
void test_status_poll_between_wren_and_write() {
    NdsCartBackup chip = make_chip(NdsCartridgeSaveType::Flash, 262144u);

    write_enable(chip);
    check((read_status(chip) & 0x02u) != 0u,
          "a status poll reports the latch still set");
    check((chip.status & 0x02u) != 0u,
          "a status poll does not consume the latch");

    page_write(chip, 0x50u, {0x6Cu});
    check(read_byte(chip, 0x50u) == 0x6Cu,
          "the write still lands after a status poll");
    check((chip.status & 0x02u) == 0u,
          "the write, not the poll, consumes the latch");
}

void test_flash_program_versus_write_and_erase() {
    NdsCartBackup chip = make_chip(NdsCartridgeSaveType::Flash, 262144u);

    // Page write (0x0A) stores the byte; page program (0x02) zeroes it, which
    // is what melonDS currently models.
    write_enable(chip);
    page_write(chip, 0x40u, {0x5Au});
    check(read_byte(chip, 0x40u) == 0x5Au, "page write stores the value");

    write_enable(chip);
    transfer(chip, {0x02u, 0x00u, 0x00u, 0x40u, 0x5Au});
    check(read_byte(chip, 0x40u) == 0x00u, "page program zeroes the byte");

    // Page erase (0xDB) clears 0x100 bytes, and only when enabled.
    write_enable(chip);
    page_write(chip, 0x800u, {0x11u});
    check(read_byte(chip, 0x800u) == 0x11u, "byte staged for erase");
    transfer(chip, {0xDBu, 0x00u, 0x08u, 0x00u});
    check(read_byte(chip, 0x800u) == 0x11u, "erase without WREN is ignored");
    write_enable(chip);
    transfer(chip, {0xDBu, 0x00u, 0x08u, 0x00u});
    check(read_byte(chip, 0x800u) == 0x00u, "page erase clears the byte");
}

void test_eeprom_write_survives_separate_wren_transfer() {
    NdsCartBackup chip = make_chip(NdsCartridgeSaveType::Eeprom, 65536u);

    write_enable(chip);
    // 0x02 write, 2-byte address for a 64 KiB EEPROM, then data.
    transfer(chip, {0x02u, 0x01u, 0x00u, 0x3Cu});
    check(chip.dirty, "EEPROM write after WREN marks the image dirty");
    check(chip.sram[0x0100u] == 0x3Cu, "EEPROM byte lands");
    check((chip.status & 0x02u) == 0u, "EEPROM write clears the latch");

    transfer(chip, {0x02u, 0x02u, 0x00u, 0x7Eu});
    check(chip.sram[0x0200u] == 0xFFu,
          "EEPROM write without WREN is dropped");
}

void test_eeprom_tiny_write_survives_separate_wren_transfer() {
    NdsCartBackup chip = make_chip(NdsCartridgeSaveType::EepromTiny, 512u);

    write_enable(chip);
    transfer(chip, {0x02u, 0x10u, 0x99u});
    check(chip.sram[0x10u] == 0x99u, "tiny EEPROM byte lands");
    check((chip.status & 0x02u) == 0u, "tiny EEPROM write clears the latch");

    transfer(chip, {0x02u, 0x11u, 0x88u});
    check(chip.sram[0x11u] == 0xFFu,
          "tiny EEPROM write without WREN is dropped");

    // 0x0A addresses the upper 256-byte half.
    write_enable(chip);
    transfer(chip, {0x0Au, 0x05u, 0x42u});
    check(chip.sram[0x105u] == 0x42u, "tiny EEPROM high-half write lands");
}

void test_reads_do_not_need_write_enable() {
    NdsCartBackup chip = make_chip(NdsCartridgeSaveType::Flash, 262144u);
    write_enable(chip);
    page_write(chip, 0u, {0x24u});
    chip.dirty = false;
    check(read_byte(chip, 0u) == 0x24u, "read works with the latch clear");
    check(!chip.dirty, "a read does not dirty the image");
}

}  // namespace

int main() {
    test_flash_write_survives_separate_wren_transfer();
    test_status_poll_between_wren_and_write();
    test_flash_program_versus_write_and_erase();
    test_eeprom_write_survives_separate_wren_transfer();
    test_eeprom_tiny_write_survives_separate_wren_transfer();
    test_reads_do_not_need_write_enable();
    return g_failures == 0 ? 0 : 1;
}
