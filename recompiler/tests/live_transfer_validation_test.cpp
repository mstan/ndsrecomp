// Verifies the compiler boundaries used by live RAM candidates. Ordinary
// live-byte validation returns every inter-function edge through the runtime
// resolver. Explicit dependency-closure mode may transfer directly only among
// bodies covered by that complete identity. Unsafe mode remains diagnostic.

#include "sha1.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

std::string read_generated(const std::filesystem::path& dir) {
    std::string value;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".c" ||
            entry.path().filename().string().find("dispatch") !=
                std::string::npos) {
            continue;
        }
        std::ifstream file(entry.path(), std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        value += text.str();
    }
    return value;
}

bool run_recompiler(const std::filesystem::path& recompile,
                    const std::filesystem::path& config,
                    const std::filesystem::path& bin,
                    const std::filesystem::path& out,
                    bool unsafe, bool closure = false) {
    std::filesystem::create_directories(out);
    std::ostringstream command;
    command << '"' << recompile.string() << '"'
            << " --config \"" << config.string() << '"'
            << " --bin \"" << bin.string() << '"'
            << " --out \"" << out.string() << '"'
            << " --bank live_edge --shards 1 --validate-live-bytes";
    if (unsafe) command << " --unsafe-live-direct-calls";
    if (closure)
        command << " --validated-live-direct-calls --coalesce-fallthroughs";
#ifdef _WIN32
    const std::string line = "\"" + command.str() + "\"";
#else
    const std::string line = command.str();
#endif
    return std::system(line.c_str()) == 0;
}

bool expect(bool value, const char* message) {
    if (value) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

bool direct_targets_have_bodies(const std::string& generated) {
    const std::regex definition(
        R"(void (live_edge_[at]func_[0-9A-Fa-f]+)\(void\) \{)");
    const std::regex call(R"((live_edge_[at]func_[0-9A-Fa-f]+)\(\);)");
    std::unordered_set<std::string> bodies;
    for (std::sregex_iterator it(generated.begin(), generated.end(),
                                 definition), end;
         it != end; ++it) {
        bodies.insert((*it)[1].str());
    }
    for (std::sregex_iterator it(generated.begin(), generated.end(), call),
                                 end;
         it != end; ++it) {
        if (!bodies.count((*it)[1].str())) return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <nds_recompile> <work dir>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path recompile = argv[1];
    const std::filesystem::path work = argv[2];
    std::filesystem::remove_all(work);
    std::filesystem::create_directories(work);

    // 02000000: BL 02000020
    // 02000004: B  02000040
    // 02000020/02000040: BX LR
    std::vector<unsigned char> program(0xE2u, 0u);
    const unsigned char root[] = {
        0x06, 0x00, 0x00, 0xEB,
        0x0D, 0x00, 0x00, 0xEA,
    };
    const unsigned char return_arm[] = {0x1E, 0xFF, 0x2F, 0xE1};
    std::copy(root, root + sizeof(root), program.begin());
    std::copy(return_arm, return_arm + sizeof(return_arm),
              program.begin() + 0x20u);
    std::copy(return_arm, return_arm + sizeof(return_arm),
              program.begin() + 0x40u);
    const unsigned char thumb_b[] = {0x0E, 0xE0};       // B 020000A0
    const unsigned char thumb_bx_lr[] = {0x70, 0x47};   // BX LR
    const unsigned char thumb_bx_r3[] = {0x18, 0x47};   // BX R3
    const unsigned char thumb_blx_r3[] = {0x98, 0x47};  // BLX R3
    const unsigned char thumb_pop_pc[] = {0x00, 0xBD};  // POP {PC}
    std::copy(thumb_b, thumb_b + sizeof(thumb_b),
              program.begin() + 0x80u);
    std::copy(thumb_bx_lr, thumb_bx_lr + sizeof(thumb_bx_lr),
              program.begin() + 0xA0u);
    std::copy(thumb_bx_r3, thumb_bx_r3 + sizeof(thumb_bx_r3),
              program.begin() + 0xC0u);
    std::copy(thumb_blx_r3, thumb_blx_r3 + sizeof(thumb_blx_r3),
              program.begin() + 0xD0u);
    std::copy(thumb_pop_pc, thumb_pop_pc + sizeof(thumb_pop_pc),
              program.begin() + 0xE0u);

    const std::filesystem::path bin = work / "live-edge.bin";
    {
        std::ofstream file(bin, std::ios::binary);
        file.write(reinterpret_cast<const char*>(program.data()),
                   static_cast<std::streamsize>(program.size()));
    }
    const std::filesystem::path config = work / "live-edge.toml";
    {
        std::ofstream file(config);
        file << "[program]\n"
                "name = \"validated live edge fixture\"\n"
                "id = \"live_edge\"\n"
                "cpu = \"arm9\"\n"
                "isa = \"armv5te\"\n"
                "load_address = 0x02000000\n"
                "size = 0x000000E2\n"
                "entry_pc = 0x02000000\n"
                "authoritative_entry_points = false\n\n"
                "[identity]\nsha1 = \""
             << gba::sha1(program.data(), program.size()).hex()
             << "\"\n\n"
                "[[entry_point]]\n"
                "addr = 0x02000000\n"
                "mode = \"arm\"\n"
                "name = \"root\"\n"
                "kind = \"test\"\n\n"
                "[[entry_point]]\n"
                "addr = 0x02000080\n"
                "mode = \"thumb\"\n"
                "name = \"thumb_tail\"\n"
                "kind = \"test\"\n\n"
                "[[entry_point]]\n"
                "addr = 0x020000C0\n"
                "mode = \"thumb\"\n"
                "name = \"thumb_bx\"\n"
                "kind = \"test\"\n\n"
                "[[entry_point]]\n"
                "addr = 0x020000D0\n"
                "mode = \"thumb\"\n"
                "name = \"thumb_blx\"\n"
                "kind = \"test\"\n\n"
                "[[entry_point]]\n"
                "addr = 0x020000E0\n"
                "mode = \"thumb\"\n"
                "name = \"thumb_pop\"\n"
                "kind = \"test\"\n";
    }

    const auto safe_dir = work / "safe";
    const auto unsafe_dir = work / "unsafe";
    const auto closure_dir = work / "closure";
    if (!run_recompiler(recompile, config, bin, safe_dir, false) ||
        !run_recompiler(recompile, config, bin, unsafe_dir, true) ||
        !run_recompiler(recompile, config, bin, closure_dir, false, true)) {
        std::fprintf(stderr, "FAIL: recompiler invocation failed\n");
        return 1;
    }
    const std::string safe = read_generated(safe_dir);
    const std::string unsafe = read_generated(unsafe_dir);
    const std::string closure = read_generated(closure_dir);
    std::ifstream closure_dispatch_file(
        closure_dir / "live_edge_dispatch.c", std::ios::binary);
    std::ostringstream closure_dispatch_text;
    closure_dispatch_text << closure_dispatch_file.rdbuf();
    const std::string closure_dispatch = closure_dispatch_text.str();
    // B2: a validated transfer now goes out through a per-callsite link
    // slot instead of a bare literal-dispatch call. The slot carries the
    // same compile-time target (bit 0 = THUMB) and the runtime resolves it
    // through the very same candidate lookup, so the property under test is
    // unchanged: no direct C call to another native body.
    if (!expect(safe.find("= {0, 0u, 0x02000020u, 0};") != std::string::npos &&
                    safe.find("runtime_link_call(&") != std::string::npos,
                "validated BL must dispatch through candidate lookup") ||
        !expect(safe.find("= {0, 0u, 0x02000040u, 0};") != std::string::npos &&
                    safe.find("runtime_link_branch(&") != std::string::npos,
                "validated tail B must dispatch through candidate lookup") ||
        !expect(safe.find("live_edge_afunc_02000020();") ==
                    std::string::npos,
                "validated BL must not direct-call another native body") ||
        !expect(unsafe.find("live_edge_afunc_02000020();") !=
                    std::string::npos,
                "unsafe diagnostic mode must isolate direct-call behavior") ||
        !expect(safe.find("= {0, 0u, 0x020000A1u, 0};") != std::string::npos,
                "validated Thumb tail branch must dispatch") ||
        !expect(safe.find("live_edge_tfunc_020000A0();") ==
                    std::string::npos,
                "validated Thumb branch must not direct-call native") ||
        !expect(unsafe.find("live_edge_tfunc_020000A0();") !=
                    std::string::npos,
                "unsafe mode must expose Thumb direct-call behavior") ||
        !expect(closure.find("live_edge_afunc_02000020();") !=
                    std::string::npos &&
                    closure.find("live_edge_tfunc_020000A0();") !=
                    std::string::npos,
                "validated closure may direct-transfer within its candidate") ||
        !expect(closure_dispatch.find("NdsStaticValidationRange") !=
                    std::string::npos &&
                    closure_dispatch.find("g_validation_closure_") !=
                    std::string::npos,
                "validated direct mode must emit an explicit exact-range closure") ||
        !expect(direct_targets_have_bodies(closure),
                "coalesced direct targets must resolve to an emitted owner body") ||
        !expect(safe.find("NDS_LIVE_TRANSFER_BX") != std::string::npos &&
                    safe.find("NDS_LIVE_TRANSFER_BLX_REG") != std::string::npos &&
                    safe.find("NDS_LIVE_TRANSFER_LDM_PC") != std::string::npos,
                "Thumb BX, BLX, and POP-PC transfers must be instrumented") ||
        !expect(safe.find("? ~1u : ~3u") != std::string::npos,
                "BX and BLX must word-align ARM-state exchange targets") ||
        !expect(safe.find("runtime_live_transfer(") != std::string::npos,
                "validated edges must remain visible in the transfer ring")) {
        return 1;
    }
    std::puts("PASS: live ARM/Thumb transfers cannot bypass validation");
    return 0;
}
