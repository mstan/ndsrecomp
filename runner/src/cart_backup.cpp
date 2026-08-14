#include "cart_backup.h"

// ── AUXSPI backup-chip protocol (melonDS CartRetail::SPIWrite) ─────────
//
// The write-enable latch (status bit 1) is set by its own WREN transfer and has
// to survive every intervening transfer until the write or erase that consumes
// it. Save drivers routinely poll the status register in between -- WREN, then
// read 0x05, then program -- so clearing the latch at the end of ANY transfer
// consumes it on that poll and silently drops the write that follows. It is
// cleared inside the write and erase commands themselves, on the transfer's
// last byte, exactly where melonDS clears SRAMStatus.

namespace {

uint8_t eeprom_spi_write(NdsCartBackup& chip, uint8_t val, uint32_t pos,
                         bool last) {
    const uint32_t mask = static_cast<uint32_t>(chip.sram.size()) - 1u;
    const uint32_t addrsize = chip.sram.size() > 65536u ? 3u : 2u;
    switch (chip.cmd) {
        case 0x01:  // write status register
            if (pos == 1) chip.status = (chip.status & 0x01u) | (val & 0x0Cu);
            return 0;
        case 0x05:  // read status register
            return chip.status;
        case 0x02:  // write
            if (pos <= addrsize) {
                chip.addr = (chip.addr << 8) | val;
            } else {
                if (chip.status & 0x02u) {
                    uint8_t& destination = chip.sram[chip.addr & mask];
                    if (destination != val) {
                        destination = val;
                        chip.dirty = true;
                    }
                }
                ++chip.addr;
            }
            if (last) chip.status &= ~0x02u;
            return 0;
        case 0x03:  // read
            if (pos <= addrsize) {
                chip.addr = (chip.addr << 8) | val;
                return 0;
            } else {
                const uint8_t ret = chip.sram[chip.addr & mask];
                ++chip.addr;
                return ret;
            }
        case 0x9F:  // read JEDEC ID
            return 0xFF;
        default:
            return 0xFF;
    }
}

uint8_t eeprom_tiny_spi_write(NdsCartBackup& chip, uint8_t val, uint32_t pos,
                              bool last) {
    const uint32_t high = (chip.cmd == 0x0Au || chip.cmd == 0x0Bu) ? 0x100u : 0u;
    switch (chip.cmd) {
        case 0x01:
            if (pos == 1) chip.status = (chip.status & 0x01u) | (val & 0x0Cu);
            return 0;
        case 0x05:
            return chip.status | 0xF0u;
        case 0x02:
        case 0x0A:
            if (pos < 2) {
                chip.addr = val;
            } else {
                if (chip.status & 0x02u) {
                    uint8_t& destination =
                        chip.sram[(chip.addr + high) & 0x1FFu];
                    if (destination != val) {
                        destination = val;
                        chip.dirty = true;
                    }
                }
                ++chip.addr;
            }
            if (last) chip.status &= ~0x02u;
            return 0;
        case 0x03:
        case 0x0B:
            if (pos < 2) {
                chip.addr = val;
                return 0;
            }
            return chip.sram[(chip.addr++ + high) & 0x1FFu];
        case 0x9F:
            return 0xFF;
        default:
            return 0xFF;
    }
}

uint8_t flash_spi_write(NdsCartBackup& chip, uint8_t val, uint32_t pos,
                        bool last) {
    const uint32_t mask = static_cast<uint32_t>(chip.sram.size()) - 1u;
    switch (chip.cmd) {
        case 0x05:
            return chip.status;
        case 0x02:
        case 0x0A:
            if (pos <= 3) {
                chip.addr = (chip.addr << 8) | val;
            } else {
                if (chip.status & 0x02u) {
                    uint8_t& destination = chip.sram[chip.addr & mask];
                    // Match melonDS's currently modelled distinction between
                    // page-program (0x02) and page-write (0x0A).
                    const uint8_t programmed = chip.cmd == 0x02 ? 0u : val;
                    if (destination != programmed) {
                        destination = programmed;
                        chip.dirty = true;
                    }
                }
                ++chip.addr;
            }
            if (last) chip.status &= ~0x02u;
            return 0;
        case 0x03:
        case 0x0B:
            if (pos <= 3) {
                chip.addr = (chip.addr << 8) | val;
                return 0;
            }
            if (chip.cmd == 0x0B && pos == 4) return 0;
            return chip.sram[chip.addr++ & mask];
        case 0x9F:
            return 0xFF;
        case 0xD8:
        case 0xDB: {
            if (pos <= 3) chip.addr = (chip.addr << 8) | val;
            if (pos == 3 && (chip.status & 0x02u)) {
                const uint32_t length = chip.cmd == 0xD8 ? 0x10000u : 0x100u;
                for (uint32_t i = 0; i < length; ++i) {
                    uint8_t& destination = chip.sram[(chip.addr + i) & mask];
                    if (destination != 0u) {
                        destination = 0u;
                        chip.dirty = true;
                    }
                }
            }
            if (last) chip.status &= ~0x02u;
            return 0;
        }
        default:
            return 0xFF;
    }
}

}  // namespace

void nds_cart_backup_reset_command(NdsCartBackup& chip) {
    chip.cmd = 0;
    chip.status = 0;
    chip.addr = 0;
}

uint8_t nds_cart_backup_spi_write(NdsCartBackup& chip, uint8_t value,
                                  uint32_t pos, bool last) {
    if (chip.sram.empty()) return 0;
    if (pos == 0) {
        switch (value) {
            case 0x04: chip.status &= ~0x02u; return 0;  // WRDI
            case 0x06: chip.status |= 0x02u; return 0;   // WREN
            default: chip.cmd = value; chip.addr = 0; break;
        }
        return 0xFF;
    }
    switch (chip.config.type) {
        case NdsCartridgeSaveType::EepromTiny:
            return eeprom_tiny_spi_write(chip, value, pos, last);
        case NdsCartridgeSaveType::Eeprom:
            return eeprom_spi_write(chip, value, pos, last);
        case NdsCartridgeSaveType::Flash:
            return flash_spi_write(chip, value, pos, last);
        case NdsCartridgeSaveType::None:
            return 0;
    }
    return 0xFF;
}
