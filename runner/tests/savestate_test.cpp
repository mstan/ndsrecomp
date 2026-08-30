#include "savestate.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "runtime_arm.h"
#include "scheduler.h"
#include "state.h"

void savestate_test_fail_next_io_import();

namespace {

constexpr uint32_t kHeaderSize = 24u;
constexpr uint32_t kDirEntrySize = 32u;
constexpr uint32_t kSectionSchd = 0x44484353u; // SCHD
constexpr uint32_t kSectionRtim = 0x4D495452u; // RTIM
constexpr uint32_t kSectionIocr = 0x52434F49u; // IOCR
constexpr size_t kEncodedArmCpuStateBytes =
    (16u + 1u + ARM_BANK_COUNT * 3u + 5u + 5u) * 4u;
constexpr size_t kEncodedSchedulerCpuBlockBytes =
    kEncodedArmCpuStateBytes + 4u + 4u + 8u + 1u + 1u +
    NDS_RUNTIME_CALL_STACK_CAPACITY * 4u;

bool expect(bool value, const char* message) {
    if (!value) std::fprintf(stderr, "FAIL: %s\n", message);
    return value;
}

uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1u) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

uint32_t read_u32le(const std::vector<uint8_t>& bytes, size_t pos) {
    return uint32_t{bytes[pos]} |
        (uint32_t{bytes[pos + 1u]} << 8u) |
        (uint32_t{bytes[pos + 2u]} << 16u) |
        (uint32_t{bytes[pos + 3u]} << 24u);
}

uint64_t read_u64le(const std::vector<uint8_t>& bytes, size_t pos) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= uint64_t{bytes[pos + i]} << (i * 8u);
    return value;
}

void write_u32le(std::vector<uint8_t>& bytes, size_t pos, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[pos + i] = static_cast<uint8_t>(value >> (i * 8u));
}

bool load_bytes(const std::filesystem::path& path,
                std::vector<uint8_t>* bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    bytes->assign(std::istreambuf_iterator<char>(in),
                  std::istreambuf_iterator<char>());
    return true;
}

bool save_bytes(const std::filesystem::path& path,
                const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

bool section_payload(const std::filesystem::path& path, uint32_t tag,
                     std::vector<uint8_t>* payload) {
    std::vector<uint8_t> bytes;
    if (!load_bytes(path, &bytes) || bytes.size() < kHeaderSize)
        return false;
    const uint32_t count = read_u32le(bytes, 12u);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t dir = kHeaderSize + size_t{i} * kDirEntrySize;
        if (dir + kDirEntrySize > bytes.size()) return false;
        if (read_u32le(bytes, dir) != tag) continue;
        const uint64_t offset = read_u64le(bytes, dir + 8u);
        const uint64_t size = read_u64le(bytes, dir + 16u);
        if (offset + size > bytes.size()) return false;
        payload->assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                        bytes.begin() +
                            static_cast<std::ptrdiff_t>(offset + size));
        return true;
    }
    return false;
}

bool patch_section_u32(const std::filesystem::path& path, uint32_t tag,
                       size_t payload_offset, uint32_t value) {
    std::vector<uint8_t> bytes;
    if (!load_bytes(path, &bytes) || bytes.size() < kHeaderSize)
        return false;
    const uint32_t count = read_u32le(bytes, 12u);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t dir = kHeaderSize + size_t{i} * kDirEntrySize;
        if (dir + kDirEntrySize > bytes.size()) return false;
        if (read_u32le(bytes, dir) != tag) continue;
        const uint64_t section_offset = read_u64le(bytes, dir + 8u);
        const uint64_t section_size = read_u64le(bytes, dir + 16u);
        if (payload_offset + 4u > section_size ||
            section_offset + section_size > bytes.size())
            return false;
        write_u32le(bytes, static_cast<size_t>(section_offset) + payload_offset,
                    value);
        const uint8_t* payload =
            bytes.data() + static_cast<size_t>(section_offset);
        write_u32le(bytes, dir + 24u,
                    crc32(payload, static_cast<size_t>(section_size)));
        return save_bytes(path, bytes);
    }
    return false;
}

bool expect_le32_at(const std::vector<uint8_t>& bytes, size_t offset,
                    uint32_t value, const char* message) {
    if (!expect(offset + 4u <= bytes.size(), message)) return false;
    return expect(bytes[offset + 0u] == static_cast<uint8_t>(value) &&
                  bytes[offset + 1u] == static_cast<uint8_t>(value >> 8u) &&
                  bytes[offset + 2u] == static_cast<uint8_t>(value >> 16u) &&
                  bytes[offset + 3u] == static_cast<uint8_t>(value >> 24u),
                  message);
}

bool fill_pattern(std::vector<uint8_t>* bytes, uint8_t seed) {
    if (!bytes) return false;
    for (size_t i = 0; i < bytes->size(); ++i)
        (*bytes)[i] = static_cast<uint8_t>(seed + i * 17u);
    return true;
}

bool seed_core_state(uint8_t mem_seed, uint32_t arm9_r0,
                     uint32_t cp15_control) {
    NdsBusMemorySnapshot mem{};
    if (!bus_savestate_export(&mem)) return false;
    fill_pattern(&mem.main_ram, mem_seed);
    if (!bus_savestate_import(mem, nullptr)) return false;

    NdsSchedulerSaveState sched{};
    if (!scheduler_savestate_export(&sched)) return false;
    sched.cpu[0].R[0] = arm9_r0;
    sched.cpu[1].R[0] = arm9_r0 ^ 0x22222222u;
    sched.cycles[0] = arm9_r0 & 0xFFFFu;
    sched.cycles[1] = sched.cycles[0] >> 1u;
    sched.system_timestamp = sched.cycles[1];
    if (!scheduler_savestate_import(sched, nullptr)) return false;

    g_cp15.control = cp15_control;
    g_cp15.high_vectors = (cp15_control & (1u << 13u)) != 0;
    return true;
}

bool expect_live_seed(uint8_t mem_seed, uint32_t arm9_r0,
                      uint32_t cp15_control) {
    NdsBusMemorySnapshot mem{};
    NdsSchedulerSaveState sched{};
    if (!bus_savestate_export(&mem) ||
        !scheduler_savestate_export(&sched))
        return false;
    bool ok = true;
    ok &= expect(!mem.main_ram.empty() && mem.main_ram[0] == mem_seed,
                 "failed load must not replace current RAM");
    ok &= expect(sched.cpu[0].R[0] == arm9_r0,
                 "failed load must not replace scheduler state");
    ok &= expect(g_cp15.control == cp15_control,
                 "failed load must not replace CP15 state");
    return ok;
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
    const NdsSavestateIdentity empty_rom{"build-a", ""};
    ok &= expect(!nds_savestate_save_core(path.string(), empty_rom, &error),
                 "empty ROM SHA-1 must not be saved");
    ok &= expect(!nds_savestate_load_core(path.string(), empty_rom, &error),
                 "empty expected ROM SHA-1 must not be loaded");
    const NdsSavestateIdentity upper_rom{
        "build-a", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"};
    ok &= expect(!nds_savestate_save_core(path.string(), upper_rom, &error),
                 "uppercase ROM SHA-1 must not be saved");
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

bool scheduler_cpu_byte_layout_is_stable() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "ndsrecomp-savestate-scheduler-layout.nss";
    std::filesystem::remove(path);

    runtime_init(nullptr);
    bus_init();
    cp15_reset();
    scheduler_init();
    scheduler_reset_cpu(0, 0xFFFF0000u, 0xD3u);
    scheduler_reset_cpu(1, 0x00000000u, 0xD3u);

    NdsSchedulerSaveState sched{};
    if (!expect(scheduler_savestate_export(&sched), "export scheduler layout"))
        return false;
    sched.cpu[0].R[0] = 0x01020304u;
    sched.cpu[0].R[15] = 0x15161718u;
    sched.cpu[0].cpsr = 0x21222324u;
    sched.cpu[0].banked_sp[ARM_BANK_IRQ] = 0x31323334u;
    sched.cpu[0].banked_lr[ARM_BANK_SUPERVISOR] = 0x41424344u;
    sched.cpu[0].banked_spsr[ARM_BANK_UNDEFINED] = 0x51525354u;
    sched.cpu[0].r8_12_user[3] = 0x61626364u;
    sched.cpu[0].r8_12_fiq[4] = 0x71727374u;
    sched.crs_depth[0] = 2u;
    sched.deferred_cycles[0] = 0x81828384u;
    sched.cycles[0] = 0x0102030405060708ull;
    sched.started[0] = 1u;
    sched.terminal_halted[0] = 0u;
    sched.crs[0][0] = 0x91929394u;
    sched.crs[0][1] = 0xA1A2A3A4u;

    sched.cpu[1].R[0] = 0xB1B2B3B4u;
    sched.cpu[1].cpsr = 0xC1C2C3C4u;
    sched.crs_depth[1] = 1u;
    sched.deferred_cycles[1] = 0xD1D2D3D4u;
    sched.cycles[1] = 0x1112131415161718ull;
    sched.started[1] = 0u;
    sched.terminal_halted[1] = 1u;
    sched.crs[1][0] = 0xE1E2E3E4u;
    sched.system_timestamp = 0x2122232425262728ull;

    if (!expect(scheduler_savestate_import(sched, nullptr),
                "import scheduler layout"))
        return false;

    const NdsSavestateIdentity identity{
        "build-layout", "0123456789abcdef0123456789abcdef01234567"};
    std::string error;
    if (!expect(nds_savestate_save_core(path.string(), identity, &error),
                error.c_str()))
        return false;

    std::vector<uint8_t> payload;
    bool ok = expect(section_payload(path, kSectionSchd, &payload),
                     "extract scheduler section");
    ok &= expect(payload.size() == 2u * kEncodedSchedulerCpuBlockBytes + 8u,
                 "scheduler section has explicit packed size");

    constexpr size_t kR0 = 0u;
    constexpr size_t kR15 = 15u * 4u;
    constexpr size_t kCpsr = 16u * 4u;
    constexpr size_t kBankedSp =
        kCpsr + 4u + ARM_BANK_IRQ * 4u;
    constexpr size_t kBankedLr =
        kCpsr + 4u + ARM_BANK_COUNT * 4u +
        ARM_BANK_SUPERVISOR * 4u;
    constexpr size_t kBankedSpsr =
        kCpsr + 4u + ARM_BANK_COUNT * 8u +
        ARM_BANK_UNDEFINED * 4u;
    constexpr size_t kR8User =
        kCpsr + 4u + ARM_BANK_COUNT * 12u + 3u * 4u;
    constexpr size_t kR8Fiq =
        kCpsr + 4u + ARM_BANK_COUNT * 12u + 5u * 4u + 4u * 4u;
    constexpr size_t kCrsDepth = kEncodedArmCpuStateBytes;
    constexpr size_t kDeferred = kCrsDepth + 4u;
    constexpr size_t kCycles = kDeferred + 4u;
    constexpr size_t kStarted = kCycles + 8u;
    constexpr size_t kTerminalHalted = kStarted + 1u;
    constexpr size_t kCrs = kTerminalHalted + 1u;

    ok &= expect_le32_at(payload, kR0, 0x01020304u,
                         "ARM9 R0 is explicit little-endian");
    ok &= expect_le32_at(payload, kR15, 0x15161718u,
                         "ARM9 R15 offset is stable");
    ok &= expect_le32_at(payload, kCpsr, 0x21222324u,
                         "ARM9 CPSR offset is stable");
    ok &= expect_le32_at(payload, kBankedSp, 0x31323334u,
                         "ARM9 banked SP offset is stable");
    ok &= expect_le32_at(payload, kBankedLr, 0x41424344u,
                         "ARM9 banked LR offset is stable");
    ok &= expect_le32_at(payload, kBankedSpsr, 0x51525354u,
                         "ARM9 banked SPSR offset is stable");
    ok &= expect_le32_at(payload, kR8User, 0x61626364u,
                         "ARM9 user R8-R12 bank offset is stable");
    ok &= expect_le32_at(payload, kR8Fiq, 0x71727374u,
                         "ARM9 FIQ R8-R12 bank offset is stable");
    ok &= expect_le32_at(payload, kCrsDepth, 2u,
                         "ARM9 CRS depth follows CPU fields without padding");
    ok &= expect_le32_at(payload, kDeferred, 0x81828384u,
                         "ARM9 deferred cycles offset is stable");
    ok &= expect_le32_at(payload, kCycles + 0u, 0x05060708u,
                         "ARM9 cycles low word is little-endian");
    ok &= expect_le32_at(payload, kCycles + 4u, 0x01020304u,
                         "ARM9 cycles high word is little-endian");
    ok &= expect(payload[kStarted] == 1u && payload[kTerminalHalted] == 0u,
                 "ARM9 scheduler flags are packed bytes");
    ok &= expect_le32_at(payload, kCrs + 0u, 0x91929394u,
                         "ARM9 CRS entry 0 is little-endian");
    ok &= expect_le32_at(payload, kCrs + 4u, 0xA1A2A3A4u,
                         "ARM9 CRS entry 1 is little-endian");

    const size_t cpu1 = kEncodedSchedulerCpuBlockBytes;
    ok &= expect_le32_at(payload, cpu1 + kR0, 0xB1B2B3B4u,
                         "ARM7 R0 starts at explicit second CPU block");
    ok &= expect_le32_at(payload, cpu1 + kCpsr, 0xC1C2C3C4u,
                         "ARM7 CPSR offset is stable");
    ok &= expect(payload[cpu1 + kStarted] == 0u &&
                 payload[cpu1 + kTerminalHalted] == 1u,
                 "ARM7 scheduler flags are packed bytes");
    ok &= expect_le32_at(payload, cpu1 + kCrs, 0xE1E2E3E4u,
                         "ARM7 CRS entry is little-endian");
    ok &= expect_le32_at(payload, 2u * kEncodedSchedulerCpuBlockBytes + 0u,
                         0x25262728u,
                         "system timestamp low word is little-endian");
    ok &= expect_le32_at(payload, 2u * kEncodedSchedulerCpuBlockBytes + 4u,
                         0x21222324u,
                         "system timestamp high word is little-endian");

    std::filesystem::remove(path);
    runtime_shutdown();
    return ok;
}

bool semantic_import_failure_rolls_back() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "ndsrecomp-savestate-rollback.nss";
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
    bool ok = true;
    ok &= expect(seed_core_state(0x11u, 0x11111111u, 0x00000001u),
                 "seed saved state");
    ok &= expect(nds_savestate_save_core(path.string(), identity, &error),
                 error.c_str());
    ok &= expect(seed_core_state(0x77u, 0x77777777u, 0x00002001u),
                 "seed current state before invalid runtime load");
    ok &= expect(patch_section_u32(path, kSectionRtim, 24u, 2u),
                 "patch runtime section with invalid active CPU");
    ok &= expect(!nds_savestate_load_core(path.string(), identity, &error),
                 "invalid runtime section must be rejected");
    ok &= expect_live_seed(0x77u, 0x77777777u, 0x00002001u);

    ok &= expect(seed_core_state(0x11u, 0x11111111u, 0x00000001u),
                 "reseed saved state");
    ok &= expect(nds_savestate_save_core(path.string(), identity, &error),
                 error.c_str());
    ok &= expect(seed_core_state(0x88u, 0x88888888u, 0x00002002u),
                 "seed current state before invalid scheduler load");
    ok &= expect(patch_section_u32(
                     path, kSectionSchd, kEncodedArmCpuStateBytes,
                     NDS_RUNTIME_CALL_STACK_CAPACITY + 1u),
                 "patch scheduler section with invalid call stack depth");
    ok &= expect(!nds_savestate_load_core(path.string(), identity, &error),
                 "invalid scheduler section must be rejected");
    ok &= expect_live_seed(0x88u, 0x88888888u, 0x00002002u);

    std::filesystem::remove(path);
    runtime_shutdown();
    return ok;
}

NdsIoCoreSaveState patterned_io(uint32_t seed) {
    NdsIoCoreSaveState out{};
    out.vcount = static_cast<uint16_t>(seed % 263u);
    out.next_vcount = static_cast<uint16_t>(seed + 17u);
    out.next_vcount_valid = 1u;
    out.in_vblank = 1u;
    out.display_last = 0x1100000000000000ull | seed;
    out.gxfifo_stall = 1u;
    for (int cpu = 0; cpu < 2; ++cpu) {
        out.ipcsync_out[cpu] = static_cast<uint16_t>(seed + cpu);
        out.postflg[cpu] = static_cast<uint8_t>((seed + cpu) & 1u);
        out.dispstat[cpu] = static_cast<uint16_t>(0x108u + seed + cpu);
        out.vcount_match[cpu] = static_cast<uint8_t>(cpu);
        out.ime[cpu] = seed + 0x100u + cpu;
        out.ie[cpu] = seed + 0x200u + cpu;
        out.irq_flags[cpu] = seed + 0x300u + cpu;
        out.haltcnt[cpu] = static_cast<uint8_t>(seed + cpu);
        out.cpu_halted[cpu] = static_cast<uint8_t>(cpu);
        out.halt_entry_cycle[cpu] = 0x2200000000000000ull + seed + cpu;
        out.fifo_count[cpu] = 16u;
        out.fifo_head[cpu] = static_cast<uint8_t>(15 - cpu);
        out.fifocnt[cpu] = 0x8404u;
        out.fifo_lastrx[cpu] = seed + 0x400u + cpu;
        out.dma_entry_cycle[cpu] = 0x3300000000000000ull + seed + cpu;
        out.timer_last[cpu] = 0x4400000000000000ull + seed + cpu;
        out.exmemcnt[cpu] = static_cast<uint16_t>(0x4000u + seed + cpu);
        out.keycnt[cpu] = static_cast<uint16_t>(0x8000u + seed + cpu);
        for (int i = 0; i < 16; ++i)
            out.fifo[cpu][i] = seed + cpu * 0x100u + i;
        for (int ch = 0; ch < 4; ++ch) {
            auto& dma = out.dma[cpu][ch];
            dma.src = seed + 0x1000u + cpu * 0x100u + ch;
            dma.dst = seed + 0x2000u + cpu * 0x100u + ch;
            dma.cnt = seed + 0x3000u + cpu * 0x100u + ch;
            dma.cur_src = dma.src + 4u;
            dma.cur_dst = dma.dst + 8u;
            dma.remaining = seed + ch + 1u;
            dma.src_inc = (ch % 3) - 1;
            dma.dst_inc = 1 - (ch % 3);
            dma.burst_index = static_cast<uint16_t>(seed + ch);
            dma.start_mode = cpu == 0 ? static_cast<uint8_t>(ch)
                                      : static_cast<uint8_t>(0x10u + ch);
            dma.running = static_cast<uint8_t>(ch & 1);
            dma.in_progress = static_cast<uint8_t>((ch + 1) & 1);
            dma.burst_start = static_cast<uint8_t>(ch & 1);
            auto& timer = out.timer[cpu][ch];
            timer.reload = static_cast<uint16_t>(seed + ch);
            timer.counter = static_cast<uint16_t>(seed + 0x100u + ch);
            timer.ctrl = static_cast<uint16_t>(0x80u | ch);
            timer.accum = static_cast<uint64_t>(seed + ch) & 63u;
        }
    }
    out.divcnt = static_cast<uint16_t>(seed);
    out.sqrtcnt = static_cast<uint16_t>(seed + 1u);
    for (int i = 0; i < 2; ++i) {
        out.div_numer[i] = seed + 0x5000u + i;
        out.div_denom[i] = seed + 0x6000u + i;
        out.div_quot[i] = seed + 0x7000u + i;
        out.div_rem[i] = seed + 0x8000u + i;
        out.sqrt_value[i] = seed + 0x9000u + i;
    }
    out.div_deadline = 0x5500000000000000ull + seed;
    out.sqrt_result = seed + 0xA000u;
    out.sqrt_deadline = 0x6600000000000000ull + seed;
    out.powercontrol7 = static_cast<uint16_t>(seed & 3u);
    out.keyinput = seed + 0xB000u;
    out.rcnt = static_cast<uint16_t>(seed + 2u);
    out.wramcnt = static_cast<uint8_t>(seed);
    out.wifiwaitcnt = static_cast<uint16_t>(seed + 3u);
    out.biosprot = seed + 0xC000u;
    out.pm_index = static_cast<uint8_t>(seed & 7u);
    out.pm_hold = 1u;
    out.powered_off = 1u;
    out.tsc_ctrl = static_cast<uint8_t>(seed + 4u);
    out.tsc_conv = static_cast<uint16_t>(seed + 5u);
    out.tsc_datapos = 2;
    out.tsc_x = static_cast<uint16_t>(seed + 6u);
    out.tsc_y = static_cast<uint16_t>(seed + 7u);
    for (size_t i = 0; i < 8u; ++i) {
        out.pm_regs[i] = static_cast<uint8_t>(seed + i);
        out.pm_masks[i] = static_cast<uint8_t>(seed + 0x10u + i);
    }
    for (size_t i = 0; i < sizeof(out.io_mem); ++i)
        out.io_mem[i] = static_cast<uint8_t>(seed + i * 13u);
    return out;
}

bool io_core_roundtrip_and_rollback() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "ndsrecomp-savestate-io-core.nss";
    std::filesystem::remove(path);
    runtime_init(nullptr);
    bus_init();
    cp15_reset();
    scheduler_init();
    scheduler_reset_cpu(0, 0xFFFF0000u, 0xD3u);
    scheduler_reset_cpu(1, 0x00000000u, 0xD3u);
    const NdsSavestateIdentity identity{
        "build-io", "0123456789abcdef0123456789abcdef01234567"};
    const NdsIoCoreSaveState saved = patterned_io(0x21u);
    const NdsIoCoreSaveState current = patterned_io(0x71u);
    std::string error;
    bool ok = expect(io_savestate_import(saved, &error), "seed IO state") &&
        expect(nds_savestate_save_core(path.string(), identity, &error),
               error.c_str()) &&
        expect(io_savestate_import(current, &error), "mutate IO state") &&
        expect(nds_savestate_load_core(path.string(), identity, &error),
               error.c_str());
    NdsIoCoreSaveState actual{};
    ok &= expect(io_savestate_export(&actual), "export loaded IO state");
    ok &= expect(std::memcmp(&actual, &saved, sizeof(saved)) == 0,
                 "every covered IO field roundtrips");

    ok &= expect(io_savestate_import(current, &error),
                 "restore current IO state before apply failure");
    savestate_test_fail_next_io_import();
    ok &= expect(!nds_savestate_load_core(path.string(), identity, &error),
                 "semantic IO apply failure rejects load");
    actual = {};
    ok &= expect(io_savestate_export(&actual),
                 "export IO state after apply rollback");
    ok &= expect(std::memcmp(&actual, &current, sizeof(current)) == 0,
                 "semantic apply failure rolls back IO state");

    // IO semantic corruption is rejected during prevalidation, before apply.
    // IOCR byte 254 is DMA[0][0].src_inc in the explicit packed layout.
    ok &= expect(patch_section_u32(path, kSectionIocr, 254u, 2u),
                 "patch invalid DMA increment");
    ok &= expect(!nds_savestate_load_core(path.string(), identity, &error),
                 "invalid IO section must be rejected");
    actual = {};
    ok &= expect(io_savestate_export(&actual), "export rolled-back IO state");
    ok &= expect(std::memcmp(&actual, &current, sizeof(current)) == 0,
                 "invalid IO load rolls back every covered field");
    std::filesystem::remove(path);
    runtime_shutdown();
    return ok;
}

bool reject_states(void* context, std::string* error) {
    ++*static_cast<unsigned*>(context);
    if (error) *error = "network session connected";
    return false;
}

bool eligibility_hook_rejects_before_export() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "ndsrecomp-savestate-policy.nss";
    std::filesystem::remove(path);
    runtime_init(nullptr);
    bus_init();
    cp15_reset();
    scheduler_init();
    const NdsSavestateIdentity identity{
        "build-policy", "0123456789abcdef0123456789abcdef01234567"};
    unsigned calls = 0;
    std::string error;
    nds_savestate_set_eligibility_hook(reject_states, &calls);
    bool ok = expect(!nds_savestate_save_core(path.string(), identity, &error),
                     "eligibility hook rejects save") &&
        expect(error == "network session connected",
               "eligibility rejection preserves policy reason") &&
        expect(calls == 1u && !std::filesystem::exists(path),
               "rejected save exports no file");
    nds_savestate_set_eligibility_hook(nullptr, nullptr);
    runtime_shutdown();
    return ok;
}

bool control_thread_is_not_quiescent_owner() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "ndsrecomp-savestate-control-thread.nss";
    std::filesystem::remove(path);
    runtime_init(nullptr);
    bus_init();
    cp15_reset();
    scheduler_init();
    const NdsSavestateIdentity identity{
        "build-thread", "0123456789abcdef0123456789abcdef01234567"};
    bool result = true;
    std::string error;
    std::thread control([&] {
        result = nds_savestate_save_core(path.string(), identity, &error);
    });
    control.join();
    bool ok = expect(!result, "control thread save is rejected") &&
        expect(error == "savestate requires the scheduler owner between rounds",
               "control thread rejection identifies owner policy") &&
        expect(!std::filesystem::exists(path),
               "control thread cannot export a partial state");
    runtime_shutdown();
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= core_roundtrip();
    ok &= identity_rejects();
    ok &= corrupt_section_rejects();
    ok &= scheduler_cpu_byte_layout_is_stable();
    ok &= semantic_import_failure_rolls_back();
    ok &= io_core_roundtrip_and_rollback();
    ok &= eligibility_hook_rejects_before_export();
    ok &= control_thread_is_not_quiescent_owner();
    return ok ? 0 : 1;
}
