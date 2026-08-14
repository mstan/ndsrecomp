// Pins the interior-entry fail-closed emission.
//
// Every generated function body opens with a resume switch over its interior
// PCs whose default arm calls runtime_dispatch_miss() and returns. That arm
// is what makes entering a compiled function at an untranslated interior PC
// degrade safely to Tier 3 instead of silently no-oping — the property the
// overlay pipeline's content-validated banks rely on (psxrecomp models the
// same contract as psx_native_bad_entry). This test runs the real
// nds_recompile over a synthetic two-instruction ARM function and fails if
// the emitted C ever loses that arm.
//
// Usage: interior_entry_fail_closed_test <nds_recompile path> <work dir>

#include "sha1.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr,
                     "usage: %s <nds_recompile> <work dir>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path recompile = argv[1];
    const std::filesystem::path work = argv[2];
    std::filesystem::create_directories(work);

    // MOV r0, #1 ; BX lr — two ARM instructions, so the resume switch has
    // exactly one interior case plus the default arm under test.
    const unsigned char program[] = {
        0x01, 0x00, 0xA0, 0xE3,   // e3a00001 MOV r0, #1
        0x1E, 0xFF, 0x2F, 0xE1,   // e12fff1e BX lr
    };
    const std::filesystem::path bin = work / "pin.bin";
    {
        std::ofstream out(bin, std::ios::binary);
        out.write(reinterpret_cast<const char*>(program), sizeof(program));
    }

    const std::string digest = gba::sha1(program, sizeof(program)).hex();
    const std::filesystem::path config = work / "pin.toml";
    {
        std::ofstream out(config);
        out << "[program]\n"
               "name = \"interior-entry pin fixture\"\n"
               "id = \"pin_arm9\"\n"
               "load_address = 0x02000000\n"
               "size = 0x00000008\n"
               "entry_pc = 0x02000000\n"
               "authoritative_entry_points = false\n"
               "\n"
               "[identity]\n"
               "sha1 = \"" << digest << "\"\n"
               "\n"
               "[[entry_point]]\n"
               "addr = 0x02000000\n"
               "mode = \"arm\"\n"
               "name = \"entry\"\n"
               "kind = \"test\"\n";
    }

    std::ostringstream command;
    command << '"' << recompile.string() << '"'
            << " --config \"" << config.string() << '"'
            << " --bin \"" << bin.string() << '"'
            << " --out \"" << work.string() << '"'
            << " --bank pin_arm9 --shards 2 --validate-live-bytes";
#ifdef _WIN32
    // cmd.exe strips the outer quotes of a quoted command line; wrap once
    // more so a space-containing recompiler path survives.
    const std::string line = "\"" + command.str() + "\"";
#else
    const std::string line = command.str();
#endif
    if (std::system(line.c_str()) != 0) {
        std::fprintf(stderr, "FAIL: nds_recompile invocation failed\n");
        return 1;
    }

    std::string generated;
    for (const char* name : {"pin_arm9_00.c", "pin_arm9_01.c", "pin_arm9.c"}) {
        generated += read_file(work / name);
    }
    if (generated.empty()) {
        std::fprintf(stderr, "FAIL: no generated bank body found\n");
        return 1;
    }

    const std::string resume_guard = "if (g_cpu.R[15] != 0x02000000u)";
    const std::string fail_closed =
        "default: runtime_dispatch_miss(g_cpu.R[15]); return;";
    if (generated.find(resume_guard) == std::string::npos) {
        std::fprintf(stderr,
            "FAIL: generated body lost its interior-PC resume switch\n");
        return 1;
    }
    if (generated.find(fail_closed) == std::string::npos) {
        std::fprintf(stderr,
            "FAIL: resume switch no longer fails closed to "
            "runtime_dispatch_miss -- overlay banks would silently no-op "
            "on interior entries\n");
        return 1;
    }
    std::puts("PASS: interior entries fail closed to runtime_dispatch_miss");
    return 0;
}
