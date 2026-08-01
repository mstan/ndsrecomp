#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

enum class NdsBatterySaveLoadResult {
    Missing,
    Loaded,
    InvalidSize,
    Error,
};

NdsBatterySaveLoadResult nds_battery_save_load_exact(
    const std::string& path, uint8_t* data, size_t size, std::string* error);

bool nds_battery_save_write_atomic(
    const std::string& path, const uint8_t* data, size_t size,
    std::string* error);
