#include "firmware_state.h"

#include "battery_save.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

bool require(bool condition) {
    return condition;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now()
                           .time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("nds-firmware-state-test-" + std::to_string(stamp));
    const fs::path state = root / "profile" / "firmware.bin";
    std::vector<uint8_t> seed(128u * 1024u);
    for (size_t i = 0; i < seed.size(); ++i)
        seed[i] = static_cast<uint8_t>(i * 17u);

    std::vector<uint8_t> loaded;
    std::string error;
    if (!require(nds_firmware_state_load_or_seed(
            state.string(), seed, &loaded, &error) ==
                 NdsFirmwareStateLoadResult::Seeded) ||
        !require(loaded == seed) || !require(fs::file_size(state) == seed.size()))
        return 1;

    std::vector<uint8_t> changed(seed.size(), 0xA5u);
    if (!require(nds_battery_save_write_atomic(
            state.string(), changed.data(), changed.size(), &error)))
        return 2;
    loaded.clear();
    if (!require(nds_firmware_state_load_or_seed(
            state.string(), seed, &loaded, &error) ==
                 NdsFirmwareStateLoadResult::Loaded) ||
        !require(loaded == changed))
        return 3;

    {
        std::ofstream invalid(state, std::ios::binary | std::ios::trunc);
        invalid.put('\0');
    }
    loaded.clear();
    if (!require(nds_firmware_state_load_or_seed(
            state.string(), seed, &loaded, &error) ==
                 NdsFirmwareStateLoadResult::Error) ||
        !require(fs::file_size(state) == 1u))
        return 4;

    const fs::path blocker = root / "not-a-directory";
    {
        std::ofstream file(blocker);
        file << "block";
    }
    loaded.clear();
    if (!require(nds_firmware_state_load_or_seed(
            (blocker / "firmware.bin").string(), seed, &loaded, &error) ==
                 NdsFirmwareStateLoadResult::Error))
        return 5;

    fs::remove_all(root);
    return 0;
}
