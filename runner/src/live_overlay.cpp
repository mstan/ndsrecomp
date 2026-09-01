#include "live_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "coverage_manifest.h"
#include "live_overlay_platform.h"
#include "runtime_arm.h"
#include "state.h"
#include "emu_profile.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace {

constexpr uint64_t kFirstTriggerTier3 = 64u;
constexpr uint64_t kRetriggerTier3 = 4096u;
constexpr uint64_t kNoProgressRetriggerTier3 = 65536u;
constexpr uint32_t kDiagRingSize = 4096u;

// ---- Adaptive queue policy (beads-yjp.51) ---------------------------------
//
// The conservative cadence (6 pages per run, one run per 30-60 s cooldown) is
// a hard 12-pages-per-minute ceiling, and field bundles show players ending a
// 7.5-minute session with dozens of hot pages still uncompiled -- exactly the
// pages driving their interpreter storms. The cadence was conservative because
// compiling was assumed to cost the player frame time; it measurably does not
// (39.0 fps during batch intervals vs 38.0 outside, with the child at
// IDLE_PRIORITY inside a kill-on-close job object).
//
// So while a backlog exists, spend the headroom: shorten the cooldown to a
// floor and ramp the per-batch cap. The ramp is driven by the CHILD'S OWN WALL
// TIME rather than a fixed schedule, which is what keeps this bounded on a
// slow machine: a box where a batch of 6 already takes half a minute never
// earns a bigger batch, while a box that finishes in seconds does. Exactly one
// child process at a time, always; none of this changes that.
constexpr uint32_t kBaseBatchPages = 6u;
// Measured bound, not a guessed one. On a contended host, a 24-page batch is a
// big enough burst that IDLE_PRIORITY stops shielding the emulator: an A/B on
// the same binary over the same 300 s MPH route showed 55.6 fps mean with the
// ramp reaching 24 (-5.5 fps during compile intervals, and -7.8 on intervals
// with NO interpreter pressure at all, so it was the child and not a tier-3
// confound) against 59.5 fps mean with the cap pinned at 6 (+0.3 fps during
// compiles -- noise). The large batch also bought nothing: 50 shards in 8 runs
// at cap 24 versus 51 shards in 12 runs at cap 6, because the shortened
// cooldown is what actually raises throughput. 12 keeps a bounded amount of
// the per-run overhead amortization the ramp is for without the burst that
// costs frames.
constexpr uint32_t kMaxBatchPages = 12u;
// Cooldown floor while draining. Short enough that a fast box gets many
// batches per minute, long enough that the runner still commits banks and
// re-snapshots coverage between runs.
constexpr uint32_t kBacklogCooldownMs = 5000u;
// A backlog run that finishes inside this budget has proven there is headroom
// for a bigger batch; one that takes more than twice it gives the batch back.
constexpr uint32_t kBacklogTargetRunMs = 20000u;
// Bound on how long the runner will wait for the shared live-index lock when
// reading the persisted queue at startup. Missing the read costs one run's
// worth of cadence, never correctness, so it must never stall the launch.
constexpr uint32_t kQueueLockWaitMs = 2000u;
// Generated code reports every control transfer, which is millions of calls
// per second in MPH. Detailed records are field diagnostics, not compiler
// input, so keep an opt-in trace useful without making tracing itself a frame
// time problem.
constexpr uint64_t kTransferDiagSampleInterval = 1024u;

enum class DiagKind : uint8_t {
    Transfer,
    Lookup,
    Write,
};

// ---- Reject-cause taxonomy (beads-yjp.53) ---------------------------------
//
// Before this existed a field bundle carried two aggregate numbers -- one
// count of shards that failed to load (banks_rejected) and one count of
// dispatch-time guard rejections (bank_rejects) -- and the REASON survived
// only in `last_error`, a single string the next failure overwrites. That is
// not enough to diagnose anything: beads-yjp.53 started from a field bundle
// showing 58 shards loaded, 0 rejected, and only 33 registered, and NOTHING
// in the record said where the other 25 went. They had been silently
// unregistered by the same-generation supersede path, which had no counter at
// all.
//
// So every distinct outcome that costs a bank (or a bank's rows) its place in
// the dispatch index now has its own always-on counter, emitted in the perf
// jsonl and the status JSON in Release exactly as in a debug build. Three
// families:
//
//   load_*   the shard never became a resident bank      (banks_rejected)
//   drop_*   the shard loaded but lost its rows to another candidate
//   guard_*  a resident row was refused at dispatch time (bank_rejects)
//
// The list is append-only: a consumer keys on the NAME, and the names are
// emitted with the counters so an older ingest never has to know this table.
#define NDS_LIVE_REJECT_REASONS(X)                                            \
    /* prepare_bank_dll: the file itself */                                   \
    X(LoadPathOutsideCache,        "load_path_outside_cache")                 \
    X(LoadNotPublishedDll,         "load_not_published_dll")                  \
    X(LoadCanonicalizeFailed,      "load_canonicalize_failed")                \
    X(LoadLibraryFailed,           "load_library_failed")                     \
    X(LoadNoBankInfo,              "load_no_bank_info")                       \
    X(LoadUnsupportedPlatform,     "load_unsupported_platform")               \
    /* validate_live_bank_info: identity and linkage */                       \
    X(LoadAbiMismatch,             "load_abi_mismatch")                       \
    X(LoadRomMismatch,             "load_rom_mismatch")                       \
    X(LoadCpuInvalid,              "load_cpu_invalid")                        \
    X(LoadImportsUnbound,          "load_imports_unbound")                    \
    /* preflight_live_bank: bank shape and dispatch-table safety */           \
    X(LoadUnknownFlags,            "load_unknown_flags")                      \
    X(LoadStaticCpuMismatch,       "load_static_cpu_mismatch")                \
    X(LoadMissingBankId,           "load_missing_bank_id")                    \
    X(LoadMissingCandidateId,      "load_missing_candidate_id")               \
    X(LoadNoDispatchRows,          "load_no_dispatch_rows")                   \
    X(LoadDispatchTooLarge,        "load_dispatch_too_large")                 \
    X(LoadRowNullFn,               "load_row_null_fn")                        \
    X(LoadRowNotOwned,             "load_row_not_owned")                      \
    X(LoadDependencyInsane,        "load_dependency_insane")                  \
    X(LoadClosureNotShared,        "load_closure_not_shared")                 \
    X(LoadRowsUnsorted,            "load_rows_unsorted")                      \
    X(LoadRowsDuplicate,           "load_rows_duplicate")                     \
    /* commit_prepared_bank */                                                \
    X(LoadContentConflict,         "load_content_conflict")                   \
    X(DropDuplicateCandidate,      "drop_duplicate_candidate")                \
    X(DropDeclinedLowerTier,       "drop_declined_lower_tier")                \
    X(DropSupersededGeneration,    "drop_superseded_generation")              \
    X(DropRedundantSubset,         "drop_redundant_subset")                   \
    X(KeptDivergentGeneration,     "kept_divergent_generation")               \
    /* dispatch_validation_live, classified where ++bank->rejects happens */  \
    X(GuardRowNotOwned,            "guard_row_not_owned")                     \
    X(GuardRangeMalformed,         "guard_range_malformed")                   \
    X(GuardNoProvenance,           "guard_no_provenance")                     \
    X(GuardBytesDiffer,            "guard_bytes_differ")                     \
    /* beads-yjp.62 -- a fourth family, defer_*. The shard loaded and passed  \
       every structural check, but the guest code it was compiled from is     \
       NOT resident at this instant, so it is held DORMANT instead of         \
       adopted. Deliberately outside banks_rejected and deliberately not      \
       futility evidence: nothing is wrong with the shard, the scene is       \
       simply elsewhere. A dormant candidate is re-preflighted the moment the \
       Tier-3 interpreter proves its code is resident again, and parked for   \
       good after kDormantMaxAttempts fruitless tries. */                     \
    X(DeferDormantGuardBytes,      "defer_dormant_guard_bytes")              \
    X(DeferDormantParked,          "defer_dormant_parked")

enum class RejectReason : uint8_t {
#define NDS_LIVE_REJECT_ENUM(id, name) id,
    NDS_LIVE_REJECT_REASONS(NDS_LIVE_REJECT_ENUM)
#undef NDS_LIVE_REJECT_ENUM
    Count,
};

constexpr uint32_t kRejectReasonCount =
    static_cast<uint32_t>(RejectReason::Count);
static_assert(kRejectReasonCount <= kNdsLiveRejectReasonCap,
              "grow kNdsLiveRejectReasonCap in live_overlay.h");

const char* const kRejectReasonNames[kRejectReasonCount] = {
#define NDS_LIVE_REJECT_NAME(id, name) name,
    NDS_LIVE_REJECT_REASONS(NDS_LIVE_REJECT_NAME)
#undef NDS_LIVE_REJECT_NAME
};

struct CandidateDesc {
    const char* bank_id = nullptr;
    const char* candidate_id = nullptr;
    const char* path = nullptr;
    uint32_t generation = 0u;
    uint32_t rows = 0u;
};

bool validation_identity_live(const NdsStaticValidation* validation);
bool validation_has_provenance(const NdsStaticValidation* validation);

struct DiagEntry {
    uint64_t seq = 0u;
    uint64_t cycles = 0u;
    DiagKind kind = DiagKind::Transfer;
    int cpu = 0;
    uint32_t pc = 0u;
    uint32_t target = 0u;
    uint32_t lr = 0u;
    uint32_t cpsr = 0u;
    uint32_t aux0 = 0u;
    uint32_t aux1 = 0u;
    uint32_t aux2 = 0u;
    uint32_t aux3 = 0u;
    uint32_t row_addr = 0u;
    uint32_t validation_addr = 0u;
    uint32_t validation_size = 0u;
    uint8_t thumb = 0u;
    uint8_t has_validation = 0u;
    uint8_t provenance_ok = 0u;
    uint8_t bytes_match = 0u;
    char outcome[24] = {};
    char bank_id[64] = {};
    char candidate_id[64] = {};
};

struct LoadedBank {
    std::string path;
    std::string bank_id;
    std::string candidate_id;
    std::string generation_id;
    uint32_t serial = 0u;
    uint32_t generation = 0u;
    int cpu = 0;
    uint32_t exc_base = 0;
    const NdsDispatchEntry* dispatch = nullptr;
    unsigned dispatch_len = 0;
    uint64_t native_hits = 0u;
    uint64_t rejects = 0u;
    uint64_t content_identity = 0u;
    // Which backend namespace the DLL was scanned out of. Consumption is
    // backend-blind â€” a bank is a bank â€” but when two of them cover the exact
    // same generation the better code generator must win. See backend_tier().
    uint32_t backend_tier = 0u;
    bool registered = false;
    bool superseded = false;
#if defined(_WIN32)
    HMODULE handle = nullptr;
#elif defined(__linux__)
    void* handle = nullptr;
#endif
};

// ---- dormant candidates (beads-yjp.62) ------------------------------------
//
// A live shard targets a guest code window the game REWRITES per scene: ITCM
// at 0x01FF8000, a swapped ARM9 overlay, a runtime copy. Its guard bytes are
// therefore resident only while that scene is up. Preflight, however, runs at
// exactly two moments -- when a finished compile run's log is read, and when
// the shard cache is rescanned -- and on a human's play rhythm those moments
// land in some other scene. A field bundle from a fresh install shows the
// steady state that produces: 307 guard-bytes failures, zero banks adopted,
// and twelve compile runs commissioned to recompile the very pages whose
// shards were already sitting in the cache. All cost, no benefit, and
// self-sustaining, because the runner kept reporting those pages as
// uncovered.
//
// So a candidate that preflights against a scene that is not up is no longer
// thrown away: it becomes DORMANT. We keep its canonical path and the 4 KB
// pages its rows own, and the next time the Tier-3 interpreter is ENTERED
// inside one of those pages -- which is the proof that the target code is
// resident right now -- the candidate is re-queued for another preflight.
// Preflight semantics are untouched: a shard still activates only when its
// guard bytes match.
constexpr uint32_t kDormantMaxAttempts = 8u;
constexpr uint32_t kDormantMaxCandidates = 4096u;
constexpr uint32_t kDormantMaxPagesPerCandidate = 64u;
// Fail OPEN past this many distinct validation ranges in one shard: the byte
// comparison is bounded work on the emulation thread, and adopting a bank
// whose bytes are stale is harmless (the dispatch guard refuses its rows).
constexpr unsigned kDormantMaxValidationProbes = 256u;

struct DormantCandidate {
    std::string key;    // canonical platform path key; the queued_paths key
    std::string path;   // canonical path; empty for a test-injected candidate
    int cpu = 0;        // 0 = ARM9, 1 = ARM7 (normalized)
    std::vector<uint32_t> pages;  // 4 KB page bases this shard's rows own
    uint32_t attempts = 0u;
    bool requeued = false;  // already pushed back onto the prepare queue
};

// A load failure crosses from the prepare worker to the emulation thread. The
// reason code travels with the message so the counter is still owned by one
// thread; the string alone would force the drain side to re-parse text.
struct PreparedError {
    std::string text;
    RejectReason reason = RejectReason::LoadNoBankInfo;
};

// ---- maintenance worker plumbing (beads-yjp.59) ---------------------------
//
// Everything the poll used to do inline that touches the filesystem: two full
// scans of the compiler log, a recursive walk of the shard cache, the three
// weakly_canonical calls per candidate path, and the snapshot manifest's
// base64/JSON/rename. Measured at 180-230 ms in the emu_attrib overlay_poll
// bucket whenever a run finished, which is a visible hitch.
enum class MaintenanceKind : uint8_t {
    Rescan,
    RunFinished,
    StartChild,
};

// A published DLL that has already passed the cheap filters and been
// canonicalized. The emulation thread only has to insert the key and push the
// path, which is what makes the queueing decision cheap enough to keep there
// -- and it must stay there, because the run attribution marks are counters
// only the emulation thread owns.
struct QueueCandidate {
    std::string key;
    std::filesystem::path path;
};

struct MaintenanceJob {
    MaintenanceKind kind = MaintenanceKind::Rescan;
    std::filesystem::path cache_dir;
    // RunFinished
    std::string log_path;
    uint64_t pending_fallback = 0u;
    uint64_t duration_ms = 0u;
    uint32_t exit_code = 0u;
    // StartChild
    std::string command;
    std::string rom_sha1;
    uint64_t run_index = 0u;
    uint32_t batch_cap = 0u;
    CoverageLiveSnapshot* snapshot = nullptr;
};

struct MaintenanceResult {
    MaintenanceKind kind = MaintenanceKind::Rescan;
    // RunFinished
    uint64_t pending = 0u;
    uint64_t duration_ms = 0u;
    uint32_t exit_code = 0u;
    uint64_t published_total = 0u;
    std::vector<QueueCandidate> published;
    std::vector<QueueCandidate> rescan;
    // StartChild
    bool ok = false;
    std::string error;
    std::string manifest_path;
    std::string log_path;
    uint64_t started_ms = 0u;
#if defined(_WIN32)
    HANDLE child = nullptr;
    HANDLE child_job = nullptr;
#elif defined(__linux__)
    pid_t child = -1;
#endif
};

struct State {
    bool enabled = false;
    bool auto_trigger = false;
    bool transfer_trace = false;
    bool initial_cache_scan_done = false;
    uint32_t activation_delay_ms = 0u;
    uint32_t auto_start_delay_ms = 0u;
    uint32_t auto_cooldown_ms = 0u;
    uint64_t configured_ms = 0u;
    uint64_t last_compile_start_ms = 0u;
    std::string command;
    std::filesystem::path cache_dir;
    std::string rom_sha1;
    uint64_t tier3[2] = {};
    uint64_t transfer_diag_seen = 0u;
    uint64_t transfer_diag_samples = 0u;
    uint64_t mismatch_rejects[2] = {};
    uint64_t next_trigger[2] = {kFirstTriggerTier3, kFirstTriggerTier3};
    bool generation_pending = false;
    uint64_t trigger_requests = 0;
    uint64_t runs_started = 0;
    uint64_t runs_finished = 0;
    uint64_t runs_failed = 0;
    uint64_t banks_loaded = 0;
    uint64_t banks_rejected = 0;
    // beads-yjp.53 reject-cause breakdown. Written from the emulation thread
    // for every cause except the prepare-worker's load failures, which are
    // carried across on the PreparedError below and counted here when the
    // emulation thread drains them -- so this array has exactly one writer.
    std::array<uint64_t, kRejectReasonCount> reject_reasons = {};
    uint64_t rows_superseded = 0;
    uint64_t rows_superseding = 0;
    // Futility guard. A compile run that published shards and had EVERY one of
    // them rejected has proven this provider cannot produce loadable banks for
    // this runner -- an ABI mismatch or a failed preflight is a property of the
    // provider, not of the workload, so the next cooldown would commission the
    // identical work and reject it identically, forever. run_watch tracks the
    // shards a finished run queued; once they have all resolved, the loaded/
    // rejected deltas say whether the run accomplished anything.
    bool run_watch = false;
    uint64_t run_published = 0;
    uint64_t run_queued = 0;
    uint64_t run_loaded_mark = 0;
    uint64_t run_rejected_mark = 0;
    uint64_t futile_runs = 0;
    bool auto_suppressed = false;
    std::string futility_reason;
    // Queue policy state. pending_candidates is the compiler's own count of
    // work it could not fit in the last batch; at startup it is seeded from
    // the persisted queue so the very first decision of the session already
    // knows a backlog exists.
    uint64_t pending_candidates = 0;
    uint32_t batch_cap = kBaseBatchPages;
    bool persisted_backlog = false;
    bool queue_reloaded = false;
    uint64_t last_run_duration_ms = 0;
    uint32_t next_bank_serial = 1u;
    uint64_t publication_generation = 0;
    std::string last_error;
    std::string manifest_path;
    std::string log_path;
    std::vector<LoadedBank> loaded;
    std::array<DiagEntry, kDiagRingSize> diag = {};
    uint64_t diag_seq = 0u;
    uint32_t diag_w = 0u;
    uint32_t diag_count = 0u;
    std::mutex publish_mutex;
    std::condition_variable publish_cv;
    std::deque<std::filesystem::path> prepare_queue;
    std::deque<LoadedBank> ready_queue;
    std::deque<PreparedError> prepare_errors;
    std::atomic<bool> prepare_results_ready{false};
    uint64_t prepare_result_drains_for_test = 0;
    // Shards the prepare worker has taken off prepare_queue but not yet
    // resolved onto ready_queue/prepare_errors. Without this the three
    // containers are all momentarily empty mid-load, which would let the
    // futility check read a verdict that has not been reached yet.
    int prepare_in_flight = 0;
    std::unordered_set<std::string> queued_paths;
    // beads-yjp.62. Guarded by publish_mutex on every write and on every
    // rebuild of the emulation thread's page cache; the Tier-3 path itself
    // never takes it (see g_dormant_any / g_dormant_version below).
    std::vector<DormantCandidate> dormant;
    uint64_t dormant_activations = 0;  // dormant -> resident
    uint64_t dormant_parked = 0;       // gave up after kDormantMaxAttempts
    uint64_t dormant_requeues = 0;     // Tier-3 entries that re-queued one
    std::thread prepare_thread;
    bool prepare_stop = false;
    // ---- maintenance worker (beads-yjp.59) -------------------------------
    //
    // Its own queue and mutex, not the prepare worker's. The two workloads
    // want opposite scheduling: prepare is an unordered bag of independent
    // LoadLibrary calls, while this is a strictly ordered pipeline (scan the
    // finished run's log -> queue what it published -> rescan the cache ->
    // snapshot -> spawn the next child). Sharing one queue would let a slow
    // LoadLibrary head-of-line-block the next compile start, and would lose
    // the ordering that makes "the manifest is durable before the child that
    // reads it exists" free.
    std::mutex maint_mutex;
    std::condition_variable maint_cv;
    std::deque<MaintenanceJob> maint_jobs;
    std::deque<MaintenanceResult> maint_results;
    std::atomic<bool> maint_results_ready{false};
    uint64_t maint_result_drains_for_test = 0;
    std::thread maint_thread;
    bool maint_stop = false;
    // Outstanding worker jobs whose results the emulation thread has not
    // applied yet. Both gate the compile-start decision so a run's state
    // transitions stay in the order they had when they were inline.
    bool run_finish_pending = false;
    bool start_pending = false;
#if defined(_WIN32)
    HANDLE child = nullptr;
    HANDLE child_job = nullptr;
#elif defined(__linux__)
    pid_t child = -1;
#endif
};

State g_live;

bool child_active() {
#if defined(_WIN32)
    return g_live.child != nullptr;
#elif defined(__linux__)
    return g_live.child > 0;
#else
    return false;
#endif
}

#if defined(__linux__)
void terminate_and_reap(pid_t& child) {
    if (child <= 0) return;
    // The compiler and every recompiler/compiler subprocess inherit the
    // process group created by posix_spawn. Kill the group first, with the
    // direct child fallback covering a provider that changed its own group.
    kill(-child, SIGKILL);
    kill(child, SIGKILL);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    child = -1;
}
#endif

// Always-on: no build-type guard, no sampling. Callers are all cold paths
// (a shard load, a bank adoption, a slow-path dispatch reject), so the cost is
// one increment per event that a player would otherwise never hear about.
void note_reject(RejectReason reason) {
    const uint32_t index = static_cast<uint32_t>(reason);
    if (index < kRejectReasonCount) ++g_live.reject_reasons[index];
}

uint64_t steady_ms() {
    using Clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now().time_since_epoch()).count());
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8u);
    for (char ch : s) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20u) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x",
                                  static_cast<unsigned char>(ch));
                    out += b;
                } else {
                    out += ch;
                }
                break;
        }
    }
    return out;
}

std::string lower_ascii(std::string s) {
    for (char& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string path_string(const std::filesystem::path& path) {
    return path.lexically_normal().generic_string();
}

std::string path_key(const std::filesystem::path& path) {
    const std::string value = path_string(path);
#if defined(_WIN32)
    return lower_ascii(value);
#else
    return value;
#endif
}

bool path_under_cache(const std::filesystem::path& cache_dir,
                      const std::filesystem::path& path) {
    if (cache_dir.empty()) return false;
    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(cache_dir, ec);
    if (ec) return false;
    const auto child = std::filesystem::weakly_canonical(path, ec);
    if (ec) return false;
    const std::string root_s = path_key(root);
    const std::string child_s = path_key(child);
    return child_s == root_s ||
        (child_s.size() > root_s.size() &&
         child_s.compare(0, root_s.size(), root_s) == 0 &&
         (root_s.empty() || root_s.back() == '/' ||
          child_s[root_s.size()] == '/'));
}

#if defined(_WIN32)
using NativeLibrarySymbol = FARPROC;
#elif defined(__linux__)
using NativeLibrarySymbol = void*;
#endif

#if defined(_WIN32) || defined(__linux__)
void close_library(LoadedBank& bank) {
    if (!bank.handle) return;
#if defined(_WIN32)
    FreeLibrary(bank.handle);
#else
    dlclose(bank.handle);
#endif
    bank.handle = nullptr;
}

NativeLibrarySymbol library_symbol(const LoadedBank& bank, const char* name) {
#if defined(_WIN32)
    return GetProcAddress(bank.handle, name);
#else
    return dlsym(bank.handle, name);
#endif
}
#endif

void copy_cstr(char* dst, std::size_t dst_len, const char* src) {
    if (!dst || dst_len == 0u) return;
    dst[0] = '\0';
    if (!src) return;
    std::snprintf(dst, dst_len, "%s", src);
}

CandidateDesc describe_candidate(int cpu, const NdsDispatchEntry* entry) {
    CandidateDesc desc{};
    if (!entry) return desc;
    for (const LoadedBank& bank : g_live.loaded) {
        if (bank.cpu != cpu || !bank.dispatch) continue;
        if (entry >= bank.dispatch && entry < bank.dispatch + bank.dispatch_len) {
            desc.bank_id = bank.bank_id.c_str();
            desc.candidate_id = bank.candidate_id.c_str();
            desc.path = bank.path.c_str();
            desc.generation = bank.generation;
            desc.rows = bank.dispatch_len;
            return desc;
        }
    }
    return desc;
}

LoadedBank* find_loaded_bank(int cpu, const NdsDispatchEntry* entry) {
    if (!entry) return nullptr;
    for (LoadedBank& bank : g_live.loaded) {
        if (bank.cpu != cpu || !bank.dispatch) continue;
        if (entry >= bank.dispatch && entry < bank.dispatch + bank.dispatch_len)
            return &bank;
    }
    return nullptr;
}

DiagEntry& push_diag(DiagKind kind, int cpu, uint32_t pc, uint32_t target,
                     uint32_t lr, uint32_t cpsr, const char* outcome) {
    DiagEntry& e = g_live.diag[g_live.diag_w];
    e = {};
    e.seq = ++g_live.diag_seq;
    e.cycles = g_runtime_cycles;
    e.kind = kind;
    e.cpu = cpu;
    e.pc = pc;
    e.target = target;
    e.lr = lr;
    e.cpsr = cpsr;
    copy_cstr(e.outcome, sizeof(e.outcome), outcome);
    g_live.diag_w = (g_live.diag_w + 1u) % kDiagRingSize;
    if (g_live.diag_count < kDiagRingSize) ++g_live.diag_count;
    return e;
}

void fill_candidate(DiagEntry& e, int cpu, const NdsDispatchEntry* entry) {
    if (!entry) return;
    e.row_addr = entry->addr;
    e.thumb = entry->thumb;
    const NdsStaticValidation* validation = entry->validation;
    if (validation) {
        e.has_validation = 1u;
        e.validation_addr = validation->addr;
        e.validation_size = validation->size;
        e.provenance_ok = validation_has_provenance(validation) ? 1u : 0u;
        e.bytes_match = validation_identity_live(validation) ? 1u : 0u;
    }
    const CandidateDesc desc = describe_candidate(cpu, entry);
    if (desc.bank_id) {
        copy_cstr(e.bank_id, sizeof(e.bank_id), desc.bank_id);
        copy_cstr(e.candidate_id, sizeof(e.candidate_id),
                  desc.candidate_id);
        e.aux2 = desc.generation;
        e.aux3 = desc.rows;
    }
}

bool validation_owns_row(const NdsStaticValidation* validation,
                         const NdsDispatchEntry& row) {
    if (!validation || !validation->expected || validation->size == 0u)
        return false;
    const uint32_t step = row.thumb ? 2u : 4u;
    if (row.thumb > 1u) return false;
    if (row.thumb) {
        if (row.addr & 1u) return false;
    } else if (row.addr & 3u) {
        return false;
    }
    const uint64_t begin = validation->addr;
    const uint64_t end = begin + validation->size;
    const uint64_t row_end = uint64_t{row.addr} + step;
    if (end > 0x1'0000'0000ull || row.addr < begin || row_end > end)
        return false;
    const uint32_t first_page = validation->addr & ~0xFFFu;
    const uint32_t last_page = static_cast<uint32_t>(end - 1u) & ~0xFFFu;
    return ((last_page - first_page) >> 12u) < 2u;
}

bool validation_dependencies_sane(const NdsStaticValidation* validation,
                                  bool require_closure) {
    if (!validation) return false;
    if (validation->dependency_count == 0u)
        return !require_closure && !validation->dependencies;
    if (!require_closure || !validation->dependencies ||
        validation->dependency_count > 4096u) {
        return false;
    }
    bool owner_covered = false;
    uint64_t previous_end = 0u;
    const uint64_t owner_begin = validation->addr;
    const uint64_t owner_end = owner_begin + validation->size;
    for (uint32_t i = 0u; i < validation->dependency_count; ++i) {
        const NdsStaticValidationRange& range =
            validation->dependencies[i];
        const uint64_t begin = range.addr;
        const uint64_t end = begin + range.size;
        if (!range.expected || range.size == 0u ||
            end > 0x1'0000'0000ull ||
            (i != 0u && begin < previous_end)) {
            return false;
        }
        previous_end = end;
        if (begin <= owner_begin && end >= owner_end) owner_covered = true;
    }
    return owner_covered;
}

bool validation_identity_live(const NdsStaticValidation* validation) {
    if (!validation) return true;
    if (validation->dependency_count != 0u) {
        if (!validation->dependencies) return false;
        for (uint32_t i = 0u; i < validation->dependency_count; ++i) {
            const NdsStaticValidationRange& range =
                validation->dependencies[i];
            if (!range.expected || range.size == 0u ||
                !bus_range_has_write_provenance(range.addr, range.size) ||
                !bus_live_bytes_equal(range.addr, range.expected,
                                      range.size)) {
                return false;
            }
        }
        return true;
    }
    return validation->expected && validation->size != 0u &&
        bus_range_has_write_provenance(validation->addr, validation->size) &&
        bus_live_bytes_equal(validation->addr, validation->expected,
                             validation->size);
}

bool validation_has_provenance(const NdsStaticValidation* validation) {
    if (!validation) return false;
    if (validation->dependency_count != 0u) {
        if (!validation->dependencies) return false;
        for (uint32_t i = 0u; i < validation->dependency_count; ++i) {
            const NdsStaticValidationRange& range =
                validation->dependencies[i];
            if (!range.expected || range.size == 0u ||
                !bus_range_has_write_provenance(range.addr, range.size)) {
                return false;
            }
        }
        return true;
    }
    return validation->expected && validation->size != 0u &&
        bus_range_has_write_provenance(validation->addr, validation->size);
}

// Why did the dispatcher refuse this row? Mirrors the order of the guard in
// runtime_arm.cpp dispatch_validation_live() exactly -- ownership first, then
// each range's shape, provenance, and bytes -- and reports the FIRST check
// that failed, so the counters partition the rejects instead of overlapping.
RejectReason classify_guard_reject(const NdsStaticValidation* validation,
                                   uint32_t pc, bool thumb) {
    if (!validation) return RejectReason::GuardRangeMalformed;
    if (!validation->expected || validation->size == 0u)
        return RejectReason::GuardRangeMalformed;
    {
        const uint32_t step = thumb ? 2u : 4u;
        const uint64_t begin = validation->addr;
        const uint64_t end = begin + validation->size;
        if (end > 0x1'0000'0000ull || pc < begin ||
            uint64_t{pc} + step > end) {
            return RejectReason::GuardRowNotOwned;
        }
    }
    const NdsStaticValidationRange owner{
        validation->addr, validation->size, validation->expected};
    const uint32_t count = validation->dependency_count;
    if (count != 0u && !validation->dependencies)
        return RejectReason::GuardRangeMalformed;
    for (uint32_t i = 0u; i < (count ? count : 1u); ++i) {
        const NdsStaticValidationRange& range =
            count ? validation->dependencies[i] : owner;
        if (!range.expected || range.size == 0u ||
            uint64_t{range.addr} + range.size > 0x1'0000'0000ull) {
            return RejectReason::GuardRangeMalformed;
        }
        if (!bus_range_has_write_provenance(range.addr, range.size))
            return RejectReason::GuardNoProvenance;
        if (!bus_live_bytes_equal(range.addr, range.expected, range.size))
            return RejectReason::GuardBytesDiffer;
    }
    // Every range passes now. The lookup that rejected the row saw a
    // transiently different memory image (a guest write landed between the
    // lookup and this classification); count it as the bytes case, which is
    // what it was.
    return RejectReason::GuardBytesDiffer;
}

// ---- dormant registry (beads-yjp.62) --------------------------------------
//
// Read on the emulation thread from the Tier-3 entry and from the Tier-3
// coverage-filing decision, both of which must stay cheap. Two levels keep
// them so:
//
//   * g_dormant_any -- one relaxed atomic load. False for every session that
//     has the overlay switched off, and for every session that has nothing
//     dormant, which is the whole cost those paths pay in that case.
//   * g_dormant_pages -- an emulation-thread-OWNED page hash set, rebuilt
//     under publish_mutex only when g_dormant_version moves. Dormancy changes
//     a handful of times per session, so the lock is never contended by the
//     hot path; the lookup itself touches no shared state at all.
std::atomic<bool> g_dormant_any{false};
std::atomic<uint32_t> g_dormant_version{0u};
uint32_t g_dormant_cache_version = 0u;
bool g_dormant_cache_valid = false;
std::unordered_set<uint64_t> g_dormant_pages;

int dormant_cpu_index(int cpu) { return cpu == NDS_ARM7 ? 1 : 0; }

uint64_t dormant_page_key(int cpu_index, uint32_t pc) {
    return (static_cast<uint64_t>(cpu_index & 1) << 32) |
        static_cast<uint64_t>(pc & ~0xFFFu);
}

// publish_mutex held. Republish the fast-path flag and invalidate the cache.
void publish_dormant_state_locked() {
    // Every record in the vector is live: parking ERASES its record rather
    // than flagging it, so there is no tombstone state to skip here.
    bool any = false;
    for (const DormantCandidate& candidate : g_live.dormant) {
        if (!candidate.pages.empty()) {
            any = true;
            break;
        }
    }
    g_dormant_any.store(any, std::memory_order_release);
    g_dormant_version.fetch_add(1u, std::memory_order_release);
}

// Emulation thread. A version read that races a write only costs one extra
// rebuild on the next call, never a stale answer that persists.
void refresh_dormant_cache() {
    const uint32_t version = g_dormant_version.load(std::memory_order_acquire);
    if (g_dormant_cache_valid && g_dormant_cache_version == version) return;
    std::unordered_set<uint64_t> pages;
    {
        std::lock_guard<std::mutex> lock(g_live.publish_mutex);
        for (const DormantCandidate& candidate : g_live.dormant) {
            for (uint32_t page : candidate.pages)
                pages.insert(dormant_page_key(candidate.cpu, page));
        }
    }
    g_dormant_pages.swap(pages);
    g_dormant_cache_version = version;
    g_dormant_cache_valid = true;
}

bool dormant_page_resident(int cpu, uint32_t pc) {
    if (!g_dormant_any.load(std::memory_order_relaxed)) return false;
    refresh_dormant_cache();
    return g_dormant_pages.find(
               dormant_page_key(dormant_cpu_index(cpu), pc)) !=
        g_dormant_pages.end();
}

// The 4 KB pages a prepared bank's rows own. Rows of one shard normally share
// a single validation, so the consecutive-pointer skip makes this one pass
// over the owner range in the common case.
std::vector<uint32_t> bank_owned_pages(const LoadedBank& bank) {
    std::vector<uint32_t> pages;
    if (!bank.dispatch) return pages;
    const auto add_page = [&pages](uint32_t base) {
        if (pages.size() >= kDormantMaxPagesPerCandidate) return;
        if (std::find(pages.begin(), pages.end(), base) == pages.end())
            pages.push_back(base);
    };
    const NdsStaticValidation* last = nullptr;
    for (unsigned i = 0; i < bank.dispatch_len; ++i) {
        const NdsDispatchEntry& row = bank.dispatch[i];
        const NdsStaticValidation* validation = row.validation;
        if (validation && validation != last) {
            last = validation;
            const uint64_t end =
                uint64_t{validation->addr} + validation->size;
            for (uint64_t page = validation->addr & ~0xFFFull;
                 page < end && page < 0x1'0000'0000ull; page += 0x1000ull) {
                add_page(static_cast<uint32_t>(page));
            }
        }
        add_page(row.addr & ~0xFFFu);
    }
    return pages;
}

// THE correctness question, asked once on the emulation thread before a
// prepared shard is adopted: is the guest code this bank was compiled from
// resident right now? One live row is enough -- partial residency still
// serves that row, and the per-row dispatch guard remains the authority.
bool bank_guard_bytes_live(const LoadedBank& bank) {
    if (!bank.dispatch || bank.dispatch_len == 0u) return false;
    const NdsStaticValidation* last = nullptr;
    unsigned probes = 0u;
    for (unsigned i = 0; i < bank.dispatch_len; ++i) {
        const NdsStaticValidation* validation = bank.dispatch[i].validation;
        if (!validation || validation == last) continue;
        last = validation;
        if (++probes > kDormantMaxValidationProbes) return true;
        if (validation_has_provenance(validation) &&
            validation_identity_live(validation)) {
            return true;
        }
    }
    return false;
}

// Emulation thread. Hold this candidate instead of discarding it, and count
// the deferral in its own family so banks_rejected -- and with it the futility
// guard -- never sees a scene change as provider failure.
void note_dormant_candidate(const LoadedBank& bank, const std::string& key) {
    std::vector<uint32_t> pages = bank_owned_pages(bank);
    bool parked = false;
    {
        std::lock_guard<std::mutex> lock(g_live.publish_mutex);
        std::size_t index = g_live.dormant.size();
        for (std::size_t i = 0; i < g_live.dormant.size(); ++i) {
            if (g_live.dormant[i].key == key) {
                index = i;
                break;
            }
        }
        if (index == g_live.dormant.size() &&
            g_live.dormant.size() < kDormantMaxCandidates) {
            DormantCandidate fresh;
            fresh.key = key;
            fresh.path = bank.path;
            fresh.cpu = dormant_cpu_index(bank.cpu);
            g_live.dormant.push_back(std::move(fresh));
        }
        if (index < g_live.dormant.size()) {
            DormantCandidate& candidate = g_live.dormant[index];
            candidate.pages = std::move(pages);
            candidate.requeued = false;
            if (++candidate.attempts >= kDormantMaxAttempts) {
                // Parking must leave NOTHING behind, and both halves of that
                // have to happen together.
                //
                // Releasing only the PAGES resumed coverage filing while the
                // candidate's canonical path stayed in queued_paths. The
                // compiler was then commissioned for the page, republished a
                // byte-identical DLL under the same content-addressed path,
                // and enqueue_candidate refused it as already-seen -- filing
                // forever, preparing never. That is the original churn bug one
                // level up, entered after eight unlucky scene misses.
                //
                // So drop the record AND the queued-paths key in one critical
                // section. The republished (or merely rescanned) DLL re-enters
                // prepare as a fresh candidate with a fresh attempt budget, so
                // progress is always possible -- slow churn at worst, bounded
                // by the compile cadence. A genuinely broken shard still
                // cannot activate: the guard-bytes admission gate decides
                // that, and it is untouched by any of this.
                parked = true;
                g_live.queued_paths.erase(key);
                g_live.dormant.erase(g_live.dormant.begin() +
                                     static_cast<std::ptrdiff_t>(index));
            }
        }
        publish_dormant_state_locked();
    }
    if (parked) ++g_live.dormant_parked;
    note_reject(parked ? RejectReason::DeferDormantParked
                       : RejectReason::DeferDormantGuardBytes);
}

// Emulation thread. Drop the dormant record for a candidate that has just
// been admitted (or is about to be). Returns whether it WAS dormant, which is
// what makes an adoption an activation rather than an ordinary first load.
bool clear_dormant_candidate(const std::string& key) {
    bool was_dormant = false;
    std::lock_guard<std::mutex> lock(g_live.publish_mutex);
    for (std::size_t i = 0; i < g_live.dormant.size(); ++i) {
        if (g_live.dormant[i].key != key) continue;
        was_dormant = true;
        g_live.dormant.erase(g_live.dormant.begin() +
                             static_cast<std::ptrdiff_t>(i));
        publish_dormant_state_locked();
        break;
    }
    return was_dormant;
}

// Emulation thread, from the Tier-3 ENTRY only. Pushes the candidate's path
// straight onto the prepare queue rather than through enqueue_candidate: its
// key deliberately stays in queued_paths so a cache rescan never spends one
// of its bounded attempts, and only this proof-of-residency path may.
bool requeue_dormant_for(int cpu, uint32_t pc) {
    bool covered = false;
    bool pushed = false;
    {
        std::lock_guard<std::mutex> lock(g_live.publish_mutex);
        const int cpu_index = dormant_cpu_index(cpu);
        const uint32_t page = pc & ~0xFFFu;
        for (DormantCandidate& candidate : g_live.dormant) {
            if (candidate.cpu != cpu_index) continue;
            if (std::find(candidate.pages.begin(), candidate.pages.end(),
                          page) == candidate.pages.end()) {
                continue;
            }
            covered = true;
            if (candidate.requeued) continue;
            candidate.requeued = true;
            ++g_live.dormant_requeues;
            if (candidate.path.empty()) continue;
            g_live.prepare_queue.push_back(
                std::filesystem::path(candidate.path));
            pushed = true;
        }
    }
    if (pushed) g_live.publish_cv.notify_one();
    return covered;
}

bool preflight_live_bank(const NdsLiveBankInfo& info,
                         std::string& error,
                         RejectReason* reason = nullptr) {
    // Every early return records WHICH check refused the bank, not just that
    // one did. `error` remains the human string; `reason` is the counter key
    // the field bundle carries (beads-yjp.53).
    const auto fail = [&](RejectReason code, const char* message) {
        error = message;
        if (reason) *reason = code;
        return false;
    };
    constexpr uint32_t kKnownFlags =
        NDS_LIVE_BANK_FLAG_DEPENDENCY_CLOSURE;
    if (info.flags & ~kKnownFlags) {
        return fail(RejectReason::LoadUnknownFlags,
                    "live bank has unknown safety flags");
    }
    // The shard build passes the CPU identity twice: as metadata `cpu` and
    // as -DNDS_STATIC_CPU, which folds the ARM9/ARM7 timing ternaries into
    // the generated bodies at compile time. If they disagree the bank runs
    // under the other CPU's timing model with nothing to show for it, so
    // the wrapper reports what it was actually compiled with and this is a
    // fail-closed cross-check, not an advisory.
    if (info.static_cpu != static_cast<uint32_t>(info.cpu)) {
        return fail(RejectReason::LoadStaticCpuMismatch,
                    "live bank static CPU build identity does not match its CPU");
    }
    if (!info.bank_id || !*info.bank_id) {
        return fail(RejectReason::LoadMissingBankId,
                    "live bank missing bank_id");
    }
    if (!info.candidate_id || !*info.candidate_id) {
        return fail(RejectReason::LoadMissingCandidateId,
                    "live bank missing candidate_id");
    }
    if (!info.dispatch || info.dispatch_len == 0u) {
        return fail(RejectReason::LoadNoDispatchRows,
                    "live bank has no dispatch rows");
    }
    if (info.dispatch_len > (1u << 20u)) {
        return fail(RejectReason::LoadDispatchTooLarge,
                    "live bank dispatch table exceeds the safety limit");
    }
    const bool closure =
        (info.flags & NDS_LIVE_BANK_FLAG_DEPENDENCY_CLOSURE) != 0u;
    const NdsStaticValidationRange* closure_ranges = nullptr;
    uint32_t closure_count = 0u;
    for (unsigned i = 0; i < info.dispatch_len; ++i) {
        const NdsDispatchEntry& row = info.dispatch[i];
        if (!row.fn) {
            return fail(RejectReason::LoadRowNullFn,
                        "live bank dispatch row has null function");
        }
        if (!validation_owns_row(row.validation, row)) {
            return fail(RejectReason::LoadRowNotOwned,
                        "live bank dispatch row is not owned by its validation");
        }
        if (!validation_dependencies_sane(row.validation, closure)) {
            return fail(RejectReason::LoadDependencyInsane,
                        closure
                            ? "live bank has an incomplete dependency closure"
                            : "live bank has unflagged dependency ranges");
        }
        if (closure) {
            if (!closure_ranges) {
                closure_ranges = row.validation->dependencies;
                closure_count = row.validation->dependency_count;
            } else if (closure_ranges != row.validation->dependencies ||
                       closure_count != row.validation->dependency_count) {
                return fail(
                    RejectReason::LoadClosureNotShared,
                    "live bank rows do not share one atomic dependency closure");
            }
        }
        if (i == 0u) continue;
        const NdsDispatchEntry& prev = info.dispatch[i - 1u];
        if (prev.addr > row.addr ||
            (prev.addr == row.addr && prev.thumb > row.thumb)) {
            return fail(RejectReason::LoadRowsUnsorted,
                        "live bank dispatch rows are not sorted");
        }
        if (prev.addr == row.addr && prev.thumb == row.thumb) {
            return fail(
                RejectReason::LoadRowsDuplicate,
                "live bank has duplicate dispatch rows inside one candidate");
        }
    }
    return true;
}

bool validate_live_bank_info(const NdsLiveBankInfo& info,
                             const std::string& expected_rom_sha1,
                             std::string& error,
                             RejectReason* reason = nullptr) {
    const auto fail = [&](RejectReason code, const char* message) {
        error = message;
        if (reason) *reason = code;
        return false;
    };
    if (info.abi_version != NDS_LIVE_BANK_ABI_VERSION) {
        return fail(RejectReason::LoadAbiMismatch,
                    "live bank ABI version mismatch");
    }
    if (!info.title_sha1 || expected_rom_sha1 != info.title_sha1) {
        return fail(RejectReason::LoadRomMismatch,
                    "live bank ROM identity mismatch");
    }
    if (info.cpu != NDS_ARM9 && info.cpu != NDS_ARM7) {
        return fail(RejectReason::LoadCpuInvalid,
                    "live bank CPU identity is invalid");
    }
    if (info.linked_g_cpu != &g_cpu ||
        info.linked_busf_main != &g_busf_main ||
        info.linked_busf_itcm != &g_busf_itcm ||
        info.linked_runtime_cycles != &g_runtime_cycles) {
        return fail(RejectReason::LoadImportsUnbound,
                    "live bank data imports are not bound to runner storage");
    }
    return preflight_live_bank(info, error, reason);
}

bool activation_delay_elapsed() {
    return !g_live.activation_delay_ms ||
        steady_ms() - g_live.configured_ms >= g_live.activation_delay_ms;
}

bool live_overlay_active() {
    return g_live.enabled && activation_delay_elapsed();
}

bool draining_backlog() {
    return g_live.pending_candidates != 0u;
}

// The cooldown the NEXT run will use. Empty backlog keeps the configured
// conservative cadence untouched; a non-empty one drops to the floor, and
// never below whatever the configuration already asked for if that is shorter.
uint32_t effective_cooldown_ms() {
    if (!draining_backlog()) return g_live.auto_cooldown_ms;
    if (g_live.auto_cooldown_ms == 0u) return 0u;
    return std::min(g_live.auto_cooldown_ms, kBacklogCooldownMs);
}

uint32_t effective_batch_cap() {
    return draining_backlog() ? g_live.batch_cap : kBaseBatchPages;
}

// Re-rate the batch after a run whose duration is known. Only backlog runs
// move the ramp: a run with nothing left to do says nothing about headroom.
void update_batch_ramp(uint64_t pending, uint64_t duration_ms) {
    g_live.last_run_duration_ms = duration_ms;
    if (pending == 0u) {
        g_live.batch_cap = kBaseBatchPages;
        return;
    }
    if (duration_ms <= kBacklogTargetRunMs) {
        g_live.batch_cap = std::min(kMaxBatchPages, g_live.batch_cap * 2u);
    } else if (duration_ms > 2ull * kBacklogTargetRunMs) {
        g_live.batch_cap = std::max(kBaseBatchPages, g_live.batch_cap / 2u);
    }
}

// The persisted queue is a small JSON document written by the compiler under
// live-index.lock. The runner needs exactly one scalar out of it -- how much
// work is waiting -- so it reads the "pending_count" field the writer puts
// there for this purpose rather than carrying a JSON parser. Anything
// unreadable, mismatched, or contended reads as "no backlog": the queue is
// scheduling advice, and a bad one must never be able to block a launch.
uint64_t read_persisted_pending_count() {
    if (g_live.cache_dir.empty() || g_live.rom_sha1.empty()) return 0u;
    std::error_code ec;
    const auto queue_path = g_live.cache_dir / "live-queue.json";
    if (!std::filesystem::is_regular_file(queue_path, ec)) return 0u;

    std::string text;
#if defined(_WIN32)

    // Same lock file and same exclusive byte-range discipline the compiler
    // uses (tools/compile_live_shards.py::exclusive_file_lock): one permanent
    // lock file, byte 0, released by the kernel if the holder dies.
    const std::string lock_name =
        path_string(g_live.cache_dir / "live-index.lock");
    HANDLE lock = CreateFileA(lock_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (lock == INVALID_HANDLE_VALUE) return 0u;
    bool held = false;
    const uint64_t deadline = steady_ms() + kQueueLockWaitMs;
    for (;;) {
        OVERLAPPED ov{};
        if (LockFileEx(lock, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                       0, 1u, 0u, &ov)) {
            held = true;
            break;
        }
        if (steady_ms() >= deadline) break;
        Sleep(25);
    }
    if (!held) {
        CloseHandle(lock);
        return 0u;
    }

    {
        std::ifstream f(queue_path, std::ios::binary);
        std::ostringstream buffer;
        buffer << f.rdbuf();
        text = buffer.str();
    }
    OVERLAPPED unlock_ov{};
    UnlockFileEx(lock, 0, 1u, 0u, &unlock_ov);
    CloseHandle(lock);
#elif defined(__linux__)
    const std::string lock_name =
        path_string(g_live.cache_dir / "live-index.lock");
    const int lock = ::open(lock_name.c_str(), O_RDWR | O_CREAT, 0666);
    if (lock < 0) return 0u;
    bool held = false;
    const uint64_t deadline = steady_ms() + kQueueLockWaitMs;
    for (;;) {
        if (flock(lock, LOCK_EX | LOCK_NB) == 0) {
            held = true;
            break;
        }
        if (steady_ms() >= deadline) break;
        usleep(25000u);
    }
    if (!held) {
        ::close(lock);
        return 0u;
    }
    {
        std::ifstream f(queue_path, std::ios::binary);
        std::ostringstream buffer;
        buffer << f.rdbuf();
        text = buffer.str();
    }
    flock(lock, LOCK_UN);
    ::close(lock);
#else
    return 0u;
#endif

    // Refuse a queue that belongs to another ROM. Sharing one cache directory
    // between titles is supported for the index, so it must be checked here.
    if (text.find("\"" + g_live.rom_sha1 + "\"") == std::string::npos)
        return 0u;
    constexpr const char* kKey = "\"pending_count\"";
    std::size_t pos = text.find(kKey);
    if (pos == std::string::npos) return 0u;
    pos = text.find(':', pos + std::strlen(kKey));
    if (pos == std::string::npos) return 0u;
    ++pos;
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(
               text[pos]))) {
        ++pos;
    }
    uint64_t value = 0u;
    bool any = false;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        if (value > (~0ull) / 10ull) return 0u;
        value = value * 10ull + static_cast<uint64_t>(text[pos] - '0');
        ++pos;
        any = true;
    }
    return any ? value : 0u;
}

void reload_persisted_queue() {
    if (g_live.queue_reloaded) return;
    g_live.queue_reloaded = true;
    const uint64_t pending = read_persisted_pending_count();
    if (pending == 0u) return;
    g_live.pending_candidates = pending;
    g_live.persisted_backlog = true;
    std::fprintf(stderr,
                 "[live-overlay] resuming a persisted backlog of %llu "
                 "candidate(s); the launch delay is waived for it\n",
                 static_cast<unsigned long long>(pending));
    // Work that was already discovered and is already waiting does not need
    // to be rediscovered by watching the guest interpret it again.
    if (g_live.auto_trigger) g_live.generation_pending = true;
}

// Whether compiling may start at all. A persisted backlog waives the launch
// delay for ITSELF only: the delay exists so a cold session is not competing
// with menu/asset loading over work nobody has proven is needed yet, and the
// backlog is precisely work already proven needed in an earlier session.
// Fresh discovery in this session still serves the full delay.
bool compile_activation_elapsed() {
    return g_live.persisted_backlog || activation_delay_elapsed();
}

bool compile_delay_elapsed() {
    const uint64_t now = steady_ms();
    const uint32_t cooldown = effective_cooldown_ms();
    const bool start_delay_done =
        g_live.persisted_backlog || !g_live.auto_start_delay_ms ||
        now - g_live.configured_ms >= g_live.auto_start_delay_ms;
    return start_delay_done &&
           (!g_live.last_compile_start_ms || !cooldown ||
            now - g_live.last_compile_start_ms >= cooldown);
}

void request_generation_compile() {
    if (!g_live.auto_trigger) return;
    g_live.generation_pending = true;
}

void schedule_pending_compile() {
    if (!g_live.generation_pending || !compile_delay_elapsed()) return;
    // A provider proven futile stays uncommissioned. The pending flag is left
    // set on purpose: an explicit live_overlay_trigger_now() lifts the
    // suppression, and the work it was going to do is still wanted then.
    if (g_live.auto_suppressed) return;
    if (g_live.trigger_requests > g_live.runs_started) return;
    g_live.generation_pending = false;
    ++g_live.trigger_requests;
}

// Called once every finished run's published shards have all resolved into
// either a load or a rejection. All-rejected means the provider is futile:
// shout the cause once, naming it, and stop auto-commissioning the identical
// work every cooldown. Mirrors psxrecomp's futility backoff plus its
// warn_on_cgtag_mismatch-style one-time shout.
void evaluate_run_futility() {
    if (!g_live.run_watch) return;
    {
        std::lock_guard<std::mutex> lock(g_live.publish_mutex);
        if (!g_live.prepare_queue.empty() || !g_live.ready_queue.empty() ||
            !g_live.prepare_errors.empty() || g_live.prepare_in_flight != 0) {
            return;
        }
    }
    if (child_active()) return;
    g_live.run_watch = false;
    const uint64_t loaded = g_live.banks_loaded - g_live.run_loaded_mark;
    const uint64_t rejected = g_live.banks_rejected - g_live.run_rejected_mark;
    // Futile means REJECTED, never merely "did not become resident". Three
    // healthy outcomes also leave banks_loaded unmoved and must not latch the
    // guard:
    //   - every shard was DECLINED because a better backend already covers
    //     that generation. This is the normal steady state on any install that
    //     ships a prebuilt gcc cache: the bundled tcc tier recompiles a page,
    //     the gcc bank wins, the generation stays covered natively. Treating
    //     it as futility would switch gap-filling off for most players.
    //   - a shard matched a candidate identity already resident.
    //   - the provider republished paths already examined, so nothing new was
    //     queued at all.
    // Requiring that EVERY newly examined shard produced a rejection is what
    // separates "this provider cannot satisfy this runner" from all of these.
    if (loaded != 0u || g_live.run_queued == 0u ||
        rejected != g_live.run_queued) {
        return;
    }
    ++g_live.futile_runs;
    g_live.auto_suppressed = true;
    g_live.futility_reason = g_live.last_error;
    std::fprintf(stderr,
        "[live-overlay] FUTILE COMPILE RUN: the provider published %llu "
        "shard(s), and all %llu newly examined were REJECTED "
        "(%s). This runner requires live bank ABI %u; a provider built against "
        "another ABI can never satisfy it. Auto-recompilation is now "
        "SUPPRESSED -- the identical work will not be re-commissioned every "
        "%u ms. Banks already resident keep running, and an explicit trigger "
        "lifts the suppression. Provider command: %s\n",
        static_cast<unsigned long long>(g_live.run_published),
        static_cast<unsigned long long>(g_live.run_queued),
        g_live.futility_reason.empty() ? "no reason recorded"
                                       : g_live.futility_reason.c_str(),
        NDS_LIVE_BANK_ABI_VERSION, g_live.auto_cooldown_ms,
        g_live.command.empty() ? "(none)" : g_live.command.c_str());
}

const char* diag_kind_name(DiagKind kind) {
    switch (kind) {
        case DiagKind::Transfer: return "transfer";
        case DiagKind::Lookup: return "lookup";
        case DiagKind::Write: return "write";
        default: return "unknown";
    }
}

void register_loaded_bank(LoadedBank& bank) {
    if (bank.registered || !bank.dispatch || bank.dispatch_len == 0u) return;
    nds_register_dispatch(bank.cpu, bank.dispatch, bank.dispatch_len,
                          bank.exc_base);
    bank.registered = true;
}

uint64_t hash_bytes(uint64_t hash, const void* data, std::size_t size) {
    constexpr uint64_t kFnvPrime = 1099511628211ull;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

uint64_t bank_content_identity(const NdsLiveBankInfo& info) {
    uint64_t hash = 1469598103934665603ull;
    hash = hash_bytes(hash, &info.cpu, sizeof(info.cpu));
    hash = hash_bytes(hash, &info.static_cpu, sizeof(info.static_cpu));
    hash = hash_bytes(hash, &info.exc_base, sizeof(info.exc_base));
    hash = hash_bytes(hash, &info.flags, sizeof(info.flags));
    for (unsigned i = 0; i < info.dispatch_len; ++i) {
        const NdsDispatchEntry& row = info.dispatch[i];
        hash = hash_bytes(hash, &row.addr, sizeof(row.addr));
        hash = hash_bytes(hash, &row.thumb, sizeof(row.thumb));
        const NdsStaticValidation& validation = *row.validation;
        hash = hash_bytes(hash, &validation.addr, sizeof(validation.addr));
        hash = hash_bytes(hash, &validation.size, sizeof(validation.size));
        hash = hash_bytes(hash, validation.expected, validation.size);
        hash = hash_bytes(hash, &validation.dependency_count,
                          sizeof(validation.dependency_count));
        for (uint32_t dependency = 0u;
             dependency < validation.dependency_count; ++dependency) {
            const NdsStaticValidationRange& range =
                validation.dependencies[dependency];
            hash = hash_bytes(hash, &range.addr, sizeof(range.addr));
            hash = hash_bytes(hash, &range.size, sizeof(range.size));
            hash = hash_bytes(hash, range.expected, range.size);
        }
    }
    return hash;
}

// Tier order for two banks covering the SAME generation: gcc > tcc > unknown.
// The compiler writes each shard into <cache>/<backend>/, so the immediate
// parent directory names the backend. An unrecognized layout scores 0 rather
// than being rejected: third-party ABI-v5 providers keep working, they just
// lose a tie against a shard we know was optimized.
uint32_t backend_tier(const std::filesystem::path& path) {
    const std::string parent =
        lower_ascii(path.parent_path().filename().string());
    if (parent == "gcc") return 2u;
    if (parent == "tcc") return 1u;
    return 0u;
}

// Runs on the prepare worker thread, so it must NOT touch the counters
// directly: it reports the cause through `reason` and the emulation thread
// records it when it drains prepare_errors.
bool prepare_bank_dll(const std::filesystem::path& path, LoadedBank& bank,
                      std::string& error, RejectReason& reason) {
#if defined(_WIN32) || defined(__linux__)
    if (!path_under_cache(g_live.cache_dir, path)) {
        reason = RejectReason::LoadPathOutsideCache;
        error = "published library outside live overlay cache: " +
            path_string(path);
        return false;
    }
    if (!live_overlay_is_final_library_path(path)) {
        reason = RejectReason::LoadNotPublishedDll;
        error = "live bank is not an atomically published library: " +
            path_string(path);
        return false;
    }

    std::error_code ec;
    const auto canon_path = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        reason = RejectReason::LoadCanonicalizeFailed;
        error = "cannot canonicalize published library: " +
            path_string(path);
        return false;
    }
    const std::string canon = path_string(canon_path);
    bank = {};
#if defined(_WIN32)
    bank.handle = LoadLibraryA(canon.c_str());
#else
    dlerror();
    bank.handle = dlopen(canon.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (!bank.handle) {
        reason = RejectReason::LoadLibraryFailed;
#if defined(_WIN32)
        error = "LoadLibrary failed: " + canon;
#else
        const char* detail = dlerror();
        error = "dlopen failed: " + canon +
            (detail ? std::string(": ") + detail : std::string{});
#endif
        return false;
    }

    using InfoFn = const NdsLiveBankInfo* (*)();
    NativeLibrarySymbol proc = library_symbol(bank, "nds_live_bank_info");
    InfoFn info_fn = nullptr;
    static_assert(sizeof(info_fn) == sizeof(proc));
    std::memcpy(&info_fn, &proc, sizeof(info_fn));
    const NdsLiveBankInfo* info = info_fn ? info_fn() : nullptr;
    if (!info) {
        close_library(bank);
        reason = RejectReason::LoadNoBankInfo;
        error = "live library does not export bank metadata: " + canon;
        return false;
    }
    std::string preflight_error;
    if (!validate_live_bank_info(*info, g_live.rom_sha1, preflight_error,
                                 &reason)) {
        close_library(bank);
        error = preflight_error + ": " + canon;
        return false;
    }

    bank.path = canon;
    bank.backend_tier = backend_tier(path);
    bank.bank_id = info->bank_id ? info->bank_id : "";
    bank.candidate_id = info->candidate_id ? info->candidate_id : "";
    using GenerationFn = const char* (*)();
    NativeLibrarySymbol generation_proc =
        library_symbol(bank, "nds_live_generation_id");
    GenerationFn generation_fn = nullptr;
    static_assert(sizeof(generation_fn) == sizeof(generation_proc));
    std::memcpy(&generation_fn, &generation_proc, sizeof(generation_fn));
    const char* generation_id = generation_fn ? generation_fn() : nullptr;
    // Generation IDs let a later candidate with additional resume roots
    // atomically supersede the same resident byte generation. The candidate
    // ID remains a fail-closed fallback for third-party ABI-v4 providers.
    bank.generation_id = generation_id && *generation_id
        ? generation_id : bank.candidate_id;
    bank.cpu = info->cpu;
    bank.exc_base = info->exc_base;
    bank.dispatch = info->dispatch;
    bank.dispatch_len = info->dispatch_len;
    bank.content_identity = bank_content_identity(*info);
    return true;
#else
    (void)path;
    (void)bank;
    reason = RejectReason::LoadUnsupportedPlatform;
    error = "live overlay loading is unsupported on this platform";
    return false;
#endif
}

bool same_candidate_key(const LoadedBank& a, const LoadedBank& b) {
    return a.cpu == b.cpu && a.bank_id == b.bank_id &&
        a.candidate_id == b.candidate_id;
}

// Does `super` cover every (addr, thumb) row `sub` covers?
//
// beads-yjp.53: two candidates for the SAME byte generation of a page are two
// translations of the identical guest bytes, differing only in which entry
// roots the capture that produced them happened to observe. Those root sets
// are NOT monotone in practice -- the live compiler only sees the roots that
// are still reaching Tier 3, so an entry already served natively drops out of
// the next capture of the same page. The player's own cache index shows it:
// page 0x0205B000 went from a 4-entry candidate to a 3-entry one, and
// 0x02034000 from 3 to 2. Replacing the resident bank with such a candidate
// DELETES the rows only the resident one had, which sends that code straight
// back to the interpreter and makes the next capture "discover" it again.
//
// So supersede is allowed only when it cannot lose a row. Anything else keeps
// both banks registered: the dispatch index already chains multiple rows for
// one PC and picks the newest whose validation is live, and both bodies were
// generated from byte-identical guest code, so either is a faithful
// translation of it.
bool dispatch_rows_cover(const LoadedBank& super, const LoadedBank& sub) {
    if (!super.dispatch || !sub.dispatch) return false;
    if (super.dispatch_len < sub.dispatch_len) return false;
    // Both tables are sorted by (addr, thumb) -- preflight_live_bank refuses a
    // bank whose rows are not -- so this is a linear merge, not a search.
    unsigned i = 0u;
    for (unsigned j = 0u; j < sub.dispatch_len; ++j) {
        const NdsDispatchEntry& want = sub.dispatch[j];
        while (i < super.dispatch_len &&
               (super.dispatch[i].addr < want.addr ||
                (super.dispatch[i].addr == want.addr &&
                 (super.dispatch[i].thumb != 0u) < (want.thumb != 0u)))) {
            ++i;
        }
        if (i >= super.dispatch_len ||
            super.dispatch[i].addr != want.addr ||
            (super.dispatch[i].thumb != 0u) != (want.thumb != 0u)) {
            return false;
        }
    }
    return true;
}

bool commit_prepared_bank(LoadedBank bank) {
#if defined(_WIN32) || defined(__linux__)
    for (const LoadedBank& loaded : g_live.loaded) {
        if (!same_candidate_key(loaded, bank)) continue;
        if (loaded.content_identity != bank.content_identity) {
            g_live.last_error =
                "conflicting live bank content for candidate identity " +
                bank.bank_id + "/" + bank.candidate_id;
            ++g_live.banks_rejected;
            note_reject(RejectReason::LoadContentConflict);
        } else {
            note_reject(RejectReason::DropDuplicateCandidate);
        }
        close_library(bank);
        return loaded.content_identity == bank.content_identity;
    }
    // ---- one decision per same-generation resident ----------------------
    //
    // Three outcomes, and the ONLY one that may unregister anything is the
    // one that provably cannot lose a row:
    //
    //   resident covers the newcomer  -> drop the newcomer (adds nothing).
    //                                    Also the original "never demote a
    //                                    generation a better backend already
    //                                    covers" rule: a dev box, or a player
    //                                    whose shipped cache was built by CI,
    //                                    can hold a gcc shard for exactly this
    //                                    generation while the local tcc tier
    //                                    compiles its own, and arrival order
    //                                    alone must not decide.
    //   newcomer covers the resident
    //     and is not a worse backend  -> retire the resident (supersede).
    //   anything else                 -> keep BOTH registered.
    //
    // The last case is the beads-yjp.53 fix and covers two real situations:
    // genuinely divergent root sets, and a newcomer that is a superset but
    // built by a weaker compiler -- there the optimized bodies stay resident
    // and the newcomer contributes only the rows nobody else has.
    //
    // Nothing is mutated in the first pass: a decision taken while already
    // unregistering could leave the generation momentarily uncovered.
    const LoadedBank* covering_resident = nullptr;
    for (const LoadedBank& loaded : g_live.loaded) {
        if (!loaded.registered || loaded.cpu != bank.cpu ||
            loaded.bank_id != bank.bank_id ||
            loaded.generation_id != bank.generation_id) {
            continue;
        }
        if (dispatch_rows_cover(loaded, bank)) {
            covering_resident = &loaded;
            break;
        }
    }
    if (covering_resident) {
        if (covering_resident->backend_tier > bank.backend_tier) {
            std::fprintf(stderr,
                         "[live-overlay] kept tier-%u %s for generation %s; "
                         "declined tier-%u candidate %s\n",
                         covering_resident->backend_tier,
                         covering_resident->bank_id.c_str(),
                         covering_resident->generation_id.c_str(),
                         bank.backend_tier, bank.candidate_id.c_str());
            note_reject(RejectReason::DropDeclinedLowerTier);
        } else {
            note_reject(RejectReason::DropRedundantSubset);
        }
        close_library(bank);
        return true;
    }
    for (LoadedBank& loaded : g_live.loaded) {
        if (!loaded.registered || loaded.cpu != bank.cpu ||
            loaded.bank_id != bank.bank_id ||
            loaded.generation_id != bank.generation_id) {
            continue;
        }
        if (!dispatch_rows_cover(bank, loaded) ||
            bank.backend_tier < loaded.backend_tier) {
            note_reject(RejectReason::KeptDivergentGeneration);
            continue;
        }
        nds_unregister_dispatch(loaded.cpu, loaded.dispatch,
                                loaded.dispatch_len);
        note_reject(RejectReason::DropSupersededGeneration);
        g_live.rows_superseded += loaded.dispatch_len;
        g_live.rows_superseding += bank.dispatch_len;
        loaded.registered = false;
        loaded.superseded = true;
        loaded.dispatch = nullptr;
        close_library(loaded);
    }
    bank.serial = g_live.next_bank_serial++;
    bank.generation = static_cast<uint32_t>(++g_live.publication_generation);
    g_live.loaded.push_back(std::move(bank));
    register_loaded_bank(g_live.loaded.back());
    ++g_live.banks_loaded;
    // beads-lqa.40: co-registering a divergent same-generation candidate no
    // longer costs the better backend the addresses the two banks share.
    // Selection ranks candidates by owned span and breaks ties by
    // FIRST-registered (dispatch_lookup.h), and two shards of one generation
    // are the same guest bytes, so their shared rows always tie: the bank that
    // was adopted first keeps them. Ordering the cache scan best-backend-first
    // (rescan_cache) is what makes "first" mean "best"; the previous
    // unregister/re-register dance that pushed the better bank to the end of
    // the index is gone with the last-registered-wins accident it worked around.
    std::fprintf(stderr,
                 "[live-overlay] published %s candidate %s (%u rows, generation %u)\n",
                 g_live.loaded.back().bank_id.c_str(),
                 g_live.loaded.back().candidate_id.c_str(),
                 g_live.loaded.back().dispatch_len,
                 g_live.loaded.back().generation);
    return true;
#else
    (void)bank;
    return false;
#endif
}

// The single gate every prepared shard passes through on the emulation
// thread (beads-yjp.62). Either its guest code is resident and the bank is
// adopted by the unchanged commit path, or it is held dormant and re-queued
// later. Nothing else may call commit_prepared_bank on the production path.
void admit_prepared_bank(LoadedBank bank, const std::string& key) {
    if (!bank_guard_bytes_live(bank)) {
        note_dormant_candidate(bank, key);
#if defined(_WIN32) || defined(__linux__)
        close_library(bank);
#endif
        return;
    }
    const bool was_dormant = clear_dormant_candidate(key);
    if (commit_prepared_bank(std::move(bank)) && was_dormant)
        ++g_live.dormant_activations;
}

void prepare_worker_main() {
    for (;;) {
        std::filesystem::path path;
        {
            std::unique_lock<std::mutex> lock(g_live.publish_mutex);
            g_live.publish_cv.wait(lock, [] {
                return g_live.prepare_stop || !g_live.prepare_queue.empty();
            });
            if (g_live.prepare_stop && g_live.prepare_queue.empty()) return;
            path = std::move(g_live.prepare_queue.front());
            g_live.prepare_queue.pop_front();
            ++g_live.prepare_in_flight;
        }
        LoadedBank bank{};
        std::string error;
        RejectReason reason = RejectReason::LoadNoBankInfo;
        const bool ok = prepare_bank_dll(path, bank, error, reason);
        {
            std::lock_guard<std::mutex> lock(g_live.publish_mutex);
            if (ok) g_live.ready_queue.push_back(std::move(bank));
            else    g_live.prepare_errors.push_back(
                        PreparedError{std::move(error), reason});
            --g_live.prepare_in_flight;
            g_live.prepare_results_ready.store(true,
                                               std::memory_order_release);
        }
    }
}

void maintenance_worker_main();

void ensure_workers() {
    if (!g_live.prepare_thread.joinable()) {
        g_live.prepare_stop = false;
        g_live.prepare_thread = std::thread(prepare_worker_main);
    }
    if (!g_live.maint_thread.joinable()) {
        g_live.maint_stop = false;
        g_live.maint_thread = std::thread(maintenance_worker_main);
    }
}

// Worker side of queueing: the filters and the canonicalization. There is no
// separate "is it already resident" scan any more -- queued_paths is never
// cleared and every resident bank's canonical path went through it, so it
// already subsumes that check without walking g_live.loaded (which only the
// emulation thread may read) or allocating per bank.
bool make_queue_candidate(const std::filesystem::path& cache_dir,
                          const std::filesystem::path& path,
                          QueueCandidate& out) {
    if (!live_overlay_is_final_library_path(path) ||
        !path_under_cache(cache_dir, path))
        return false;
    std::error_code ec;
    auto canon_path = std::filesystem::weakly_canonical(path, ec);
    if (ec) return false;
    out.key = path_key(canon_path);
    out.path = std::move(canon_path);
    return true;
}

// Shrink a worker-built candidate list to what is plausibly new. The emulation
// thread still does the authoritative insert, so a race here costs nothing.
void drop_already_queued(std::vector<QueueCandidate>& items) {
    if (items.empty()) return;
    std::lock_guard<std::mutex> lock(g_live.publish_mutex);
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const QueueCandidate& item) {
                                   return g_live.queued_paths.count(item.key) !=
                                       0u;
                               }),
                items.end());
}

// Emulation thread. Cheap by construction: one set insert and one deque push.
bool enqueue_candidate(const QueueCandidate& candidate) {
    {
        std::lock_guard<std::mutex> lock(g_live.publish_mutex);
        if (!g_live.queued_paths.insert(candidate.key).second) return false;
        g_live.prepare_queue.push_back(candidate.path);
    }
    g_live.publish_cv.notify_one();
    return true;
}

void commit_one_ready_bank() {
    LoadedBank bank{};
    PreparedError error;
    bool have_bank = false;
    {
        ++g_live.prepare_result_drains_for_test;
        std::lock_guard<std::mutex> lock(g_live.publish_mutex);
        if (!g_live.prepare_errors.empty()) {
            error = std::move(g_live.prepare_errors.front());
            g_live.prepare_errors.pop_front();
        }
        if (!g_live.ready_queue.empty()) {
            bank = std::move(g_live.ready_queue.front());
            g_live.ready_queue.pop_front();
            have_bank = true;
        }
        g_live.prepare_results_ready.store(
            !g_live.prepare_errors.empty() || !g_live.ready_queue.empty(),
            std::memory_order_release);
    }
    if (!error.text.empty()) {
        g_live.last_error = std::move(error.text);
        ++g_live.banks_rejected;
        note_reject(error.reason);
    }
    if (have_bank) {
        const std::string key = path_key(std::filesystem::path(bank.path));
        admit_prepared_bank(std::move(bank), key);
    }
}

struct CachedDllPath {
    std::filesystem::path path;
    std::filesystem::file_time_type write_time{};
    uint32_t tier = 0u;
};

// Runs on the maintenance worker (or, at startup, on the launching thread).
std::vector<QueueCandidate> scan_cache(
    const std::filesystem::path& cache_dir) {
    std::vector<QueueCandidate> out;
    if (cache_dir.empty()) return out;
    std::error_code ec;
    if (!std::filesystem::exists(cache_dir, ec)) return out;
    std::vector<CachedDllPath> paths;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(cache_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const auto path = entry.path();
        if (!live_overlay_is_final_library_path(path)) continue;
        const auto write_time = entry.last_write_time(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        paths.push_back({path, write_time, backend_tier(path)});
    }
    // Best backend FIRST (beads-lqa.40). When a cold scan finds both a tcc and
    // a gcc shard for one generation, the gcc one is adopted first and the tcc
    // one is then declined outright (it covers nothing the resident lacks) or,
    // if it carries an extra root, co-registered -- where the dispatch rank's
    // first-registered tie-break leaves the shared addresses with the gcc bank.
    // Queuing the weaker shard first would hand it those addresses instead.
    // Within a tier the original oldest-first ordering stands, so the newest
    // revision of a generation still supersedes on recompile.
    std::sort(paths.begin(), paths.end(), [](const CachedDllPath& a,
                                             const CachedDllPath& b) {
        if (a.tier != b.tier) return a.tier > b.tier;
        if (a.write_time != b.write_time) return a.write_time < b.write_time;
        return path_key(a.path) < path_key(b.path);
    });
    out.reserve(paths.size());
    for (const CachedDllPath& item : paths) {
        QueueCandidate candidate;
        if (make_queue_candidate(cache_dir, item.path, candidate))
            out.push_back(std::move(candidate));
    }
    drop_already_queued(out);
    return out;
}

std::vector<std::filesystem::path> published_paths_from_log(
    const std::string& log_path) {
    std::vector<std::filesystem::path> out;
    std::ifstream f(log_path);
    std::string line;
    constexpr const char* marker = "NDS_SHARD_PUBLISHED ";
    constexpr std::size_t marker_len = 20u;
    while (std::getline(f, line)) {
        const std::size_t pos = line.find(marker);
        if (pos == std::string::npos) continue;
        std::string p = line.substr(pos + marker_len);
        while (!p.empty() && (p.back() == '\r' || p.back() == '\n' ||
                              p.back() == ' ' || p.back() == '\t'))
            p.pop_back();
        if (!p.empty()) out.emplace_back(p);
    }
    return out;
}

// The backlog the finished run reported. Absent marker (an older provider, or
// a run that died before printing) leaves the previous count in place: the
// caller passes it in as `fallback` so a crashed batch does not silently look
// like a drained queue and collapse the cadence back to conservative.
uint64_t pending_from_log(const std::string& log_path, uint64_t fallback) {
    std::ifstream f(log_path);
    std::string line;
    constexpr const char* marker = "NDS_SHARD_PENDING ";
    const std::size_t marker_len = std::strlen(marker);
    uint64_t value = fallback;
    bool found = false;
    while (std::getline(f, line)) {
        const std::size_t pos = line.find(marker);
        if (pos == std::string::npos) continue;
        const char* p = line.c_str() + pos + marker_len;
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(p, &end, 10);
        if (end == p) continue;
        value = parsed;
        found = true;  // last marker in the log wins
    }
    (void)found;
    return value;
}

unsigned long current_process_id() {
#if defined(_WIN32)
    return static_cast<unsigned long>(GetCurrentProcessId());
#elif defined(__linux__)
    return static_cast<unsigned long>(getpid());
#else
    return 0ul;
#endif
}

// ---- maintenance worker jobs ----------------------------------------------

MaintenanceResult run_finished_job(const MaintenanceJob& job) {
    MaintenanceResult result;
    result.kind = MaintenanceKind::RunFinished;
    result.duration_ms = job.duration_ms;
    result.exit_code = job.exit_code;
    // Read the backlog BEFORE anything else: the ramp decision the emulation
    // thread makes from it is "did this run leave work behind, and how fast
    // was it".
    result.pending = pending_from_log(job.log_path, job.pending_fallback);
    const auto published = published_paths_from_log(job.log_path);
    result.published_total = published.size();
    result.published.reserve(published.size());
    for (const auto& path : published) {
        QueueCandidate candidate;
        if (make_queue_candidate(job.cache_dir, path, candidate))
            result.published.push_back(std::move(candidate));
    }
    drop_already_queued(result.published);
    result.rescan = scan_cache(job.cache_dir);
    return result;
}

MaintenanceResult start_child_job(MaintenanceJob& job) {
    MaintenanceResult result;
    result.kind = MaintenanceKind::StartChild;
    std::error_code ec;
    std::filesystem::create_directories(job.cache_dir / "snapshots", ec);
    if (ec) {
        result.error = "cannot create live overlay snapshot directory";
        return result;
    }
    // Qualify the name with the process id. The run index alone restarts at 1
    // every launch, so a resumed session would OVERWRITE the very snapshots
    // the persisted queue points at, replacing the page payloads a queued
    // candidate needs and silently turning every cross-session resume into a
    // rediscovery. Names stay glob-compatible with manifest-*.json.
    char name[80];
    std::snprintf(name, sizeof(name), "manifest-%08lx-%06llu.json",
                  static_cast<unsigned long>(current_process_id()),
                  static_cast<unsigned long long>(job.run_index));
    const auto path = job.cache_dir / "snapshots" / name;
    char error[256] = {};
    // The child reads this file, so it has to be durably renamed into place
    // before CreateProcess -- which is exactly why both halves live on this
    // one serial worker rather than being handed back and forth.
    if (!coverage_manifest_write_captured_snapshot(
            job.snapshot, path_string(path).c_str(), error, sizeof(error))) {
        result.error = error[0] ? error : "cannot write the live overlay "
                                          "snapshot manifest";
        return result;
    }
    result.manifest_path = path_string(path);

    std::filesystem::create_directories(job.cache_dir / "logs", ec);
    if (ec) {
        result.error = "cannot create live overlay log directory";
        return result;
    }
    char log_name[80];
    std::snprintf(log_name, sizeof(log_name), "compile-%08lx-%06llu.log",
                  current_process_id(),
                  static_cast<unsigned long long>(job.run_index));
    result.log_path = path_string(job.cache_dir / "logs" / log_name);

#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{sizeof(sa), NULL, TRUE};
    HANDLE log = CreateFileA(result.log_path.c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE) {
        result.error = "cannot open live overlay compiler log";
        return result;
    }

    SetEnvironmentVariableA("NDS_LIVE_OVERLAY_MANIFEST",
                            result.manifest_path.c_str());
    const std::string cache = path_string(job.cache_dir);
    SetEnvironmentVariableA("NDS_LIVE_OVERLAY_CACHE", cache.c_str());
    SetEnvironmentVariableA("NDS_LIVE_OVERLAY_ROM_SHA1", job.rom_sha1.c_str());
    // The batch cap for THIS run. The provider reads it as the default for
    // --max-pages, so the runner owns the throughput policy in one place and
    // the shipped command string does not have to hard-code a number.
    const std::string cap = std::to_string(job.batch_cap);
    SetEnvironmentVariableA("NDS_LIVE_OVERLAY_MAX_PAGES", cap.c_str());

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = log;
    si.hStdError = log;
    PROCESS_INFORMATION pi{};
    std::string full = "cmd.exe /C \"" + job.command + "\"";
    std::vector<char> cmd(full.begin(), full.end());
    cmd.push_back('\0');

    HANDLE child_job = CreateJobObjectA(NULL, NULL);
    if (child_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(child_job,
                                     JobObjectExtendedLimitInformation, &jeli,
                                     sizeof(jeli))) {
            CloseHandle(child_job);
            child_job = NULL;
        }
    }

    BOOL ok = CreateProcessA(NULL, cmd.data(), NULL, NULL, TRUE,
                             CREATE_NO_WINDOW | IDLE_PRIORITY_CLASS |
                                 CREATE_SUSPENDED,
                             NULL, NULL, &si, &pi);
    CloseHandle(log);
    if (!ok) {
        if (child_job) CloseHandle(child_job);
        result.error = "CreateProcess failed for live overlay compiler";
        return result;
    }
    if (child_job && !AssignProcessToJobObject(child_job, pi.hProcess)) {
        CloseHandle(child_job);
        child_job = NULL;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    result.child = pi.hProcess;
    result.child_job = child_job;
    result.started_ms = steady_ms();
    result.ok = true;
    return result;
#elif defined(__linux__)
    const int log = ::open(result.log_path.c_str(),
                           O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log < 0) {
        result.error = "cannot open live overlay compiler log: " +
            std::string(std::strerror(errno));
        return result;
    }

    const std::string cache = path_string(job.cache_dir);
    const std::array<std::string, 4> overrides = {
        "NDS_LIVE_OVERLAY_MANIFEST=" + result.manifest_path,
        "NDS_LIVE_OVERLAY_CACHE=" + cache,
        "NDS_LIVE_OVERLAY_ROM_SHA1=" + job.rom_sha1,
        "NDS_LIVE_OVERLAY_MAX_PAGES=" + std::to_string(job.batch_cap),
    };
    std::vector<std::string> environment;
    for (char** item = environ; item && *item; ++item) {
        const std::string value(*item);
        bool replaced = false;
        for (const std::string& override_value : overrides) {
            const std::size_t equals = override_value.find('=');
            if (value.compare(0, equals + 1u, override_value, 0,
                              equals + 1u) == 0) {
                replaced = true;
                break;
            }
        }
        if (!replaced) environment.push_back(value);
    }
    environment.insert(environment.end(), overrides.begin(), overrides.end());
    std::vector<char*> envp;
    envp.reserve(environment.size() + 1u);
    for (std::string& value : environment) envp.push_back(value.data());
    envp.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int rc = posix_spawn_file_actions_init(&actions);
    const bool actions_initialized = rc == 0;
    if (rc == 0) rc = posix_spawn_file_actions_adddup2(&actions, log,
                                                       STDOUT_FILENO);
    if (rc == 0) rc = posix_spawn_file_actions_adddup2(&actions, log,
                                                       STDERR_FILENO);
    if (rc == 0) rc = posix_spawn_file_actions_addclose(&actions, log);
    if (rc == 0) rc = posix_spawnattr_init(&attributes);
    const bool attributes_initialized = rc == 0;
    if (rc == 0) rc = posix_spawnattr_setflags(
        &attributes, POSIX_SPAWN_SETPGROUP);
    if (rc == 0) rc = posix_spawnattr_setpgroup(&attributes, 0);
    pid_t child = -1;
    // Run the provider in its own process group and below emulator priority.
    // $0/$1 carry the command without interpolating it into this wrapper.
    char shell[] = "/bin/sh";
    char wrapper[] = "exec nice -n 10 \"$0\" -c \"$1\"";
    char* argv[] = {shell, const_cast<char*>("-c"), wrapper, shell,
                    job.command.data(), nullptr};
    if (rc == 0) {
        rc = posix_spawn(&child, shell, &actions, &attributes, argv,
                         envp.data());
    }
    if (actions_initialized) posix_spawn_file_actions_destroy(&actions);
    if (attributes_initialized) posix_spawnattr_destroy(&attributes);
    ::close(log);
    if (rc != 0) {
        result.error = "posix_spawn failed for live overlay compiler: " +
            std::string(std::strerror(rc));
        return result;
    }
    result.child = child;
    result.started_ms = steady_ms();
    result.ok = true;
    return result;
#else
    (void)job;
    result.error = "live overlay compilation is unsupported on this platform";
    return result;
#endif
}

void maintenance_worker_main() {
    for (;;) {
        MaintenanceJob job;
        {
            std::unique_lock<std::mutex> lock(g_live.maint_mutex);
            g_live.maint_cv.wait(lock, [] {
                return g_live.maint_stop || !g_live.maint_jobs.empty();
            });
            // Teardown ABANDONS queued work rather than draining it: a
            // snapshot write or a CreateProcess started here would outlive the
            // session it belongs to. live_overlay_shutdown() releases them.
            if (g_live.maint_stop) return;
            job = std::move(g_live.maint_jobs.front());
            g_live.maint_jobs.pop_front();
        }
        MaintenanceResult result;
        switch (job.kind) {
            case MaintenanceKind::Rescan:
                result.kind = MaintenanceKind::Rescan;
                result.rescan = scan_cache(job.cache_dir);
                break;
            case MaintenanceKind::RunFinished:
                result = run_finished_job(job);
                break;
            case MaintenanceKind::StartChild:
                result = start_child_job(job);
                break;
        }
        coverage_manifest_release_snapshot(job.snapshot);
        job.snapshot = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_live.maint_mutex);
            g_live.maint_results.push_back(std::move(result));
            g_live.maint_results_ready.store(true,
                                             std::memory_order_release);
        }
    }
}

// ---- emulation-thread side of the worker ----------------------------------

void push_maintenance_job(MaintenanceJob job) {
    {
        std::lock_guard<std::mutex> lock(g_live.maint_mutex);
        g_live.maint_jobs.push_back(std::move(job));
    }
    g_live.maint_cv.notify_one();
}

void queue_rescan_job() {
    if (g_live.cache_dir.empty()) return;
    MaintenanceJob job;
    job.kind = MaintenanceKind::Rescan;
    job.cache_dir = g_live.cache_dir;
    push_maintenance_job(std::move(job));
}

bool request_child_start() {
    if (g_live.command.empty()) return false;
    // The ONLY part of the snapshot that has to happen here: a bounded copy of
    // the resident page store and the guest page payloads, both of which the
    // emulation thread owns. base64, the sort and the file write ride along on
    // the job.
    CoverageLiveSnapshot* snapshot =
        coverage_manifest_capture_live_snapshot(64u);
    if (!snapshot) {
        g_live.last_error = "cannot capture the live overlay coverage snapshot";
        ++g_live.runs_failed;
        return false;
    }
    MaintenanceJob job;
    job.kind = MaintenanceKind::StartChild;
    job.cache_dir = g_live.cache_dir;
    job.command = g_live.command;
    job.rom_sha1 = g_live.rom_sha1;
    job.run_index = g_live.runs_started + 1u;
    job.batch_cap = effective_batch_cap();
    job.snapshot = snapshot;
    g_live.start_pending = true;
    push_maintenance_job(std::move(job));
    return true;
}

void apply_run_finished(MaintenanceResult& result) {
    g_live.run_finish_pending = false;
    g_live.pending_candidates = result.pending;
    if (result.pending == 0u) g_live.persisted_backlog = false;
    update_batch_ramp(result.pending, result.duration_ms);
    if (result.exit_code != 0u) {
        ++g_live.runs_failed;
        g_live.last_error = "live overlay compiler exited with code " +
            std::to_string(result.exit_code);
    }
    // Watch this run's own shards through to their verdict. Note the marks are
    // taken BEFORE queueing so nothing this run produced can be attributed to
    // an earlier one -- which is why queueing stayed on this thread.
    g_live.run_loaded_mark = g_live.banks_loaded;
    g_live.run_rejected_mark = g_live.banks_rejected;
    g_live.run_queued = 0;
    g_live.run_published = result.published_total;
    for (const QueueCandidate& candidate : result.published)
        if (enqueue_candidate(candidate)) ++g_live.run_queued;
    g_live.run_watch = result.published_total != 0u;
    // Keep draining. Conditioned on the run having actually PUBLISHED
    // something so the no-progress backoff below still owns the case where the
    // batch produced nothing: a provider that can see work but never emit a
    // shard for it must not be re-commissioned every cooldown just because its
    // queue file says work remains.
    if (result.exit_code == 0u && result.pending != 0u &&
        result.published_total != 0u) {
        request_generation_compile();
    }
    if (result.exit_code == 0u && result.published_total == 0u) {
        for (int cpu = 0; cpu < 2; ++cpu) {
            g_live.next_trigger[cpu] = std::max(
                g_live.next_trigger[cpu],
                g_live.tier3[cpu] + kNoProgressRetriggerTier3);
        }
    }
    for (const QueueCandidate& candidate : result.rescan)
        enqueue_candidate(candidate);
}

void apply_start_child(MaintenanceResult& result) {
    g_live.start_pending = false;
    if (!result.log_path.empty()) g_live.log_path = std::move(result.log_path);
    if (!result.ok) {
        g_live.last_error = std::move(result.error);
        ++g_live.runs_failed;
        // The request is spent; a fresh trigger has to ask again.
        g_live.trigger_requests = g_live.runs_started;
        return;
    }
#if defined(_WIN32)
    g_live.child = result.child;
    g_live.child_job = result.child_job;
    result.child = nullptr;
    result.child_job = nullptr;
#elif defined(__linux__)
    g_live.child = result.child;
    result.child = -1;
#endif
    g_live.manifest_path = std::move(result.manifest_path);
    ++g_live.runs_started;
    g_live.last_compile_start_ms = result.started_ms;
    std::fprintf(stderr, "[live-overlay] compiling from %s\n",
                 g_live.manifest_path.c_str());
}

void drain_maintenance_results() {
    std::deque<MaintenanceResult> results;
    {
        ++g_live.maint_result_drains_for_test;
        std::lock_guard<std::mutex> lock(g_live.maint_mutex);
        results.swap(g_live.maint_results);
        g_live.maint_results_ready.store(!g_live.maint_results.empty(),
                                         std::memory_order_release);
    }
    for (MaintenanceResult& result : results) {
        switch (result.kind) {
            case MaintenanceKind::Rescan:
                for (const QueueCandidate& candidate : result.rescan)
                    enqueue_candidate(candidate);
                break;
            case MaintenanceKind::RunFinished:
                apply_run_finished(result);
                break;
            case MaintenanceKind::StartChild:
                apply_start_child(result);
                break;
        }
    }
}

void queue_run_finished(uint32_t exit_code) {
    ++g_live.runs_finished;
    MaintenanceJob job;
    job.kind = MaintenanceKind::RunFinished;
    job.cache_dir = g_live.cache_dir;
    job.log_path = g_live.log_path;
    job.pending_fallback = g_live.pending_candidates;
    job.exit_code = exit_code;
    job.duration_ms = g_live.last_compile_start_ms
        ? steady_ms() - g_live.last_compile_start_ms
        : 0u;
    g_live.run_finish_pending = true;
    push_maintenance_job(std::move(job));
}

}  // namespace

bool live_overlay_preflight_for_test(const NdsLiveBankInfo* info,
                                     char* error,
                                     uint32_t error_len) {
    std::string message;
    const bool ok = info && preflight_live_bank(*info, message);
    if (!ok && error && error_len != 0u)
        copy_cstr(error, error_len,
                  info ? message.c_str() : "missing live bank info");
    return ok;
}

bool live_overlay_info_for_test(const NdsLiveBankInfo* info,
                                const char* expected_rom_sha1,
                                char* error,
                                uint32_t error_len) {
    std::string message;
    const bool ok = info && validate_live_bank_info(
        *info, expected_rom_sha1 ? expected_rom_sha1 : "", message);
    if (!ok && error && error_len != 0u)
        copy_cstr(error, error_len,
                  info ? message.c_str() : "missing live bank info");
    return ok;
}

void live_overlay_publish_bank_for_test(int cpu,
                                        const char* bank_id,
                                        const char* candidate_id,
                                        const NdsDispatchEntry* dispatch,
                                        unsigned dispatch_len) {
    // Test-only injection of a resident bank. Loading a real DLL needs a
    // compiled shard; the registration lifecycle this exercises (publish ->
    // runtime reset -> poll re-registers) is independent of where the rows
    // came from, so the unit layer supplies its own rows.
    LoadedBank bank{};
    bank.bank_id = bank_id ? bank_id : "";
    bank.candidate_id = candidate_id ? candidate_id : "";
    bank.generation_id = bank.candidate_id;
    bank.cpu = cpu;
    bank.exc_base = cpu == NDS_ARM7 ? 0x00000000u : 0xFFFF0000u;
    bank.dispatch = dispatch;
    bank.dispatch_len = dispatch_len;
    bank.serial = g_live.next_bank_serial++;
    bank.generation = static_cast<uint32_t>(++g_live.publication_generation);
    g_live.loaded.push_back(std::move(bank));
    register_loaded_bank(g_live.loaded.back());
    ++g_live.banks_loaded;
}

void live_overlay_configure(bool enabled, bool auto_trigger,
                            uint32_t activation_delay_ms,
                            uint32_t auto_start_delay_ms,
                            uint32_t auto_cooldown_ms,
                            const char* command, const char* cache_dir,
                            const char* rom_sha1) {
    g_live.enabled = enabled;
    g_live.auto_trigger = auto_trigger;
    g_live.transfer_trace = false;
    g_live.initial_cache_scan_done = false;
    g_live.activation_delay_ms = activation_delay_ms;
    g_live.auto_start_delay_ms = auto_start_delay_ms;
    g_live.auto_cooldown_ms = auto_cooldown_ms;
    g_live.configured_ms = steady_ms();
    g_live.last_compile_start_ms = 0u;
    g_live.command = command ? command : "";
    g_live.cache_dir = cache_dir ? std::filesystem::path(cache_dir)
                                 : std::filesystem::path{};
    g_live.rom_sha1 = rom_sha1 ? rom_sha1 : "";
    g_live.tier3[0] = g_live.tier3[1] = 0u;
    g_live.transfer_diag_seen = 0u;
    g_live.transfer_diag_samples = 0u;
    g_live.mismatch_rejects[0] = g_live.mismatch_rejects[1] = 0u;
    g_live.next_trigger[0] = g_live.next_trigger[1] = kFirstTriggerTier3;
    g_live.generation_pending = false;
    g_live.last_error.clear();
    g_live.run_watch = false;
    g_live.run_published = 0;
    g_live.run_queued = 0;
    g_live.run_loaded_mark = 0;
    g_live.run_rejected_mark = 0;
    g_live.futile_runs = 0;
    g_live.auto_suppressed = false;
    g_live.futility_reason.clear();
    g_live.pending_candidates = 0;
    g_live.batch_cap = kBaseBatchPages;
    g_live.persisted_backlog = false;
    g_live.queue_reloaded = false;
    g_live.last_run_duration_ms = 0;
    g_live.reject_reasons.fill(0u);
    g_live.rows_superseded = 0;
    g_live.rows_superseding = 0;
    {
        std::lock_guard<std::mutex> lock(g_live.publish_mutex);
        g_live.dormant.clear();
        g_live.dormant_activations = 0;
        g_live.dormant_parked = 0;
        g_live.dormant_requeues = 0;
        publish_dormant_state_locked();
    }
    if (g_live.enabled &&
        (g_live.cache_dir.empty() || g_live.rom_sha1.empty())) {
        g_live.enabled = false;
        g_live.last_error = "live overlay provider needs cache and ROM SHA-1";
    }
    // Read the persisted queue HERE, during configuration, and not from the
    // first poll. The read takes the shared live-index lock and can therefore
    // wait; live_overlay_poll() runs on the emulation thread, where a wait of
    // any length is a dropped frame. Configuration happens before emulation
    // starts, so the same wait costs nothing anyone can see.
    if (g_live.enabled) reload_persisted_queue();
}

void live_overlay_shutdown() {
    // Stop the maintenance worker FIRST: it is the only thread that can still
    // be spawning a compiler child, and a child born after the teardown of the
    // session it belongs to would outlive its job object.
    {
        std::lock_guard<std::mutex> lock(g_live.maint_mutex);
        g_live.maint_stop = true;
    }
    g_live.maint_cv.notify_all();
    if (g_live.maint_thread.joinable()) g_live.maint_thread.join();
    {
        std::lock_guard<std::mutex> lock(g_live.maint_mutex);
        for (MaintenanceJob& job : g_live.maint_jobs)
            coverage_manifest_release_snapshot(job.snapshot);
        g_live.maint_jobs.clear();
#if defined(_WIN32)
        // A start that completed while we were joining still owns handles.
        for (MaintenanceResult& result : g_live.maint_results) {
            if (result.child_job) {
                CloseHandle(result.child_job);
            } else if (result.child) {
                TerminateProcess(result.child, 1u);
            }
            if (result.child) CloseHandle(result.child);
        }
#elif defined(__linux__)
        for (MaintenanceResult& result : g_live.maint_results)
            terminate_and_reap(result.child);
#endif
        g_live.maint_results.clear();
        g_live.maint_results_ready.store(false, std::memory_order_release);
    }
    g_live.start_pending = false;
    g_live.run_finish_pending = false;
#if defined(_WIN32)
    if (g_live.child) {
        // The compiler publishes through an atomic rename. Closing the job
        // can therefore leave only an ignored .stage.dll, never a partially
        // loadable cache entry.
        if (g_live.child_job) {
            CloseHandle(g_live.child_job);
            g_live.child_job = nullptr;
        } else {
            TerminateProcess(g_live.child, 1u);
        }
        CloseHandle(g_live.child);
        g_live.child = nullptr;
    }
#elif defined(__linux__)
    terminate_and_reap(g_live.child);
#endif
    {
        std::lock_guard<std::mutex> lock(g_live.publish_mutex);
        g_live.prepare_stop = true;
        g_live.prepare_queue.clear();
        // Nothing may stay dormant across a teardown: the DLLs are gone and
        // the Tier-3 fast path must fall straight back to its atomic load.
        g_live.dormant.clear();
        publish_dormant_state_locked();
    }
    g_live.publish_cv.notify_all();
    if (g_live.prepare_thread.joinable()) g_live.prepare_thread.join();
    {
        std::lock_guard<std::mutex> lock(g_live.publish_mutex);
#if defined(_WIN32) || defined(__linux__)
        for (LoadedBank& bank : g_live.ready_queue) {
            close_library(bank);
        }
#endif
        g_live.ready_queue.clear();
        g_live.prepare_errors.clear();
        g_live.prepare_results_ready.store(false, std::memory_order_release);
    }
}

void live_overlay_runtime_reset() {
    for (LoadedBank& bank : g_live.loaded)
        bank.registered = false;
    // beads-yjp.41: runtime_init() clears the dispatch index and this call
    // clears the matching registration bits, so the resident cache is now
    // unregistered on both sides. The one-shot guard in live_overlay_poll()
    // is what puts it back; leaving the guard latched would leave every
    // cached shard dark until the process restarts.
    g_live.initial_cache_scan_done = false;
}

void live_overlay_register_cached_banks() {
    if (!g_live.enabled) return;
    ensure_workers();
    // Called from launch, before emulation starts, so the cache walk is
    // synchronous here on purpose: the resident shards are wanted BEFORE the
    // first frame, and nothing is losing frame time to it yet. The poll's own
    // re-scan path (after a runtime reset) hands the same walk to the worker.
    for (const QueueCandidate& candidate : scan_cache(g_live.cache_dir))
        enqueue_candidate(candidate);
    for (LoadedBank& bank : g_live.loaded)
        register_loaded_bank(bank);
    g_live.initial_cache_scan_done = true;
}

void live_overlay_set_transfer_trace(bool enabled) {
    g_live.transfer_trace = enabled;
    g_live.transfer_diag_seen = 0u;
    g_live.transfer_diag_samples = 0u;
}

void live_overlay_note_tier3(int cpu, uint32_t pc) {
    (void)pc;
    if (!live_overlay_active()) return;
    const int index = cpu == NDS_ARM7 ? 1 : 0;
    ++g_live.tier3[index];
    if (g_live.tier3[index] >= g_live.next_trigger[index]) {
        request_generation_compile();
        g_live.next_trigger[index] = g_live.tier3[index] + kRetriggerTier3;
    }
}

bool live_overlay_note_tier3_entry(int cpu, uint32_t pc) {
    // Tier-3 ENTRY, not per interpreted instruction. One relaxed atomic load
    // is the entire cost when the overlay is off or nothing is dormant.
    if (!g_dormant_any.load(std::memory_order_relaxed)) return false;
    if (!dormant_page_resident(cpu, pc)) return false;
    // Entering the interpreter here PROVES the guest code a dormant candidate
    // was compiled from is resident at this instant -- the one thing the
    // compile-finish and rescan preflight moments could not observe.
    return requeue_dormant_for(cpu, pc);
}

bool live_overlay_dormant_covers(int cpu, uint32_t pc) {
    return dormant_page_resident(cpu, pc);
}

void live_overlay_note_transfer(int cpu, uint32_t source_pc, uint32_t target,
                                uint32_t lr, uint32_t cpsr, uint32_t type) {
    // Keep this first branch cheaper than live_overlay_active(): generated
    // banks call here for every control transfer, while detailed ring entries
    // are unrelated to root discovery, compilation, or native-hit accounting.
    if (!g_live.transfer_trace) return;
    const uint64_t seen = g_live.transfer_diag_seen++;
    if (seen % kTransferDiagSampleInterval != 0u) return;
    if (!live_overlay_active()) return;
    DiagEntry& e = push_diag(DiagKind::Transfer, cpu, source_pc, target, lr,
                             cpsr, "transfer");
    e.aux0 = type;
    ++g_live.transfer_diag_samples;
}

void live_overlay_note_lookup(int cpu, uint32_t pc, uint32_t target_pc,
                              uint32_t lr, uint32_t cpsr,
                              const NdsDispatchEntry* selected,
                              const NdsDispatchEntry* inactive,
                              uint32_t candidate_count,
                              const char* outcome) {
    if (!g_live.enabled) return;
    if (selected) {
        if (LoadedBank* bank = find_loaded_bank(cpu, selected))
            ++bank->native_hits;
    } else if (inactive) {
        if (LoadedBank* bank = find_loaded_bank(cpu, inactive))
            ++bank->rejects;
        const NdsStaticValidation* validation = inactive->validation;
        // beads-yjp.53: bank_rejects alone cannot tell "the guest has not
        // written this page yet" from "the page holds a different overlay
        // generation" from "the shard's rows do not actually own this PC".
        // Those three want three different responses, so the cause is counted
        // here, on the slow path that already re-derives both halves of the
        // guard for the diagnostics ring.
        note_reject(classify_guard_reject(validation, pc,
                                          inactive->thumb != 0u));
        if (validation && validation_has_provenance(validation) &&
            !validation_identity_live(validation)) {
            if (live_overlay_active()) {
                ++g_live.mismatch_rejects[cpu == NDS_ARM7 ? 1 : 0];
                request_generation_compile();
            }
        }
    }
    if (!live_overlay_active()) return;
    DiagEntry& e = push_diag(DiagKind::Lookup, cpu, pc, target_pc, lr, cpsr,
                             outcome ? outcome : "lookup");
    e.aux0 = candidate_count;
    fill_candidate(e, cpu, selected ? selected : inactive);
}

uint32_t live_overlay_candidate_serial(int cpu, const NdsDispatchEntry* entry) {
    if (!g_live.enabled) return 0u;
    if (LoadedBank* bank = find_loaded_bank(cpu, entry))
        return bank->serial;
    return 0u;
}

void live_overlay_note_cached_hit(uint32_t serial) {
    if (!g_live.enabled || serial == 0u || serial > g_live.loaded.size())
        return;
    LoadedBank& bank = g_live.loaded[serial - 1u];
    if (bank.serial == serial) ++bank.native_hits;
}

void live_overlay_note_write(int cpu, uint32_t pc, uint32_t addr,
                             uint32_t width, uint32_t old_value,
                             uint32_t new_value) {
    if (!live_overlay_active()) return;
    constexpr uint32_t kWatchBase = 0x027E0000u;
    constexpr uint32_t kWatchEnd = 0x027E0040u;
    if (addr + width <= kWatchBase || addr >= kWatchEnd) return;
    DiagEntry& e = push_diag(DiagKind::Write, cpu, pc, addr, g_cpu.R[14],
                             g_cpu.cpsr, "write");
    e.aux0 = width;
    e.aux1 = old_value;
    e.aux2 = new_value;
}

// The real poll. Every path that wants the overlay serviced RIGHT NOW calls
// this: the countdown gate in live_overlay_poll() below, an explicit trigger,
// and the debug server. See live_overlay_poll() for why the scheduler's
// per-round call does not.
void live_overlay_poll_now() {
#if defined(_WIN32) || defined(__linux__)
    if (!g_live.enabled) return;
    // Past the enabled gate, so a session without live sharding pays nothing.
    // When it IS enabled this runs a queue reload, a bank publication, a
    // futility evaluation and a GetExitCodeProcess syscall -- measured at
    // 0.2-0.4 ms/frame in the NDS_EMU_OVERLAY bucket (MPH Kanden) back when
    // the scheduler ran this body every round.
    NdsEmuScope emu_region(NDS_EMU_OVERLAY);
    // The persisted queue was read at configure time, off the emulation
    // thread. This is only the belt-and-braces path for a caller that
    // enabled the overlay without going through live_overlay_configure; it
    // is a no-op in every normal session.
    reload_persisted_queue();
    if (!g_live.initial_cache_scan_done) {
        ensure_workers();
        // beads-yjp.59: the cache walk is a worker job, but re-registering the
        // resident banks after a runtime reset is not -- the dispatch index is
        // emulation-thread-owned and lookup reads it lock-free.
        queue_rescan_job();
        for (LoadedBank& bank : g_live.loaded) register_loaded_bank(bank);
        g_live.initial_cache_scan_done = true;
    }
    // Publication is an emulation-thread operation. The worker may prepare
    // multiple candidates, but at most one complete bank becomes visible per
    // poll so lookup can never observe a partially populated bundle.
    // The exchange claims one drain opportunity; the drain itself republishes
    // true under the queue mutex if more work remains, so a producer cannot
    // have its ready store overwritten by a consumer-side empty poll.
    if (g_live.prepare_results_ready.exchange(false,
                                              std::memory_order_acquire)) {
        commit_one_ready_bank();
    }
    if (g_live.maint_results_ready.exchange(false,
                                            std::memory_order_acquire)) {
        drain_maintenance_results();
    }
    // A finished run whose log has not been read yet has not had its shards
    // queued, so its verdict is not in yet either.
    if (!g_live.run_finish_pending) evaluate_run_futility();
    if (child_active()) {
#if defined(_WIN32)
        DWORD exit_code = STILL_ACTIVE;
        if (GetExitCodeProcess(g_live.child, &exit_code) &&
            exit_code != STILL_ACTIVE) {
            CloseHandle(g_live.child);
            g_live.child = nullptr;
            if (g_live.child_job) {
                CloseHandle(g_live.child_job);
                g_live.child_job = nullptr;
            }
            queue_run_finished(static_cast<uint32_t>(exit_code));
        }
#elif defined(__linux__)
        int status = 0;
        const pid_t waited = waitpid(g_live.child, &status, WNOHANG);
        if (waited == g_live.child) {
            g_live.child = -1;
            const uint32_t exit_code = WIFEXITED(status)
                ? static_cast<uint32_t>(WEXITSTATUS(status))
                : (WIFSIGNALED(status)
                       ? 128u + static_cast<uint32_t>(WTERMSIG(status))
                       : 1u);
            queue_run_finished(exit_code);
        } else if (waited < 0 && errno != EINTR) {
            g_live.last_error = "waitpid failed for live overlay compiler: " +
                std::string(std::strerror(errno));
            g_live.child = -1;
            queue_run_finished(1u);
        }
#endif
    }
    if (!compile_activation_elapsed()) return;
    schedule_pending_compile();
    // Never commission the next run past a start or a run-completion the
    // worker still owes an answer for: the batch cap, the cooldown and the
    // backlog all come out of that answer.
    if (!child_active() && !g_live.start_pending &&
        !g_live.run_finish_pending &&
        g_live.trigger_requests > g_live.runs_started) {
        if (!request_child_start())
            g_live.trigger_requests = g_live.runs_started;
    }
#endif
}

// The scheduler's per-round hook. A scheduler round is ~1.7 us of emulated
// time, so this is called ~600k times a second, and the body above was running
// in full on every one of them: an atomic exchange pair, a futility
// evaluation, a compile-cadence check and (with a child alive) a
// GetExitCodeProcess syscall, measured at 0.2-0.4 ms/frame in the
// NDS_EMU_OVERLAY bucket on MPH Kanden. Nothing that body does is
// round-granular -- it services a background compiler whose jobs take seconds
// -- so the round rate bought nothing and cost a measurable share of the frame.
//
// A countdown gate, not a wall-clock check: reading a clock here would put
// back a large part of the cost the gate exists to remove, and the round rate
// is stable enough that 1024 rounds is a bounded ~1.7 ms of emulated time --
// a bank becomes visible at most that late, which is invisible next to a
// compile that took seconds to produce it.
//
// The enabled early-out stays FIRST so a session without live sharding still
// pays nothing at all, and the gated-out call must not open the
// NDS_EMU_OVERLAY region: two rdtsc reads per round to measure a countdown is
// the exact bias emu_profile.h warns about, and the bucket would then report
// instrumentation rather than work.
//
// Modulo-of-a-counter rather than a decrement so the FIRST call after start-up
// fires: the one-shot cache re-registration and the persisted-queue reload
// live in that body, and there is no reason to delay them (same shape as
// NdsEmuRound's sampler).
void live_overlay_poll() {
    if (!g_live.enabled) return;
    static uint64_t poll_counter = 0;
    constexpr uint64_t kPollInterval = 1024u;
    if ((poll_counter++ % kPollInterval) != 0u) return;
    live_overlay_poll_now();
}

bool live_overlay_trigger_now() {
    if (!g_live.enabled) return false;
    if (!activation_delay_elapsed()) return false;
    // An explicit request is a human (or a test) saying "try anyway" -- most
    // likely because the provider was just replaced. Lift the futility
    // suppression so the next failure is judged on its own evidence.
    if (g_live.auto_suppressed) {
        g_live.auto_suppressed = false;
        std::fprintf(stderr,
            "[live-overlay] explicit trigger lifts futility suppression "
            "(previous cause: %s)\n",
            g_live.futility_reason.empty() ? "unrecorded"
                                           : g_live.futility_reason.c_str());
    }
    ++g_live.trigger_requests;
    // Straight to the body, never through the countdown gate: an explicit
    // trigger means "now", and landing on a gated-out call would defer the
    // commissioned run by up to 1024 rounds for no reason.
    live_overlay_poll_now();
    return true;
}

void live_overlay_note_backlog_for_test(uint64_t pending,
                                        uint64_t run_duration_ms) {
    g_live.pending_candidates = pending;
    if (pending == 0u) g_live.persisted_backlog = false;
    update_batch_ramp(pending, run_duration_ms);
    // Mirror the real drain path, which asks for another run while work
    // remains. Without this a test could assert "no child was started" for
    // the trivial reason that nothing had ever been requested.
    if (pending != 0u) request_generation_compile();
}

uint32_t live_overlay_batch_cap_for_test() { return effective_batch_cap(); }

uint32_t live_overlay_cooldown_for_test() { return effective_cooldown_ms(); }

void live_overlay_poll_drain_counts_for_test(
    uint64_t* prepare_result_drains,
    uint64_t* maintenance_result_drains) {
    if (prepare_result_drains)
        *prepare_result_drains = g_live.prepare_result_drains_for_test;
    if (maintenance_result_drains)
        *maintenance_result_drains = g_live.maint_result_drains_for_test;
}

void live_overlay_suppress_for_test(const char* reason) {
    g_live.auto_suppressed = true;
    g_live.futility_reason = reason ? reason : "test";
}

bool live_overlay_suppressed_for_test() { return g_live.auto_suppressed; }

uint64_t live_overlay_runs_started_for_test() { return g_live.runs_started; }

bool live_overlay_commit_bank_for_test(int cpu, const char* bank_id,
                                       const char* candidate_id,
                                       const char* generation_id,
                                       unsigned backend_tier,
                                       const NdsDispatchEntry* dispatch,
                                       unsigned dispatch_len) {
    LoadedBank bank{};
    bank.bank_id = bank_id ? bank_id : "";
    bank.candidate_id = candidate_id ? candidate_id : "";
    bank.generation_id = generation_id && *generation_id
        ? generation_id : bank.candidate_id;
    bank.backend_tier = backend_tier;
    bank.cpu = cpu;
    bank.exc_base = cpu == NDS_ARM7 ? 0x00000000u : 0xFFFF0000u;
    bank.dispatch = dispatch;
    bank.dispatch_len = dispatch_len;
    // Distinct per candidate so the same-candidate-identity short circuit in
    // commit_prepared_bank does not absorb a tie-break case.
    bank.content_identity = hash_bytes(1469598103934665603ull,
                                       bank.candidate_id.data(),
                                       bank.candidate_id.size());
    return commit_prepared_bank(std::move(bank));
}

bool live_overlay_admit_bank_for_test(int cpu, const char* bank_id,
                                      const char* candidate_id,
                                      const char* generation_id,
                                      unsigned backend_tier,
                                      const char* path_text,
                                      const NdsDispatchEntry* dispatch,
                                      unsigned dispatch_len) {
    LoadedBank bank{};
    bank.bank_id = bank_id ? bank_id : "";
    bank.candidate_id = candidate_id ? candidate_id : "";
    bank.generation_id = generation_id && *generation_id
        ? generation_id : bank.candidate_id;
    bank.backend_tier = backend_tier;
    bank.cpu = cpu;
    bank.exc_base = cpu == NDS_ARM7 ? 0x00000000u : 0xFFFF0000u;
    bank.dispatch = dispatch;
    bank.dispatch_len = dispatch_len;
    bank.path = path_text ? path_text : "";
    bank.content_identity = hash_bytes(1469598103934665603ull,
                                       bank.candidate_id.data(),
                                       bank.candidate_id.size());
    const uint64_t loaded_before = g_live.banks_loaded;
    const std::string key = path_key(std::filesystem::path(bank.path));
    admit_prepared_bank(std::move(bank), key);
    return g_live.banks_loaded != loaded_before;
}

bool live_overlay_enqueue_path_for_test(const char* path_text) {
    // The REAL queueing path, not a reimplementation: queued_paths is what
    // decides whether a rescanned or republished DLL can ever reach the
    // prepare worker again, so "parking released the key" is only observable
    // by asking enqueue_candidate itself.
    QueueCandidate candidate;
    candidate.key = path_key(std::filesystem::path(
        path_text ? path_text : ""));
    candidate.path = std::filesystem::path(path_text ? path_text : "");
    return enqueue_candidate(candidate);
}

bool live_overlay_generation_registered_for_test(const char* generation_id,
                                                 unsigned* backend_tier_out) {
    const std::string wanted = generation_id ? generation_id : "";
    for (const LoadedBank& bank : g_live.loaded) {
        if (!bank.registered || bank.superseded) continue;
        if (bank.generation_id != wanted) continue;
        if (backend_tier_out) *backend_tier_out = bank.backend_tier;
        return true;
    }
    return false;
}

void live_overlay_summary(NdsLiveOverlaySummary* out) {
    if (!out) return;
    *out = NdsLiveOverlaySummary{};
    out->enabled = g_live.enabled;
    out->active = live_overlay_active();
    out->banks_loaded = g_live.banks_loaded;
    out->banks_rejected = g_live.banks_rejected;
    out->tier3[0] = g_live.tier3[0];
    out->tier3[1] = g_live.tier3[1];
    out->mismatch_rejects[0] = g_live.mismatch_rejects[0];
    out->mismatch_rejects[1] = g_live.mismatch_rejects[1];
    out->futile_runs = g_live.futile_runs;
    out->auto_suppressed = g_live.auto_suppressed;
    out->pending_candidates = g_live.pending_candidates;
    out->batch_cap = effective_batch_cap();
    out->cooldown_ms = effective_cooldown_ms();
    out->persisted_backlog = g_live.persisted_backlog;
    out->runs_started = g_live.runs_started;
    out->reason_count = kRejectReasonCount;
    out->reason_names = kRejectReasonNames;
    for (uint32_t i = 0u; i < kRejectReasonCount; ++i)
        out->reason_counts[i] = g_live.reject_reasons[i];
    out->rows_superseded = g_live.rows_superseded;
    out->rows_superseding = g_live.rows_superseding;
    out->dormant_activations = g_live.dormant_activations;
    out->dormant_parked = g_live.dormant_parked;
    out->dormant_requeues = g_live.dormant_requeues;
    out->busy = child_active();
    // g_live.loaded is mutated under publish_mutex when a prepared shard is
    // adopted, so walk it under the same lock the status JSON uses.
    std::lock_guard<std::mutex> lock(g_live.publish_mutex);
    out->dormant_candidates = g_live.dormant.size();
    for (const LoadedBank& bank : g_live.loaded) {
        out->native_hits += bank.native_hits;
        out->bank_rejects += bank.rejects;
        if (!bank.registered || bank.superseded) continue;
        ++out->registered_banks;
        if (bank.backend_tier > out->backend_tier)
            out->backend_tier = bank.backend_tier;
    }
}

std::string live_overlay_status_json() {
    std::size_t preparing = 0u;
    std::size_t ready = 0u;
    std::size_t dormant = 0u;
    {
        std::lock_guard<std::mutex> lock(g_live.publish_mutex);
        preparing = g_live.prepare_queue.size();
        ready = g_live.ready_queue.size();
        dormant = g_live.dormant.size();
    }
    std::ostringstream out;
    out << "{\"enabled\":" << (g_live.enabled ? "true" : "false")
        << ",\"active\":" << (live_overlay_active() ? "true" : "false")
        << ",\"auto_trigger\":" << (g_live.auto_trigger ? "true" : "false")
        << ",\"activation_delay_ms\":" << g_live.activation_delay_ms
        << ",\"initial_cache_scan_done\":"
        << (g_live.initial_cache_scan_done ? "true" : "false")
        << ",\"auto_start_delay_ms\":" << g_live.auto_start_delay_ms
        << ",\"auto_cooldown_ms\":" << g_live.auto_cooldown_ms
        << ",\"pending_candidates\":" << g_live.pending_candidates
        << ",\"batch_cap\":" << effective_batch_cap()
        << ",\"cooldown_ms\":" << effective_cooldown_ms()
        << ",\"persisted_backlog\":"
        << (g_live.persisted_backlog ? "true" : "false")
        << ",\"last_run_ms\":" << g_live.last_run_duration_ms
        << ",\"busy\":";
    out << (child_active() ? "true" : "false");
    out << ",\"transfer_trace\":"
        << (g_live.transfer_trace ? "true" : "false")
        << ",\"transfer_diag_sample_interval\":"
        << kTransferDiagSampleInterval
        << ",\"transfer_diag_seen\":" << g_live.transfer_diag_seen
        << ",\"transfer_diag_samples\":" << g_live.transfer_diag_samples
        << ",\"tier3_arm9\":" << g_live.tier3[0]
        << ",\"tier3_arm7\":" << g_live.tier3[1]
        << ",\"mismatch_rejects_arm9\":" << g_live.mismatch_rejects[0]
        << ",\"mismatch_rejects_arm7\":" << g_live.mismatch_rejects[1]
        << ",\"generation_pending\":"
        << (g_live.generation_pending ? "true" : "false")
        << ",\"preparing_banks\":" << preparing
        << ",\"ready_banks\":" << ready
        << ",\"trigger_requests\":" << g_live.trigger_requests
        << ",\"runs_started\":" << g_live.runs_started
        << ",\"runs_finished\":" << g_live.runs_finished
        << ",\"runs_failed\":" << g_live.runs_failed
        << ",\"banks_loaded\":" << g_live.banks_loaded
        << ",\"banks_rejected\":" << g_live.banks_rejected
        << ",\"rows_superseded\":" << g_live.rows_superseded
        << ",\"rows_superseding\":" << g_live.rows_superseding
        // beads-yjp.62: compiled output we hold but could not activate yet,
        // how much of it the Tier-3 entry proof has since woken, and how much
        // was given up on. dormant_candidates high with dormant_activations
        // at zero is the field failure this was built to make visible.
        << ",\"dormant_candidates\":" << dormant
        << ",\"dormant_activations\":" << g_live.dormant_activations
        << ",\"dormant_parked\":" << g_live.dormant_parked
        << ",\"dormant_requeues\":" << g_live.dormant_requeues
        << ",\"reject_reasons\":{";
    for (uint32_t i = 0u; i < kRejectReasonCount; ++i) {
        out << (i ? "," : "") << "\"" << kRejectReasonNames[i] << "\":"
            << g_live.reject_reasons[i];
    }
    out << "}"
        << ",\"live_bank_abi\":" << NDS_LIVE_BANK_ABI_VERSION
        << ",\"futile_runs\":" << g_live.futile_runs
        << ",\"auto_suppressed\":"
        << (g_live.auto_suppressed ? "true" : "false")
        << ",\"futility_reason\":\""
        << json_escape(g_live.futility_reason) << "\""
        << ",\"loaded\":[";
    for (std::size_t i = 0; i < g_live.loaded.size(); ++i) {
        const LoadedBank& bank = g_live.loaded[i];
        out << (i ? "," : "") << "{\"bank_id\":\""
            << json_escape(bank.bank_id) << "\",\"cpu\":"
            << (bank.cpu == NDS_ARM7 ? 7 : 9) << ",\"rows\":"
            << bank.dispatch_len << ",\"serial\":"
            << bank.serial << ",\"candidate_id\":\""
            << json_escape(bank.candidate_id) << "\",\"generation\":"
            << bank.generation << ",\"generation_id\":\""
            << json_escape(bank.generation_id) << "\",\"native_hits\":"
            << bank.native_hits << ",\"rejects\":"
            << bank.rejects << ",\"registered\":"
            << (bank.registered ? "true" : "false")
            << ",\"superseded\":"
            << (bank.superseded ? "true" : "false") << "}";
    }
    out << "],\"cache\":\"" << json_escape(path_string(g_live.cache_dir))
        << "\",\"manifest\":\"" << json_escape(g_live.manifest_path)
        << "\",\"log\":\"" << json_escape(g_live.log_path)
        << "\",\"last_error\":\"" << json_escape(g_live.last_error)
        << "\"}";
    return out.str();
}

std::string live_overlay_diagnostics_json(uint32_t max_entries) {
    if (max_entries == 0u || max_entries > g_live.diag_count)
        max_entries = g_live.diag_count;
    std::ostringstream out;
    out << "{\"count\":" << g_live.diag_count
        << ",\"latest_seq\":" << g_live.diag_seq
        << ",\"entries\":[";
    const uint32_t start =
        (g_live.diag_w + kDiagRingSize - max_entries) % kDiagRingSize;
    for (uint32_t i = 0; i < max_entries; ++i) {
        const DiagEntry& e = g_live.diag[(start + i) % kDiagRingSize];
        out << (i ? "," : "") << "{\"seq\":" << e.seq
            << ",\"cycles\":" << e.cycles
            << ",\"kind\":\"" << diag_kind_name(e.kind) << "\""
            << ",\"cpu\":" << (e.cpu == NDS_ARM7 ? 7 : 9)
            << ",\"pc\":" << e.pc
            << ",\"target\":" << e.target
            << ",\"lr\":" << e.lr
            << ",\"cpsr\":" << e.cpsr
            << ",\"aux0\":" << e.aux0
            << ",\"aux1\":" << e.aux1
            << ",\"aux2\":" << e.aux2
            << ",\"aux3\":" << e.aux3
            << ",\"row_addr\":" << e.row_addr
            << ",\"thumb\":" << unsigned(e.thumb)
            << ",\"validation_addr\":" << e.validation_addr
            << ",\"validation_size\":" << e.validation_size
            << ",\"has_validation\":" << unsigned(e.has_validation)
            << ",\"provenance_ok\":" << unsigned(e.provenance_ok)
            << ",\"bytes_match\":" << unsigned(e.bytes_match)
            << ",\"outcome\":\"" << json_escape(e.outcome) << "\""
            << ",\"bank_id\":\"" << json_escape(e.bank_id) << "\""
            << ",\"candidate_id\":\"" << json_escape(e.candidate_id)
            << "\"}";
    }
    out << "]}";
    return out.str();
}
