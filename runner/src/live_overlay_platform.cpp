#include "live_overlay_platform.h"

#include <system_error>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace {

std::string lower_ascii(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    }
    return text;
}

std::string shell_quote(const std::filesystem::path& path) {
#if defined(_WIN32)
    return "\"" + path.string() + "\"";
#else
    std::string out = "'";
    for (const char ch : path.string()) {
        if (ch == '\'') out += "'\\''";
        else out += ch;
    }
    out += "'";
    return out;
#endif
}

bool is_executable_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
#if defined(__linux__)
    return access(path.c_str(), X_OK) == 0;
#else
    return true;
#endif
}

}  // namespace

const char* live_overlay_library_suffix() {
#if defined(_WIN32)
    return ".dll";
#elif defined(__linux__)
    return ".so";
#else
    return "";
#endif
}

bool live_overlay_is_final_library_path(const std::filesystem::path& path) {
    const std::string suffix = live_overlay_library_suffix();
    if (suffix.empty()) return false;
    const std::string filename = lower_ascii(path.filename().string());
    const std::string stage_suffix = ".stage" + suffix;
    if (filename.size() >= stage_suffix.size() &&
        filename.compare(filename.size() - stage_suffix.size(),
                         stage_suffix.size(), stage_suffix) == 0) {
        return false;
    }
    return lower_ascii(path.extension().string()) == suffix;
}

std::string live_overlay_bundled_tcc_command(
    const std::filesystem::path& exe_dir) {
    std::error_code ec;
    const auto toolchain = exe_dir / "overlay_toolchain";
#if defined(_WIN32)
    const auto python = toolchain / "python" / "python.exe";
    const auto recompiler = toolchain / "nds_recompile.exe";
    const auto tcc = toolchain / "tcc" / "tcc.exe";
    const auto runner = exe_dir / "nds_runner.exe";
#elif defined(__linux__)
    auto python = toolchain / "python" / "bin" / "python3";
    if (!std::filesystem::is_regular_file(python, ec)) {
        ec.clear();
        python = toolchain / "python" / "bin" / "python";
    }
    const auto recompiler = toolchain / "nds_recompile";
    const auto tcc = toolchain / "tcc" / "tcc";
    const auto runner = exe_dir / "nds_runner";
#else
    return {};
#endif
    const auto script = toolchain / "compile_live_shards.py";
    const auto include = toolchain / "include";
    ec.clear();
    if (!std::filesystem::is_regular_file(script, ec)) return {};
    for (const auto& required : {python, recompiler, tcc, runner}) {
        ec.clear();
        if (!is_executable_file(required)) return {};
    }
    ec.clear();
    if (!std::filesystem::is_directory(include, ec)) return {};

    // Root-only snapshots are the fresh-install steady state, so
    // --include-roots is part of the provider contract, not a tuning flag.
    return shell_quote(python) + " " + shell_quote(script) +
        " --recompiler " + shell_quote(recompiler) +
        " --runtime-include " + shell_quote(include) +
        " --runner-exe " + shell_quote(runner) +
        " --compiler tcc --tcc " + shell_quote(tcc) +
        " --include-roots --min-hits 8";
}
