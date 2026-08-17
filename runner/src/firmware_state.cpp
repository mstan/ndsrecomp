#include "firmware_state.h"

#include "battery_save.h"

#include <utility>

NdsFirmwareStateLoadResult nds_firmware_state_load_or_seed(
    const std::string& path, const std::vector<uint8_t>& seed,
    std::vector<uint8_t>* firmware, std::string* error) {
    if (error) error->clear();
    if (path.empty() || seed.empty() || !firmware) {
        if (error) *error = "invalid firmware-state request";
        return NdsFirmwareStateLoadResult::Error;
    }

    std::vector<uint8_t> loaded(seed.size());
    const NdsBatterySaveLoadResult result = nds_battery_save_load_exact(
        path, loaded.data(), loaded.size(), error);
    if (result == NdsBatterySaveLoadResult::Loaded) {
        *firmware = std::move(loaded);
        return NdsFirmwareStateLoadResult::Loaded;
    }
    if (result != NdsBatterySaveLoadResult::Missing)
        return NdsFirmwareStateLoadResult::Error;

    if (!nds_battery_save_write_atomic(
            path, seed.data(), seed.size(), error))
        return NdsFirmwareStateLoadResult::Error;
    *firmware = seed;
    return NdsFirmwareStateLoadResult::Seeded;
}
