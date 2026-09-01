#include "live_overlay.h"
#include "runtime_arm.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

extern "C" ArmCpuState g_cpu = {};
extern "C" unsigned long long g_runtime_cycles = 0;
extern "C" uint64_t g_insn_count[2] = {};
extern "C" uint32_t g_insn_hook_armed = 0;
extern "C" unsigned long long g_nds_fast_limit = 0;
extern "C" unsigned char g_nds_unwinding = 0;
NdsCpu g_nds_active = NDS_ARM9;
NdsBusFastWin g_busf_main = {};
NdsBusFastWin g_busf_itcm = {};

extern "C" uint32_t runtime_code_cycles(uint32_t) { return 0; }
extern "C" uint32_t arm9_refill_cycles(uint32_t) { return 0; }
extern "C" void runtime_dispatch_bad_entry(uint32_t) {}
extern "C" void runtime_dispatch_with_exchange(uint32_t) {}
extern "C" void runtime_live_transfer(uint32_t, uint32_t, uint32_t) {}
extern "C" void runtime_call_push_return(uint32_t) {}
extern "C" int runtime_call_should_return(uint32_t) { return 0; }
extern "C" void runtime_call_cancel_return(uint32_t) {}
extern "C" void runtime_insn_slow(void) {}
extern "C" void runtime_tick_slow(uint32_t) {}
extern "C" bool runtime_should_yield_slow(void) { return false; }

unsigned g_registrations = 0u;
unsigned g_unregistrations = 0u;

extern "C" void nds_register_dispatch(int, const NdsDispatchEntry*, unsigned,
                                       uint32_t) { ++g_registrations; }
extern "C" void nds_unregister_dispatch(int, const NdsDispatchEntry*,
                                         unsigned) { ++g_unregistrations; }
bool coverage_manifest_write(const char*, char*, unsigned) { return false; }

// beads-yjp.59: the live snapshot is captured on the emulation thread and
// written on the maintenance worker. The capture has to succeed for the unit
// layer to exercise the async path at all; the write is what refuses.
struct CoverageLiveSnapshot { int unused; };
bool g_allow_snapshot_write = false;
CoverageLiveSnapshot* coverage_manifest_capture_live_snapshot(uint32_t) {
    return new CoverageLiveSnapshot{0};
}
bool coverage_manifest_write_captured_snapshot(const CoverageLiveSnapshot*,
                                               const char* path, char* error,
                                               unsigned error_cap) {
    if (g_allow_snapshot_write && path) {
        std::ofstream(path, std::ios::binary) << "{}\n";
        return true;
    }
    if (error && error_cap)
        std::snprintf(error, error_cap, "test stub refuses to write");
    return false;
}
void coverage_manifest_release_snapshot(CoverageLiveSnapshot* snapshot) {
    delete snapshot;
}
// beads-yjp.62: the guard-bytes preflight now runs before a prepared shard is
// adopted, so "is the guest code resident right now" has to be steerable from
// the unit layer. Default false, which is the state a fresh install is in for
// every overlay/ITCM window that is not the scene currently on screen.
bool g_bytes_live = false;
bool bus_range_has_write_provenance(uint32_t, uint32_t) { return g_bytes_live; }
bool bus_live_bytes_equal(uint32_t, const uint8_t*, uint32_t) {
    return g_bytes_live;
}
bool g_static_cover_enabled = false;
uint32_t g_static_cover_pc = 0u;
extern "C" bool nds_dispatch_static_bank_covers(int, uint32_t pc,
                                                uint8_t) {
    return g_static_cover_enabled && pc == g_static_cover_pc;
}

namespace {

void body_a() {}
void body_b() {}

const uint8_t bytes_a[8] = {};
const uint8_t bytes_b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
// A wider owner so beads-yjp.53 can build row sets that genuinely diverge:
// four ARM slots instead of two.
const uint8_t bytes_wide[16] = {};

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

// live_overlay_configure() deliberately does NOT reset the lifetime run
// counters, so anything asserted about them has to be a delta.
unsigned long long status_number(const char* key) {
    const std::string json = live_overlay_status_json();
    const std::string needle = std::string("\"") + key + "\":";
    const std::size_t pos = json.find(needle);
    if (pos == std::string::npos) return ~0ull;
    return std::strtoull(json.c_str() + pos + needle.size(), nullptr, 10);
}

// How many times a literal appears in the status JSON. Used to assert that
// two banks are BOTH resident, which no single-value probe can express.
unsigned status_count(const char* needle) {
    const std::string json = live_overlay_status_json();
    unsigned found = 0u;
    for (std::size_t pos = json.find(needle); pos != std::string::npos;
         pos = json.find(needle, pos + 1u)) {
        ++found;
    }
    return found;
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

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--load-cache") == 0) {
        g_bytes_live = true;
        live_overlay_configure(true, false, 0u, 0u, 0u, "", argv[2], "test");
        live_overlay_register_cached_banks();
        for (int i = 0; i < 200; ++i) {
            live_overlay_poll_now();
            if (live_overlay_status_json().find("\"banks_loaded\":1") !=
                std::string::npos) {
                live_overlay_shutdown();
                std::puts("PASS: live overlay cache library loaded");
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::fprintf(stderr, "FAIL: cache library was not adopted: %s\n",
                     live_overlay_status_json().c_str());
        live_overlay_shutdown();
        return 1;
    }
#if defined(__linux__)
    if (argc == 3 && std::strcmp(argv[1], "--spawn-command") == 0) {
        std::filesystem::create_directories(argv[2]);
        g_allow_snapshot_write = true;
        const char* command =
            "test -f \"$NDS_LIVE_OVERLAY_MANIFEST\" && "
            "test \"$NDS_LIVE_OVERLAY_ROM_SHA1\" = test && "
            "test -n \"$NDS_LIVE_OVERLAY_CACHE\" && "
            "test -n \"$NDS_LIVE_OVERLAY_MAX_PAGES\" && "
            "printf 'NDS_SHARD_PENDING 0\\n'";
        live_overlay_configure(true, true, 0u, 0u, 0u, command, argv[2],
                               "test");
        if (!live_overlay_trigger_now()) return 1;
        for (int i = 0; i < 1000; ++i) {
            live_overlay_poll_now();
            if (status_number("runs_finished") == 1u &&
                status_number("runs_failed") == 0u &&
                live_overlay_status_json().find("\"busy\":false") !=
                    std::string::npos) {
                live_overlay_shutdown();
                std::puts("PASS: Linux live overlay child lifecycle");
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::fprintf(stderr, "FAIL: compiler child did not finish: %s\n",
                     live_overlay_status_json().c_str());
        live_overlay_shutdown();
        return 1;
    }
#endif
    char error[256];
    const NdsStaticValidation owner_a{0x02000000u, sizeof(bytes_a), bytes_a};
    const NdsStaticValidation owner_b{0x02000000u, sizeof(bytes_b), bytes_b};

    const NdsDispatchEntry interior_rows[] = {
        {0x02000000u, 0u, body_a, &owner_a},
        {0x02000004u, 0u, body_a, &owner_a},
    };

    // beads-yjp.53 row sets over one 16-byte owner. p and q each hold a row
    // the other lacks, so neither covers the other; super covers both.
    static const NdsStaticValidation owner_wide{
        0x02000000u, sizeof(bytes_wide), bytes_wide};
    const NdsDispatchEntry wide_rows_p[] = {
        {0x02000000u, 0u, body_a, &owner_wide},
        {0x02000004u, 0u, body_a, &owner_wide},
    };
    const NdsDispatchEntry wide_rows_q[] = {
        {0x02000000u, 0u, body_b, &owner_wide},
        {0x02000008u, 0u, body_b, &owner_wide},
    };
    const NdsDispatchEntry wide_rows_super[] = {
        {0x02000000u, 0u, body_b, &owner_wide},
        {0x02000004u, 0u, body_b, &owner_wide},
        {0x02000008u, 0u, body_b, &owner_wide},
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

#if defined(_WIN32) || defined(__linux__)
    const unsigned before_poll = g_registrations;
    live_overlay_poll_now();
    if (!expect(g_registrations == before_poll + 1u,
                "one poll after a reset should re-register the cached bank"))
        return 1;
    if (!expect(live_overlay_status_json().find(
                    "\"registered\":true") != std::string::npos,
                "the revived bank should report itself registered"))
        return 1;
#endif
    live_overlay_shutdown();

#if defined(_WIN32) || defined(__linux__)
    // beads-w184: once the one-shot startup work is complete, an idle live
    // overlay poll must not take the empty publish/maintenance queue locks.
    // These counters are bumped inside the drain functions, so this fails if
    // the poll goes back to unconditionally processing empty queues.
    //
    // Every poll in this file is live_overlay_poll_now(), the unconditional
    // body, and NOT live_overlay_poll(), which is the scheduler's 1-in-1024
    // countdown gate. Driving the gate here would make each assertion pass for
    // the wrong reason -- 64 gated calls run the body at most once, so "no
    // drains happened" would be true because nothing ran at all. The gate's
    // own shape is pinned structurally in tests/test_emu_attrib_guards.py.
    live_overlay_configure(true, false, 0u, 0u, 0u, "",
                           "live-overlay-test-cache-does-not-exist", "test");
    live_overlay_register_cached_banks();
    uint64_t prepare_drains_before = 0u;
    uint64_t maint_drains_before = 0u;
    live_overlay_poll_drain_counts_for_test(&prepare_drains_before,
                                            &maint_drains_before);
    for (int i = 0; i < 64; ++i) live_overlay_poll_now();
    uint64_t prepare_drains_after = 0u;
    uint64_t maint_drains_after = 0u;
    live_overlay_poll_drain_counts_for_test(&prepare_drains_after,
                                            &maint_drains_after);
    if (!expect(prepare_drains_after == prepare_drains_before &&
                maint_drains_after == maint_drains_before,
                "idle overlay polls must not drain empty result queues"))
        return 1;

    // beads-w184: generated banks report every control transfer. Full
    // DiagEntry writes on that path cost measurable frame time, while these
    // records are not compiler input. Ordinary sharding must do no transfer
    // trace accounting; the explicit field trace remains bounded by sampling.
    for (unsigned i = 0; i < 4096u; ++i)
        live_overlay_note_transfer(NDS_ARM9, i * 4u, i * 4u + 4u, 0u, 0u, 0u);
    if (!expect(status_number("transfer_diag_seen") == 0u &&
                    status_number("transfer_diag_samples") == 0u,
                "ordinary live sharding must not trace generated transfers"))
        return 1;
    live_overlay_set_transfer_trace(true);
    for (unsigned i = 0; i < 2050u; ++i)
        live_overlay_note_transfer(NDS_ARM9, i * 4u, i * 4u + 4u, 0u, 0u, 0u);
    if (!expect(status_number("transfer_diag_seen") == 2050u &&
                    status_number("transfer_diag_samples") == 3u,
                "the opt-in transfer trace must sample one call per 1024"))
        return 1;
    if (!expect(live_overlay_diagnostics_json(1u).find(
                    "\"kind\":\"transfer\"") != std::string::npos,
                "the sampled transfer must remain available to field tools"))
        return 1;
    live_overlay_set_transfer_trace(false);
    for (unsigned i = 0; i < 4096u; ++i)
        live_overlay_note_transfer(NDS_ARM9, i * 4u, i * 4u + 4u, 0u, 0u, 0u);
    if (!expect(status_number("transfer_diag_seen") == 0u &&
                    status_number("transfer_diag_samples") == 0u,
                "disabling transfer trace must restore the zero-work path"))
        return 1;
    live_overlay_shutdown();
#endif

    // ---- beads-yjp.51: adaptive queue policy ----------------------------
    //
    // Cadence is a pure function of the reported backlog plus the last run's
    // wall time, so it pins at the unit layer without a compiler child.
    live_overlay_configure(true, true, 0u, 0u, 60000u, "",
                           "live-overlay-test-cache-does-not-exist", "test");
    if (!expect(live_overlay_batch_cap_for_test() == 6u &&
                    live_overlay_cooldown_for_test() == 60000u,
                "an empty backlog must keep the conservative cadence"))
        return 1;

    // A backlog run that finishes well inside the budget has proven headroom:
    // the batch doubles, and the cooldown drops to the drain floor.
    live_overlay_note_backlog_for_test(40u, 3000u);
    if (!expect(live_overlay_batch_cap_for_test() == 12u,
                "a fast backlog run should double the batch cap"))
        return 1;
    if (!expect(live_overlay_cooldown_for_test() == 5000u,
                "a pending backlog should drop to the drain cooldown floor"))
        return 1;
    live_overlay_note_backlog_for_test(30u, 3000u);
    live_overlay_note_backlog_for_test(20u, 3000u);
    if (!expect(live_overlay_batch_cap_for_test() == 12u,
                "the batch ramp should saturate at its measured bound"))
        return 1;
    live_overlay_note_backlog_for_test(10u, 3000u);
    if (!expect(live_overlay_batch_cap_for_test() == 12u,
                "the batch ramp must not exceed its bound"))
        return 1;
    // A machine that cannot keep up gives the batch back.
    live_overlay_note_backlog_for_test(10u, 90000u);
    if (!expect(live_overlay_batch_cap_for_test() == 6u,
                "a slow backlog run should halve the batch cap"))
        return 1;
    live_overlay_note_backlog_for_test(10u, 90000u);
    if (!expect(live_overlay_batch_cap_for_test() == 6u,
                "the batch cap must not fall below the conservative base"))
        return 1;
    // Drained: straight back to the configured conservative cadence.
    live_overlay_note_backlog_for_test(0u, 3000u);
    if (!expect(live_overlay_batch_cap_for_test() == 6u &&
                    live_overlay_cooldown_for_test() == 60000u,
                "a drained queue must restore the conservative cadence"))
        return 1;
    // A configured cooldown SHORTER than the drain floor is already more
    // aggressive than the floor and must not be lengthened by it.
    live_overlay_configure(true, true, 0u, 0u, 1000u, "",
                           "live-overlay-test-cache-does-not-exist", "test");
    live_overlay_note_backlog_for_test(5u, 1000u);
    if (!expect(live_overlay_cooldown_for_test() == 1000u,
                "the drain floor must never slow a faster configured cadence"))
        return 1;

    if (!expect(live_overlay_status_json().find("\"pending_candidates\":5") !=
                    std::string::npos &&
                live_overlay_status_json().find("\"batch_cap\":12") !=
                    std::string::npos &&
                live_overlay_status_json().find("\"cooldown_ms\":1000") !=
                    std::string::npos,
                "the queue state must be visible in diagnostics"))
        return 1;
    NdsLiveOverlaySummary summary{};
    live_overlay_summary(&summary);
    if (!expect(summary.pending_candidates == 5u && summary.batch_cap == 12u &&
                    summary.cooldown_ms == 1000u,
                "the diagnostics summary must carry the queue state"))
        return 1;
    live_overlay_shutdown();

#if defined(_WIN32) || defined(__linux__)
    // ---- first auto trigger is reachable by real MPH multiplayer --------
    //
    // A cold mp_bots_blank route observed only ~70-95 Tier-3 executions before
    // drain. The former 100k first-trigger threshold meant runtime TCC never
    // even launched: the cache stayed empty with runs_started=0, not because
    // compilation failed but because it was never commissioned.
    live_overlay_configure(true, true, 0u, 0u, 0u, "some-provider-command",
                           "live-overlay-test-cache-does-not-exist", "test");
    const unsigned long long auto_failed_before = status_number("runs_failed");
    for (int i = 0; i < 63; ++i)
        live_overlay_note_tier3(NDS_ARM9, 0x02000000u + i * 4u);
    live_overlay_poll_now();
    if (!expect(status_number("runs_failed") == auto_failed_before,
                "63 Tier-3 hits must not yet auto-commission the provider"))
        return 1;
    live_overlay_note_tier3(NDS_ARM9, 0x02000100u);
    for (int i = 0;
         i < 500 && status_number("runs_failed") == auto_failed_before;
         ++i) {
        live_overlay_poll_now();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!expect(status_number("runs_failed") == auto_failed_before + 1u,
                "64 Tier-3 hits must auto-commission the provider"))
        return 1;
    live_overlay_shutdown();
#endif

    // ---- futility guard is UNCHANGED by the queue policy ----------------
    //
    // The drain path exists to commission more work sooner. A provider proven
    // unable to produce a loadable bank must still stay uncommissioned, no
    // matter how large the backlog says it is: otherwise the guard's whole
    // purpose (not re-running identical rejected work forever) is inverted
    // into re-running it FASTER.
#if defined(_WIN32) || defined(__linux__)
    // Control first, so the suppressed case cannot pass for the wrong reason.
    // A backlog on an UNsuppressed provider does reach start_child (which
    // fails here only because this unit build stubs the manifest writer, and
    // that failure is itself the observable that the attempt happened).
    live_overlay_configure(true, true, 0u, 0u, 0u, "some-provider-command",
                           "live-overlay-test-cache-does-not-exist", "test");
    live_overlay_note_backlog_for_test(500u, 1000u);
    const unsigned long long control_before = status_number("runs_failed");
    // beads-yjp.59: the snapshot write and the CreateProcess are a maintenance
    // worker job now, so the poll that commissions the run is not the poll that
    // records its outcome. The contract is that the outcome still ARRIVES, and
    // that it is still counted exactly once.
    for (int i = 0; i < 500 && status_number("runs_failed") == control_before;
         ++i) {
        live_overlay_poll_now();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!expect(status_number("runs_failed") == control_before + 1u,
                "an unsuppressed backlog should reach the provider"))
        return 1;
    live_overlay_shutdown();

    live_overlay_configure(true, true, 0u, 0u, 0u, "some-provider-command",
                           "live-overlay-test-cache-does-not-exist", "test");
    live_overlay_suppress_for_test("test ABI mismatch");
    live_overlay_note_backlog_for_test(500u, 1000u);
    const unsigned long long failed_before = status_number("runs_failed");
    const uint64_t started_before = live_overlay_runs_started_for_test();
    for (int i = 0; i < 8; ++i) live_overlay_poll_now();
    if (!expect(live_overlay_suppressed_for_test(),
                "a backlog must not clear the futility suppression"))
        return 1;
    if (!expect(status_number("runs_failed") == failed_before &&
                live_overlay_runs_started_for_test() == started_before,
                "a suppressed provider must not be commissioned by the drain, "
                "however large the backlog says it is"))
        return 1;
    // And an explicit trigger still lifts it, exactly as before.
    live_overlay_trigger_now();
    if (!expect(!live_overlay_suppressed_for_test(),
                "an explicit trigger must still lift futility suppression"))
        return 1;
    live_overlay_shutdown();
#endif

#if defined(_WIN32) || defined(__linux__)
    // ---- gcc-wins tie-break is UNCHANGED -------------------------------
    //
    // Reordering and enlarging batches changes WHICH shard arrives when, so
    // the arrival-order-independent rule that a better backend keeps a
    // generation is exactly the thing a queue-policy change could regress.
    live_overlay_configure(true, false, 0u, 0u, 0u, "",
                           "live-overlay-test-cache-does-not-exist", "test");
    if (!expect(live_overlay_commit_bank_for_test(
                    NDS_ARM9, "tiebreak_bank", "cand-gcc", "generation-1", 2u,
                    interior_rows, 2u),
                "a gcc-tier bank should be adopted"))
        return 1;
    unsigned tier = 0u;
    if (!expect(live_overlay_generation_registered_for_test("generation-1",
                                                            &tier) &&
                    tier == 2u,
                "the gcc-tier bank should hold the generation"))
        return 1;
    // The tcc shard for the same generation arrives LATER and must be
    // declined rather than superseding -- and declining is a success, not a
    // rejection, so the futility guard must not see it as one.
    if (!expect(live_overlay_commit_bank_for_test(
                    NDS_ARM9, "tiebreak_bank", "cand-tcc", "generation-1", 1u,
                    interior_rows, 2u),
                "a declined lower-tier shard is not a rejection"))
        return 1;
    tier = 0u;
    if (!expect(live_overlay_generation_registered_for_test("generation-1",
                                                            &tier) &&
                    tier == 2u,
                "the gcc-tier bank must still own the generation"))
        return 1;
    if (!expect(live_overlay_status_json().find("\"candidate_id\":\"cand-tcc\"")
                    == std::string::npos,
                "the declined tcc shard must not have become resident"))
        return 1;
    live_overlay_shutdown();
#endif

#if defined(_WIN32) || defined(__linux__)
    // ---- beads-yjp.53: supersede must never LOSE a row -----------------
    //
    // Two candidates for the same page byte generation are two translations
    // of identical guest bytes that differ only in which entry roots the
    // capture behind them observed, and those root sets are not monotone: an
    // entry already served natively stops appearing in Tier-3 coverage, so
    // the next capture of the same page carries a SMALLER set. Replacing the
    // resident bank with it deleted rows and sent that code back to the
    // interpreter, where the next capture rediscovered it. Field evidence:
    // a player's own cache index recorded page 0x0205B000 with 4 roots and
    // then 3, and 0x02034000 with 3 and then 2.
    live_overlay_configure(true, false, 0u, 0u, 0u, "",
                           "live-overlay-test-cache-does-not-exist", "test");
    // banks_rejected is a lifetime counter that configure() deliberately does
    // not clear, so the "no drop inflates it" claim has to be a delta.
    const unsigned long long rejected_before = status_number("banks_rejected");
    if (!expect(live_overlay_commit_bank_for_test(
                    NDS_ARM9, "yjp53_bank", "cand-p", "generation-D", 2u,
                    wide_rows_p, 2u),
                "the first candidate for a generation should be adopted"))
        return 1;
    // Divergent: cand-q has 0x02000008, which cand-p lacks, and lacks
    // 0x02000004, which cand-p has. Neither may evict the other.
    if (!expect(live_overlay_commit_bank_for_test(
                    NDS_ARM9, "yjp53_bank", "cand-q", "generation-D", 2u,
                    wide_rows_q, 2u),
                "a divergent same-generation candidate should be adopted"))
        return 1;
    if (!expect(status_count("\"candidate_id\":\"cand-p\"") == 1u &&
                status_count("\"candidate_id\":\"cand-q\"") == 1u &&
                status_count("\"superseded\":true") == 0u,
                "two divergent same-generation candidates must BOTH stay "
                "registered; neither covers the other's rows"))
        return 1;
    if (!expect(status_number("kept_divergent_generation") >= 1ull,
                "keeping a divergent candidate must be counted"))
        return 1;
    if (!expect(status_number("rows_superseded") == 0ull,
                "no rows may be retired while no candidate is a superset"))
        return 1;
    // A true superset arrives: NOW retiring the two partial banks is safe,
    // and the retirement is accounted for in rows.
    if (!expect(live_overlay_commit_bank_for_test(
                    NDS_ARM9, "yjp53_bank", "cand-super", "generation-D", 2u,
                    wide_rows_super, 3u),
                "a superset candidate should be adopted"))
        return 1;
    if (!expect(status_count("\"superseded\":true") == 2u,
                "a superset candidate must retire both partial banks"))
        return 1;
    if (!expect(status_number("rows_superseded") == 4ull &&
                status_number("drop_superseded_generation") == 2ull,
                "retiring two 2-row banks must report 4 superseded rows"))
        return 1;
    // A strict subset of what is already resident adds nothing but index
    // rows, so it is dropped -- and dropping it is a success, not a reject.
    if (!expect(live_overlay_commit_bank_for_test(
                    NDS_ARM9, "yjp53_bank", "cand-sub", "generation-D", 2u,
                    wide_rows_super, 1u),
                "a redundant subset candidate is not a rejection"))
        return 1;
    if (!expect(live_overlay_status_json().find("\"candidate_id\":\"cand-sub\"")
                    == std::string::npos &&
                status_number("drop_redundant_subset") >= 1ull,
                "a redundant subset must be dropped, and counted"))
        return 1;
    if (!expect(status_number("banks_rejected") == rejected_before,
                "none of the drop family may inflate banks_rejected"))
        return 1;
    // The backend tie-break may not cost coverage either: a tcc shard that
    // carries a root the resident gcc shard lacks has to be kept.
    if (!expect(live_overlay_commit_bank_for_test(
                    NDS_ARM9, "yjp53_tier", "cand-gcc-partial", "generation-E",
                    2u, wide_rows_p, 2u),
                "a gcc-tier bank should be adopted"))
        return 1;
    // beads-lqa.40: the dispatch index now ranks co-validating rows by owned
    // span and breaks ties by FIRST-registered, and two shards of one
    // generation are the same guest bytes over the same owners -- so every row
    // the two banks share is a tie the already-resident gcc bank wins. Adopting
    // the weaker newcomer is therefore ONE plain registration: the
    // unregister/re-register dance that used to push the better bank to the end
    // of the index is gone, and its absence is what this delta pins.
    const unsigned reg_before = g_registrations;
    const unsigned unreg_before = g_unregistrations;
    if (!expect(live_overlay_commit_bank_for_test(
                    NDS_ARM9, "yjp53_tier", "cand-tcc-extra", "generation-E",
                    1u, wide_rows_q, 2u),
                "a lower-tier candidate with extra rows should be adopted"))
        return 1;
    if (!expect(status_count("\"candidate_id\":\"cand-tcc-extra\"") == 1u,
                "a lower-tier shard covering a row the better backend lacks "
                "must be kept, not declined"))
        return 1;
    if (!expect(g_registrations == reg_before + 1u &&
                g_unregistrations == unreg_before,
                "co-registering the weaker backend must be a single plain "
                "registration; span ranking keeps the shared addresses with "
                "the better bank without touching the index"))
        return 1;

    // ---- beads-yjp.68: stale shard rows covered by static banks ----------
    //
    // A cached shard produced by an older codegen version must not shadow a
    // current baked/static bank. Rows not covered by static code may remain
    // registered as a gap-filling fallback, but covered rows are stripped
    // before registration.
    static const uint8_t stale_bytes[16] = {};
    static const NdsStaticValidation stale_owner{
        0x02200000u, sizeof(stale_bytes), stale_bytes};
    const NdsDispatchEntry stale_rows[] = {
        {0x02200000u, 0u, body_a, &stale_owner},
        {0x02200004u, 0u, body_a, &stale_owner},
    };
    g_bytes_live = true;
    g_static_cover_enabled = true;
    g_static_cover_pc = 0x02200000u;
    const unsigned stale_reg_before = g_registrations;
    const unsigned long long stale_rejected_before =
        status_number("banks_rejected");
    if (!expect(!live_overlay_admit_bank_with_codegen_for_test(
                    NDS_ARM9, "yjp68_bank", "cand-stale-covered",
                    "generation-stale-covered", 2u, 1u,
                    "C:/cache/gcc/stale-covered.dll", stale_rows, 1u),
                "a stale shard fully covered by static code must not register"))
        return 1;
    if (!expect(g_registrations == stale_reg_before &&
                status_number("banks_rejected") ==
                    stale_rejected_before + 1ull &&
                status_number("load_stale_static_covered") == 1ull &&
                status_number("drop_stale_static_rows") == 1ull,
                "fully covered stale rows must be counted as a load rejection"))
        return 1;

    if (!expect(live_overlay_admit_bank_with_codegen_for_test(
                    NDS_ARM9, "yjp68_bank", "cand-stale-gap",
                    "generation-stale-gap", 2u, 1u,
                    "C:/cache/gcc/stale-gap.dll", stale_rows, 2u),
                "a stale shard with at least one uncovered row may still fill "
                "the gap"))
        return 1;
    if (!expect(g_registrations == stale_reg_before + 1u &&
                status_count("\"candidate_id\":\"cand-stale-gap\"") == 1u &&
                live_overlay_status_json().find("\"rows\":1") !=
                    std::string::npos,
                "partial stale registration must keep only uncovered rows"))
        return 1;

    const NdsDispatchEntry current_rows[] = {
        {0x02200000u, 0u, body_b, &stale_owner},
    };
    if (!expect(live_overlay_admit_bank_for_test(
                    NDS_ARM9, "yjp68_bank", "cand-current-covered",
                    "generation-current-covered", 2u,
                    "C:/cache/gcc/current-covered.dll", current_rows, 1u),
                "a current-codegen shard should not be filtered by the stale "
                "static coverage gate"))
        return 1;
    g_static_cover_enabled = false;
    live_overlay_shutdown();
#endif

#if defined(_WIN32) || defined(__linux__)
    // ---- beads-yjp.62: dormant shards, woken by a Tier-3 entry ----------
    //
    // A shard for a per-scene guest code window (ITCM 0x01FF8000, a swapped
    // ARM9 overlay, a runtime copy) preflights only at two moments: when a
    // finished compile run's log is read and when the cache is rescanned. On
    // a human's play rhythm those land in the wrong scene, so a field bundle
    // from a fresh install showed 307 guard-bytes failures, ZERO banks, and
    // twelve compile runs commissioned to reproduce shards the cache already
    // held. The fix keeps such a candidate DORMANT and re-preflights it when
    // the interpreter proves its code is resident -- without ever relaxing
    // the rule that a shard activates only when its guard bytes match.
    static const NdsStaticValidation dormant_owner_a{
        0x02300000u, sizeof(bytes_a), bytes_a};
    static const NdsStaticValidation dormant_owner_b{
        0x02400000u, sizeof(bytes_a), bytes_a};
    const NdsDispatchEntry dormant_rows_a[] = {
        {0x02300000u, 0u, body_a, &dormant_owner_a},
        {0x02300004u, 0u, body_a, &dormant_owner_a},
    };
    const NdsDispatchEntry dormant_rows_b[] = {
        {0x02400000u, 0u, body_b, &dormant_owner_b},
    };
    constexpr const char* kDormantPathA = "C:/cache/gcc/dormant-a.dll";
    constexpr const char* kDormantPathB = "C:/cache/gcc/dormant-b.dll";

    live_overlay_configure(true, false, 0u, 0u, 0u, "",
                           "live-overlay-test-cache-does-not-exist", "test");
    const unsigned long long dormant_rejected_before =
        status_number("banks_rejected");
    g_bytes_live = false;
    if (!expect(!live_overlay_admit_bank_for_test(
                    NDS_ARM9, "yjp62_bank", "cand-dormant", "generation-F", 2u,
                    kDormantPathA, dormant_rows_a, 2u),
                "a shard whose guest code is not resident must NOT activate"))
        return 1;
    if (!expect(status_number("dormant_candidates") == 1ull &&
                status_number("defer_dormant_guard_bytes") == 1ull,
                "the deferred shard must be held dormant, and counted"))
        return 1;
    if (!expect(live_overlay_status_json().find(
                    "\"candidate_id\":\"cand-dormant\"") == std::string::npos,
                "a dormant candidate must not be resident"))
        return 1;

    // Fix 2: its pages stop being filed as Tier-3 coverage, so the compiler
    // is no longer commissioned for output that already exists.
    if (!expect(live_overlay_dormant_covers(NDS_ARM9, 0x02300004u),
                "a dormant candidate's pages must suppress coverage filing"))
        return 1;
    if (!expect(!live_overlay_dormant_covers(NDS_ARM7, 0x02300004u),
                "dormancy is per CPU; the other core must still file"))
        return 1;
    if (!expect(!live_overlay_dormant_covers(NDS_ARM9, 0x02500000u),
                "an unrelated page must stay filable"))
        return 1;

    // Fix 1: the Tier-3 ENTRY is the proof of residency, and it re-queues.
    if (!expect(!live_overlay_note_tier3_entry(NDS_ARM9, 0x02500000u) &&
                status_number("dormant_requeues") == 0ull,
                "a Tier-3 entry outside every dormant range must do nothing"))
        return 1;
    if (!expect(live_overlay_note_tier3_entry(NDS_ARM9, 0x02300000u) &&
                status_number("dormant_requeues") == 1ull &&
                status_number("preparing_banks") == 1ull,
                "a Tier-3 entry inside a dormant range must re-queue the "
                "candidate for another preflight"))
        return 1;
    live_overlay_note_tier3_entry(NDS_ARM9, 0x02300004u);
    if (!expect(status_number("dormant_requeues") == 1ull &&
                status_number("preparing_banks") == 1ull,
                "an outstanding re-queue must not be pushed a second time"))
        return 1;

    // The re-preflight now finds the scene up: the shard activates, and only
    // now -- the guard-bytes contract is what decides, exactly as before.
    g_bytes_live = true;
    if (!expect(live_overlay_admit_bank_for_test(
                    NDS_ARM9, "yjp62_bank", "cand-dormant", "generation-F", 2u,
                    kDormantPathA, dormant_rows_a, 2u),
                "a re-preflighted dormant shard whose bytes now match must "
                "activate"))
        return 1;
    if (!expect(status_number("dormant_candidates") == 0ull &&
                status_number("dormant_activations") == 1ull,
                "activation must clear the dormant record and be counted"))
        return 1;
    if (!expect(status_count("\"candidate_id\":\"cand-dormant\"") == 1u,
                "the activated candidate must be resident"))
        return 1;
    if (!expect(!live_overlay_dormant_covers(NDS_ARM9, 0x02300004u),
                "an activated candidate's pages are covered by a bank now, "
                "not by a dormant record"))
        return 1;

    // Attempts are capped. A candidate that never finds its bytes is parked --
    // and parking must leave NOTHING behind, because the two halves of the
    // release are what keep progress possible. Releasing only the pages
    // resumed coverage filing while the path stayed in queued_paths, so the
    // compiler was commissioned for the page, republished a byte-identical
    // DLL under the same content-addressed path, and enqueue_candidate refused
    // it as already-seen: filing forever, preparing never. Seed the
    // queued-paths set the way a real scan would, so the release is observable
    // through the production queueing path rather than asserted about state.
    g_bytes_live = false;
    if (!expect(live_overlay_enqueue_path_for_test(kDormantPathB),
                "a path the queue has not seen must be accepted"))
        return 1;
    if (!expect(!live_overlay_enqueue_path_for_test(kDormantPathB),
                "queued_paths must refuse a path it already holds -- this is "
                "the dedup that a stale parked key turns into a dead end"))
        return 1;
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (!expect(!live_overlay_admit_bank_for_test(
                        NDS_ARM9, "yjp62_bank", "cand-parked", "generation-G",
                        2u, kDormantPathB, dormant_rows_b, 1u),
                    "a shard that never finds its bytes must never activate"))
            return 1;
    }
    if (!expect(status_number("dormant_parked") == 1ull &&
                status_number("defer_dormant_parked") == 1ull &&
                status_number("dormant_candidates") == 0ull,
                "a candidate must be parked once its attempt cap is spent"))
        return 1;
    if (!expect(!live_overlay_dormant_covers(NDS_ARM9, 0x02400000u) &&
                !live_overlay_note_tier3_entry(NDS_ARM9, 0x02400000u),
                "a parked candidate must hand its pages back to the compiler "
                "and stop being re-queued"))
        return 1;
    // Half one of the release: the key is gone, so the recompiled or merely
    // rescanned DLL can reach the prepare worker again.
    if (!expect(live_overlay_enqueue_path_for_test(kDormantPathB),
                "parking must release the queued-paths key, or the page is "
                "filed forever and never prepared again"))
        return 1;
    // Half two: the record is gone, so the candidate that comes back gets a
    // FRESH attempt budget instead of parking on its first miss.
    if (!expect(!live_overlay_admit_bank_for_test(
                    NDS_ARM9, "yjp62_bank", "cand-parked", "generation-G", 2u,
                    kDormantPathB, dormant_rows_b, 1u),
                "the re-prepared candidate still must not activate"))
        return 1;
    if (!expect(status_number("dormant_candidates") == 1ull &&
                status_number("dormant_parked") == 1ull,
                "a re-prepared candidate must start a fresh dormant record "
                "with a fresh attempt budget, not park again immediately"))
        return 1;
    if (!expect(live_overlay_dormant_covers(NDS_ARM9, 0x02400000u),
                "the fresh dormant record must suppress filing again"))
        return 1;
    if (!expect(status_number("banks_rejected") == dormant_rejected_before,
                "deferral is NOT rejection: banks_rejected must not move, or "
                "the futility guard would suppress a healthy provider on any "
                "install whose scenes simply were not up at preflight time"))
        return 1;

    NdsLiveOverlaySummary dormant_summary{};
    live_overlay_summary(&dormant_summary);
    if (!expect(dormant_summary.dormant_candidates == 1ull &&
                dormant_summary.dormant_activations == 1ull &&
                dormant_summary.dormant_parked == 1ull &&
                dormant_summary.dormant_requeues == 1ull,
                "the diagnostics summary must carry the dormant counters"))
        return 1;
    live_overlay_shutdown();
    g_bytes_live = false;
#endif

    std::puts("PASS: live overlay preflight rejects malformed bundles; "
              "adaptive queue policy, futility guard, gcc tie-break and "
              "beads-yjp.62 dormant activation hold");
    return 0;
}
