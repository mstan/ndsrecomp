#pragma once

#include <cstdint>
#include <vector>

#include "cartridge_config.h"

// AUXSPI backup-chip (save memory) protocol, modelled on melonDS
// CartRetail::SPIWrite / SRAMWrite_*. Kept free of the rest of the runtime so
// the guest-visible behaviour can be pinned by cart_backup_test.
struct NdsCartBackup {
    NdsCartridgeSaveConfig config{};
    std::vector<uint8_t> sram;
    uint8_t cmd = 0;
    uint8_t status = 0;
    uint32_t addr = 0;
    // Set whenever a command actually changed a byte of `sram`; the caller owns
    // deciding when to write the image out and when to clear this.
    bool dirty = false;
};

// Feeds one AUXSPI byte to the backup chip. `pos` is the byte's index within
// the current chip-select transfer and `last` marks the transfer's final byte.
uint8_t nds_cart_backup_spi_write(NdsCartBackup& chip, uint8_t value,
                                  uint32_t pos, bool last);

// Clears the latched command state; call on reset.
void nds_cart_backup_reset_command(NdsCartBackup& chip);
