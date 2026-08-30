#include "savestate.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "runtime_arm.h"
#include "scheduler.h"
#include "state.h"

namespace {

bool expect(bool value, const char* message) {
    if (!value) std::fprintf(stderr, "FAIL: %s\n", message);
    return value;
}

bool fill_pattern(std::vector<uint8_t>* bytes, uint8_t seed) {
    if (!bytes) return false;
    for (size_t i = 0; i < bytes->size(); ++i)
        (*bytes)[i] = static_cast<uint8_t>(seed + i * 17u);
    return true;
}

bool core_roundtrip() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "ndsrecomp-savestate-test.nss";
    std::filesystem::remove(path);

    runtime_init(nullptr);
    bus_init();
    cp15_reset();
    scheduler_init();
    scheduler_reset_cpu(0, 0xFFFF0100u, 0xD3u);
    scheduler_reset_cpu(1, 0x00000100u, 0xD3u);

    NdsBusMemorySnapshot mem{};
    if (!expect(bus_savestate_export(&mem), "export initial memory"))
        return false;
    if (!expect(mem.main_ram.size() == 4u * 1024u * 1024u,
                "main RAM is present in core snapshot"))
        return false;
    fill_pattern(&mem.main_ram, 0x31u);
    fill_pattern(&mem.main_ram_written, 0x01u);
    for (size_t i = 0; i < mem.main_ram_generation.size(); ++i)
        mem.main_ram_generation[i] = static_cast<uint32_t>(i + 7u);
    if (!expect(bus_savestate_import(mem, nullptr), "seed main memory"))
        return false;

    NdsSchedulerSaveState sched{};
    if (!expect(scheduler_savestate_export(&sched), "export scheduler"))
        return false;
    sched.cpu[0].R[0] = 0x90000001u;
    sched.cpu[0].R[15] = 0x02000100u;
    sched.cpu[0].cpsr = 0x13u;
    sched.cpu[1].R[0] = 0x70000001u;
    sched.cpu[1].R[15] = 0x03800100u;
    sched.cpu[1].cpsr = 0x33u;
    sched.cycles[0] = 123456u;
    sched.cycles[1] = 61728u;
    sched.system_timestamp = 61728u;
    sched.crs_depth[0] = 2u;
    sched.crs[0][0] = 0x02001001u;
    sched.crs[0][1] = 0x02002000u;
    sched.deferred_cycles[0] = 5u;
    if (!expect(scheduler_savestate_import(sched, nullptr),
                "seed scheduler state"))
        return false;

    g_cp15.control = 0x00053078u;
    g_cp15.high_vectors = true;
    g_cp15.itcm_enable = true;
    g_cp15.dtcm_enable = true;
    g_cp15.itcm_size = 32u * 1024u;
    g_cp15.dtcm_base = 0x027C0000u;
    g_cp15.dtcm_size = 16u * 1024u;

    const NdsSavestateIdentity identity{
        "build-for-roundtrip", "0123456789abcdef0123456789abcdef01234567"};
    std::string error;
    if (!expect(nds_savestate_save_core(path.string(), identity, &error),
                error.c_str()))
        return false;

    bus_init();
    cp15_reset();
    scheduler_init();
    scheduler_reset_cpu(0, 0xFFFF0000u, 0xD3u);
    scheduler_reset_cpu(1, 0x00000000u, 0xD3u);

    if (!expect(nds_savestate_load_core(path.string(), identity, &error),
                error.c_str()))
        return false;

    NdsBusMemorySnapshot after_mem{};
    NdsSchedulerSaveState after_sched{};
    if (!expect(bus_savestate_export(&after_mem), "export loaded memory") ||
        !expect(scheduler_savestate_export(&after_sched),
                "export loaded scheduler"))
        return false;

    bool ok = true;
    ok &= expect(after_mem.main_ram == mem.main_ram,
                 "main RAM roundtrips exactly");
    ok &= expect(after_mem.main_ram_written == mem.main_ram_written,
                 "main RAM provenance roundtrips exactly");
    ok &= expect(after_mem.main_ram_generation == mem.main_ram_generation,
                 "main RAM generations roundtrip exactly");
    ok &= expect(after_sched.cpu[0].R[0] == sched.cpu[0].R[0],
                 "ARM9 register state roundtrips");
    ok &= expect(after_sched.cpu[1].R[0] == sched.cpu[1].R[0],
                 "ARM7 register state roundtrips");
    ok &= expect(after_sched.cycles[0] == sched.cycles[0] &&
                 after_sched.cycles[1] == sched.cycles[1] &&
                 after_sched.system_timestamp == sched.system_timestamp,
                 "scheduler timing roundtrips");
    ok &= expect(after_sched.crs_depth[0] == sched.crs_depth[0] &&
                 after_sched.crs[0][1] == sched.crs[0][1],
                 "call-return stack roundtrips");
    ok &= expect(g_cp15.control == 0x00053078u &&
                 g_cp15.dtcm_base == 0x027C0000u,
                 "CP15 timing-visible state roundtrips");
    ok &= expect(g_nds_fast_limit == 0u,
                 "host dispatch/tick caches are invalidated after load");

    std::filesystem::remove(path);
    runtime_shutdown();
    return ok;
}

bool identity_rejects() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "ndsrecomp-savestate-id.nss";
    std::filesystem::remove(path);
    runtime_init(nullptr);
    bus_init();
    cp15_reset();
    scheduler_init();
    scheduler_reset_cpu(0, 0xFFFF0000u, 0xD3u);
    scheduler_reset_cpu(1, 0x00000000u, 0xD3u);

    const NdsSavestateIdentity identity{
        "build-a", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    std::string error;
    bool ok = expect(nds_savestate_save_core(path.string(), identity, &error),
                     error.c_str());
    const NdsSavestateIdentity wrong_build{
        "build-b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    ok &= expect(!nds_savestate_load_core(path.string(), wrong_build, &error),
                 "wrong exact build id must be rejected");
    const NdsSavestateIdentity wrong_rom{
        "build-a", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"};
    ok &= expect(!nds_savestate_load_core(path.string(), wrong_rom, &error),
                 "wrong ROM SHA-1 must be rejected");
    std::filesystem::remove(path);
    runtime_shutdown();
    return ok;
}

bool corrupt_section_rejects() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "ndsrecomp-savestate-corrupt.nss";
    std::filesystem::remove(path);
    runtime_init(nullptr);
    bus_init();
    cp15_reset();
    scheduler_init();
    scheduler_reset_cpu(0, 0xFFFF0000u, 0xD3u);
    scheduler_reset_cpu(1, 0x00000000u, 0xD3u);

    const NdsSavestateIdentity identity{
        "build-a", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    std::string error;
    if (!expect(nds_savestate_save_core(path.string(), identity, &error),
                error.c_str()))
        return false;
    FILE* f = std::fopen(path.string().c_str(), "r+b");
    if (!expect(f != nullptr, "open state for corruption"))
        return false;
    if (std::fseek(f, -1, SEEK_END) == 0) {
        int ch = std::fgetc(f);
        std::fseek(f, -1, SEEK_END);
        std::fputc((ch ^ 0x5A) & 0xFF, f);
    }
    std::fclose(f);
    const bool rejected =
        !nds_savestate_load_core(path.string(), identity, &error);
    std::filesystem::remove(path);
    runtime_shutdown();
    return expect(rejected, "corrupt section checksum must be rejected");
}

}  // namespace

int main() {
    bool ok = true;
    ok &= core_roundtrip();
    ok &= identity_rejects();
    ok &= corrupt_section_rejects();
    return ok ? 0 : 1;
}
