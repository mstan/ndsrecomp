#include "live_overlay.h"
#include "runtime_arm.h"

#include <cstdio>
#include <cstring>

extern "C" ArmCpuState g_cpu = {};
extern "C" unsigned long long g_runtime_cycles = 0;
NdsCpu g_nds_active = NDS_ARM9;
NdsBusFastWin g_busf_main = {};
NdsBusFastWin g_busf_itcm = {};

unsigned g_registrations = 0u;
unsigned g_unregistrations = 0u;

extern "C" void nds_register_dispatch(int, const NdsDispatchEntry*, unsigned,
                                       uint32_t) { ++g_registrations; }
extern "C" void nds_unregister_dispatch(int, const NdsDispatchEntry*,
                                         unsigned) { ++g_unregistrations; }
bool coverage_manifest_write(const char*, char*, unsigned) { return false; }
bool coverage_manifest_write_live_snapshot(const char*, uint32_t, char*,
                                           unsigned) { return false; }
bool bus_range_has_write_provenance(uint32_t, uint32_t) { return false; }
bool bus_live_bytes_equal(uint32_t, const uint8_t*, uint32_t) { return false; }

namespace {

void body_a() {}
void body_b() {}

const uint8_t bytes_a[8] = {};
const uint8_t bytes_b[8] = {1, 2, 3, 4, 5, 6, 7, 8};

bool expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

NdsLiveBankInfo bank_info(const char* candidate_id,
                          const NdsDispatchEntry* rows,
                          unsigned row_count) {
    NdsLiveBankInfo info{};
    info.abi_version = NDS_LIVE_BANK_ABI_VERSION;
    info.bank_id = "test_live_bank";
    info.candidate_id = candidate_id;
    info.title_sha1 = "test";
    info.cpu = NDS_ARM9;
    info.static_cpu = static_cast<uint32_t>(NDS_ARM9);
    info.exc_base = 0xFFFF0000u;
    info.dispatch = rows;
    info.dispatch_len = row_count;
    info.linked_g_cpu = &g_cpu;
    info.linked_busf_main = &g_busf_main;
    info.linked_busf_itcm = &g_busf_itcm;
    info.linked_runtime_cycles = &g_runtime_cycles;
    return info;
}

bool preflight(const NdsLiveBankInfo& info, char* error, uint32_t error_len) {
    std::memset(error, 0, error_len);
    return live_overlay_preflight_for_test(&info, error, error_len);
}

bool metadata(const NdsLiveBankInfo& info, char* error, uint32_t error_len) {
    std::memset(error, 0, error_len);
    return live_overlay_info_for_test(&info, "test", error, error_len);
}

}  // namespace

int main() {
    char error[256];
    const NdsStaticValidation owner_a{0x02000000u, sizeof(bytes_a), bytes_a};
    const NdsStaticValidation owner_b{0x02000000u, sizeof(bytes_b), bytes_b};

    const NdsDispatchEntry interior_rows[] = {
        {0x02000000u, 0u, body_a, &owner_a},
        {0x02000004u, 0u, body_a, &owner_a},
    };
    if (!expect(preflight(bank_info("candidate-a", interior_rows, 2u),
                          error, sizeof(error)),
                "valid owner-backed interior row should be accepted"))
        return 1;

    const NdsStaticValidationRange closure_ranges[] = {
        {0x02000000u, sizeof(bytes_a), bytes_a},
    };
    const NdsStaticValidation closure_owner{
        0x02000000u, sizeof(bytes_a), bytes_a,
        closure_ranges, 1u};
    const NdsDispatchEntry closure_rows[] = {
        {0x02000000u, 0u, body_a, &closure_owner},
        {0x02000004u, 0u, body_a, &closure_owner},
    };
    NdsLiveBankInfo closure_info =
        bank_info("candidate-closure", closure_rows, 2u);
    closure_info.flags = NDS_LIVE_BANK_FLAG_DEPENDENCY_CLOSURE;
    if (!expect(preflight(closure_info, error, sizeof(error)),
                "complete shared dependency closure should be accepted"))
        return 1;

    NdsLiveBankInfo missing_closure =
        bank_info("candidate-missing-closure", interior_rows, 2u);
    missing_closure.flags = NDS_LIVE_BANK_FLAG_DEPENDENCY_CLOSURE;
    if (!expect(!preflight(missing_closure, error, sizeof(error)),
                "closure flag without dependency ranges should reject"))
        return 1;

    const NdsStaticValidationRange other_closure_ranges[] = {
        {0x02000000u, sizeof(bytes_a), bytes_a},
    };
    const NdsStaticValidation other_closure_owner{
        0x02000000u, sizeof(bytes_a), bytes_a,
        other_closure_ranges, 1u};
    const NdsDispatchEntry split_closure_rows[] = {
        {0x02000000u, 0u, body_a, &closure_owner},
        {0x02000004u, 0u, body_a, &other_closure_owner},
    };
    NdsLiveBankInfo split_closure =
        bank_info("candidate-split-closure", split_closure_rows, 2u);
    split_closure.flags = NDS_LIVE_BANK_FLAG_DEPENDENCY_CLOSURE;
    if (!expect(!preflight(split_closure, error, sizeof(error)),
                "rows with different dependency closures should reject"))
        return 1;

    const NdsDispatchEntry duplicate_rows[] = {
        {0x02000000u, 0u, body_a, &owner_a},
        {0x02000000u, 0u, body_b, &owner_b},
    };
    if (!expect(!preflight(bank_info("candidate-dup", duplicate_rows, 2u),
                           error, sizeof(error)),
                "duplicate rows inside one candidate should be rejected"))
        return 1;
    if (!expect(std::strstr(error, "duplicate") != nullptr,
                "duplicate rejection should be explicit"))
        return 1;

    const NdsDispatchEntry bad_interior[] = {
        {0x02000008u, 0u, body_a, &owner_a},
    };
    if (!expect(!preflight(bank_info("candidate-bad-interior", bad_interior,
                                     1u),
                           error, sizeof(error)),
                "interior row outside its owner validation should be rejected"))
        return 1;

    const NdsDispatchEntry null_validation[] = {
        {0x02000000u, 0u, body_a, nullptr},
    };
    if (!expect(!preflight(bank_info("candidate-null-validation",
                                     null_validation, 1u),
                           error, sizeof(error)),
                "live rows without validation should be rejected"))
        return 1;

    if (!expect(!preflight(bank_info("", interior_rows, 2u),
                           error, sizeof(error)),
                "candidate identity is required"))
        return 1;

    const NdsDispatchEntry unsorted_rows[] = {
        {0x02000004u, 0u, body_a, &owner_a},
        {0x02000000u, 0u, body_a, &owner_a},
    };
    if (!expect(!preflight(bank_info("candidate-unsorted", unsorted_rows,
                                     2u), error, sizeof(error)),
                "unsorted rows should be rejected"))
        return 1;

    const NdsDispatchEntry unaligned_arm[] = {
        {0x02000002u, 0u, body_a, &owner_a},
    };
    if (!expect(!preflight(bank_info("candidate-unaligned", unaligned_arm,
                                     1u), error, sizeof(error)),
                "unaligned ARM row should be rejected"))
        return 1;

    const NdsStaticValidation missing_bytes{
        0x02000000u, sizeof(bytes_a), nullptr};
    const NdsDispatchEntry partial_rows[] = {
        {0x02000000u, 0u, body_a, &missing_bytes},
    };
    if (!expect(!preflight(bank_info("candidate-partial", partial_rows, 1u),
                           error, sizeof(error)),
                "partial validation should be rejected"))
        return 1;

    NdsLiveBankInfo info = bank_info("candidate-metadata", interior_rows, 2u);
    if (!expect(metadata(info, error, sizeof(error)),
                "valid metadata should pass full preflight"))
        return 1;
    info.abi_version = NDS_LIVE_BANK_ABI_VERSION + 1u;
    if (!expect(!metadata(info, error, sizeof(error)) &&
                    std::strstr(error, "ABI") != nullptr,
                "wrong ABI should be rejected explicitly"))
        return 1;
    info = bank_info("candidate-metadata", interior_rows, 2u);
    info.title_sha1 = "wrong";
    if (!expect(!metadata(info, error, sizeof(error)) &&
                    std::strstr(error, "ROM") != nullptr,
                "wrong ROM should be rejected explicitly"))
        return 1;
    info = bank_info("candidate-metadata", interior_rows, 2u);
    info.cpu = 42;
    if (!expect(!metadata(info, error, sizeof(error)) &&
                    std::strstr(error, "CPU") != nullptr,
                "wrong CPU should be rejected explicitly"))
        return 1;
    info = bank_info("candidate-metadata", interior_rows, 2u);
    info.linked_g_cpu = nullptr;
    if (!expect(!metadata(info, error, sizeof(error)) &&
                    std::strstr(error, "imports") != nullptr,
                "shadowed DLL data imports should be rejected"))
        return 1;

    // The build hands the shard its CPU identity twice (metadata cpu and
    // -DNDS_STATIC_CPU). A disagreement runs one CPU's bodies under the
    // other's folded timing model, so it must fail closed.
    NdsLiveBankInfo wrong_static_cpu =
        bank_info("candidate-static-cpu", interior_rows, 2u);
    wrong_static_cpu.static_cpu = static_cast<uint32_t>(NDS_ARM7);
    if (!expect(!preflight(wrong_static_cpu, error, sizeof(error)) &&
                    std::strstr(error, "static CPU") != nullptr,
                "a shard built for the other CPU should be rejected"))
        return 1;
    if (!expect(!metadata(wrong_static_cpu, error, sizeof(error)),
                "the static CPU cross-check must also run in full preflight"))
        return 1;

    NdsLiveBankInfo arm7_bank = bank_info("candidate-arm7", interior_rows, 2u);
    arm7_bank.cpu = NDS_ARM7;
    arm7_bank.static_cpu = static_cast<uint32_t>(NDS_ARM7);
    arm7_bank.exc_base = 0x00000000u;
    if (!expect(preflight(arm7_bank, error, sizeof(error)),
                "a matching ARM7 static CPU identity should be accepted"))
        return 1;

    // beads-yjp.41: an in-process reset (runtime_init clears the dispatch
    // index, live_overlay_runtime_reset clears the registration bits) must
    // also unlatch the one-shot cache-scan guard, or every resident cached
    // shard stays dark until the process restarts.
    live_overlay_configure(true, false, 0u, 0u, 0u, "",
                           "live-overlay-test-cache-does-not-exist", "test");
    const unsigned before_publish = g_registrations;
    live_overlay_publish_bank_for_test(NDS_ARM9, "test_live_bank",
                                       "candidate-resident", interior_rows,
                                       2u);
    if (!expect(g_registrations == before_publish + 1u,
                "publishing a bank should register it once"))
        return 1;
    live_overlay_register_cached_banks();
    if (!expect(live_overlay_status_json().find(
                    "\"initial_cache_scan_done\":true") != std::string::npos,
                "the initial cache scan should latch after registration"))
        return 1;

    live_overlay_runtime_reset();
    if (!expect(live_overlay_status_json().find(
                    "\"registered\":false") != std::string::npos,
                "a runtime reset should clear the registration bits"))
        return 1;
    if (!expect(live_overlay_status_json().find(
                    "\"initial_cache_scan_done\":false") != std::string::npos,
                "a runtime reset should unlatch the cache-scan guard"))
        return 1;

#if defined(_WIN32)
    const unsigned before_poll = g_registrations;
    live_overlay_poll();
    if (!expect(g_registrations == before_poll + 1u,
                "one poll after a reset should re-register the cached bank"))
        return 1;
    if (!expect(live_overlay_status_json().find(
                    "\"registered\":true") != std::string::npos,
                "the revived bank should report itself registered"))
        return 1;
#endif
    live_overlay_shutdown();

    std::puts("PASS: live overlay preflight rejects malformed bundles");
    return 0;
}
