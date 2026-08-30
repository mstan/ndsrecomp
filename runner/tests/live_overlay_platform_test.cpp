#include "live_overlay_platform.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

void touch(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).put('\n');
}

#if defined(__linux__)
void make_executable(const std::filesystem::path& path) {
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_exec |
                  std::filesystem::perms::group_exec |
                  std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add);
}
#endif

}  // namespace

int main() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::temp_directory_path() /
        "ndsrecomp live overlay platform test";
    fs::remove_all(root, ec);
    fs::create_directories(root / "overlay_toolchain" / "include");
    touch(root / "overlay_toolchain" / "compile_live_shards.py");
#if defined(_WIN32)
    touch(root / "overlay_toolchain" / "python" / "python.exe");
    touch(root / "overlay_toolchain" / "nds_recompile.exe");
    touch(root / "overlay_toolchain" / "tcc" / "tcc.exe");
    touch(root / "nds_runner.exe");
    constexpr const char* suffix = ".dll";
    constexpr char shell_quote = '"';
#elif defined(__linux__)
    const auto python = root / "overlay_toolchain" / "python" / "bin" /
        "python3";
    const auto recompiler = root / "overlay_toolchain" / "nds_recompile";
    const auto tcc = root / "overlay_toolchain" / "tcc" / "tcc";
    const auto runner = root / "nds_runner";
    for (const auto& executable : {python, recompiler, tcc, runner}) {
        touch(executable);
        make_executable(executable);
    }
    constexpr const char* suffix = ".so";
    constexpr char shell_quote = '\'';
#else
    constexpr const char* suffix = "";
    constexpr char shell_quote = '\0';
#endif

    bool ok = true;
    ok &= expect(std::string(live_overlay_library_suffix()) == suffix,
                 "platform shared-library suffix");
    if (*suffix) {
        ok &= expect(live_overlay_is_final_library_path(
                         root / (std::string("bank") + suffix)),
                     "published shared library accepted");
        ok &= expect(!live_overlay_is_final_library_path(
                         root / (std::string("bank.stage") + suffix)),
                     "staging shared library rejected");
        ok &= expect(!live_overlay_is_final_library_path(root / "bank.txt"),
                     "unrelated artifact rejected");
        const std::string command = live_overlay_bundled_tcc_command(root);
        ok &= expect(!command.empty(), "complete bundled toolchain detected");
        ok &= expect(command.front() == shell_quote,
                     "bundled command quotes paths containing spaces");
        ok &= expect(command.find("--compiler tcc") != std::string::npos &&
                         command.find("--include-roots") != std::string::npos &&
                         command.find("--runner-exe") != std::string::npos,
                     "bundled command carries required producer contract");
#if defined(__linux__)
        fs::permissions(tcc, fs::perms::owner_exec |
                                 fs::perms::group_exec |
                                 fs::perms::others_exec,
                        fs::perm_options::remove);
        ok &= expect(live_overlay_bundled_tcc_command(root).empty(),
                     "non-executable bundled tool rejected");
        make_executable(tcc);
#endif
        fs::remove(root / "overlay_toolchain" / "compile_live_shards.py", ec);
        ok &= expect(live_overlay_bundled_tcc_command(root).empty(),
                     "incomplete bundled toolchain rejected");
    }
    fs::remove_all(root, ec);
    if (ok) std::puts("PASS: live overlay platform contract");
    return ok ? 0 : 1;
}
