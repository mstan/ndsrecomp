#include "savestate_slots.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

enum class CoreMode { Ok, RefuseNetwork, WrongRom, WrongBuild, Corrupt, Io };
CoreMode g_mode = CoreMode::Ok;
unsigned g_resets = 0;

bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool shortcuts() {
    bool ok = true;
    NdsSavestateSlotCommand command{};
    ok &= expect(nds_savestate_slot_shortcut(1, false, false, &command),
                 "F1 is recognized");
    ok &= expect(command.slot == 1 &&
                 command.action == NdsSavestateSlotAction::Load,
                 "F1 loads slot 1");
    ok &= expect(nds_savestate_slot_shortcut(12, true, false, &command),
                 "Shift+F12 is recognized");
    ok &= expect(command.slot == 12 &&
                 command.action == NdsSavestateSlotAction::Save,
                 "Shift+F12 saves slot 12");
    ok &= expect(nds_savestate_slot_shortcut(4, false, true, &command) &&
                 command.slot == 0,
                 "key repeat is consumed without dispatch");
    ok &= expect(!nds_savestate_slot_shortcut(0, false, false, &command) &&
                 !nds_savestate_slot_shortcut(13, false, false, &command),
                 "non-slot function keys are ignored");
    return ok;
}

bool paths_and_metadata(const std::filesystem::path& root) {
    bool ok = true;
    std::string error;
    const std::filesystem::path path =
        nds_savestate_slot_path(root.string(), 7, &error);
    ok &= expect(path.parent_path() == root &&
                 path.filename() == "slot07.nss",
                 "slot path is confined beneath configured directory");
    ok &= expect(nds_savestate_slot_path(root.string(), 0, &error).empty(),
                 "slot zero is rejected");
    ok &= expect(nds_savestate_slot_path({}, 1, &error).empty() &&
                 error == "savestate directory is not configured",
                 "missing directory is explicit");

    std::filesystem::create_directories(root);
    std::ofstream(path, std::ios::binary) << "state";
    const NdsSavestateSlotMetadata metadata =
        nds_savestate_slot_metadata(root.string(), 7);
    ok &= expect(metadata.exists && metadata.size_bytes == 5 &&
                 metadata.modified_unix_seconds > 0,
                 "slot metadata reports existence size and timestamp");
    return ok;
}

bool execution(const std::filesystem::path& root) {
    bool ok = true;
    const NdsSavestateIdentity identity{
        "exact-build", "0123456789abcdef0123456789abcdef01234567"};
    NdsSavestateSlotCommand command{NdsSavestateSlotAction::Load, 1};
    std::filesystem::remove(root / "slot01.nss");
    auto result = nds_savestate_slot_execute(root.string(), identity, command);
    ok &= expect(!result.success && result.message == "State slot 1 is empty",
                 "missing slot has a precise message");

    g_mode = CoreMode::RefuseNetwork;
    result = nds_savestate_slot_execute(root.string(), identity, command);
    ok &= expect(!result.success &&
                 result.message.find("connected to a Wi-Fi network") !=
                     std::string::npos,
                 "network refusal takes precedence over an empty slot");

    command.action = NdsSavestateSlotAction::Save;
    g_mode = CoreMode::Ok;
    result = nds_savestate_slot_execute(root.string(), identity, command);
    ok &= expect(result.success && result.metadata.exists &&
                 result.message == "Saved state slot 1",
                 "successful save reports metadata");

    command.action = NdsSavestateSlotAction::Load;
    g_resets = 0;
    result = nds_savestate_slot_execute(root.string(), identity, command);
    ok &= expect(result.success && result.message == "Loaded state slot 1" &&
                 g_resets == 12,
                 "successful load resets every host history surface");

    const struct {
        CoreMode mode;
        const char* detail;
    } failures[] = {
        {CoreMode::RefuseNetwork,
         "savestate unavailable while connected to a Wi-Fi network"},
        {CoreMode::WrongRom, "savestate ROM SHA-1 mismatch"},
        {CoreMode::WrongBuild, "savestate build id mismatch"},
        {CoreMode::Corrupt, "savestate section is corrupt"},
        {CoreMode::Io, "could not read savestate file"},
    };
    for (const auto& failure : failures) {
        g_mode = failure.mode;
        g_resets = 0;
        result = nds_savestate_slot_execute(root.string(), identity, command);
        ok &= expect(!result.success &&
                     result.message.find(failure.detail) != std::string::npos &&
                     g_resets == 0,
                     "load failure preserves exact reason and skips reset");
    }
    return ok;
}

}  // namespace

bool nds_savestate_save_core(const std::string& path,
                             const NdsSavestateIdentity&,
                             std::string* error) {
    if (g_mode == CoreMode::Io) {
        if (error) *error = "could not write savestate file";
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "state";
    if (!out) {
        if (error) *error = "could not write savestate file";
        return false;
    }
    return true;
}

bool nds_savestate_load_core(const std::string&,
                             const NdsSavestateIdentity&,
                             std::string* error) {
    switch (g_mode) {
        case CoreMode::Ok: return true;
        case CoreMode::RefuseNetwork:
            *error = "savestate unavailable while connected to a Wi-Fi network";
            return false;
        case CoreMode::WrongRom: *error = "savestate ROM SHA-1 mismatch"; return false;
        case CoreMode::WrongBuild: *error = "savestate build id mismatch"; return false;
        case CoreMode::Corrupt: *error = "savestate section is corrupt"; return false;
        case CoreMode::Io: *error = "could not read savestate file"; return false;
    }
    return false;
}

bool nds_savestate_check_eligibility(std::string* error) {
    if (g_mode != CoreMode::RefuseNetwork) return true;
    *error = "savestate unavailable while connected to a Wi-Fi network";
    return false;
}

void runtime_savestate_reset_host_history() { ++g_resets; }
void bus_debug_history_reset() { ++g_resets; }
void nds_io_debug_history_reset() { ++g_resets; }
void net_ring_reset() { ++g_resets; }
void scheduler_profile_reset() { ++g_resets; }
void nds_emu_profile_reset() { ++g_resets; }
void nds_gpu2d_profile_reset() { ++g_resets; }
void nds_gpu3d_debug_history_reset() { ++g_resets; }
void nds_dispatch_timing_reset() { ++g_resets; }
void tier3_reset() { ++g_resets; }
void coverage_pages_reset() { ++g_resets; }
void nds_diagnostics_reset_performance_history() { ++g_resets; }

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "ndsrecomp-slots-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const bool ok = shortcuts() && paths_and_metadata(root) && execution(root);
    std::filesystem::remove_all(root, ec);
    return ok ? 0 : 1;
}
