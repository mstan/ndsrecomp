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

int main() {
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

    // ---- futility guard is UNCHANGED by the queue policy ----------------
    //
    // The drain path exists to commission more work sooner. A provider proven
    // unable to produce a loadable bank must still stay uncommissioned, no
    // matter how large the backlog says it is: otherwise the guard's whole
    // purpose (not re-running identical rejected work forever) is inverted
    // into re-running it FASTER.
#if defined(_WIN32)
    // Control first, so the suppressed case cannot pass for the wrong reason.
    // A backlog on an UNsuppressed provider does reach start_child (which
    // fails here only because this unit build stubs the manifest writer, and
    // that failure is itself the observable that the attempt happened).
    live_overlay_configure(true, true, 0u, 0u, 0u, "some-provider-command",
                           "live-overlay-test-cache-does-not-exist", "test");
    live_overlay_note_backlog_for_test(500u, 1000u);
    const unsigned long long control_before = status_number("runs_failed");
    live_overlay_poll();
    if (!expect(status_number("runs_failed") > control_before,
                "an unsuppressed backlog should reach the provider"))
        return 1;
    live_overlay_shutdown();

    live_overlay_configure(true, true, 0u, 0u, 0u, "some-provider-command",
                           "live-overlay-test-cache-does-not-exist", "test");
    live_overlay_suppress_for_test("test ABI mismatch");
    live_overlay_note_backlog_for_test(500u, 1000u);
    const unsigned long long failed_before = status_number("runs_failed");
    const uint64_t started_before = live_overlay_runs_started_for_test();
    for (int i = 0; i < 8; ++i) live_overlay_poll();
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

#if defined(_WIN32)
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

#if defined(_WIN32)
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
    // beads-lqa.40 interaction: the dispatch index selects the LAST live row
    // for a PC, so co-registering the weaker backend must not hand it the
    // addresses both banks share. Adopting it re-registers the better bank so
    // it lands last: one register for the newcomer, then one unregister and
    // one register for the gcc bank it must not shadow.
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
    if (!expect(g_registrations == reg_before + 2u &&
                g_unregistrations == unreg_before + 1u,
                "the better same-generation bank must be re-registered LAST "
                "so it keeps the addresses it shares with the newcomer"))
        return 1;
    live_overlay_shutdown();
#endif

    std::puts("PASS: live overlay preflight rejects malformed bundles; "
              "adaptive queue policy, futility guard and gcc tie-break hold");
    return 0;
}
