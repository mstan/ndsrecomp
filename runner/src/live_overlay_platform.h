#pragma once

#include <filesystem>
#include <string>

const char* live_overlay_library_suffix();
bool live_overlay_is_final_library_path(const std::filesystem::path& path);
std::string live_overlay_bundled_tcc_command(
    const std::filesystem::path& exe_dir);
