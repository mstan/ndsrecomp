#pragma once

#include <cstdint>

enum class NdsCartridgeSaveType : uint8_t {
    None,
    EepromTiny,
    Eeprom,
    Flash,
};

struct NdsCartridgeSaveConfig {
    // Preserve the historical runner default for projects without a
    // [cartridge] table. New commercial-title projects should declare both
    // fields explicitly from a trusted cartridge database.
    NdsCartridgeSaveType type = NdsCartridgeSaveType::Eeprom;
    uint32_t size = 8192;
};
