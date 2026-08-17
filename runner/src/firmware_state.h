#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class NdsFirmwareStateLoadResult {
    Loaded,
    Seeded,
    Error,
};

// Load an exact-size mutable firmware image, or atomically seed a missing
// state file from the already-validated/generated base image.
NdsFirmwareStateLoadResult nds_firmware_state_load_or_seed(
    const std::string& path, const std::vector<uint8_t>& seed,
    std::vector<uint8_t>* firmware, std::string* error);
