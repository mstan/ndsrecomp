// runtime_arm.cpp — DS runner implementation of the recomp C ABI.
//
// The condition-code / shifter / flag-updater / PSR-banking / call-return
// helpers are reused verbatim from the (verified) single-CPU runtime in
// recompiler/armv4t/runtime_arm.cpp — they are CPU-architecture-agnostic.
// What is DS-specific and rewritten here: per-CPU dispatch registration,
// the exception-vector base (ARM9 high vectors vs ARM7 low), a simple
// always-on trace ring, the cooperative yield/halt, and lifecycle.

#include "runtime_arm.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#if defined(NDS_PROFILE_HLE_HEAT)
#include <chrono>
#endif
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>
#if !defined(NDEBUG)
#include <thread>
#endif

#include "state.h"
#include "savestate.h"
#include "io.h"
#include "tier3.h"
#include "diagnostics.h"
#include "dispatch_lookup.h"
#include "hle_profile.h"
#include "dispatch_stats.h"
#include "dispatch_timing.h"
#include "coverage_manifest.h"
#include "live_overlay.h"
#include "pc_profile.h"

// ── Dispatch-composition counters (always on; see dispatch_stats.h) ─────
NdsDispatchStats g_nds_dispatch_stats[2] = {};

// Deadline-bounded machinery selector state (beads-yjp.42). Defined here so
// the stats JSON below can report it; the mechanism itself is further down.
namespace { bool g_cycle_fast_limit_enabled = true; }
unsigned long long g_nds_fast_limit_publishes = 0;

std::string nds_dispatch_stats_json() {
    std::string out = "{";
    const char* cpus[2] = {"arm9", "arm7"};
    for (int cpu = 0; cpu < 2; ++cpu) {
        const NdsDispatchStats& s = g_nds_dispatch_stats[cpu];
        if (cpu) out += ",";
        out += std::string("\"") + cpus[cpu] + "\":{" +
            "\"resume_dispatch\":" + std::to_string(s.resume_dispatch) +
            ",\"dispatch_total\":" + std::to_string(s.dispatch_total) +
            ",\"dispatch_slice_yield\":" +
                std::to_string(s.dispatch_slice_yield) +
            ",\"dispatch_exchange\":" + std::to_string(s.dispatch_exchange) +
            ",\"literal_branch\":" + std::to_string(s.literal_branch) +
            ",\"literal_call\":" + std::to_string(s.literal_call) +
            ",\"literal_fallthrough\":" +
                std::to_string(s.literal_fallthrough) +
            ",\"exception_dispatch\":" +
                std::to_string(s.exception_dispatch) +
            ",\"cache_hit\":" + std::to_string(s.cache_hit) +
            ",\"cache_hit_absent\":" + std::to_string(s.cache_hit_absent) +
            ",\"cache_slow_lookup\":" +
                std::to_string(s.cache_slow_lookup) +
            ",\"crs_push\":" + std::to_string(s.crs_push) +
            ",\"crs_hit\":" + std::to_string(s.crs_hit) +
            ",\"crs_miss\":" + std::to_string(s.crs_miss) +
            ",\"crs_scan_iters\":" + std::to_string(s.crs_scan_iters) + "}";
    }
    // Selector state, so a harness can VERIFY the tier it believes it is
    // measuring instead of assuming it. forced_tier3_misses is the proof the
    // selector actually converted lookups, not merely that a flag was set.
    out += std::string(",\"forced_tier3\":") +
        (g_nds_force_tier3 ? "true" : "false") +
        ",\"forced_tier3_misses\":" +
        std::to_string(g_nds_force_tier3_misses);
    // Same idea for the deadline-bounded machinery selector: the flag AND a
    // counter proving deadlines were actually published (beads-yjp.42).
    out += std::string(",\"cycle_fast_limit\":") +
        (g_cycle_fast_limit_enabled ? "true" : "false") +
        ",\"fast_limit_publishes\":" +
        std::to_string(g_nds_fast_limit_publishes);
    out += "}";
    return out;
}

// ── Globals the ABI exposes ─────────────────────────────────────────────
extern "C" ArmCpuState g_cpu = {};
extern "C" unsigned long long g_runtime_cycles = 0;
// On by default: the recompiled banks gate their per-instruction hook on this,
// and the hook is what keeps the always-on insn9/insn7 retired-instruction
// counters live (DEBUG.md "all on in Release"). The hook only bumps a counter
// + break-check — no control-flow / cycle effect — so behaviour is unchanged.
extern "C" unsigned g_runtime_insn_trace = 1;
extern "C" uint32_t g_runtime_break_pc = 0;

NdsCpu      g_nds_active = NDS_ARM9;
bool        g_nds_terminal = false;
const char* g_nds_halt_reason = nullptr;
bool        g_discover_static_misses = false;
bool        g_nds_force_tier3 = false;
unsigned long long g_nds_force_tier3_misses = 0;

// Deadline-bounded per-instruction machinery (beads-yjp.42 phase 1).
// Contract and rationale: recompiler/armv4t/runtime_arm.h. Zero = "every
// per-instruction poll takes the faithful path", which is both the initial
// state and the state every re-arm site restores.
extern "C" unsigned long long g_nds_fast_limit = 0;
// The host-stack unwind flag, promoted from a file-local bool to exported
// data so runtime_unwinding() is a load in the caller instead of a call.
extern "C" unsigned char g_nds_unwinding = 0;

// ── Per-CPU dispatch registration ───────────────────────────────────────
// DispatchEntry is declared in state.h (layout-matches the generated table).

namespace {
struct DispatchBank {
    const DispatchEntry* table = nullptr;
    unsigned             len = 0;
};
struct CpuCtx {
    std::vector<DispatchBank> banks;
    // Flat candidate index across all immutable and live banks. Entries with
    // the same (address,state) remain registration-ordered so lookup can walk
    // an exact identity chain and choose the newest matching generation
    // without scanning every unrelated DLL.
    std::vector<const DispatchEntry*> dispatch_index;
    // Adding or removing a live candidate invalidates prior positive and
    // negative cache answers. Bump an epoch instead of zeroing the entire
    // 8 MiB per-CPU table for every DLL published at a scheduler boundary.
    uint32_t dispatch_epoch = 1u;
    uint32_t             exc_base = 0;  // exception-vector base for this CPU
};
CpuCtx g_ctx[2];

// A live page candidate normally owns one executable page. Keep a small exact
// union for dependency closures and reject anything larger rather than run
// without active-code invalidation.
struct StaticExecutionGuard {
    const NdsStaticValidation* validation = nullptr;
    uint32_t page_addr[4] = {};
    uint32_t generation[4] = {};
    const uint32_t* generation_ptr[4] = {};
    uint32_t page_count = 0u;
    bool invalidated = false;
};
StaticExecutionGuard* g_static_guard = nullptr;

#if defined(NDS_PROFILE_HLE_HEAT)
struct HleHeatStats {
    const NdsHleProfileDescriptor* descriptor = nullptr;
    int cpu = 0;
    uint64_t entries = 0;
    uint64_t start_entries = 0;
    uint64_t resume_entries = 0;
    uint64_t normal_segments = 0;
    uint64_t unwind_segments = 0;
    uint64_t sampled_segments = 0;
    uint64_t accepted_samples = 0;
    uint64_t accepted_normal_samples = 0;
    uint64_t accepted_unwind_samples = 0;
    uint64_t irq_rejects = 0;
    uint64_t instruction_rejects = 0;
    uint64_t guard_mismatches = 0;
    uint64_t pc_mismatches = 0;
    uint64_t nested_entries = 0;
    uint64_t depth_mismatches = 0;
    uint32_t max_depth = 0;
    uint64_t host_ns = 0;
    uint64_t instructions = 0;
    uint64_t cycles = 0;
};

std::vector<HleHeatStats> g_hle_heat;
uint32_t g_hle_irq_epoch[2]{};
uint32_t g_hle_active_depth[2]{};

unsigned hle_sample_log2() {
    static const unsigned value = [] {
        const char* text = std::getenv("NDS_HLE_SAMPLE_LOG2");
        if (!text || !*text) return 6u;  // deterministic 1/64 sampling
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(text, &end, 10);
        if (*end != '\0' || parsed > 16u) {
            std::fprintf(stderr,
                "invalid NDS_HLE_SAMPLE_LOG2 (expected 0..16); using 6\n");
            return 6u;
        }
        return static_cast<unsigned>(parsed);
    }();
    return value;
}

uint64_t hle_sample_phase() {
    static const uint64_t value = [] {
        const unsigned log2 = hle_sample_log2();
        const uint64_t limit = uint64_t{1} << log2;
        const char* text = std::getenv("NDS_HLE_SAMPLE_PHASE");
        if (!text || !*text) return uint64_t{0};
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(text, &end, 10);
        if (*end != '\0' || parsed >= limit) {
            std::fprintf(stderr,
                "invalid NDS_HLE_SAMPLE_PHASE for current log2; using 0\n");
            return uint64_t{0};
        }
        return static_cast<uint64_t>(parsed);
    }();
    return value;
}

uint64_t hle_host_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<
        std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

HleHeatStats* hle_stats(const NdsHleProfileDescriptor* descriptor) {
    for (auto& stats : g_hle_heat)
        if (stats.descriptor == descriptor) return &stats;
    return nullptr;
}

void append_json_string(std::string& out, const char* text) {
    out.push_back('"');
    for (const unsigned char c : std::string(text ? text : "")) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20u) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof escaped, "\\u%04X", c);
                    out += escaped;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}
#endif

constexpr uint32_t kDispatchCacheSize = 65536u;
struct alignas(64) CachedStaticLookup {
    uint32_t pc = 0u;
    uint32_t page_addr[4]{};
    uint32_t generation[4]{};
    const uint32_t* generation_ptr[4]{};
    void (*fn)(void) = nullptr;
    const NdsStaticValidation* validation = nullptr;
    uint32_t live_bank_serial = 0u;
    uint32_t dispatch_epoch = 0u;
    uint8_t page_count = 0u;
    uint8_t thumb = 0u;
    uint8_t occupied = 0u;
};
static_assert(sizeof(CachedStaticLookup) == 128u);
std::array<std::array<CachedStaticLookup, kDispatchCacheSize>, 2>
    g_dispatch_cache{};

void clear_dispatch_cache_cpu(unsigned cpu) {
    CachedStaticLookup empty{};
    for (CachedStaticLookup& slot : g_dispatch_cache[cpu & 1u])
        slot = empty;
}

void clear_dispatch_cache_all() {
    clear_dispatch_cache_cpu(0u);
    clear_dispatch_cache_cpu(1u);
}

// Cycle budget for the run slice; runtime_should_yield trips when reached
// so a guest spin loop can't hang the host.
unsigned long long g_cycle_cap = 0;  // 0 = unlimited
std::vector<uint64_t> g_discovery_seen;
uint32_t g_yield_poll_hint = 1u;
bool g_cpu_fast_poll = true;

// Deadline-bounded per-instruction machinery (beads-yjp.42 phase 1). See the
// contract in recompiler/armv4t/runtime_arm.h. NDS_CYCLE_FAST_LIMIT=0 pins
// the limit at zero for the process, so the SAME BINARY runs the faithful
// path end to end (the one-binary selector rule in
// docs/host_optimization_strategy.md); =1 (default) is the normal policy.
bool configured_cycle_fast_limit() {
    static const bool enabled = [] {
        const char* value = std::getenv("NDS_CYCLE_FAST_LIMIT");
        if (!value || (value[0] == '1' && value[1] == '\0')) return true;
        if (value[0] == '0' && value[1] == '\0') return false;
        std::fprintf(stderr,
                     "invalid NDS_CYCLE_FAST_LIMIT value (expected 0 or 1); "
                     "using the faithful unbounded path\n");
        return false;
    }();
    return enabled;
}

// ARM9 content-validated banks have no direct C-to-C calls: every body is
// entered through runtime_dispatch. Let a generated body return its adjacent
// fallthrough target to that dispatch invocation instead of recursively
// nesting another runtime_dispatch frame. Nested BL/computed dispatches
// install their own state and restore their caller's state on return.
struct TailDispatchState {
    TailDispatchState* saved = nullptr;
    uint32_t target_pc = 0u;
    // Link slot carried across the tail hand-off so the enclosing dispatch
    // loop can take the linked fast path for the fall-through target
    // instead of re-probing the 8 MiB lookup cache.
    NdsLinkSlot* linked = nullptr;
    int cpu = 0;
    bool pending = false;
};
TailDispatchState* g_tail_dispatch = nullptr;

struct TailDispatchScope {
    TailDispatchState state;
    TailDispatchScope() {
        state.saved = g_tail_dispatch;
        state.cpu = static_cast<int>(g_nds_active);
        g_tail_dispatch = &state;
    }
    ~TailDispatchScope() { g_tail_dispatch = state.saved; }
};

// RE-ARM FUNNEL. Every site that can make a per-instruction service condition
// true earlier than a previously published deadline routes through here (or
// through runtime_clear_fast_limit below), and both drop the limit to zero so
// the very next poll runs the full faithful scan and republishes.
void request_yield_poll() {
    g_yield_poll_hint = 1u;
    g_nds_fast_limit = 0u;
}

bool configured_cpu_fast_poll() {
    static const bool enabled = [] {
        const char* value = std::getenv("NDS_CPU_FAST_POLL");
        if (!value || (value[0] == '1' && value[1] == '\0')) return true;
        if (value[0] == '0' && value[1] == '\0') return false;
        std::fprintf(stderr,
                     "invalid NDS_CPU_FAST_POLL value (expected 0 or 1); "
                     "using faithful full polling\n");
        return false;
    }();
    return enabled;
}

bool static_bios_pc(uint32_t pc) {
    return (g_nds_active == NDS_ARM9)
        ? (pc >= 0xFFFF0000u && pc < 0xFFFF1000u)
        : (pc < 0x00004000u);
}

template <typename Fn>
bool for_each_validation_range(const NdsStaticValidation* validation,
                               Fn&& fn) {
    if (!validation) return true;
    if (validation->dependency_count != 0u) {
        if (!validation->dependencies) return false;
        for (uint32_t i = 0u; i < validation->dependency_count; ++i) {
            if (!fn(validation->dependencies[i])) return false;
        }
        return true;
    }
    const NdsStaticValidationRange owner{
        validation->addr, validation->size, validation->expected};
    return fn(owner);
}

bool validation_identity_live(const NdsStaticValidation* validation) {
    return for_each_validation_range(
        validation, [](const NdsStaticValidationRange& range) {
            return range.expected && range.size != 0u &&
                uint64_t{range.addr} + range.size <= 0x1'0000'0000ull &&
                bus_range_has_write_provenance(range.addr, range.size) &&
                bus_live_bytes_equal(range.addr, range.expected, range.size);
        });
}

bool dispatch_validation_live(const NdsStaticValidation* validation,
                              uint32_t pc, bool thumb, void*) {
    // Captured firmware variants are executable only after the LLE guest or
    // one of its hardware bus masters has actually installed every backing
    // page. Byte equality alone is not provenance: pristine zero-filled RAM
    // can otherwise select a late all-zero variant before firmware boot has
    // materialized it.
    return nds_dispatch_validation_owns_entry(validation, pc, thumb) &&
        validation_identity_live(validation);
}

const DispatchEntry* lookup_static(
        const CpuCtx& c, uint32_t pc, bool thumb,
        uint32_t* candidate_count,
        const DispatchEntry** inactive_candidate) {
    if (candidate_count) *candidate_count = 0u;
    if (inactive_candidate) *inactive_candidate = nullptr;
    const NdsDispatchLookupResult result = nds_dispatch_lookup_index(
        c.dispatch_index, pc, thumb, dispatch_validation_live, nullptr);
    if (candidate_count) *candidate_count = result.candidate_count;
    if (inactive_candidate) *inactive_candidate = result.inactive;
    return result.selected;
}

bool cached_lookup_live(const CachedStaticLookup& cached) {
    // page_count is populated exactly when the cached candidate has a
    // writable-RAM validation. Keep the common immutable-bank hit entirely
    // inside the cache line instead of chasing entry->validation.
    if (cached.page_count == 0u) return true;
    for (uint32_t i = 0; i < cached.page_count; ++i) {
        const uint32_t live_generation = cached.generation_ptr[i]
            ? *cached.generation_ptr[i]
            : bus_exec_page_generation(cached.page_addr[i]);
        if (live_generation != cached.generation[i])
            return false;
    }
    return true;
}

bool cache_page_generation(CachedStaticLookup& slot, uint32_t page) {
    for (uint32_t i = 0; i < slot.page_count; ++i) {
        if (slot.page_addr[i] == page) return true;
    }
    if (slot.page_count >= 4u) return false;
    const uint32_t index = slot.page_count++;
    slot.page_addr[index] = page;
    if (page - 0x02000000u < 0x01000000u && g_busf_main.gen) {
        const uint32_t offset = page & g_busf_main.mask;
        slot.generation_ptr[index] =
            g_busf_main.gen + (offset >> 12u);
        slot.generation[index] = *slot.generation_ptr[index];
    } else {
        slot.generation[index] = bus_exec_page_generation(page);
    }
    return true;
}

bool cache_validation_pages(CachedStaticLookup& slot,
                            const NdsStaticValidation* validation) {
    return for_each_validation_range(
        validation, [&](const NdsStaticValidationRange& range) {
            if (!range.expected || range.size == 0u) return false;
            const uint64_t end = uint64_t{range.addr} + range.size;
            if (end > 0x1'0000'0000ull) return false;
            const uint32_t first_page = range.addr & ~0xFFFu;
            const uint32_t last_page =
                static_cast<uint32_t>(end - 1u) & ~0xFFFu;
            for (uint32_t page = first_page;; page += 4096u) {
                if (!cache_page_generation(slot, page)) return false;
                if (page == last_page) break;
            }
            return true;
        });
}

bool cache_rejected_candidate_chain(const CpuCtx& c, uint32_t pc, bool thumb,
                                    CachedStaticLookup& slot) {
    const uint64_t wanted = (uint64_t{pc} << 1u) | uint64_t{thumb};
    auto it = std::lower_bound(
        c.dispatch_index.begin(), c.dispatch_index.end(), wanted,
        [](const DispatchEntry* entry, uint64_t value) {
            return nds_dispatch_entry_key(entry) < value;
        });
    for (; it != c.dispatch_index.end() &&
           nds_dispatch_entry_key(*it) == wanted; ++it) {
        const NdsStaticValidation* validation = (*it)->validation;
        if (!validation || validation->size == 0u) continue;
        if (!cache_validation_pages(slot, validation)) return false;
    }
    return slot.page_count != 0u;
}

const CachedStaticLookup* lookup_static_cached_impl(const CpuCtx& c,
                                                    uint32_t pc, bool thumb) {
    auto& cache = g_dispatch_cache[g_nds_active];
    CachedStaticLookup& slot =
        cache[((pc >> 1u) ^ (pc >> 13u) ^ uint32_t{thumb}) &
              (kDispatchCacheSize - 1u)];
    if (slot.occupied && slot.dispatch_epoch == c.dispatch_epoch &&
        slot.pc == pc && slot.thumb == uint8_t{thumb} &&
        cached_lookup_live(slot)) {
        auto& stats = g_nds_dispatch_stats[g_nds_active];
        if (slot.fn) {
            ++stats.cache_hit;
            live_overlay_note_cached_hit(slot.live_bank_serial);
            return &slot;
        }
        ++stats.cache_hit_absent;
        return nullptr;
    }
    ++g_nds_dispatch_stats[g_nds_active].cache_slow_lookup;

    uint32_t candidate_count = 0u;
    const DispatchEntry* inactive_candidate = nullptr;
    const DispatchEntry* hit = lookup_static(
        c, pc, thumb, &candidate_count, &inactive_candidate);
    if ((hit && hit->validation) || inactive_candidate) {
        live_overlay_note_lookup(g_nds_active, pc, pc, g_cpu.R[14],
                                 g_cpu.cpsr, hit, inactive_candidate,
                                 candidate_count,
                                 hit ? "native_candidate"
                                     : "candidate_reject");
    }
    // Copied RAM code has no static entry and is handled by Tier 3. Remember
    // that definitive miss so every interpreted block does not binary-search
    // every static bank again. A single inactive validation candidate is also
    // safe to cache: its backing-page generations invalidate the entry after
    // a guest write. Multiple variants are left uncached because any one of
    // their distinct backing ranges could become live. Cache the complete
    // rejected chain against the union of its backing-page generations; this
    // prevents an FMV/current overlay generation from re-hashing every stale
    // cached candidate on every interpreted instruction.
    slot = {};
    slot.pc = pc;
    slot.thumb = static_cast<uint8_t>(thumb);
    slot.dispatch_epoch = c.dispatch_epoch;
    const DispatchEntry* const candidate = hit ? hit : inactive_candidate;
    slot.fn = hit ? hit->fn : nullptr;
    slot.validation = candidate ? candidate->validation : nullptr;
    slot.live_bank_serial = hit
        ? live_overlay_candidate_serial(g_nds_active, hit)
        : 0u;
    slot.occupied = 1u;
    if (!hit && candidate_count > 1u) {
        if (!cache_rejected_candidate_chain(c, pc, thumb, slot)) {
            slot = {};
            return nullptr;
        }
    } else if (slot.validation &&
               !cache_validation_pages(slot, slot.validation)) {
        slot = {};
        return nullptr;
    }
    return hit ? &slot : nullptr;
}

// Single dispatch chokepoint. Both consumers -- runtime_dispatch (native
// entry) and nds_has_bank (Tier 3's per-instruction takeover poll) -- go
// through here, on both CPUs, so one branch covers the whole selector.
//
// The forced miss is applied AFTER the real lookup, not instead of it. Short-
// circuiting ahead of the cache would delete the very per-instruction poll
// cost the campaign exists to measure and would report a flattering number
// for a path no player ever runs. The lookup runs, the result is discarded.
const CachedStaticLookup* lookup_static_cached(const CpuCtx& c, uint32_t pc,
                                               bool thumb) {
    const CachedStaticLookup* hit = lookup_static_cached_impl(c, pc, thumb);
    if (!g_nds_force_tier3 || !hit || static_bios_pc(pc)) return hit;
    ++g_nds_force_tier3_misses;
    return nullptr;
}

bool arm_static_guard(const NdsStaticValidation* validation,
                      StaticExecutionGuard& guard) {
    guard = {};
    if (!validation) return true;
    guard.validation = validation;
    auto add_page = [&](uint32_t page) {
        for (uint32_t i = 0u; i < guard.page_count; ++i) {
            if (guard.page_addr[i] == page) return true;
        }
        if (guard.page_count >= 4u) return false;
        const uint32_t i = guard.page_count++;
        guard.page_addr[i] = page;
        // Main RAM's generation storage is stable for a runtime instance.
        // Keep the exact counter used by the dispatch cache so every nested
        // static return does not resolve the same virtual page again.
        if (page - 0x02000000u < 0x01000000u && g_busf_main.gen) {
            const uint32_t offset = page & g_busf_main.mask;
            guard.generation_ptr[i] =
                g_busf_main.gen + (offset >> 12u);
            guard.generation[i] = *guard.generation_ptr[i];
        } else {
            guard.generation[i] = bus_exec_page_generation(page);
        }
        return true;
    };
    return for_each_validation_range(
        validation, [&](const NdsStaticValidationRange& range) {
            if (!range.expected || range.size == 0u) return false;
            const uint64_t end = uint64_t{range.addr} + range.size;
            if (end > 0x1'0000'0000ull) return false;
            const uint32_t first_page = range.addr & ~0xFFFu;
            const uint32_t last_page =
                static_cast<uint32_t>(end - 1u) & ~0xFFFu;
            for (uint32_t page = first_page;; page += 4096u) {
                if (!add_page(page)) return false;
                if (page == last_page) break;
            }
            return true;
        });
}

bool cached_static_guard(const CachedStaticLookup& cached,
                         StaticExecutionGuard& guard) {
    guard = {};
    if (!cached.validation) return true;
    // lookup_static_cached() just proved this exact slot live using these
    // generation values. Copy that validated snapshot into the active guard
    // instead of resolving and rereading the same pages a second time.
    // Fall back to the reference builder if a future cache representation
    // does not carry the complete guard snapshot.
    if (cached.page_count == 0u || cached.page_count > 4u)
        return arm_static_guard(cached.validation, guard);

    guard.validation = cached.validation;
    guard.page_count = cached.page_count;
    for (uint32_t i = 0; i < cached.page_count; ++i) {
        guard.page_addr[i] = cached.page_addr[i];
        guard.generation[i] = cached.generation[i];
        guard.generation_ptr[i] = cached.generation_ptr[i];
    }
    return true;
}

bool guard_generation_changed(const StaticExecutionGuard& guard) {
    if (!guard.validation) return false;
    for (uint32_t i = 0; i < guard.page_count; ++i) {
        const uint32_t live_generation = guard.generation_ptr[i]
            ? *guard.generation_ptr[i]
            : bus_exec_page_generation(guard.page_addr[i]);
        if (live_generation != guard.generation[i])
            return true;
    }
    return false;
}

bool refresh_guard_after_generation_change(StaticExecutionGuard& guard) {
    if (!guard.validation) return true;
    if (!validation_identity_live(guard.validation)) return false;
    for (uint32_t i = 0; i < guard.page_count; ++i) {
        guard.generation[i] = guard.generation_ptr[i]
            ? *guard.generation_ptr[i]
            : bus_exec_page_generation(guard.page_addr[i]);
    }
    return true;
}

bool active_static_code_changed() {
    return g_static_guard && g_static_guard->invalidated;
}

struct LinkGuard {
    const NdsStaticValidation* validation = nullptr;
    // Mirrors CachedStaticLookup::live_bank_serial so a link hit keeps
    // live_overlay's native_hits accounting identical to a cache hit.
    // A serial is only ever nonzero for a live shard, which is always
    // content validated, so it can live in the guard record and stay out
    // of the 24-byte slot.
    uint32_t serial = 0u;
    uint32_t page_count = 0u;
    uint32_t page_addr[4]{};
    uint32_t generation[4]{};
    const uint32_t* generation_ptr[4]{};
};

struct StaticGuardScope {
    StaticExecutionGuard active;
    StaticExecutionGuard* saved;
    StaticGuardScope() : saved(g_static_guard) { g_static_guard = nullptr; }
    ~StaticGuardScope() {
        // A nested static call may write through an alias of its caller's
        // backing page. Revalidate the saved guard once on unwind so that
        // write cannot be lost when the outer guard becomes active again.
        if (saved && !saved->invalidated && guard_generation_changed(*saved) &&
            !refresh_guard_after_generation_change(*saved)) {
            saved->invalidated = true;
        }
        g_static_guard = saved;
        if (saved && saved->invalidated) request_yield_poll();
    }
    // Split in two so the dispatch-cost sampler can close its region between
    // the machinery (guard construction) and the guest code (fn()). Timing
    // fn() would make literal_call inclusive of the entire callee and every
    // dispatch nested inside it, so the per-class buckets would nest and
    // their sum would exceed wall time. prepare() does exactly what call()'s
    // first half did, in the same order and with the same side effects.
    bool prepare(const CachedStaticLookup* hit) {
        if (!hit || !hit->fn ||
            !cached_static_guard(*hit, active))
            return false;
        g_static_guard = &active;
        ready = hit->fn;
        return true;
    }
    // B2 linked entry. Identical to prepare() with the same resolution, but
    // sourced from a link slot's own snapshot instead of a cache slot's, so
    // the hot path never has to materialize a 128-byte CachedStaticLookup.
    // guard == null is the immutable-target case, whose guard is empty --
    // exactly what cached_static_guard() produces for a null validation.
    bool prepare_linked(void (*fn)(void), const LinkGuard* guard) {
        if (!fn) return false;
        if (guard) {
            active.validation = guard->validation;
            active.page_count = guard->page_count;
            for (uint32_t i = 0u; i < guard->page_count; ++i) {
                active.page_addr[i] = guard->page_addr[i];
                active.generation[i] = guard->generation[i];
                active.generation_ptr[i] = guard->generation_ptr[i];
            }
        }
        g_static_guard = &active;
        ready = fn;
        return true;
    }
    void invoke() { ready(); }
    bool call(const CachedStaticLookup* hit) {
        if (!prepare(hit)) return false;
        invoke();
        return true;
    }
    void (*ready)(void) = nullptr;
};

// ── B2 validated direct linking ──────────────────────────────────────
// A link slot holds one literal transfer's already-resolved target in the
// CALLER's data. It replaces the g_dispatch_cache probe (a direct-mapped
// 8 MiB-per-CPU table, i.e. a near-certain cache + TLB miss pair on every
// hot dispatch) with a compare against storage the callsite has just
// touched. It replaces NOTHING else: the guest-visible sequence -- LR/R15
// publication by the emitted code, the content guard, the generation
// revalidation, ordered overlapping-bank selection, and the slice-yield
// preemption point for tail transfers -- is preserved exactly.
//
// SAFETY ARGUMENT, in the order the checks run:
//  1. `key` pins a resolution to one (link epoch, CPU). The link epoch
//     bumps on every nds_register_dispatch, every nds_unregister_dispatch,
//     and every runtime_init, so any change to the set of candidate banks
//     invalidates every slot in the program at once, with no walk over
//     emitted code and no registry of slots. This is the same invalidation
//     event the dispatch cache uses, taken globally instead of per CPU
//     (an ARM7 bank load also drops ARM9 slots; bank loads are rare).
//  2. CPSR.T must still agree with the mode the target was resolved in.
//     B/BL never interwork, so this normally holds, but the runtime, not
//     the emitter, gets to decide.
//  3. A content-validated target revalidates its backing-page generations
//     on EVERY use from the snapshot taken at resolve time -- byte for
//     byte what cached_lookup_live() does for a cache hit. A guest write
//     to the target's pages therefore downgrades the slot to the full
//     dispatcher on the very next transfer, which re-resolves through
//     lookup_static_cached() and so re-runs the full byte-identity proof.
//  4. Resolution itself always goes through lookup_static_cached(), never
//     a private search, so the ranked winner among co-validating candidates
//     (largest owned span, first-registered on a tie; dispatch_lookup.h) is
//     inherited rather than reimplemented.
//
// The slot is pure cache: clearing one at any point is always correct.
// Stable addresses: a slot stores a raw pointer to one of these. Cleared
// whenever the link epoch bumps, at which point no slot can reach one.
std::deque<LinkGuard> g_link_guards;
constexpr std::size_t kLinkGuardCap = 1u << 16u;
uint32_t g_link_epoch = 1u;
uint64_t g_link_hits[2]{};
uint64_t g_link_resolves[2]{};
uint64_t g_link_falls[2]{};
// beads-yjp.67 inline-leaf accounting. `admits` counts expansions actually
// run inline; `falls` counts sites that dropped to the faithful link call
// because the slot was unresolved, the resolution named a different body, or
// the content guard had moved.
uint64_t g_inline_leaf_admits[2]{};
uint64_t g_inline_leaf_falls[2]{};

bool configured_inline_leaves() {
    static const bool enabled = [] {
        const char* value = std::getenv("NDS_INLINE_LEAVES");
        if (!value || (value[0] == '1' && value[1] == '\0')) return true;
        if (value[0] == '0' && value[1] == '\0') return false;
        std::fprintf(stderr,
                     "invalid NDS_INLINE_LEAVES value (expected 0 or 1); "
                     "running every inlined leaf through the dispatcher\n");
        return false;
    }();
    return enabled;
}
bool g_inline_leaves = true;

bool configured_direct_link() {
    static const bool enabled = [] {
        const char* value = std::getenv("NDS_DIRECT_LINK");
        if (!value || (value[0] == '1' && value[1] == '\0')) return true;
        if (value[0] == '0' && value[1] == '\0') return false;
        std::fprintf(stderr,
                     "invalid NDS_DIRECT_LINK value (expected 0 or 1); "
                     "using the unlinked dispatcher\n");
        return false;
    }();
    return enabled;
}
bool g_direct_link = true;

// DEEP-TRACE POLICY, decided explicitly and deliberately NOT the bus fast
// path's policy. The bus fast path gates on g_runtime_deep_trace because it
// SKIPS the per-access ring recording, so a fast hit is invisible to a
// query surface that would otherwise have seen it. A link hit skips nothing
// observable: it enters the very same dispatch loop and emits the very same
// RUNTIME_TRACE_DISPATCH event, the same slice-yield decision, the same
// dispatch_total, and (below) the same live-overlay native-hit accounting.
// The only diagnostic that moves is the cache_hit / cache_slow_lookup
// split, which is the measurement of this feature, not a casualty of it.
//
// Gating on deep trace would also make the feature UNTESTABLE at the level
// that matters: --serve keeps deep trace on, so the byte-exact checkpoint
// probe would have compared two identical unlinked legs and reported a
// meaningless pass. Linking is therefore live in every mode, and
// NDS_DIRECT_LINK=0 remains the single, total off switch for oracle probes
// and debugging.
inline bool link_enabled() { return g_direct_link; }

inline uint32_t link_key() {
    return (g_link_epoch << 1u) | static_cast<uint32_t>(g_nds_active);
}

void link_epoch_bump() {
    if (++g_link_epoch == 0u) g_link_epoch = 1u;
    g_link_guards.clear();
}

// Fast admission test for an already-resolved slot. Deliberately does not
// touch the guard: the generation check is a separate, colder step.
inline bool link_slot_ready(const NdsLinkSlot* slot) {
    return slot->key == link_key() && slot->fn != nullptr &&
        (((g_cpu.cpsr & CPSR_T_BIT) != 0u) ==
         ((slot->target_pc & 1u) != 0u));
}

inline bool link_guard_live(const LinkGuard* guard) {
    for (uint32_t i = 0u; i < guard->page_count; ++i) {
        const uint32_t live = guard->generation_ptr[i]
            ? *guard->generation_ptr[i]
            : bus_exec_page_generation(guard->page_addr[i]);
        if (live != guard->generation[i]) return false;
    }
    return true;
}

// Publish a resolved lookup into a slot. Mirrors the exact snapshot the
// dispatch cache would have held, so the two paths cannot disagree.
void link_slot_fill(NdsLinkSlot* slot, const CachedStaticLookup* hit) {
    slot->fn = nullptr;
    slot->guard = nullptr;
    slot->key = 0u;
    if (!hit || !hit->fn) return;
    if (hit->validation) {
        // Only the complete snapshot is linkable. A cache entry that could
        // not record its pages falls back to the dispatcher forever, which
        // is correct and rare.
        if (hit->page_count == 0u || hit->page_count > 4u) return;
        // A validated target re-resolves every time the guest writes its
        // backing pages, and each resolve wants a fresh snapshot. Cap the
        // pool so a churning overlay cannot grow it without bound between
        // epoch bumps; past the cap validated targets simply stay on the
        // dispatcher until the next bank event clears the pool.
        if (g_link_guards.size() >= kLinkGuardCap) return;
        g_link_guards.emplace_back();
        LinkGuard& guard = g_link_guards.back();
        guard.validation = hit->validation;
        guard.serial = hit->live_bank_serial;
        guard.page_count = hit->page_count;
        for (uint32_t i = 0u; i < hit->page_count; ++i) {
            guard.page_addr[i] = hit->page_addr[i];
            guard.generation[i] = hit->generation[i];
            guard.generation_ptr[i] = hit->generation_ptr[i];
        }
        slot->guard = &guard;
    }
    slot->fn = hit->fn;
    slot->key = link_key();
}

// Admit an already-resolved slot for this transfer. Returns the slot's
// guard record through `guard` (null for an immutable target, which needs
// none) and false whenever the slot must fall back to the dispatcher.
bool link_slot_admit(const NdsLinkSlot* slot, const LinkGuard** guard) {
    if (!link_slot_ready(slot)) return false;
    const LinkGuard* g = static_cast<const LinkGuard*>(slot->guard);
    if (g && !link_guard_live(g)) return false;
    *guard = g;
    return true;
}

// Bracket the static function range containing `pc` in a dispatch table
// (sorted by addr): [*start, *end). Returns false if the table is empty or
// `pc` precedes the first entry. Used only by the dispatch-miss diagnostic
// to localize a non-function-start entry to its containing recompiled func.
bool bracket_static_range(const DispatchEntry* table, unsigned len,
                          uint32_t pc, uint32_t* start, uint32_t* end) {
    if (!table || len == 0) return false;
    unsigned lo = 0, hi = len;
    while (lo < hi) {                       // first index with addr > pc
        unsigned mid = (lo + hi) >> 1u;
        if (table[mid].addr <= pc) lo = mid + 1u; else hi = mid;
    }
    if (lo == 0) return false;              // pc precedes the first entry
    *start = table[lo - 1u].addr;
    unsigned j = lo;                        // first entry with a larger addr
    while (j < len && table[j].addr == *start) ++j;
    *end = (j < len) ? table[j].addr : *start;
    return true;
}
}  // namespace

extern "C" void runtime_request_yield_poll(void) { request_yield_poll(); }

// Narrow re-arm for sites that must invalidate the deadline but have no
// reason to force the hint's other consumers (io.cpp's irq_recompute is the
// motivating case: an IRQ that becomes pending must be delivered at the next
// instruction boundary, and the fast tick path does not look at it).
extern "C" void runtime_clear_fast_limit(void) { g_nds_fast_limit = 0u; }

extern "C" void runtime_note_code_write(void) {
    // beads-yjp.28: every guest RAM write funnels through here -- the slow bus
    // path (bus.cpp note_ram_write) and the generated-bank inline fast path
    // (runtime_arm.h nds_busf_note_write) alike -- so this single counter is
    // what lets the coverage page cache stay trusted between writes instead of
    // paying a generation lookup per interpreted instruction.
    ++g_coverage_write_epoch;
    if (g_static_guard && !g_static_guard->invalidated &&
        guard_generation_changed(*g_static_guard)) {
        if (!refresh_guard_after_generation_change(*g_static_guard)) {
            g_static_guard->invalidated = true;
            request_yield_poll();
        }
    }
}

extern "C" void runtime_note_live_write(uint32_t addr, uint32_t width,
                                        uint32_t old_value,
                                        uint32_t new_value) {
    live_overlay_note_write(g_nds_active, g_cpu.R[15], addr, width,
                            old_value, new_value);
}

// ── Dispatch registration threading convention ─────────────────────────
// nds_register_dispatch / nds_unregister_dispatch mutate ctx.banks and
// ctx.dispatch_index as raw std::vectors, and the lookup path reads those
// vectors with no lock or atomics. That is sound only because every caller
// runs on the single emulation thread: static banks are registered from
// main() before the scheduler starts, and live-overlay banks from
// live_overlay_poll() at the scheduler rendezvous (scheduler.cpp:462).
// The live-overlay prepare worker deliberately stops at LoadLibrary +
// preflight and hands the prepared bank back through a queue precisely so
// that it never touches these vectors.
//
// Debug builds pin the first caller's thread and assert every later call
// matches; the check compiles out entirely under NDEBUG (Release), where
// the registration path is on the boot/publication path and not hot enough
// to matter either way.
#if !defined(NDEBUG)
namespace {
std::thread::id g_dispatch_thread{};
bool g_dispatch_thread_bound = false;

void assert_dispatch_thread(const char* who) {
    const std::thread::id self = std::this_thread::get_id();
    if (!g_dispatch_thread_bound) {
        g_dispatch_thread = self;
        g_dispatch_thread_bound = true;
        return;
    }
    if (g_dispatch_thread == self) return;
    std::fprintf(stderr,
                 "[dispatch] %s called off the emulation thread; the dispatch "
                 "index is not thread-safe\n", who);
    std::abort();
}
}  // namespace
#define NDS_ASSERT_DISPATCH_THREAD(who) assert_dispatch_thread(who)
#else
#define NDS_ASSERT_DISPATCH_THREAD(who) ((void)0)
#endif

// Re-pin the dispatch-registration thread. Only for a runner that moves
// emulation onto a different thread after boot; the debug assert otherwise
// binds itself to the first caller.
extern "C" void nds_dispatch_bind_thread(void) {
#if !defined(NDEBUG)
    g_dispatch_thread = std::this_thread::get_id();
    g_dispatch_thread_bound = true;
#endif
}

extern "C" void nds_register_dispatch(int cpu, const DispatchEntry* t,
                                      unsigned len, uint32_t exc_base) {
    NDS_ASSERT_DISPATCH_THREAD("nds_register_dispatch");
    CpuCtx& ctx = g_ctx[cpu & 1];
    ctx.banks.push_back({t, len});
    nds_dispatch_index_add(ctx.dispatch_index, t, len);
    ctx.exc_base = exc_base;
    // A PC cached as absent before a newly registered bank may now resolve.
    // Direct-link slots are invalidated wholesale by the same event: the
    // upgrade from dispatcher to native for a target this bank now owns is
    // the slot's next resolve.
    link_epoch_bump();
    if (++ctx.dispatch_epoch == 0u) {
        clear_dispatch_cache_cpu(cpu);
        ctx.dispatch_epoch = 1u;
    }
}

extern "C" void nds_unregister_dispatch(int cpu, const DispatchEntry* t,
                                         unsigned len) {
    NDS_ASSERT_DISPATCH_THREAD("nds_unregister_dispatch");
    CpuCtx& ctx = g_ctx[cpu & 1];
    ctx.banks.erase(
        std::remove_if(ctx.banks.begin(), ctx.banks.end(),
                       [&](const DispatchBank& bank) {
                           return bank.table == t && bank.len == len;
                       }),
        ctx.banks.end());
    std::unordered_set<const DispatchEntry*> removed;
    removed.reserve(len);
    for (unsigned i = 0u; i < len; ++i) removed.insert(&t[i]);
    ctx.dispatch_index.erase(
        std::remove_if(ctx.dispatch_index.begin(), ctx.dispatch_index.end(),
                       [&](const DispatchEntry* entry) {
                           return removed.count(entry) != 0u;
                       }),
        ctx.dispatch_index.end());
    // Downgrade every slot back to the dispatcher; any that pointed into
    // this bank's bodies can no longer be reached without re-resolving.
    link_epoch_bump();
    if (++ctx.dispatch_epoch == 0u) {
        clear_dispatch_cache_cpu(cpu);
        ctx.dispatch_epoch = 1u;
    }
}

extern "C" bool nds_dispatch_static_bank_covers(int cpu, uint32_t pc,
                                                uint8_t thumb) {
    NDS_ASSERT_DISPATCH_THREAD("nds_dispatch_static_bank_covers");
    const CpuCtx& ctx = g_ctx[cpu & 1];
    const uint64_t wanted = (uint64_t{pc} << 1u) | uint64_t{thumb != 0u};
    auto it = std::lower_bound(
        ctx.dispatch_index.begin(), ctx.dispatch_index.end(), wanted,
        [](const DispatchEntry* entry, uint64_t value) {
            return nds_dispatch_entry_key(entry) < value;
        });
    for (; it != ctx.dispatch_index.end() &&
           nds_dispatch_entry_key(*it) == wanted; ++it) {
        const DispatchEntry* entry = *it;
        if (live_overlay_candidate_serial(cpu, entry) != 0u) continue;
        const NdsStaticValidation* validation = entry->validation;
        if (validation &&
            !dispatch_validation_live(validation, pc, thumb != 0u, nullptr)) {
            continue;
        }
        return true;
    }
    return false;
}

extern "C" void nds_register_hle_profile_descriptors(
        int cpu, const NdsHleProfileDescriptor* const* descriptors,
        unsigned count) {
#if defined(NDS_PROFILE_HLE_HEAT)
    for (unsigned index = 0; index < count; ++index) {
        const NdsHleProfileDescriptor* descriptor = descriptors[index];
        if (!descriptor || hle_stats(descriptor)) continue;
        g_hle_heat.push_back(HleHeatStats{descriptor, cpu & 1});
    }
#else
    (void)cpu;
    (void)descriptors;
    (void)count;
#endif
}

extern "C" NdsHleProfileToken runtime_hle_profile_begin(
        const NdsHleProfileDescriptor* descriptor) {
    NdsHleProfileToken token{};
#if defined(NDS_PROFILE_HLE_HEAT)
    HleHeatStats* stats = hle_stats(descriptor);
    if (!stats) return token;
    ++stats->entries;
    if (stats->cpu != static_cast<int>(g_nds_active) ||
        !g_static_guard ||
        descriptor->validation != g_static_guard->validation) {
        ++stats->guard_mismatches;
        return token;
    }
    const uint32_t pc = g_cpu.R[15] & ~1u;
    const bool thumb = (g_cpu.cpsr & CPSR_T_BIT) != 0u;
    if (thumb != (descriptor->thumb != 0u) ||
        pc < descriptor->address || pc >= descriptor->end_address) {
        ++stats->pc_mismatches;
        return token;
    }
    if (pc == descriptor->address) ++stats->start_entries;
    else ++stats->resume_entries;
    token.active = 1u;
    token.depth = ++g_hle_active_depth[g_nds_active];
    if (token.depth > 1u) ++stats->nested_entries;
    stats->max_depth = std::max(stats->max_depth, token.depth);
    const unsigned log2 = hle_sample_log2();
    const uint64_t mask = (uint64_t{1} << log2) - 1u;
    const uint64_t eligible = stats->start_entries + stats->resume_entries;
    if (((eligible - 1u + hle_sample_phase()) & mask) != 0u) return token;
    ++stats->sampled_segments;
    token.sampled = 1u;
    token.irq_epoch = g_hle_irq_epoch[g_nds_active];
    token.instructions = g_insn_count[g_nds_active];
    token.cycles = g_runtime_cycles;
    token.host_ns = hle_host_ns();
#else
    (void)descriptor;
#endif
    return token;
}

extern "C" void runtime_hle_profile_end(
        const NdsHleProfileDescriptor* descriptor,
        NdsHleProfileToken token) {
#if defined(NDS_PROFILE_HLE_HEAT)
    HleHeatStats* stats = hle_stats(descriptor);
    if (!stats || !token.active) return;
    if (g_hle_active_depth[g_nds_active] != token.depth) {
        ++stats->depth_mismatches;
        g_hle_active_depth[g_nds_active] =
            token.depth == 0u ? 0u : token.depth - 1u;
    } else {
        --g_hle_active_depth[g_nds_active];
    }
    const bool unwinding = runtime_unwinding();
    if (unwinding) {
        ++stats->unwind_segments;
    } else {
        ++stats->normal_segments;
    }
    if (!token.sampled) return;
    const uint64_t end_ns = hle_host_ns();
    const uint64_t end_instructions = g_insn_count[g_nds_active];
    const uint64_t end_cycles = g_runtime_cycles;
    if (token.irq_epoch != g_hle_irq_epoch[g_nds_active]) {
        ++stats->irq_rejects;
        return;
    }
    const uint64_t instructions = end_instructions - token.instructions;
    if (instructions == 0u ||
        instructions > descriptor->instruction_count) {
        ++stats->instruction_rejects;
        return;
    }
    ++stats->accepted_samples;
    if (unwinding) ++stats->accepted_unwind_samples;
    else ++stats->accepted_normal_samples;
    stats->host_ns += end_ns - token.host_ns;
    stats->instructions += instructions;
    stats->cycles += end_cycles - token.cycles;
#else
    (void)descriptor;
    (void)token;
#endif
}

extern "C" void nds_set_cycle_cap(unsigned long long cap) {
    g_cycle_cap = cap;
    g_nds_fast_limit = 0u;      // the deadline is derived from the cap
}
extern "C" void nds_reschedule_slice(unsigned long long system_deadline) {
    const unsigned long long cpu_deadline =
        (g_nds_active == NDS_ARM9) ? (system_deadline << 1u) : system_deadline;
    if (g_cycle_cap == 0 || cpu_deadline < g_cycle_cap) {
        g_cycle_cap = cpu_deadline;
        // A device scheduled an EARLIER deadline than the published limit.
        g_nds_fast_limit = 0u;
    }
}

// Tier-3 helpers: does the active CPU have a Tier-1 bank fn at (pc, thumb)?
// And is this slice's cycle cap reached? (The interpreter uses these to
// decide when to hand control back to the dispatcher / scheduler.)
extern "C" int nds_has_bank(uint32_t pc, int thumb) {
    const CpuCtx& c = g_ctx[g_nds_active];
    return lookup_static_cached(c, pc & ~1u, thumb != 0) ? 1 : 0;
}
extern "C" int nds_slice_over(void) {
    return (g_cycle_cap != 0 && g_runtime_cycles >= g_cycle_cap) ? 1 : 0;
}
extern "C" uint32_t nds_exception_base(void) {
    return g_ctx[g_nds_active].exc_base;
}

// Terminal halt for the active CPU: unwind now and don't resume it.
extern "C" void nds_halt(const char* reason) {
    g_nds_terminal = true;
    g_nds_halt_reason = reason;
    request_yield_poll();
}

// Slice control used by the scheduler: arm the cycle cap, clear terminal.
extern "C" void nds_slice_begin(unsigned long long cap) {
    g_nds_terminal = false;
    g_cycle_cap = cap;
    request_yield_poll();
}

// ── Trace ring (dispatch / branch / swi / irq events) ───────────────────
namespace {
constexpr uint32_t kTraceSize = 4096;
RuntimeTraceEntry g_trace[kTraceSize] = {};
uint32_t g_trace_w = 0, g_trace_count = 0, g_trace_seq = 0;
const char* trace_kind_name(uint32_t k) {
    switch (k) {
        case RUNTIME_TRACE_DISPATCH: return "dispatch";
        case RUNTIME_TRACE_EXCHANGE: return "exchange";
        case RUNTIME_TRACE_SWI:      return "swi";
        case RUNTIME_TRACE_MEM_WRITE:return "mem_w";
        case RUNTIME_TRACE_BRANCH:   return "branch";
        case RUNTIME_TRACE_IRQ:      return "irq";
        case RUNTIME_TRACE_CALL:     return "call";
        case RUNTIME_TRACE_MEM_READ: return "mem_r";
        default:                     return "?";
    }
}
}  // namespace

extern "C" uint32_t g_runtime_deep_trace = 1u;

extern "C" void runtime_set_deep_trace(uint32_t on) {
    g_runtime_deep_trace = on ? 1u : 0u;
    nds_insn_hook_recompute();
}

extern "C" void runtime_trace_event(uint32_t kind, uint32_t pc, uint32_t addr,
                                    uint32_t value, uint32_t aux) {
    // The trace ring is diagnostic state, not guest-visible state. Avoid all
    // recording overhead during normal play; the debug server can enable it
    // live with the deep_trace command when a trace is needed.
    if (!g_runtime_deep_trace) return;
    RuntimeTraceEntry& e = g_trace[g_trace_w];
    e.seq = ++g_trace_seq; e.cycles = g_runtime_cycles; e.kind = kind;
    e.pc = pc; e.cpsr = g_cpu.cpsr; e.addr = addr; e.value = value; e.aux = aux;
    e.r0 = g_cpu.R[0]; e.r1 = g_cpu.R[1]; e.r2 = g_cpu.R[2]; e.r3 = g_cpu.R[3];
    e.r4 = g_cpu.R[4]; e.r5 = g_cpu.R[5]; e.r12 = g_cpu.R[12];
    e.r13 = g_cpu.R[13]; e.r14 = g_cpu.R[14];
    g_trace_w = (g_trace_w + 1u) % kTraceSize;
    if (g_trace_count < kTraceSize) ++g_trace_count;
}

extern "C" void runtime_trace_reset(void) {
    g_trace_w = g_trace_count = g_trace_seq = 0;
    g_runtime_cycles = 0;
    // Always on: the per-insn hook owns the insn9/insn7 retired counters
    // (event ordinals for run_to_event / traversal / selftest), which must
    // count in every mode. The deep-trace policy only gates the hook's
    // ring-entry payload (see nds_note_insn_retired).
    g_runtime_insn_trace = 1u;
}

extern "C" void runtime_trace_dump_recent(uint32_t max_entries) {
    if (max_entries > g_trace_count) max_entries = g_trace_count;
    std::fprintf(stderr, "[trace] last %u event(s):\n", max_entries);
    uint32_t start = (g_trace_w + kTraceSize - max_entries) % kTraceSize;
    for (uint32_t i = 0; i < max_entries; ++i) {
        const RuntimeTraceEntry& e = g_trace[(start + i) % kTraceSize];
        std::fprintf(stderr,
            "  #%u %-8s pc=0x%08X cpsr=0x%08X addr=0x%08X val=0x%08X aux=0x%X "
            "sp=0x%08X lr=0x%08X\n",
            e.seq, trace_kind_name(e.kind), e.pc, e.cpsr, e.addr, e.value,
            e.aux, e.r13, e.r14);
    }
}

extern "C" uint32_t runtime_trace_copy_recent(RuntimeTraceEntry* out,
                                              uint32_t max_entries) {
    if (!out || !max_entries) return 0;
    if (max_entries > g_trace_count) max_entries = g_trace_count;
    uint32_t start = (g_trace_w + kTraceSize - max_entries) % kTraceSize;
    for (uint32_t i = 0; i < max_entries; ++i)
        out[i] = g_trace[(start + i) % kTraceSize];
    return max_entries;
}

namespace {
// Pending melonDS ARM::Cycles debt carried across a HALT. This is distinct
// from g_runtime_cycles (the committed CPU timestamp) and is saved/restored by
// the dual-CPU scheduler just like the call-return stack.
uint32_t g_deferred_cycles = 0;
}

// Per-instruction hook, fired once at the top of every recompiled-bank guest
// instruction (g_runtime_insn_trace on). Bumps the active CPU's retired-insn
// counter so insn9/insn7 can anchor the fp-stream bisector. Tier-3 bumps the
// same counters from its own step loop (tier3.cpp).
extern "C" void runtime_insn_fp(void) {
    nds_note_insn_retired(g_nds_active);
    // Timing belongs to each generated instruction's runtime_tick expression,
    // not this observer. ARM7 AddCycles_CD/CDI reconstructs the complete
    // nonsequential code cost; adding the sequential correction here too
    // double-charges loads when ARM7 executes on the 16-bit main-RAM bus.
}
extern "C" void runtime_fp_reset(void) {}
extern "C" uint32_t runtime_fp_count(void) { return 0; }
extern "C" uint32_t runtime_fp_save_file(const char*) { return 0; }

// ── Tick / yield ────────────────────────────────────────────────────────
namespace {
// Publish the deadline. Called only from a faithful scan that has just
// established there is nothing to service right now — the bank path's
// runtime_should_yield_slow, and the Tier-3 loop's own exit scan through
// runtime_publish_fast_limit().
//
// It deliberately does NOT consult g_yield_poll_hint. On the bank path the
// hint is provably zero here (both call sites reach this either inside the
// hint-clear branch or immediately after clearing it), so the check was
// always dead; and Tier 3 does not maintain that flag at all, so requiring
// it would have silently pinned the deadline at zero for every fully
// interpreted stretch — which is exactly what the first Tier-3 measurement
// showed. Instead the four conditions the hint stood for that this function
// could not otherwise see are checked directly below. That makes the
// predicate self-sufficient and identical for both tiers.
//
// The limit is the scheduler's LIVE slice cap and nothing else. That is the
// dual-CPU safety argument in one line: runtime_should_yield already bounds
// every inline run by g_cycle_cap, so a fast run bounded by the same value
// cannot carry this CPU one cycle further than the faithful path would have,
// and therefore cannot cross a rendezvous the peer could observe.
//
// Every disqualifying condition below yields zero — "always take the
// faithful path". A too-small limit is only slow, never wrong.
void publish_fast_limit() {
    if (!g_cycle_fast_limit_enabled ||      // selector: NDS_CYCLE_FAST_LIMIT=0
        !g_cpu_fast_poll ||                 // faithful full-poll reference mode
        g_runtime_break_pc ||               // per-PC predicate, not cycle-based
        g_nds_terminal ||
        g_cycle_cap == 0u ||                // no slice bound to inherit
        g_deferred_cycles != 0u ||          // tick must commit HALT debt
        active_static_code_changed() ||     // guest rewrote executing code
        g_nds_insn_stop ||                  // exact-index observer armed
        nds_event_break_hit() ||            // debug event break already fired
        nds_cpu_halted(g_nds_active) ||     // resumable hardware sleep
        nds_dma_cpu_stalled(g_nds_active) ||  // DMA owns this CPU's bus slot
        g_nds_irq_pending_cache[g_nds_active] != 0u) {  // IRQ due at next tick
        g_nds_fast_limit = 0u;
        return;
    }
    g_nds_fast_limit = g_cycle_cap;
    // Witness for the harness: a selector flag that is set but publishes no
    // deadline proves nothing (the forced-tier3 lesson, one level down).
    // Counted here and not on the per-instruction path, which stays clean.
    ++g_nds_fast_limit_publishes;
}
}  // namespace

// Tier 3's entry point to the same publisher. The interpreter loop runs its
// own equivalent of the faithful scan (event break, instruction stop, halt,
// DMA stall, IRQ, slice boundary) at the bottom of every iteration; when that
// scan finds nothing, this arms the deadline so the next iterations can skip
// the whole set. Without it the deadline is only ever published by the bank
// path, and a fully interpreted stretch never gets one.
extern "C" void runtime_publish_fast_limit(void) { publish_fast_limit(); }

extern "C" void runtime_tick_slow(uint32_t cycles) {
    // Generated-code ticks are guest instruction boundaries. Commit any
    // ARM::Cycles debt carried across HALT together with this instruction.
    g_runtime_cycles += cycles + g_deferred_cycles;
    g_deferred_cycles = 0;
    // Deliver a pending IRQ to the active CPU at this instruction boundary
    // (R15 already points at the next instruction = the return address).
    // runtime_irq masks CPSR.I before vectoring, so it cannot re-enter here
    // while the handler runs.
    const uint32_t pending = g_cpu_fast_poll
        ? g_nds_irq_pending_cache[g_nds_active]
        : nds_irq_pending(g_nds_active);
    if (!(g_cpu.cpsr & CPSR_I_BIT) && pending)
        runtime_irq(g_cpu.R[15]);
}
// Per-instruction unwind: terminal halts only (a guest spin waiting on the
// other core is NOT a fault — it is preempted at a backward branch instead).
namespace {
bool g_preserved_unwind_state_valid = false;
ArmCpuState g_preserved_unwind_state{};
}

extern "C" bool runtime_should_yield_slow(void) {
    // In fast mode rare state transitions eagerly set the hint. While it is
    // clear, only the two per-instruction dynamic predicates remain. The full
    // scan below is unchanged and is also the NDS_CPU_FAST_POLL=0 reference.
    if (g_cpu_fast_poll && !g_yield_poll_hint) {
        const bool break_pc_hit =
            g_runtime_break_pc &&
            (g_cpu.R[15] & ~1u) == (g_runtime_break_pc & ~1u);
        const bool cycle_cap_hit =
            g_cycle_cap != 0 && g_runtime_cycles >= g_cycle_cap;
        // Nothing to service: this is one of the two "all clear" exits, so
        // republish the deadline (see publish_fast_limit). Reaching here with
        // a zero limit is exactly how a re-armed limit gets recomputed.
        if (!break_pc_hit && !cycle_cap_hit) { publish_fast_limit(); return false; }
    }

    // insn7/insn9 anchor reached → stop at this exact instruction (see io.cpp
    // g_nds_insn_stop). The bisector resets per K, so the mid-function unwind
    // (which does not preserve the call-return stack) is never resumed from.
    if (g_nds_insn_stop || nds_event_break_hit()) {
        g_nds_unwinding = 1u;
        return true;
    }
    // HALTCNT/CP15 sleep is a resumable hardware state. Unwind the current
    // static function before its next instruction; the scheduler owns wakeup
    // and timestamp advancement while no guest instructions retire.
    if (nds_cpu_halted(g_nds_active) || nds_dma_cpu_stalled(g_nds_active)) {
        g_nds_unwinding = 1u;
        return true;
    }
    // A guest store touched a page containing the currently executing
    // content-validated function. Stop before the next guest instruction;
    // redispatch will either select a matching generation or enter Tier 3 for
    // the guest's newly written bytes. Never continue stale native semantics.
    if (active_static_code_changed()) {
        g_nds_unwinding = 1u;
        return true;
    }
    if (g_runtime_break_pc &&
        (g_cpu.R[15] & ~1u) == (g_runtime_break_pc & ~1u))
        nds_halt("break pc");
    if (g_cycle_cap != 0 && g_runtime_cycles >= g_cycle_cap) {
        g_nds_unwinding = 1u;
        return true;
    }
    if (g_nds_terminal) { g_nds_unwinding = 1u; return true; }
    g_yield_poll_hint = 0u;
    // The second "all clear" exit: the full faithful scan found nothing to
    // service, so it is safe to publish a fresh deadline.
    publish_fast_limit();
    return false;
}
// Cooperative slice preemption: trips once this slice's cycle cap is
// reached. Checked only at backward branches (loop tops = dispatch
// entries). When it fires it arms g_unwinding so the BL/BLX return-checks
// PRESERVE their pending returns as the host stack unwinds — the call-
// return stack is saved/restored per-CPU by the scheduler, so a spin at
// any call depth can be preempted and cleanly resumed.
extern "C" bool runtime_slice_yield(void) {
    if (g_cycle_cap != 0 && g_runtime_cycles >= g_cycle_cap) {
        g_nds_unwinding = 1u;
        return true;
    }
    return false;
}
// runtime_unwinding() is now a static-inline load over g_nds_unwinding
// (runtime_arm.h); the exported out-of-line symbol that pre-existing live
// shards import lives in runner/src/runtime_abi_shims.cpp.
extern "C" void nds_clear_unwinding(void) {
    g_nds_unwinding = 0u;
    g_preserved_unwind_state_valid = false;
}
void nds_preserve_unwind_state() {
    g_preserved_unwind_state = g_cpu;
    g_preserved_unwind_state_valid = true;
    g_nds_unwinding = 1u;
}
void nds_restore_unwind_state() {
    if (!g_preserved_unwind_state_valid) return;
    g_cpu = g_preserved_unwind_state;
    g_preserved_unwind_state_valid = false;
}

// ── Condition codes (verbatim) ──────────────────────────────────────────
extern "C" int arm_cond_passes(unsigned cond) {
    const uint32_t n = cpsr_n(), z = cpsr_z(), c = cpsr_c(), v = cpsr_v();
    switch (cond & 0xFu) {
        case 0x0: return z != 0;                 case 0x1: return z == 0;
        case 0x2: return c != 0;                 case 0x3: return c == 0;
        case 0x4: return n != 0;                 case 0x5: return n == 0;
        case 0x6: return v != 0;                 case 0x7: return v == 0;
        case 0x8: return (c != 0) && (z == 0);   case 0x9: return (c == 0) || (z != 0);
        case 0xA: return n == v;                 case 0xB: return n != v;
        case 0xC: return (z == 0) && (n == v);   case 0xD: return (z != 0) || (n != v);
        case 0xE: return 1;                      default:  return 0;  // NV
    }
}

// ── Shifters (verbatim) ─────────────────────────────────────────────────
extern "C" uint32_t arm_shift_lsl(uint32_t v, uint32_t n, int sc) {
    if (n == 0) return v;
    if (n >= 32) { if (sc) { uint32_t c = (n == 32) ? (v & 1u) : 0u;
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (c ? CPSR_C_BIT : 0u); } return 0; }
    if (sc) { uint32_t c = (v >> (32u - n)) & 1u;
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (c ? CPSR_C_BIT : 0u); }
    return v << n;
}
extern "C" uint32_t arm_shift_lsr(uint32_t v, uint32_t n, int sc) {
    if (n == 0) return v;
    if (n >= 32) { if (sc) { uint32_t c = (n == 32) ? ((v >> 31) & 1u) : 0u;
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (c ? CPSR_C_BIT : 0u); } return 0; }
    if (sc) { uint32_t c = (v >> (n - 1u)) & 1u;
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (c ? CPSR_C_BIT : 0u); }
    return v >> n;
}
extern "C" uint32_t arm_shift_asr(uint32_t v, uint32_t n, int sc) {
    if (n == 0) return v;
    if (n >= 32) { uint32_t c = (v >> 31) & 1u;
        if (sc) g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (c ? CPSR_C_BIT : 0u);
        return c ? 0xFFFFFFFFu : 0u; }
    if (sc) { uint32_t c = (v >> (n - 1u)) & 1u;
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (c ? CPSR_C_BIT : 0u); }
    return static_cast<uint32_t>(static_cast<int32_t>(v) >> n);
}
extern "C" uint32_t arm_shift_ror(uint32_t v, uint32_t n, int sc) {
    if (n == 0) return v;
    n &= 31u;
    if (n == 0) { if (sc) g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) |
        ((v & 0x80000000u) ? CPSR_C_BIT : 0u); return v; }
    uint32_t r = (v >> n) | (v << (32u - n));
    if (sc) g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | ((r & 0x80000000u) ? CPSR_C_BIT : 0u);
    return r;
}

// CLZ helper (ARMv5).
extern "C" uint32_t runtime_clz(uint32_t v) {
    return v ? static_cast<uint32_t>(__builtin_clz(v)) : 32u;
}
// ARM7TDMI early-termination multiply timing. ARM9 uses its separate static
// numI combine, so this runtime value is ignored on that path.
extern "C" uint32_t runtime_mul_cycles(uint32_t v, uint32_t signed_variant,
                                        uint32_t extra) {
    if (g_nds_active == NDS_ARM9) return 1u;
    if (g_cpu.cpsr & CPSR_T_BIT) {
        // melonDS T_MUL_REG tests the original destination operand by unsigned
        // significant bytes and documents C as destroyed (modeled as clear).
        g_cpu.cpsr &= ~CPSR_C_BIT;
        if (v & 0xFF000000u) return 4u;
        if (v & 0x00FF0000u) return 3u;
        if (v & 0x0000FF00u) return 2u;
        return 1u;
    }

    uint32_t m;
    if (signed_variant) {
        if ((v & 0xFFFFFF00u) == 0u || (v & 0xFFFFFF00u) == 0xFFFFFF00u) m = 1u;
        else if ((v & 0xFFFF0000u) == 0u || (v & 0xFFFF0000u) == 0xFFFF0000u) m = 2u;
        else if ((v & 0xFF000000u) == 0u || (v & 0xFF000000u) == 0xFF000000u) m = 3u;
        else m = 4u;
    } else {
        if ((v & 0xFFFFFF00u) == 0u) m = 1u;
        else if ((v & 0xFFFF0000u) == 0u) m = 2u;
        else if ((v & 0xFF000000u) == 0u) m = 3u;
        else m = 4u;
    }
    return m + extra;
}

// ── Flag updaters (verbatim) ────────────────────────────────────────────
extern "C" void arm_set_nz(uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT; if (r == 0) c |= CPSR_Z_BIT;
    g_cpu.cpsr = c;
}
extern "C" void arm_set_nzc_logic(uint32_t r, uint32_t sh) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT | CPSR_C_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT; if (r == 0) c |= CPSR_Z_BIT;
    if (sh) c |= CPSR_C_BIT; g_cpu.cpsr = c;
}
extern "C" void arm_set_nzcv_add(uint32_t a, uint32_t b, uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT | CPSR_C_BIT | CPSR_V_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT; if (r == 0) c |= CPSR_Z_BIT;
    if (r < a) c |= CPSR_C_BIT;
    if ((~(a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT; g_cpu.cpsr = c;
}
extern "C" void arm_set_nzcv_adc(uint32_t a, uint32_t b, uint32_t ci, uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT | CPSR_C_BIT | CPSR_V_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT; if (r == 0) c |= CPSR_Z_BIT;
    uint64_t w = static_cast<uint64_t>(a) + b + ci; if (w >> 32) c |= CPSR_C_BIT;
    if ((~(a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT; g_cpu.cpsr = c;
}
extern "C" void arm_set_nzcv_sub(uint32_t a, uint32_t b, uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT | CPSR_C_BIT | CPSR_V_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT; if (r == 0) c |= CPSR_Z_BIT;
    if (a >= b) c |= CPSR_C_BIT;
    if (((a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT; g_cpu.cpsr = c;
}
extern "C" void arm_set_nzcv_sbc(uint32_t a, uint32_t b, uint32_t ci, uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT | CPSR_C_BIT | CPSR_V_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT; if (r == 0) c |= CPSR_Z_BIT;
    uint64_t w = static_cast<uint64_t>(a) + (~b & 0xFFFFFFFFu) + ci;
    if (w >> 32) c |= CPSR_C_BIT;
    if (((a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT; g_cpu.cpsr = c;
}

// ── Dispatch ────────────────────────────────────────────────────────────
// `linked`, when non-null, is a resolved literal-transfer slot offered for
// the FIRST loop iteration only. It short-circuits the lookup-cache probe
// and nothing else; a slot that is stale, mode-mismatched, or whose backing
// pages moved simply is not used, and the ordinary lookup then refills it.
namespace {
void runtime_dispatch_impl(uint32_t target_pc, NdsLinkSlot* linked) {
    TailDispatchScope tail;
    for (;;) {
        // One exclusive dispatch-machinery region per loop iteration: opened
        // here, closed the instant before guest code runs (see
        // dispatch_timing.h). The destructor is the backstop for the early
        // returns that never reach the close.
        NdsDispatchRegion cost_region(g_nds_active);
        tail.state.pending = false;
        tail.state.linked = nullptr;
        const bool thumb = (g_cpu.cpsr & CPSR_T_BIT) != 0;
        // CPSR.T, not stale low bits left by the producer, owns the current
        // instruction set. ARM-state BX/BLX targets are word aligned;
        // preserving bit 1 manufactures impossible entries such as BIOS 0x2.
        uint32_t pc = target_pc & (thumb ? ~1u : ~3u);
        ++g_nds_dispatch_stats[g_nds_active].dispatch_total;
        // WHICH guest code is being entered (pc_profile.h, NDS_PC_HOT_EXEC).
        // This one line is the whole exec population, and it sits here rather
        // than at any of the callers because THIS is the funnel: every way
        // guest code can be reached -- a static bank body, a live-overlay bank,
        // a Tier-3 stretch, a BIOS body, a B2 link-slot transfer, an
        // exchange/literal transfer, a scheduler slice resume -- arrives as an
        // iteration of this loop and increments the counter above. A hook at
        // any caller would cover one path; a hook here cannot miss one.
        //
        // Deliberately NOT also at scheduler.cpp's resume_dispatch increment:
        // a resume calls straight into this loop, so noting there as well would
        // count resumed entries twice and inflate exactly the PCs a slice
        // happens to restart on -- the park population's bias, reintroduced
        // into the population that exists to avoid it.
        //
        // No clock, no region. The gate lives inside the note (a decrement and
        // a predicted branch on the 99.2 percent of entries it skips) so the
        // cost that remains is charged to the enclosing NDS_EMU_EXEC_* bucket,
        // where the work it samples already is.
        nds_pc_profile_note_exec(g_nds_active, pc);
        // Slice-preemption point. `pc` is a dispatch entry, so this is a safe
        // place to yield to the scheduler — including for loops whose back-edge
        // is an INDIRECT transfer (BX / pop pc / computed jump) and therefore
        // has no backward-branch yield site. runtime_slice_yield() arms the
        // unwind so pending BL/BLX returns are preserved.
        if (runtime_slice_yield()) {
            ++g_nds_dispatch_stats[g_nds_active].dispatch_slice_yield;
            g_cpu.R[15] = pc;
            return;
        }
        // CPSR.T owns the instruction-set state. Some interpreted BX/POP paths
        // preserve the interworking bit in their target value; generated bank
        // prologues and the architectural register view require aligned R15.
        g_cpu.R[15] = pc;
        runtime_trace_event(RUNTIME_TRACE_DISPATCH, pc, target_pc, 0, 0);
        const CpuCtx& c = g_ctx[g_nds_active];
        StaticGuardScope guard_scope;
        // Same construction order and same operations as the former
        // `guard_scope.call(lookup_static_cached(c, pc, thumb))`: the guard
        // scope is built first, then the lookup runs, then the guard is
        // prepared. Only the hand-off to guest code is split out, so the
        // cost region can close before it.
        // Cache-path breakdown of this dispatch's lookup, nested inside
        // cost_region. The outcome is only knowable after the fact, so it is
        // read off the exact NdsDispatchStats counters the lookup itself
        // bumps -- and only when this lookup is actually being sampled, so an
        // unsampled dispatch pays one countdown and nothing more. Timing the
        // lookup inside lookup_static_cached_impl instead would also time
        // Tier 3's per-instruction poll, which outnumbers dispatch lookups
        // ~8:1 and cost 1.7 percent of emulation time for a number that is
        // not a dispatch cost.
        const CachedStaticLookup* found = nullptr;
        const LinkGuard* link_guard = nullptr;
        bool entering_static = false;
        NdsLinkSlot* const offered = linked;
        linked = nullptr;   // the offer is for this iteration only
        if (offered && link_enabled() &&
            link_slot_admit(offered, &link_guard)) {
            ++g_link_hits[g_nds_active];
            live_overlay_note_cached_hit(link_guard ? link_guard->serial : 0u);
            entering_static =
                guard_scope.prepare_linked(offered->fn, link_guard);
        } else {
            NdsDispatchCacheRegion cache_region(g_nds_active);
            const NdsDispatchStats& counters =
                g_nds_dispatch_stats[g_nds_active];
            const uint64_t hit_before = counters.cache_hit;
            const uint64_t absent_before = counters.cache_hit_absent;
            found = lookup_static_cached(c, pc, thumb);
            const NdsDispatchCachePath cache_path =
                counters.cache_hit != hit_before
                    ? NDS_DISPATCH_CACHE_HIT
                    : (counters.cache_hit_absent != absent_before
                           ? NDS_DISPATCH_CACHE_HIT_ABSENT
                           : NDS_DISPATCH_CACHE_SLOW);
            ++g_nds_dispatch_timing[g_nds_active].cache_events[cache_path];
            cache_region.close_as(cache_path);
            // Refill the offered slot from the authoritative resolution.
            // Only when the slot's own target is the PC actually resolved
            // here: the tail hand-off can carry a slot whose fall-through
            // target the loop has since replaced.
            if (offered && link_enabled() &&
                (offered->target_pc & ~1u) == pc &&
                ((offered->target_pc & 1u) != 0u) == thumb) {
                ++g_link_resolves[g_nds_active];
                link_slot_fill(offered, found);
            } else if (offered) {
                ++g_link_falls[g_nds_active];
            }
            entering_static = guard_scope.prepare(found);
        }
        cost_region.close();
        if (entering_static) {
            guard_scope.invoke();
            if (!tail.state.pending) return;
            target_pc = tail.state.target_pc;
            linked = tail.state.linked;
            continue;
        }
        if (g_discover_static_misses && static_bios_pc(pc)) {
            runtime_discovery_note_static(pc, thumb ? 1u : 0u);
            tier3_run(pc);
            return;
        }
        // Tier 3: code copied into RAM at runtime (firmware boot, menu, and the
        // ITCM-resident IRQ handler) has no static bank — run the guest's OWN
        // bytes through the interpreter (PRINCIPLES.md "the one exception"),
        // never an HLE model. The bus owns the memory map (covers ITCM mirror).
        const bool mapped_writable = bus_addr_is_writable_ram(pc);
        const bool has_provenance = mapped_writable &&
            bus_range_has_write_provenance(pc, thumb ? 2u : 4u);
        if (nds_dispatch_miss_decision(mapped_writable, has_provenance) ==
            NdsDispatchMissDecision::Tier3) {
            live_overlay_note_lookup(g_nds_active, pc, target_pc, g_cpu.R[14],
                                     g_cpu.cpsr, nullptr, nullptr, 0u,
                                     "tier3");
            tier3_run(pc);
            return;
        }
        if (mapped_writable) tier3_note_clean_ram_reject();
        live_overlay_note_lookup(g_nds_active, pc, target_pc, g_cpu.R[14],
                                 g_cpu.cpsr, nullptr, nullptr, 0u,
                                 "fatal");
        runtime_dispatch_miss(target_pc);
        return;
    }
}
}  // namespace

extern "C" void runtime_dispatch(uint32_t target_pc) {
    runtime_dispatch_impl(target_pc, nullptr);
}

extern "C" void runtime_discovery_note_static(uint32_t pc, uint32_t thumb) {
    pc &= ~1u;
    if (!static_bios_pc(pc)) return;
    const uint64_t key = (uint64_t(g_nds_active) << 33u) |
                         (uint64_t(thumb != 0u) << 32u) | pc;
    for (uint64_t seen : g_discovery_seen)
        if (seen == key) return;
    g_discovery_seen.push_back(key);

    const char* cpu = (g_nds_active == NDS_ARM9) ? "arm9" : "arm7";
    const char* mode = thumb ? "thumb" : "arm";
    std::fprintf(stderr,
        "[static-discovery] cpu=%s pc=0x%08X %s lr=0x%08X\n",
        cpu, pc, mode, g_cpu.R[14]);
    if (std::FILE* f = std::fopen("dispatch_candidates.log", "ab")) {
        std::fprintf(f,
            "# cpu=%s lr=0x%08X (discovery interpreter; validate before promotion)\n"
            "[[entry_point]]\naddr = 0x%08X\nmode = \"%s\"\n"
            "kind = \"runtime_candidate\"\n\n",
            cpu, g_cpu.R[14], pc, mode);
        std::fclose(f);
    }
}
extern "C" void runtime_dispatch_with_exchange(uint32_t target_pc) {
    ++g_nds_dispatch_stats[g_nds_active].dispatch_exchange;
    nds_dispatch_tag(NDS_DISPATCH_CLASS_EXCHANGE);
    if (target_pc & 1u) g_cpu.cpsr |= CPSR_T_BIT; else g_cpu.cpsr &= ~CPSR_T_BIT;
    const uint32_t pc = target_pc & ((target_pc & 1u) ? ~1u : ~3u);
    runtime_trace_event(RUNTIME_TRACE_EXCHANGE, pc, target_pc, 0, 0);
    runtime_dispatch(target_pc);
}
extern "C" void runtime_dispatch_literal_branch(uint32_t target_pc) {
    ++g_nds_dispatch_stats[g_nds_active].literal_branch;
    nds_dispatch_tag(NDS_DISPATCH_CLASS_LITERAL_BRANCH);
    runtime_dispatch(target_pc);
}
extern "C" void runtime_dispatch_literal_call(uint32_t target_pc) {
    ++g_nds_dispatch_stats[g_nds_active].literal_call;
    nds_dispatch_tag(NDS_DISPATCH_CLASS_LITERAL_CALL);
    runtime_dispatch(target_pc);
}
extern "C" void runtime_dispatch_literal_fallthrough(uint32_t target_pc) {
    ++g_nds_dispatch_stats[g_nds_active].literal_fallthrough;
    if (g_nds_active == NDS_ARM9 && g_tail_dispatch &&
        g_tail_dispatch->cpu == static_cast<int>(g_nds_active)) {
        g_tail_dispatch->target_pc = target_pc;
        g_tail_dispatch->pending = true;
        return;
    }
    nds_dispatch_tag(NDS_DISPATCH_CLASS_LITERAL_FALLTHROUGH);
    runtime_dispatch(target_pc);
}

// ── B2 linked literal transfers ─────────────────────────────────────────
// These are the linked twins of the three runtime_dispatch_literal_*
// entries above and are deliberately NOT shortcuts around them. Each keeps
// the identical guest-visible sequence -- the class counter, the class tag,
// dispatch_total, the slice-yield preemption point, the R15 publication,
// the trace event, the tail-dispatch hand-off, and the nested static guard
// -- and offers the dispatcher a pre-resolved target so the only work that
// disappears is the g_dispatch_cache probe.
//
// The tempting further step, calling slot->fn() straight from here the way
// an intra-shard direct call does, was REJECTED: it deletes the slice-yield
// point that every cross-shard literal transfer performs today, which moves
// scheduler preemption to different guest instructions and is guest
// visible. For a literal BRANCH it would additionally turn a guest tail
// transfer into a host call, growing the stack without bound around a
// cross-function guest loop -- the hazard docs/host_optimization_strategy.md
// B2 names explicitly.
extern "C" void runtime_link_branch(NdsLinkSlot* slot) {
    ++g_nds_dispatch_stats[g_nds_active].literal_branch;
    nds_dispatch_tag(NDS_DISPATCH_CLASS_LITERAL_BRANCH);
    runtime_dispatch_impl(slot->target_pc & ~1u, slot);
}

extern "C" void runtime_link_call(NdsLinkSlot* slot) {
    ++g_nds_dispatch_stats[g_nds_active].literal_call;
    nds_dispatch_tag(NDS_DISPATCH_CLASS_LITERAL_CALL);
    runtime_dispatch_impl(slot->target_pc & ~1u, slot);
}

extern "C" void runtime_link_fallthrough(NdsLinkSlot* slot) {
    const uint32_t target_pc = slot->target_pc & ~1u;
    ++g_nds_dispatch_stats[g_nds_active].literal_fallthrough;
    if (g_nds_active == NDS_ARM9 && g_tail_dispatch &&
        g_tail_dispatch->cpu == static_cast<int>(g_nds_active)) {
        g_tail_dispatch->target_pc = target_pc;
        g_tail_dispatch->linked = slot;
        g_tail_dispatch->pending = true;
        return;
    }
    nds_dispatch_tag(NDS_DISPATCH_CLASS_LITERAL_FALLTHROUGH);
    runtime_dispatch_impl(target_pc, slot);
}

// ── Inline-leaf admission (beads-yjp.67) ────────────────────────────────
// A BL site that expanded a tiny `bx lr` leaf inline asks here whether the
// copy may run for this transfer. Every check below is one the linked
// dispatch path already performs before entering the same body, plus one
// that path gets for free and this one must state: that the resolution is
// the body we inlined.
//
//  1. NDS_INLINE_LEAVES / NDS_DIRECT_LINK off -> never admit. Either switch
//     alone restores the faithful dispatch path for the whole process.
//  2. link_slot_ready: the slot was resolved under the CURRENT link epoch
//     (so no bank has registered or unregistered since) and CPSR.T still
//     agrees with the mode the target was resolved in.
//  3. slot->fn == expected: the ranked winner for this address is the very
//     generated body whose bytes were inlined. An overlapping bank that
//     out-ranks it — a live shard, an alias bank — fails here and takes the
//     dispatcher, which enters ITS body rather than our copy.
//  4. link_guard_live: the winner's content guard still matches the live
//     backing-page generations, i.e. the guest has not written the leaf's
//     bytes since they were proven identical. A write downgrades the site to
//     the dispatcher on the very next call, which re-resolves and re-proves.
//
// The expansion therefore cannot outlive its proof, and a refusal is always
// correct: the caller runs runtime_link_call instead.
extern "C" int runtime_inline_leaf_admit(const NdsLinkSlot* slot,
                                         void (*expected)(void)) {
    if (g_inline_leaves && link_enabled() && slot->fn == expected &&
        link_slot_ready(slot)) {
        const LinkGuard* guard = static_cast<const LinkGuard*>(slot->guard);
        if (!guard || link_guard_live(guard)) {
            // Keep live_overlay's native-hit accounting identical to the
            // linked dispatch this replaces. Without it a live bank whose
            // bodies are reached only through inlined leaves would report
            // zero native hits while running natively — an instrument going
            // dark exactly where the new path took over. Free for static
            // banks: their serial is 0 and the note returns immediately.
            live_overlay_note_cached_hit(guard ? guard->serial : 0u);
            ++g_inline_leaf_admits[g_nds_active];
            return 1;
        }
    }
    ++g_inline_leaf_falls[g_nds_active];
    return 0;
}

extern "C" const char* nds_direct_link_json(void) {
    static std::string out;
    char buf[520];
    std::snprintf(buf, sizeof buf,
        "{\"enabled\":%s,\"deep_trace\":%s,\"epoch\":%u,"
        "\"guards\":%zu,\"inline_leaves\":%s,"
        "\"arm9\":{\"hits\":%llu,\"resolves\":%llu,\"skipped\":%llu,"
        "\"inline_leaf_admits\":%llu,\"inline_leaf_falls\":%llu},"
        "\"arm7\":{\"hits\":%llu,\"resolves\":%llu,\"skipped\":%llu,"
        "\"inline_leaf_admits\":%llu,\"inline_leaf_falls\":%llu}}",
        g_direct_link ? "true" : "false",
        g_runtime_deep_trace ? "true" : "false",
        g_link_epoch, g_link_guards.size(),
        g_inline_leaves ? "true" : "false",
        static_cast<unsigned long long>(g_link_hits[0]),
        static_cast<unsigned long long>(g_link_resolves[0]),
        static_cast<unsigned long long>(g_link_falls[0]),
        static_cast<unsigned long long>(g_inline_leaf_admits[0]),
        static_cast<unsigned long long>(g_inline_leaf_falls[0]),
        static_cast<unsigned long long>(g_link_hits[1]),
        static_cast<unsigned long long>(g_link_resolves[1]),
        static_cast<unsigned long long>(g_link_falls[1]),
        static_cast<unsigned long long>(g_inline_leaf_admits[1]),
        static_cast<unsigned long long>(g_inline_leaf_falls[1]));
    out = buf;
    return out.c_str();
}

extern "C" void runtime_live_transfer(uint32_t source_pc, uint32_t target_pc,
                                      uint32_t transfer_type) {
    live_overlay_note_transfer(g_nds_active, source_pc, target_pc, g_cpu.R[14],
                               g_cpu.cpsr, transfer_type);
}

extern "C" void runtime_dispatch_bad_entry(uint32_t target_pc) {
    const bool thumb = (g_cpu.cpsr & CPSR_T_BIT) != 0;
    const uint32_t pc = target_pc & (thumb ? ~1u : ~3u);
    g_cpu.R[15] = pc;
    const bool mapped_writable = bus_addr_is_writable_ram(pc);
    const bool has_provenance = mapped_writable &&
        bus_range_has_write_provenance(pc, thumb ? 2u : 4u);
    if (nds_dispatch_miss_decision(mapped_writable, has_provenance) ==
        NdsDispatchMissDecision::Tier3) {
        live_overlay_note_lookup(g_nds_active, pc, target_pc, g_cpu.R[14],
                                 g_cpu.cpsr, nullptr, nullptr, 0u,
                                 "bad-entry-tier3");
        tier3_run(pc);
        return;
    }
    live_overlay_note_lookup(g_nds_active, pc, target_pc, g_cpu.R[14],
                             g_cpu.cpsr, nullptr, nullptr, 0u,
                             "bad-entry-fatal");
    runtime_dispatch_miss(target_pc);
}

extern "C" void runtime_dispatch_miss(uint32_t target_pc) {
    const char* cpu = (g_nds_active == NDS_ARM9) ? "arm9" : "arm7";
    const bool thumb = (g_cpu.cpsr & CPSR_T_BIT) != 0;
    const char* mode = thumb ? "thumb" : "arm";
    const uint32_t t = target_pc & (thumb ? ~1u : ~3u);
    std::fprintf(stderr,
        "[dispatch-miss] cpu=%s pc=0x%08X %s (lr=0x%08X)\n",
        cpu, t, mode, g_cpu.R[14]);

    // A miss INSIDE recompiled static ROM is a real non-function-start entry
    // PC (a BIOS-ABI landing pad reached by a runtime-computed branch), NOT a
    // Tier-3/HLE case. The finder's landing-pad discovery should normally seed
    // it; if one slips through, it is a genuine entry fact — declare it. Never
    // route static ROM to Tier-3.
    const bool arm9 = (g_nds_active == NDS_ARM9);
    const bool in_static_rom =
        (arm9 && t >= 0xFFFF0000u) || (!arm9 && t < 0x00004000u);

    // Localize the pad to its containing recompiled function.
    uint32_t rs = 0, re = 0;
    const CpuCtx& cc = g_ctx[g_nds_active];
    bool have_range = false;
    for (const DispatchBank& bank : cc.banks) {
        if (bracket_static_range(bank.table, bank.len, t, &rs, &re)) {
            have_range = true;
            break;
        }
    }

    if (in_static_rom) {
        std::fprintf(stderr,
            "  [!] dispatch miss INSIDE executable static %s BIOS ROM at 0x%08X\n"
            "      real non-function-start entry reached by a computed branch.\n"
            "      The finder's landing-pad discovery should seed it; if not,\n"
            "      add this entry_point to the bank config (do NOT interpret ROM):\n",
            arm9 ? "ARM9" : "ARM7", t);
        if (have_range)
            std::fprintf(stderr,
                "      # containing static range: 0x%08X..0x%08X\n", rs, re);
        std::fprintf(stderr,
            "        [[entry_point]]\n"
            "        addr = 0x%08X\n"
            "        mode = \"%s\"\n"
            "        kind = \"runtime_confirmed\"\n",
            t, mode);
    }

    // Discovery-loop log (CLAUDE.md BUILD LOOP step 5): a copy-pasteable
    // [[entry_point]] block per miss, directly appendable to the config.
    if (nds_diagnostics_enabled()) {
        const std::string dispatch_log =
            nds_diagnostics_dispatch_miss_log_path();
        if (std::FILE* f = std::fopen(dispatch_log.c_str(), "ab")) {
            std::fprintf(f, "# cpu=%s pc=0x%08X lr=0x%08X%s\n",
                         cpu, t, g_cpu.R[14],
                         in_static_rom
                             ? " (static ROM non-function-start entry)"
                             : "");
            if (in_static_rom && have_range)
                std::fprintf(f,
                             "#   containing static range 0x%08X..0x%08X\n",
                             rs, re);
            std::fprintf(f,
                "[[entry_point]]\naddr = 0x%08X\nmode = \"%s\"\n"
                "kind = \"runtime_confirmed\"\n\n",
                t, mode);
            std::fclose(f);
        }
    }

    // RAM-resident target (copied firmware code): this is the Tier-3 dirty-RAM
    // case — dump the bytes there to show what would run.
    if (bus_addr_is_writable_ram(t)) {
        std::fprintf(stderr, "  [ram-provenance] generation=%u (%s)\n",
                     bus_exec_page_generation(t),
                     bus_addr_has_write_provenance(t) ? "written" : "clean");
        std::fprintf(stderr, "  [ram@0x%08X]", t);
        for (int i = 0; i < 16; i += 4)
            std::fprintf(stderr, " %08X", bus_read_u32(t + i));
        std::fprintf(stderr, "\n");
    }

    runtime_trace_dump_recent(16);
    nds_halt("dispatch miss");
}

// ── Call-return stack (verbatim) ────────────────────────────────────────
namespace {
constexpr uint32_t kCRS = NDS_RUNTIME_CALL_STACK_CAPACITY;
uint32_t g_crs[kCRS] = {};
uint32_t g_crs_depth = 0;
}  // namespace
extern "C" void runtime_call_push_return(uint32_t return_pc) {
    uint32_t pc = return_pc & ~1u;
    uint32_t key = pc | ((g_cpu.cpsr & CPSR_T_BIT) ? 1u : 0u);
    if (g_crs_depth >= kCRS) {
        const uint32_t keep = kCRS / 2u;
        runtime_trace_event(RUNTIME_TRACE_CALL, pc,
                            g_crs[g_crs_depth - 1u] & ~1u,
                            g_crs_depth, 6u);
        std::memmove(g_crs, g_crs + (g_crs_depth - keep),
                     keep * sizeof(g_crs[0]));
        g_crs_depth = keep;
    }
    ++g_nds_dispatch_stats[g_nds_active].crs_push;
    g_crs[g_crs_depth++] = key;
    runtime_trace_event(RUNTIME_TRACE_CALL, pc, pc, g_crs_depth, 1u);
}
extern "C" int runtime_call_should_return(uint32_t target_pc) {
    uint32_t pc = target_pc & ~1u;
    uint32_t key = pc | ((g_cpu.cpsr & CPSR_T_BIT) ? 1u : 0u);
    auto& stats = g_nds_dispatch_stats[g_nds_active];
    for (uint32_t i = g_crs_depth; i != 0; --i) {
        ++stats.crs_scan_iters;
        if (g_crs[i - 1u] == key) {
            ++stats.crs_hit;
            runtime_trace_event(RUNTIME_TRACE_CALL, pc, pc, g_crs_depth,
                                (i == g_crs_depth) ? 2u : 5u);
            g_crs_depth = i - 1u; return 1;
        }
    }
    ++stats.crs_miss;
    runtime_trace_event(RUNTIME_TRACE_CALL, pc,
        g_crs_depth ? (g_crs[g_crs_depth - 1u] & ~1u) : 0xFFFFFFFFu,
        g_crs_depth, 3u);
    return 0;
}
extern "C" void runtime_call_cancel_return(uint32_t return_pc) {
    uint32_t pc = return_pc & ~1u;
    if (g_crs_depth && (g_crs[g_crs_depth - 1u] & ~1u) == pc) {
        runtime_trace_event(RUNTIME_TRACE_CALL, pc, pc, g_crs_depth, 4u);
        --g_crs_depth;
    }
}
extern "C" uint32_t runtime_call_stack_depth(void) { return g_crs_depth; }
extern "C" const uint32_t* runtime_call_stack_data(void) { return g_crs; }
extern "C" void runtime_call_stack_restore(const uint32_t* e, uint32_t d) {
    if (d > kCRS) d = kCRS; g_crs_depth = d;
    for (uint32_t i = 0; i < d; ++i) g_crs[i] = e[i];
}
extern "C" uint32_t runtime_deferred_cycles(void) {
    return g_deferred_cycles;
}
extern "C" void runtime_deferred_cycles_set(uint32_t cycles) {
    g_deferred_cycles = cycles;
    // HALT/DMA debt must be committed by the next tick, which only the
    // faithful path does. (The scheduler re-arms the slice right after this,
    // which would zero the limit anyway; this makes it unconditional.)
    if (cycles) g_nds_fast_limit = 0u;
}
extern "C" uint32_t runtime_deferred_cycles_take(void) {
    const uint32_t cycles = g_deferred_cycles;
    g_deferred_cycles = 0;
    return cycles;
}

// ── PSR transfer + banking (verbatim) ───────────────────────────────────
namespace {
unsigned mode_to_bank(uint32_t mode) {
    switch (mode & 0x1Fu) {
        case 0x11u: return ARM_BANK_FIQ;        case 0x12u: return ARM_BANK_IRQ;
        case 0x13u: return ARM_BANK_SUPERVISOR; case 0x17u: return ARM_BANK_ABORT;
        case 0x1Bu: return ARM_BANK_UNDEFINED;  default:    return ARM_BANK_USER;
    }
}
void bank_out(unsigned ob, uint32_t om) {
    g_cpu.banked_sp[ob] = g_cpu.R[13]; g_cpu.banked_lr[ob] = g_cpu.R[14];
    if ((om & 0x1Fu) == 0x11u) for (unsigned i = 0; i < 5; ++i) g_cpu.r8_12_fiq[i] = g_cpu.R[8 + i];
    else                       for (unsigned i = 0; i < 5; ++i) g_cpu.r8_12_user[i] = g_cpu.R[8 + i];
}
void bank_in(unsigned nb, uint32_t nm) {
    g_cpu.R[13] = g_cpu.banked_sp[nb]; g_cpu.R[14] = g_cpu.banked_lr[nb];
    if ((nm & 0x1Fu) == 0x11u) for (unsigned i = 0; i < 5; ++i) g_cpu.R[8 + i] = g_cpu.r8_12_fiq[i];
    else                       for (unsigned i = 0; i < 5; ++i) g_cpu.R[8 + i] = g_cpu.r8_12_user[i];
}
}  // namespace

extern "C" uint32_t runtime_read_user_reg(uint32_t reg) {
    reg &= 15u; uint32_t mode = g_cpu.cpsr & 0x1Fu;
    if (reg < 8u || reg == 15u) return g_cpu.R[reg];
    if (reg < 13u) return (mode == 0x11u) ? g_cpu.r8_12_user[reg - 8u] : g_cpu.R[reg];
    if (mode == 0x10u || mode == 0x1Fu) return g_cpu.R[reg];
    return (reg == 13u) ? g_cpu.banked_sp[ARM_BANK_USER] : g_cpu.banked_lr[ARM_BANK_USER];
}
extern "C" void runtime_write_user_reg(uint32_t reg, uint32_t value) {
    reg &= 15u; uint32_t mode = g_cpu.cpsr & 0x1Fu;
    if (reg < 8u || reg == 15u) { g_cpu.R[reg] = value; return; }
    if (reg < 13u) { if (mode == 0x11u) g_cpu.r8_12_user[reg - 8u] = value; else g_cpu.R[reg] = value; return; }
    if (mode == 0x10u || mode == 0x1Fu) g_cpu.R[reg] = value;
    else if (reg == 13u) g_cpu.banked_sp[ARM_BANK_USER] = value;
    else                 g_cpu.banked_lr[ARM_BANK_USER] = value;
}
extern "C" uint32_t runtime_mrs_cpsr(void) { return g_cpu.cpsr; }
extern "C" uint32_t runtime_mrs_spsr(void) { return g_cpu.banked_spsr[mode_to_bank(g_cpu.cpsr)]; }
extern "C" void runtime_msr_cpsr(uint32_t value, uint32_t mask) {
    uint32_t bw = 0;
    if (mask & 1u) bw |= 0x000000FFu; if (mask & 2u) bw |= 0x0000FF00u;
    if (mask & 4u) bw |= 0x00FF0000u; if (mask & 8u) bw |= 0xFF000000u;
    if ((g_cpu.cpsr & 0x1Fu) == 0x10u) bw &= 0xFF000000u;  // User: flags only
    uint32_t oc = g_cpu.cpsr, nc = (oc & ~bw) | (value & bw);
    unsigned ob = mode_to_bank(oc), nb = mode_to_bank(nc);
    g_cpu.cpsr = nc;
    if (ob != nb) { bank_out(ob, oc); bank_in(nb, nc); }
}
extern "C" void runtime_msr_spsr(uint32_t value, uint32_t mask) {
    unsigned bank = mode_to_bank(g_cpu.cpsr);
    if (bank == ARM_BANK_USER) return;
    uint32_t bw = 0;
    if (mask & 1u) bw |= 0x000000FFu; if (mask & 2u) bw |= 0x0000FF00u;
    if (mask & 4u) bw |= 0x00FF0000u; if (mask & 8u) bw |= 0xFF000000u;
    uint32_t old = g_cpu.banked_spsr[bank];
    g_cpu.banked_spsr[bank] = (old & ~bw) | (value & bw);
}
extern "C" void runtime_exception_return(uint32_t new_pc) {
    uint32_t oc = g_cpu.cpsr, om = oc & 0x1Fu;
    if (om == 0x10u || om == 0x1Fu) { g_cpu.R[15] = new_pc; return; }
    unsigned ob = mode_to_bank(oc); uint32_t spsr = g_cpu.banked_spsr[ob];
    bank_out(ob, oc); g_cpu.cpsr = spsr; bank_in(mode_to_bank(spsr), spsr);
    g_cpu.R[15] = new_pc;
}
extern "C" void runtime_restore_cpsr_from_spsr(void) {
    uint32_t oc = g_cpu.cpsr, om = oc & 0x1Fu;
    if (om == 0x10u || om == 0x1Fu) return;
    unsigned ob = mode_to_bank(oc); uint32_t spsr = g_cpu.banked_spsr[ob];
    bank_out(ob, oc); g_cpu.cpsr = spsr; bank_in(mode_to_bank(spsr), spsr);
}

// ── SWI / IRQ (per-CPU exception base) ──────────────────────────────────
extern "C" void runtime_swi(uint32_t swi_imm) {
    uint32_t ret = g_cpu.R[15], saved = g_cpu.cpsr;
    runtime_trace_event(RUNTIME_TRACE_SWI, ret, swi_imm, saved, 0);
    uint32_t nc = (saved & ~(0x1Fu | CPSR_T_BIT)) | 0x13u | CPSR_I_BIT;
    unsigned ob = mode_to_bank(saved), nb = mode_to_bank(nc);
    if (ob != nb) { bank_out(ob, saved); bank_in(nb, nc); }
    g_cpu.cpsr = nc; g_cpu.banked_spsr[nb] = saved; g_cpu.R[14] = ret;
    uint32_t base = g_ctx[g_nds_active].exc_base;
    g_cpu.R[15] = base + 0x08u;
    // ARM7: flat 3 (2S+1N), matching the static base table exactly (unchanged).
    // ARM9: melonDS charges SWI entry as a taken branch to the exception vector
    // — 2*numC(exception base+8) (no class cost of its own; the SWI instruction
    // itself already ticked nothing here, see arm_codegen.cpp emit_swi). CPSR.T
    // was just cleared above (ARM mode, exception entry), so runtime_code_cycles
    // sees the correct target-mode state.
    runtime_tick(g_nds_active == NDS_ARM9
                     ? arm9_refill_cycles(base + 0x08u)
                     : arm7_refill_cycles(base + 0x08u));
    ++g_nds_dispatch_stats[g_nds_active].exception_dispatch;
    nds_dispatch_tag(NDS_DISPATCH_CLASS_EXCEPTION);
    runtime_dispatch(base + 0x08u);
}
extern "C" void runtime_irq(uint32_t return_address) {
#if defined(NDS_PROFILE_HLE_HEAT)
    ++g_hle_irq_epoch[g_nds_active];
#endif
    nds_note_irq_accept(g_nds_active, return_address);
    uint32_t saved = g_cpu.cpsr;
    runtime_trace_event(RUNTIME_TRACE_IRQ, return_address, 0, saved, 0);
    // melonDS ARM::TriggerIRQ clears CPSR[7:0] then installs 0xD2: IRQ mode,
    // ARM state, with both IRQ and FIQ masked.  Preserving the old F bit makes
    // the BIOS IRQ prologue observably diverge on its first exception.
    uint32_t nc = (saved & ~0xFFu) | 0xD2u;
    unsigned ob = mode_to_bank(saved), nb = mode_to_bank(nc);
    if (ob != nb) { bank_out(ob, saved); bank_in(nb, nc); }
    g_cpu.cpsr = nc; g_cpu.banked_spsr[nb] = saved; g_cpu.R[14] = return_address + 4u;
    uint32_t base = g_ctx[g_nds_active].exc_base;
    g_cpu.R[15] = base + 0x18u;
    const uint32_t refill = g_nds_active == NDS_ARM9
        ? arm9_refill_cycles(base + 0x18u)
        : arm7_refill_cycles(base + 0x18u);
    if (g_deferred_cycles) {
        // IRQ accepted directly out of HALT: melonDS JumpTo appends the refill
        // to the still-pending ARM::Cycles debt. The first vector instruction
        // commits both; it must not become visible before that instruction.
        g_deferred_cycles += refill;
    } else {
        runtime_tick(refill);
    }
    ++g_nds_dispatch_stats[g_nds_active].exception_dispatch;
    nds_dispatch_tag(NDS_DISPATCH_CLASS_EXCEPTION);
    runtime_dispatch(base + 0x18u);
}

// ── Fallback / lifecycle ────────────────────────────────────────────────
extern "C" void runtime_unimplemented_op(const char* op_name, uint32_t pc) {
    std::fprintf(stderr, "[UNIMPLEMENTED] op=%s pc=0x%08X cpu=%s cpsr=0x%08X\n",
                 op_name, pc, g_nds_active == NDS_ARM9 ? "arm9" : "arm7", g_cpu.cpsr);
    runtime_trace_dump_recent(12);
    nds_halt("unimplemented op");
}
extern "C" void runtime_init(void*) {
    g_cpu_fast_poll = configured_cpu_fast_poll();
    g_cycle_fast_limit_enabled = configured_cycle_fast_limit();
    request_yield_poll();       // re-arm site: machine reset
    g_crs_depth = 0;
    g_deferred_cycles = 0;
    g_discovery_seen.clear();
    for (CpuCtx& ctx : g_ctx) {
        ctx.banks.clear();
        ctx.dispatch_index.clear();
        ctx.dispatch_epoch = 1u;
        ctx.exc_base = 0u;
    }
    g_static_guard = nullptr;
    clear_dispatch_cache_all();
    // dispatch_epoch restarts at 1 on reset, so the per-CPU epoch alone
    // cannot distinguish a pre-reset slot from a post-reset one. The link
    // epoch is monotonic across resets and closes that hole.
    g_direct_link = configured_direct_link();
    link_epoch_bump();
    g_link_hits[0] = g_link_hits[1] = 0u;
    g_link_resolves[0] = g_link_resolves[1] = 0u;
    g_link_falls[0] = g_link_falls[1] = 0u;
    g_inline_leaves = configured_inline_leaves();
    g_inline_leaf_admits[0] = g_inline_leaf_admits[1] = 0u;
    g_inline_leaf_falls[0] = g_inline_leaf_falls[1] = 0u;
#if defined(NDS_PROFILE_HLE_HEAT)
    g_hle_heat.clear();
    g_hle_irq_epoch[0] = 0u;
    g_hle_irq_epoch[1] = 0u;
    g_hle_active_depth[0] = 0u;
    g_hle_active_depth[1] = 0u;
#endif
    tier3_reset();
}
extern "C" void runtime_shutdown(void) {
    g_crs_depth = 0;
    g_deferred_cycles = 0;
}

void runtime_savestate_export(NdsRuntimeSaveState* out) {
    if (!out) return;
    out->insn_count[0] = g_insn_count[0];
    out->insn_count[1] = g_insn_count[1];
    out->force_tier3_misses = g_nds_force_tier3_misses;
    out->active_cpu = static_cast<uint32_t>(g_nds_active);
    out->force_tier3 = g_nds_force_tier3 ? 1u : 0u;
}

bool runtime_savestate_import(const NdsRuntimeSaveState& in,
                              std::string* error) {
    if (in.active_cpu > 1u) {
        if (error) *error = "savestate active CPU is invalid";
        return false;
    }
    g_insn_count[0] = in.insn_count[0];
    g_insn_count[1] = in.insn_count[1];
    g_nds_force_tier3_misses = in.force_tier3_misses;
    g_nds_active = in.active_cpu ? NDS_ARM7 : NDS_ARM9;
    g_nds_force_tier3 = in.force_tier3 != 0u;
    g_runtime_cycles = 0;
    g_nds_terminal = false;
    g_nds_halt_reason = nullptr;
    g_nds_unwinding = 0;
    runtime_savestate_invalidate_host_caches();
    return true;
}

void runtime_savestate_invalidate_host_caches() {
    request_yield_poll();
    g_static_guard = nullptr;
    clear_dispatch_cache_all();
    link_epoch_bump();
}

void runtime_savestate_reset_host_history() {
    g_insn_count[0] = g_insn_count[1] = 0u;
    g_nds_force_tier3_misses = 0u;
    g_nds_dispatch_stats[0] = {};
    g_nds_dispatch_stats[1] = {};
    g_link_hits[0] = g_link_hits[1] = 0u;
    g_link_resolves[0] = g_link_resolves[1] = 0u;
    g_link_falls[0] = g_link_falls[1] = 0u;
    runtime_trace_reset();
}

std::string nds_hle_profile_json() {
#if !defined(NDS_PROFILE_HLE_HEAT)
    return "{\"enabled\":false,\"routines\":[]}";
#else
    std::string out = "{\"enabled\":true,\"sample_log2\":" +
        std::to_string(hle_sample_log2()) + ",\"sample_phase\":" +
        std::to_string(hle_sample_phase()) + ",\"routines\":[";
    bool first = true;
    for (const auto& stats : g_hle_heat) {
        if (!first) out.push_back(',');
        first = false;
        const NdsHleProfileDescriptor& descriptor = *stats.descriptor;
        out += "{\"id\":";
        append_json_string(out, descriptor.id);
        out += ",\"bank\":";
        append_json_string(out, descriptor.bank);
        out += ",\"cpu\":" + std::to_string(stats.cpu) +
               ",\"address\":" + std::to_string(descriptor.address) +
               ",\"end_address\":" +
                   std::to_string(descriptor.end_address) +
               ",\"thumb\":" + std::to_string(descriptor.thumb) +
               ",\"instruction_count\":" +
                   std::to_string(descriptor.instruction_count) +
               ",\"content_sha1\":";
        append_json_string(out, descriptor.content_sha1);
        out += ",\"entries\":" + std::to_string(stats.entries) +
               ",\"start_entries\":" +
                   std::to_string(stats.start_entries) +
               ",\"resume_entries\":" +
                   std::to_string(stats.resume_entries) +
               ",\"normal_segments\":" +
                   std::to_string(stats.normal_segments) +
               ",\"unwind_segments\":" +
                   std::to_string(stats.unwind_segments) +
               ",\"sampled_segments\":" +
                   std::to_string(stats.sampled_segments) +
               ",\"accepted_samples\":" +
                   std::to_string(stats.accepted_samples) +
               ",\"accepted_normal_samples\":" +
                   std::to_string(stats.accepted_normal_samples) +
               ",\"accepted_unwind_samples\":" +
                   std::to_string(stats.accepted_unwind_samples) +
               ",\"irq_rejects\":" +
                   std::to_string(stats.irq_rejects) +
               ",\"instruction_rejects\":" +
                   std::to_string(stats.instruction_rejects) +
               ",\"guard_mismatches\":" +
                   std::to_string(stats.guard_mismatches) +
               ",\"pc_mismatches\":" +
                   std::to_string(stats.pc_mismatches) +
               ",\"nested_entries\":" +
                   std::to_string(stats.nested_entries) +
               ",\"depth_mismatches\":" +
                   std::to_string(stats.depth_mismatches) +
               ",\"max_depth\":" +
                   std::to_string(stats.max_depth) +
               ",\"sample_host_ns\":" +
                   std::to_string(stats.host_ns) +
               ",\"sample_instructions\":" +
                   std::to_string(stats.instructions) +
               ",\"sample_cycles\":" +
                   std::to_string(stats.cycles) + "}";
    }
    out += "]}";
    return out;
#endif
}
