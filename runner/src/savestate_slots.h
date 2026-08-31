#pragma once

#include <cstdint>
#include <string>

#include "savestate.h"

constexpr unsigned NDS_SAVESTATE_SLOT_COUNT = 12u;

enum class NdsSavestateSlotAction : uint8_t {
    Load,
    Save,
};

struct NdsSavestateSlotCommand {
    NdsSavestateSlotAction action = NdsSavestateSlotAction::Load;
    unsigned slot = 0;
};

struct NdsSavestateSlotMetadata {
    bool exists = false;
    uint64_t size_bytes = 0;
    int64_t modified_unix_seconds = 0;
};

struct NdsSavestateSlotResult {
    bool success = false;
    NdsSavestateSlotCommand command{};
    NdsSavestateSlotMetadata metadata{};
    std::string message;
};

// Converts an F-key number (1..12) into the public shortcut contract.
// Repeated key-down events are consumed but never execute a second command.
bool nds_savestate_slot_shortcut(unsigned function_key, bool shift,
                                 bool repeat,
                                 NdsSavestateSlotCommand* command);

// Slot filenames are fixed by the framework. Callers never supply a filename,
// so a slot cannot escape the configured per-ROM directory.
std::string nds_savestate_slot_path(const std::string& directory,
                                    unsigned slot,
                                    std::string* error);

NdsSavestateSlotMetadata nds_savestate_slot_metadata(
    const std::string& directory, unsigned slot);

NdsSavestateSlotResult nds_savestate_slot_execute(
    const std::string& directory, const NdsSavestateIdentity& identity,
    const NdsSavestateSlotCommand& command);

// Successful historical loads begin a new host-observation epoch. This resets
// diagnostic counters, rings and attribution only; guest-visible state has
// already been restored by nds_savestate_load_core and is not changed here.
void nds_savestate_reset_host_history();
