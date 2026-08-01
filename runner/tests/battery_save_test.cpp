#include "battery_save.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

bool require(bool condition) {
    return condition;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now()
                           .time_since_epoch().count();
    const fs::path root =
        fs::temp_directory_path() /
        ("nds-battery-save-test-" + std::to_string(stamp));
    const fs::path save = root / "nested" / "game.sav";
    std::array<uint8_t, 32> expected{};
    std::array<uint8_t, 32> actual{};
    for (size_t i = 0; i < expected.size(); ++i)
        expected[i] = static_cast<uint8_t>(i * 7u);

    std::string error;
    if (!require(nds_battery_save_load_exact(
            save.string(), actual.data(), actual.size(), &error) ==
                 NdsBatterySaveLoadResult::Missing))
        return 1;
    if (!require(nds_battery_save_write_atomic(
            save.string(), expected.data(), expected.size(), &error)))
        return 2;
    if (!require(nds_battery_save_load_exact(
            save.string(), actual.data(), actual.size(), &error) ==
                 NdsBatterySaveLoadResult::Loaded) ||
        !require(actual == expected))
        return 3;

    expected.fill(0xA5u);
    if (!require(nds_battery_save_write_atomic(
            save.string(), expected.data(), expected.size(), &error)))
        return 4;
    actual.fill(0u);
    if (!require(nds_battery_save_load_exact(
            save.string(), actual.data(), actual.size(), &error) ==
                 NdsBatterySaveLoadResult::Loaded) ||
        !require(actual == expected))
        return 5;

    {
        std::ofstream invalid(save, std::ios::binary | std::ios::trunc);
        invalid.put('\0');
    }
    if (!require(nds_battery_save_load_exact(
            save.string(), actual.data(), actual.size(), &error) ==
                 NdsBatterySaveLoadResult::InvalidSize))
        return 6;

    fs::remove(save);
    fs::remove(save.parent_path());
    fs::remove(root);
    return 0;
}
