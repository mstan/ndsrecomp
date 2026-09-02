#pragma once

#include <filesystem>
#include <string>

namespace ndsrecomp::launcher {

struct NdsOnlineLaunchOptions {
    std::wstring network_backend = L"slirp";
    std::wstring wfc_provider = L"wiimmfi";
    bool generated_firmware = true;
};

inline std::wstring quote_arg(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

inline void append_arg(std::wstring& command, const wchar_t* name,
                       const std::wstring& value) {
    command += L" ";
    command += name;
    command += L" ";
    command += quote_arg(value);
}

inline std::filesystem::path default_firmware_state_path(
    const std::filesystem::path& launcher_settings_path) {
    return launcher_settings_path.parent_path() / "firmware-generated.bin";
}

inline void append_online_release_args(
    std::wstring& command,
    const std::filesystem::path& firmware_state_path,
    const NdsOnlineLaunchOptions& options = {}) {
    command += L" --startup-mode automatic --boot direct";
    append_arg(command, L"--network", L"on");
    append_arg(command, L"--network-backend", options.network_backend);
    append_arg(command, L"--wfc", L"on");
    append_arg(command, L"--wfc-provider", options.wfc_provider);
    if (options.generated_firmware) {
        append_arg(command, L"--firmware-state-path",
                   firmware_state_path.wstring());
        command += L" --freebios --generated-firmware";
    }
}

}  // namespace ndsrecomp::launcher
