// codegen_golden_test — pins nds_recompile's generated-C emission to the
// declared codegen version in src/codegen_identity.h.
//
// beads-yjp.52. The live-shard provider identity no longer hashes the
// recompiler executable (a PE's bytes move on every rebuild, which threw away
// every player's shard cache for nothing). It folds kCodegenVersion instead.
// A declared constant is only safe if forgetting to bump it is caught, and
// that is this test: it runs the recompiler over a fixed corpus covering every
// emission mode the shard pipeline uses, hashes the generated C, and compares
// the digest against tests/codegen_golden.txt.
//
//   digest moved, version did not  -> FAIL (emission changed silently)
//   digest moved, version moved    -> FAIL until the golden is regenerated
//   --update refuses to write a moved digest under an unmoved version
//
// so emission cannot change without the version changing, and the version
// cannot change without a deliberate golden capture.
//
// Regenerate:  codegen_golden_test <nds_recompile> <work> <golden> --update

#include "codegen_identity.h"
#include "sha1.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kPageSize = 4096;

// One emission run: a corpus program plus the exact flag set to emit it under.
struct Case {
    const char* name;
    int cpu;             // 9 = ARMv5TE @ 0x02000000, 7 = ARMv4T @ 0x037F8000
    const char* flags;
};

// Deterministic filler. Random-looking bytes are the cheapest way to reach a
// wide slice of the decoder and codegen surface; the constants are fixed so
// the corpus is byte-identical on every machine and every run.
std::vector<uint8_t> corpus_page(uint32_t seed) {
    std::vector<uint8_t> page(kPageSize, 0);
    uint32_t state = seed;
    for (std::size_t i = 0; i < kPageSize; i++) {
        state = state * 1664525u + 1013904223u;
        page[i] = static_cast<uint8_t>(state >> 24);
    }
    // Hand-placed, well-formed sequences so the corpus is not purely noise:
    // every one of these is a shape the shard pipeline emits special code for
    // (direct call, tail branch, interworking, multi-register transfer, the
    // ARMv5TE extensions, and the supervisor/PSR paths).
    struct Blob { std::size_t at; std::vector<uint8_t> bytes; };
    const std::vector<Blob> blobs = {
        // 0x000 root: BL +0x20; B +0x40 (tail)
        {0x000, {0x06,0x00,0x00,0xEB, 0x0D,0x00,0x00,0xEA}},
        // 0x020 leaf: MOV r0,#1; BX LR
        {0x020, {0x01,0x00,0xA0,0xE3, 0x1E,0xFF,0x2F,0xE1}},
        // 0x040 tail target: MOV r0,#3; BX LR
        {0x040, {0x03,0x00,0xA0,0xE3, 0x1E,0xFF,0x2F,0xE1}},
        // 0x060 PUSH {r4,lr}; LDR r0,[r4,#4]; STR r0,[r4,#8]; POP {r4,pc}
        {0x060, {0x10,0x40,0x2D,0xE9, 0x04,0x00,0x94,0xE5,
                 0x08,0x00,0x84,0xE5, 0x10,0x80,0xBD,0xE8}},
        // 0x080 MRS r0,CPSR; MSR CPSR_c,r0; SWI #0x06; MOV pc,lr
        {0x080, {0x00,0x00,0x0F,0xE1, 0x00,0xF0,0x21,0xE1,
                 0x06,0x00,0x00,0xEF, 0x0E,0xF0,0xA0,0xE1}},
        // 0x0A0 MUL r0,r1,r2; MLA r0,r1,r2,r3; UMULL r0,r1,r2,r3; BX LR
        {0x0A0, {0x91,0x02,0x00,0xE0, 0x91,0x32,0x20,0xE0,
                 0x92,0x03,0x81,0xE0, 0x1E,0xFF,0x2F,0xE1}},
        // 0x0C0 ARMv5TE: CLZ r0,r1; QADD r0,r1,r2; BLX r3; BX LR
        {0x0C0, {0x11,0x0F,0x6F,0xE1, 0x51,0x00,0x02,0xE1,
                 0x33,0xFF,0x2F,0xE1, 0x1E,0xFF,0x2F,0xE1}},
        // 0x0E0 ARMv5TE LDRD/STRD + PLD, then BX LR
        {0x0E0, {0xD0,0x00,0xC4,0xE1, 0xF0,0x00,0xC4,0xE1,
                 0x00,0xF0,0xD4,0xF5, 0x1E,0xFF,0x2F,0xE1}},
        // 0x100 Thumb: B +0x1C
        {0x100, {0x0E,0xE0}},
        // 0x120 Thumb: BX LR
        {0x120, {0x70,0x47}},
        // 0x140 Thumb: BX r3
        {0x140, {0x18,0x47}},
        // 0x150 Thumb: BLX r3
        {0x150, {0x98,0x47}},
        // 0x160 Thumb: PUSH {r4,lr}; MOV r0,#7; POP {r4,pc}
        {0x160, {0x10,0xB5, 0x07,0x20, 0x10,0xBD}},
        // 0x170 Thumb: LDR r0,[r1,#0]; STR r0,[r1,#4]; SWI #2; POP {pc}
        {0x170, {0x08,0x68, 0x48,0x60, 0x02,0xDF, 0x00,0xBD}},
        // 0x180 PC-relative literal load + wide LDM/STM, then MOV pc,lr
        {0x180, {0x08,0x00,0x9F,0xE5, 0xFF,0x0F,0x91,0xE8,
                 0xFF,0x0F,0x82,0xE8, 0x0E,0xF0,0xA0,0xE1,
                 0x21,0x43,0x65,0x87, 0x89,0xAB,0xCD,0xEF}},
    };
    for (const auto& blob : blobs)
        std::copy(blob.bytes.begin(), blob.bytes.end(),
                  page.begin() + static_cast<std::ptrdiff_t>(blob.at));
    // 0x200: one deliberately LONG straight-line body (41 x MOV r0,r0 then
    // BX LR = 168 bytes). Without it nothing in the corpus exceeds the
    // --max-function-bytes split threshold, and the split case would emit the
    // same bodies as the unsplit one -- i.e. cover nothing.
    for (std::size_t i = 0; i < 41; i++) {
        const std::array<uint8_t, 4> nop = {0x00, 0x00, 0xA0, 0xE1};
        std::copy(nop.begin(), nop.end(),
                  page.begin() + static_cast<std::ptrdiff_t>(0x200 + i * 4));
    }
    const std::array<uint8_t, 4> bx_lr = {0x1E, 0xFF, 0x2F, 0xE1};
    std::copy(bx_lr.begin(), bx_lr.end(),
              page.begin() + static_cast<std::ptrdiff_t>(0x200 + 41 * 4));
    return page;
}

std::string config_text(const std::vector<uint8_t>& page, int cpu,
                        uint32_t base) {
    std::ostringstream out;
    const char* cpu_name = (cpu == 9) ? "arm9" : "arm7";
    const char* isa = (cpu == 9) ? "armv5te" : "armv4t";
    out << "[program]\n"
        << "name = \"codegen golden " << cpu_name << "\"\n"
        << "id = \"codegen_golden_" << cpu_name << "\"\n"
        << "cpu = \"" << cpu_name << "\"\n"
        << "isa = \"" << isa << "\"\n";
    char line[64];
    std::snprintf(line, sizeof line, "load_address = 0x%08X\n", base);
    out << line;
    std::snprintf(line, sizeof line, "size = 0x%08X\n",
                  static_cast<unsigned>(kPageSize));
    out << line;
    std::snprintf(line, sizeof line, "entry_pc = 0x%08X\n", base);
    out << line;
    out << "authoritative_entry_points = false\n\n"
        << "[identity]\nsha1 = \""
        << gba::sha1(page.data(), page.size()).hex() << "\"\n\n";
    // Every hand-placed sequence above is declared as an observed root, the
    // way the live pipeline declares the PCs it saw execute.
    const std::pair<uint32_t, const char*> roots[] = {
        {0x000, "arm"},  {0x020, "arm"},  {0x040, "arm"},  {0x060, "arm"},
        {0x080, "arm"},  {0x0A0, "arm"},  {0x0C0, "arm"},  {0x0E0, "arm"},
        {0x100, "thumb"}, {0x120, "thumb"}, {0x140, "thumb"},
        {0x150, "thumb"}, {0x160, "thumb"}, {0x170, "thumb"},
        {0x180, "arm"},  {0x200, "arm"},
    };
    for (const auto& [offset, mode] : roots) {
        std::snprintf(line, sizeof line, "addr = 0x%08X\n", base + offset);
        out << "[[entry_point]]\n" << line
            << "mode = \"" << mode << "\"\n"
            << "kind = \"runtime_generation_observed\"\n\n";
    }
    return out.str();
}

bool write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
    return static_cast<bool>(file);
}

// Concatenate every generated artifact in `dir`, name-tagged and sorted, so
// the digest covers file NAMES (shard/bank naming is emission too) as well as
// contents.
std::string read_generated(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> files;
    if (std::filesystem::is_directory(dir))
        for (const auto& entry : std::filesystem::directory_iterator(dir))
            if (entry.is_regular_file()) files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    std::string value;
    for (const auto& path : files) {
        value += "\n=== ";
        value += path.filename().string();
        value += " ===\n";
        std::ifstream file(path, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        value += text.str();
    }
    return value;
}

std::string trim(std::string text) {
    while (!text.empty() &&
           (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    std::size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
        start++;
    return text.substr(start);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "usage: codegen_golden_test <nds_recompile> <work-dir> "
            "<golden-file> [--update]\n");
        return 2;
    }
    const std::filesystem::path recompile = argv[1];
    const std::filesystem::path work = argv[2];
    const std::filesystem::path golden_path = argv[3];
    const bool update = (argc > 4 && std::string(argv[4]) == "--update");

    // The exact flag sets the shard pipeline and the static bank build use,
    // plus the diagnostic modes, so a change to any of them moves the digest.
    const Case cases[] = {
        {"static_bank",   9, "--shards 1"},
        {"static_shards", 9, "--shards 3"},
        {"split_small",   9, "--shards 1 --max-function-bytes 64"},
        {"live_safe",     9, "--shards 1 --validate-live-bytes"},
        {"live_closure",  9, "--shards 1 --stable-address-shards "
                             "--max-function-bytes 512 --validate-live-bytes "
                             "--validated-live-direct-calls "
                             "--coalesce-fallthroughs"},
        {"live_unsafe",   9, "--shards 1 --validate-live-bytes "
                             "--unsafe-live-direct-calls"},
        {"arm7_static",   7, "--shards 1"},
        {"arm7_closure",  7, "--shards 1 --stable-address-shards "
                             "--max-function-bytes 512 --validate-live-bytes "
                             "--validated-live-direct-calls "
                             "--coalesce-fallthroughs"},
    };

    std::error_code ec;
    std::filesystem::remove_all(work, ec);
    std::filesystem::create_directories(work, ec);

    // The provider identity in tools/compile_live_shards.py asks the binary
    // itself, so the CLI contract is part of what this test pins: a binary
    // whose --codegen-identity disagrees with the header it was built from
    // would hand the shard cache a version nothing else believes.
    {
        const auto reported_path = work / "codegen-identity.txt";
        std::ostringstream command;
        command << '"' << recompile.string() << '"' << " --codegen-identity > \""
                << reported_path.string() << '"';
#ifdef _WIN32
        const std::string line = "\"" + command.str() + "\"";
#else
        const std::string line = command.str();
#endif
        if (std::system(line.c_str()) != 0) {
            std::fprintf(stderr,
                "FAIL: %s --codegen-identity failed; the live-shard provider "
                "identity depends on this flag\n", recompile.string().c_str());
            return 1;
        }
        std::ifstream file(reported_path);
        std::string reported;
        std::getline(file, reported);
        reported = trim(reported);
        const std::string expected =
            std::string(ndsrecomp::kCodegenIdentityPrefix) +
            std::to_string(ndsrecomp::kCodegenVersion);
        if (reported != expected) {
            std::fprintf(stderr,
                "FAIL: --codegen-identity reported \"%s\", expected \"%s\" "
                "(stale binary, or the flag stopped reporting the header "
                "constant)\n", reported.c_str(), expected.c_str());
            return 1;
        }
    }

    // `corpus` is the emitted C; `inputs` is what it was emitted FROM. The
    // golden records both, because "the digest moved" only means "emission
    // changed" while the inputs are unchanged. Editing the corpus here moves
    // the digest for a reason that has nothing to do with codegen semantics,
    // and the update guard has to be able to tell the two apart.
    std::string corpus;
    std::string inputs;
    for (const Case& item : cases) {
        const uint32_t base = (item.cpu == 9) ? 0x02000000u : 0x037F8000u;
        const auto page = corpus_page(item.cpu == 9 ? 0xC0DE9u : 0xC0DE7u);
        const auto case_dir = work / item.name;
        std::filesystem::create_directories(case_dir, ec);
        const auto bin = case_dir / "page.bin";
        const auto config = case_dir / "page.toml";
        const auto out = case_dir / "out";
        std::filesystem::create_directories(out, ec);
        {
            std::ofstream file(bin, std::ios::binary);
            file.write(reinterpret_cast<const char*>(page.data()),
                       static_cast<std::streamsize>(page.size()));
            if (!file) {
                std::fprintf(stderr, "FAIL: cannot write %s\n",
                             bin.string().c_str());
                return 1;
            }
        }
        const std::string config_body = config_text(page, item.cpu, base);
        if (!write_file(config, config_body)) {
            std::fprintf(stderr, "FAIL: cannot write %s\n",
                         config.string().c_str());
            return 1;
        }
        inputs += item.name;
        inputs += '\0';
        inputs += item.flags;
        inputs += '\0';
        inputs += config_body;
        inputs += '\0';
        inputs.append(reinterpret_cast<const char*>(page.data()), page.size());
        inputs += '\0';
        std::ostringstream command;
        command << '"' << recompile.string() << '"'
                << " --config \"" << config.string() << '"'
                << " --bin \"" << bin.string() << '"'
                << " --out \"" << out.string() << '"'
                << " --bank codegen_golden_" << item.name
                << ' ' << item.flags;
#ifdef _WIN32
        const std::string line = "\"" + command.str() + "\" > nul 2>&1";
#else
        const std::string line = command.str() + " > /dev/null 2>&1";
#endif
        if (std::system(line.c_str()) != 0) {
            std::fprintf(stderr, "FAIL: recompiler failed for case %s\n",
                         item.name);
            return 1;
        }
        const std::string generated = read_generated(out);
        if (generated.size() < 512) {
            std::fprintf(stderr,
                "FAIL: case %s emitted %zu bytes; the corpus must actually "
                "reach the emission path\n", item.name, generated.size());
            return 1;
        }
        corpus += "\n##### case ";
        corpus += item.name;
        corpus += " #####\n";
        corpus += generated;
    }

    const std::string digest =
        gba::sha1(corpus.data(), corpus.size()).hex();
    const std::string corpus_digest =
        gba::sha1(inputs.data(), inputs.size()).hex();

    unsigned golden_version = 0;
    std::string golden_digest;
    std::string golden_corpus;
    bool have_golden = false;
    {
        std::ifstream file(golden_path);
        std::string key, value;
        while (file >> key >> value) {
            if (key == "codegen_version")
                golden_version = static_cast<unsigned>(std::strtoul(
                    value.c_str(), nullptr, 10));
            else if (key == "digest") { golden_digest = trim(value); }
            else if (key == "corpus_digest") { golden_corpus = trim(value); }
        }
        have_golden = !golden_digest.empty();
    }
    const bool same_corpus = (golden_corpus == corpus_digest);

    if (update) {
        if (have_golden && same_corpus && golden_digest != digest &&
            ndsrecomp::kCodegenVersion <= golden_version) {
            std::fprintf(stderr,
                "REFUSING to update the golden: generated-C emission changed "
                "but kCodegenVersion is still %u.\n"
                "Emission changed, bump kCodegenVersion in "
                "recompiler/src/codegen_identity.h (and SHARD_CODEGEN_VERSION "
                "in tools/compile_live_shards.py if the shard pipeline's own "
                "emission changed too), then regenerate goldens.\n",
                ndsrecomp::kCodegenVersion);
            return 1;
        }
        if (have_golden && !same_corpus)
            std::printf(
                "note: the corpus in codegen_golden_test.cpp changed, so the "
                "previous digest is not comparable and the unbumped-version "
                "guard does not apply to this update.\n");
        std::ostringstream out;
        out << "# nds_recompile generated-C emission golden.\n"
               "# Regenerate: build codegen_golden_test and run it with "
               "--update.\n"
               "# See recompiler/src/codegen_identity.h for the contract.\n"
               "# corpus_digest covers the test's INPUTS (cases, flags,\n"
               "# configs, page bytes); digest covers the C they emit.\n"
            << "codegen_version " << ndsrecomp::kCodegenVersion << "\n"
            << "corpus_digest " << corpus_digest << "\n"
            << "digest " << digest << "\n";
        if (!write_file(golden_path, out.str())) {
            std::fprintf(stderr, "FAIL: cannot write %s\n",
                         golden_path.string().c_str());
            return 1;
        }
        std::printf("wrote %s: codegen_version %u corpus %s digest %s\n",
                    golden_path.string().c_str(),
                    ndsrecomp::kCodegenVersion, corpus_digest.c_str(),
                    digest.c_str());
        return 0;
    }

    if (!have_golden) {
        std::fprintf(stderr,
            "FAIL: no golden at %s. Run codegen_golden_test with --update.\n",
            golden_path.string().c_str());
        return 1;
    }

    if (!same_corpus) {
        std::fprintf(stderr,
            "FAIL: the corpus in codegen_golden_test.cpp changed (golden %s, "
            "actual %s) without the golden being regenerated. This says "
            "nothing about emission; rerun with --update.\n",
            golden_corpus.empty() ? "<none>" : golden_corpus.c_str(),
            corpus_digest.c_str());
        return 1;
    }

    bool ok = true;
    if (golden_version != ndsrecomp::kCodegenVersion) {
        std::fprintf(stderr,
            "FAIL: golden was captured at codegen_version %u but this binary "
            "reports %u.\n"
            "Emission changed, bump SHARD_CODEGEN_VERSION (recompiler "
            "equivalent: kCodegenVersion in recompiler/src/codegen_identity.h) "
            "and regenerate goldens.\n",
            golden_version, ndsrecomp::kCodegenVersion);
        ok = false;
    }
    if (golden_digest != digest) {
        std::fprintf(stderr,
            "FAIL: generated-C emission changed.\n"
            "  golden digest %s\n"
            "  actual digest %s\n"
            "Emission changed, bump SHARD_CODEGEN_VERSION (recompiler "
            "equivalent: kCodegenVersion in recompiler/src/codegen_identity.h) "
            "and regenerate goldens.\n"
            "Every cached live shard is compiled by this emission path and "
            "binds directly to runner symbols, so an unbumped change is silent "
            "memory corruption on a warm cache, not a clean failure.\n",
            golden_digest.c_str(), digest.c_str());
        ok = false;
    }
    if (!ok) return 1;
    std::printf("PASS: emission matches golden (codegen_version %u, %s)\n",
                ndsrecomp::kCodegenVersion, digest.c_str());
    return 0;
}
