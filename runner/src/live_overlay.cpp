#include "live_overlay.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
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
#include "runtime_arm.h"
#include "state.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

constexpr uint64_t kFirstTriggerTier3 = 100000u;
constexpr uint64_t kRetriggerTier3 = 250000u;
constexpr uint64_t kNoProgressRetriggerTier3 = 5000000u;
constexpr uint32_t kDiagRingSize = 4096u;

enum class DiagKind : uint8_t {
    Transfer,
    Lookup,
    Write,
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
    bool registered = false;
    bool superseded = false;
#if defined(_WIN32)
    HMODULE handle = nullptr;
#endif
};

struct State {
    bool enabled = false;
    bool auto_trigger = false;
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
    uint64_t mismatch_rejects[2] = {};
    uint64_t next_trigger[2] = {kFirstTriggerTier3, kFirstTriggerTier3};
    bool generation_pending = false;
    uint64_t trigger_requests = 0;
    uint64_t runs_started = 0;
    uint64_t runs_finished = 0;
    uint64_t runs_failed = 0;
    uint64_t banks_loaded = 0;
    uint64_t banks_rejected = 0;
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
    std::deque<std::string> prepare_errors;
    std::unordered_set<std::string> queued_paths;
    std::thread prepare_thread;
    bool prepare_stop = false;
#if defined(_WIN32)
    HANDLE child = nullptr;
    HANDLE child_job = nullptr;
#endif
};

State g_live;

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

bool path_under_cache(const std::filesystem::path& path) {
    if (g_live.cache_dir.empty()) return false;
    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(g_live.cache_dir, ec);
    if (ec) return false;
    const auto child = std::filesystem::weakly_canonical(path, ec);
    if (ec) return false;
    const std::string root_s = lower_ascii(path_string(root));
    const std::string child_s = lower_ascii(path_string(child));
    return child_s == root_s ||
        (child_s.size() > root_s.size() &&
         child_s.compare(0, root_s.size(), root_s) == 0 &&
         (root_s.empty() || root_s.back() == '/' ||
          child_s[root_s.size()] == '/'));
}

bool already_loaded(const std::filesystem::path& path) {
    std::error_code ec;
    const std::string canon =
        path_string(std::filesystem::weakly_canonical(path, ec));
    if (ec) return false;
    return std::any_of(g_live.loaded.begin(), g_live.loaded.end(),
                       [&](const LoadedBank& b) {
                           return lower_ascii(b.path) == lower_ascii(canon);
                       });
}

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

bool preflight_live_bank(const NdsLiveBankInfo& info,
                         std::string& error) {
    constexpr uint32_t kKnownFlags =
        NDS_LIVE_BANK_FLAG_DEPENDENCY_CLOSURE;
    if (info.flags & ~kKnownFlags) {
        error = "live bank has unknown safety flags";
        return false;
    }
    if (!info.bank_id || !*info.bank_id) {
        error = "live bank missing bank_id";
        return false;
    }
    if (!info.candidate_id || !*info.candidate_id) {
        error = "live bank missing candidate_id";
        return false;
    }
    if (!info.dispatch || info.dispatch_len == 0u) {
        error = "live bank has no dispatch rows";
        return false;
    }
    if (info.dispatch_len > (1u << 20u)) {
        error = "live bank dispatch table exceeds the safety limit";
        return false;
    }
    const bool closure =
        (info.flags & NDS_LIVE_BANK_FLAG_DEPENDENCY_CLOSURE) != 0u;
    const NdsStaticValidationRange* closure_ranges = nullptr;
    uint32_t closure_count = 0u;
    for (unsigned i = 0; i < info.dispatch_len; ++i) {
        const NdsDispatchEntry& row = info.dispatch[i];
        if (!row.fn) {
            error = "live bank dispatch row has null function";
            return false;
        }
        if (!validation_owns_row(row.validation, row)) {
            error = "live bank dispatch row is not owned by its validation";
            return false;
        }
        if (!validation_dependencies_sane(row.validation, closure)) {
            error = closure
                ? "live bank has an incomplete dependency closure"
                : "live bank has unflagged dependency ranges";
            return false;
        }
        if (closure) {
            if (!closure_ranges) {
                closure_ranges = row.validation->dependencies;
                closure_count = row.validation->dependency_count;
            } else if (closure_ranges != row.validation->dependencies ||
                       closure_count != row.validation->dependency_count) {
                error = "live bank rows do not share one atomic dependency closure";
                return false;
            }
        }
        if (i == 0u) continue;
        const NdsDispatchEntry& prev = info.dispatch[i - 1u];
        if (prev.addr > row.addr ||
            (prev.addr == row.addr && prev.thumb > row.thumb)) {
            error = "live bank dispatch rows are not sorted";
            return false;
        }
        if (prev.addr == row.addr && prev.thumb == row.thumb) {
            error = "live bank has duplicate dispatch rows inside one candidate";
            return false;
        }
    }
    return true;
}

bool validate_live_bank_info(const NdsLiveBankInfo& info,
                             const std::string& expected_rom_sha1,
                             std::string& error) {
    if (info.abi_version != NDS_LIVE_BANK_ABI_VERSION) {
        error = "live bank ABI version mismatch";
        return false;
    }
    if (!info.title_sha1 || expected_rom_sha1 != info.title_sha1) {
        error = "live bank ROM identity mismatch";
        return false;
    }
    if (info.cpu != NDS_ARM9 && info.cpu != NDS_ARM7) {
        error = "live bank CPU identity is invalid";
        return false;
    }
    if (info.linked_g_cpu != &g_cpu ||
        info.linked_busf_main != &g_busf_main ||
        info.linked_busf_itcm != &g_busf_itcm ||
        info.linked_runtime_cycles != &g_runtime_cycles) {
        error = "live bank data imports are not bound to runner storage";
        return false;
    }
    return preflight_live_bank(info, error);
}

bool activation_delay_elapsed() {
    return !g_live.activation_delay_ms ||
        steady_ms() - g_live.configured_ms >= g_live.activation_delay_ms;
}

bool live_overlay_active() {
    return g_live.enabled && activation_delay_elapsed();
}

bool compile_delay_elapsed() {
    const uint64_t now = steady_ms();
    return (!g_live.auto_start_delay_ms ||
            now - g_live.configured_ms >= g_live.auto_start_delay_ms) &&
           (!g_live.last_compile_start_ms || !g_live.auto_cooldown_ms ||
            now - g_live.last_compile_start_ms >= g_live.auto_cooldown_ms);
}

void request_generation_compile() {
    if (!g_live.auto_trigger) return;
    g_live.generation_pending = true;
}

void schedule_pending_compile() {
    if (!g_live.generation_pending || !compile_delay_elapsed()) return;
    if (g_live.trigger_requests > g_live.runs_started) return;
    g_live.generation_pending = false;
    ++g_live.trigger_requests;
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

bool is_final_dll_path(const std::filesystem::path& path) {
    const std::string filename = lower_ascii(path.filename().string());
    constexpr const char* kStageSuffix = ".stage.dll";
    if (filename.size() >= std::strlen(kStageSuffix) &&
        filename.compare(filename.size() - std::strlen(kStageSuffix),
                         std::strlen(kStageSuffix), kStageSuffix) == 0) {
        return false;
    }
    return lower_ascii(path.extension().string()) == ".dll";
}

bool prepare_bank_dll(const std::filesystem::path& path, LoadedBank& bank,
                      std::string& error) {
#if defined(_WIN32)
    if (!path_under_cache(path)) {
        error = "published DLL outside live overlay cache: " +
            path_string(path);
        return false;
    }
    if (!is_final_dll_path(path)) {
        error = "live bank is not an atomically published DLL: " +
            path_string(path);
        return false;
    }

    std::error_code ec;
    const auto canon_path = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        error = "cannot canonicalize published DLL: " +
            path_string(path);
        return false;
    }
    const std::string canon = path_string(canon_path);
    HMODULE handle = LoadLibraryA(canon.c_str());
    if (!handle) {
        error = "LoadLibrary failed: " + canon;
        return false;
    }

    using InfoFn = const NdsLiveBankInfo* (*)();
    FARPROC proc = GetProcAddress(handle, "nds_live_bank_info");
    InfoFn info_fn = nullptr;
    static_assert(sizeof(info_fn) == sizeof(proc));
    std::memcpy(&info_fn, &proc, sizeof(info_fn));
    const NdsLiveBankInfo* info = info_fn ? info_fn() : nullptr;
    if (!info) {
        FreeLibrary(handle);
        error = "live DLL does not export bank metadata: " + canon;
        return false;
    }
    std::string preflight_error;
    if (!validate_live_bank_info(*info, g_live.rom_sha1, preflight_error)) {
        FreeLibrary(handle);
        error = preflight_error + ": " + canon;
        return false;
    }

    bank = {};
    bank.path = canon;
    bank.bank_id = info->bank_id ? info->bank_id : "";
    bank.candidate_id = info->candidate_id ? info->candidate_id : "";
    using GenerationFn = const char* (*)();
    FARPROC generation_proc =
        GetProcAddress(handle, "nds_live_generation_id");
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
    bank.handle = handle;
    return true;
#else
    (void)path;
    (void)bank;
    error = "live overlay loading is implemented for Windows only";
    return false;
#endif
}

bool same_candidate_key(const LoadedBank& a, const LoadedBank& b) {
    return a.cpu == b.cpu && a.bank_id == b.bank_id &&
        a.candidate_id == b.candidate_id;
}

bool commit_prepared_bank(LoadedBank bank) {
#if defined(_WIN32)
    for (const LoadedBank& loaded : g_live.loaded) {
        if (!same_candidate_key(loaded, bank)) continue;
        if (loaded.content_identity != bank.content_identity) {
            g_live.last_error =
                "conflicting live bank content for candidate identity " +
                bank.bank_id + "/" + bank.candidate_id;
            ++g_live.banks_rejected;
        }
        if (bank.handle) FreeLibrary(bank.handle);
        return loaded.content_identity == bank.content_identity;
    }
    // A compiler pass may discover additional resume roots for a generation
    // already cached at this page. Replace that revision at a scheduler
    // boundary; do not leave two differently owned bodies both eligible for
    // the same exact generation. Other generation IDs remain chained.
    for (LoadedBank& loaded : g_live.loaded) {
        if (!loaded.registered || loaded.cpu != bank.cpu ||
            loaded.bank_id != bank.bank_id ||
            loaded.generation_id != bank.generation_id) {
            continue;
        }
        nds_unregister_dispatch(loaded.cpu, loaded.dispatch,
                                loaded.dispatch_len);
        loaded.registered = false;
        loaded.superseded = true;
        loaded.dispatch = nullptr;
        if (loaded.handle) {
            FreeLibrary(loaded.handle);
            loaded.handle = nullptr;
        }
    }
    bank.serial = g_live.next_bank_serial++;
    bank.generation = static_cast<uint32_t>(++g_live.publication_generation);
    g_live.loaded.push_back(std::move(bank));
    register_loaded_bank(g_live.loaded.back());
    ++g_live.banks_loaded;
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
        }
        LoadedBank bank{};
        std::string error;
        const bool ok = prepare_bank_dll(path, bank, error);
        {
            std::lock_guard<std::mutex> lock(g_live.publish_mutex);
            if (ok) g_live.ready_queue.push_back(std::move(bank));
            else    g_live.prepare_errors.push_back(std::move(error));
        }
    }
}

void ensure_prepare_worker() {
    if (g_live.prepare_thread.joinable()) return;
    g_live.prepare_stop = false;
    g_live.prepare_thread = std::thread(prepare_worker_main);
}

bool queue_bank_dll(const std::filesystem::path& path) {
    if (!is_final_dll_path(path) || !path_under_cache(path) ||
        already_loaded(path)) {
        return false;
    }
    std::error_code ec;
    const auto canon_path = std::filesystem::weakly_canonical(path, ec);
    if (ec) return false;
    const std::string key = lower_ascii(path_string(canon_path));
    {
        std::lock_guard<std::mutex> lock(g_live.publish_mutex);
        if (!g_live.queued_paths.insert(key).second) return false;
        g_live.prepare_queue.push_back(canon_path);
    }
    ensure_prepare_worker();
    g_live.publish_cv.notify_one();
    return true;
}

void commit_one_ready_bank() {
    LoadedBank bank{};
    std::string error;
    bool have_bank = false;
    {
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
    }
    if (!error.empty()) {
        g_live.last_error = std::move(error);
        ++g_live.banks_rejected;
    }
    if (have_bank) commit_prepared_bank(std::move(bank));
}

struct CachedDllPath {
    std::filesystem::path path;
    std::filesystem::file_time_type write_time{};
};

void rescan_cache() {
    if (!g_live.enabled || g_live.cache_dir.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(g_live.cache_dir, ec)) return;
    std::vector<CachedDllPath> paths;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(g_live.cache_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const auto path = entry.path();
        if (!is_final_dll_path(path)) continue;
        const auto write_time = entry.last_write_time(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        paths.push_back({path, write_time});
    }
    std::sort(paths.begin(), paths.end(), [](const CachedDllPath& a,
                                             const CachedDllPath& b) {
        if (a.write_time != b.write_time) return a.write_time < b.write_time;
        return lower_ascii(path_string(a.path)) <
            lower_ascii(path_string(b.path));
    });
    for (const CachedDllPath& item : paths) queue_bank_dll(item.path);
}

std::vector<std::filesystem::path> published_paths_from_log() {
    std::vector<std::filesystem::path> out;
    std::ifstream f(g_live.log_path);
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

bool write_snapshot_manifest() {
    std::error_code ec;
    std::filesystem::create_directories(g_live.cache_dir / "snapshots", ec);
    if (ec) {
        g_live.last_error = "cannot create live overlay snapshot directory";
        return false;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "manifest-%06llu.json",
                  static_cast<unsigned long long>(g_live.runs_started + 1u));
    const auto path = g_live.cache_dir / "snapshots" / name;
    char error[256] = {};
    if (!coverage_manifest_write_live_snapshot(
            path_string(path).c_str(), 64u, error, sizeof(error))) {
        g_live.last_error = error;
        return false;
    }
    g_live.manifest_path = path_string(path);
    return true;
}

bool start_child() {
#if defined(_WIN32)
    if (g_live.child || g_live.command.empty()) return false;
    if (!write_snapshot_manifest()) {
        ++g_live.runs_failed;
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(g_live.cache_dir / "logs", ec);
    if (ec) {
        g_live.last_error = "cannot create live overlay log directory";
        ++g_live.runs_failed;
        return false;
    }
    char log_name[64];
    std::snprintf(log_name, sizeof(log_name), "compile-%06llu.log",
                  static_cast<unsigned long long>(g_live.runs_started + 1u));
    const auto log_path = g_live.cache_dir / "logs" / log_name;
    g_live.log_path = path_string(log_path);

    SECURITY_ATTRIBUTES sa{sizeof(sa), NULL, TRUE};
    HANDLE log = CreateFileA(g_live.log_path.c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE) {
        g_live.last_error = "cannot open live overlay compiler log";
        ++g_live.runs_failed;
        return false;
    }

    SetEnvironmentVariableA("NDS_LIVE_OVERLAY_MANIFEST",
                            g_live.manifest_path.c_str());
    const std::string cache = path_string(g_live.cache_dir);
    SetEnvironmentVariableA("NDS_LIVE_OVERLAY_CACHE", cache.c_str());
    SetEnvironmentVariableA("NDS_LIVE_OVERLAY_ROM_SHA1",
                            g_live.rom_sha1.c_str());

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = log;
    si.hStdError = log;
    PROCESS_INFORMATION pi{};
    std::string full = "cmd.exe /C \"" + g_live.command + "\"";
    std::vector<char> cmd(full.begin(), full.end());
    cmd.push_back('\0');

    HANDLE job = CreateJobObjectA(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                     &jeli, sizeof(jeli))) {
            CloseHandle(job);
            job = NULL;
        }
    }

    BOOL ok = CreateProcessA(NULL, cmd.data(), NULL, NULL, TRUE,
                             CREATE_NO_WINDOW | IDLE_PRIORITY_CLASS |
                                 CREATE_SUSPENDED,
                             NULL, NULL, &si, &pi);
    CloseHandle(log);
    if (!ok) {
        if (job) CloseHandle(job);
        g_live.last_error = "CreateProcess failed for live overlay compiler";
        ++g_live.runs_failed;
        return false;
    }
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        CloseHandle(job);
        job = NULL;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_live.child = pi.hProcess;
    g_live.child_job = job;
    ++g_live.runs_started;
    g_live.last_compile_start_ms = steady_ms();
    std::fprintf(stderr, "[live-overlay] compiling from %s\n",
                 g_live.manifest_path.c_str());
    return true;
#else
    g_live.last_error = "live overlay compilation is implemented for Windows only";
    ++g_live.runs_failed;
    return false;
#endif
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

void live_overlay_configure(bool enabled, bool auto_trigger,
                            uint32_t activation_delay_ms,
                            uint32_t auto_start_delay_ms,
                            uint32_t auto_cooldown_ms,
                            const char* command, const char* cache_dir,
                            const char* rom_sha1) {
    g_live.enabled = enabled;
    g_live.auto_trigger = auto_trigger;
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
    g_live.mismatch_rejects[0] = g_live.mismatch_rejects[1] = 0u;
    g_live.next_trigger[0] = g_live.next_trigger[1] = kFirstTriggerTier3;
    g_live.generation_pending = false;
    g_live.last_error.clear();
    if (g_live.enabled &&
        (g_live.cache_dir.empty() || g_live.rom_sha1.empty())) {
        g_live.enabled = false;
        g_live.last_error = "live overlay provider needs cache and ROM SHA-1";
    }
}

void live_overlay_shutdown() {
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
#endif
    {
        std::lock_guard<std::mutex> lock(g_live.publish_mutex);
        g_live.prepare_stop = true;
        g_live.prepare_queue.clear();
    }
    g_live.publish_cv.notify_all();
    if (g_live.prepare_thread.joinable()) g_live.prepare_thread.join();
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(g_live.publish_mutex);
    for (LoadedBank& bank : g_live.ready_queue) {
        if (bank.handle) FreeLibrary(bank.handle);
    }
#endif
    g_live.ready_queue.clear();
}

void live_overlay_runtime_reset() {
    for (LoadedBank& bank : g_live.loaded)
        bank.registered = false;
}

void live_overlay_register_cached_banks() {
    if (!g_live.enabled) return;
    rescan_cache();
    for (LoadedBank& bank : g_live.loaded)
        register_loaded_bank(bank);
    g_live.initial_cache_scan_done = true;
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

void live_overlay_note_transfer(int cpu, uint32_t source_pc, uint32_t target,
                                uint32_t lr, uint32_t cpsr, uint32_t type) {
    if (!live_overlay_active()) return;
    DiagEntry& e = push_diag(DiagKind::Transfer, cpu, source_pc, target, lr,
                             cpsr, "transfer");
    e.aux0 = type;
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

void live_overlay_poll() {
#if defined(_WIN32)
    if (!g_live.enabled) return;
    if (!g_live.initial_cache_scan_done)
        live_overlay_register_cached_banks();
    // Publication is an emulation-thread operation. The worker may prepare
    // multiple candidates, but at most one complete bank becomes visible per
    // poll so lookup can never observe a partially populated bundle.
    commit_one_ready_bank();
    if (g_live.child) {
        DWORD exit_code = STILL_ACTIVE;
        if (GetExitCodeProcess(g_live.child, &exit_code) &&
            exit_code != STILL_ACTIVE) {
            CloseHandle(g_live.child);
            g_live.child = nullptr;
            if (g_live.child_job) {
                CloseHandle(g_live.child_job);
                g_live.child_job = nullptr;
            }
            ++g_live.runs_finished;
            if (exit_code != 0) {
                ++g_live.runs_failed;
                g_live.last_error = "live overlay compiler exited with code " +
                    std::to_string(exit_code);
            }
            const auto published = published_paths_from_log();
            for (const auto& path : published)
                queue_bank_dll(path);
            if (exit_code == 0 && published.empty()) {
                for (int cpu = 0; cpu < 2; ++cpu) {
                    g_live.next_trigger[cpu] = std::max(
                        g_live.next_trigger[cpu],
                        g_live.tier3[cpu] + kNoProgressRetriggerTier3);
                }
            }
            rescan_cache();
        }
    }
    if (!activation_delay_elapsed()) return;
    schedule_pending_compile();
    if (!g_live.child && g_live.trigger_requests > g_live.runs_started) {
        if (!start_child())
            g_live.trigger_requests = g_live.runs_started;
    }
#endif
}

bool live_overlay_trigger_now() {
    if (!g_live.enabled) return false;
    if (!activation_delay_elapsed()) return false;
    ++g_live.trigger_requests;
    live_overlay_poll();
    return true;
}

std::string live_overlay_status_json() {
    std::size_t preparing = 0u;
    std::size_t ready = 0u;
    {
        std::lock_guard<std::mutex> lock(g_live.publish_mutex);
        preparing = g_live.prepare_queue.size();
        ready = g_live.ready_queue.size();
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
        << ",\"busy\":";
#if defined(_WIN32)
    out << (g_live.child ? "true" : "false");
#else
    out << "false";
#endif
    out << ",\"tier3_arm9\":" << g_live.tier3[0]
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
