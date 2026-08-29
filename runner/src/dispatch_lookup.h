#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "runtime_arm.h"

struct NdsDispatchBankView {
    const NdsDispatchEntry* table = nullptr;
    unsigned len = 0u;
};

struct NdsDispatchLookupResult {
    const NdsDispatchEntry* selected = nullptr;
    const NdsDispatchEntry* inactive = nullptr;
    uint32_t candidate_count = 0u;
};

enum class NdsDispatchMissDecision : uint8_t {
    Tier3,
    Fatal,
};

using NdsDispatchValidationFn =
    bool (*)(const NdsStaticValidation* validation, uint32_t pc, bool thumb,
             void* user);

inline bool nds_dispatch_validation_owns_entry(
        const NdsStaticValidation* validation, uint32_t pc, bool thumb) {
    if (!validation || !validation->expected || validation->size == 0u)
        return false;
    const uint32_t step = thumb ? 2u : 4u;
    const uint64_t begin = validation->addr;
    const uint64_t end = begin + validation->size;
    const uint64_t entry_end = uint64_t{pc} + step;
    return end <= 0x1'0000'0000ull && pc >= begin && entry_end <= end;
}

// SELECTION CONTRACT (beads-lqa.40)
//
// Several banks may hold a row for one (addr, thumb) key and all of them may
// validate at the same instant: their expected bytes are copied from images
// that agree over the address, so byte equality proves nothing about WHICH
// body should run. The winner is the candidate that owns the LARGEST proven
// span around pc, i.e. the one that executes the most guest code before it
// has to come back through the dispatcher.
//
// Rank of a candidate = the size of the concrete [addr, size) resume body its
// NdsStaticValidation proves, which is exactly the region
// nds_dispatch_validation_owns_entry() tests pc against. A dependency-closure
// validation keeps that same per-row body in addr/size and points `dependencies`
// at the SHARED transfer closure of the whole bank generation (recompiler
// emit: one merged range list, one NdsStaticValidation per function), so the
// closure ranges say nothing about how much code THIS row owns and are
// deliberately not part of the rank.
//
// A row with no validation at all (an immutable BIOS/ROM bank) is
// unconditionally live but declares no span: it proves nothing about the bytes
// currently at pc, so it ranks BELOW every live validating candidate and is
// selected only when no validating candidate is live. Rank 0 expresses that.
//
// Ties keep the FIRST-registered candidate: registration order is the runner's
// documented bank priority (alphabetical *_dispatch.c glob plus
// registration-order.txt), so two rows that prove the same span resolve the
// same way on every run.
inline uint32_t nds_dispatch_candidate_rank(const NdsDispatchEntry* entry) {
    return entry->validation ? entry->validation->size : 0u;
}

// True when `candidate` must replace the current best. First-registered wins a
// tie, so only a strictly larger owned span displaces an incumbent.
inline bool nds_dispatch_rank_wins(const NdsDispatchEntry* best,
                                   uint32_t best_rank,
                                   uint32_t candidate_rank) {
    return !best || candidate_rank > best_rank;
}

inline const NdsDispatchEntry* nds_dispatch_lookup_one(
        const NdsDispatchEntry* table, unsigned len, uint32_t pc, bool thumb,
        uint32_t* candidate_count, const NdsDispatchEntry** inactive_candidate,
        NdsDispatchValidationFn validation_live, void* user) {
    if (!table) return nullptr;
    unsigned lo = 0u, hi = len;
    while (lo < hi) {
        const unsigned mid = (lo + hi) >> 1u;
        if (table[mid].addr < pc) lo = mid + 1u;
        else                      hi = mid;
    }
    const NdsDispatchEntry* best = nullptr;
    uint32_t best_rank = 0u;
    for (unsigned i = lo; i < len && table[i].addr == pc; ++i) {
        if ((table[i].thumb != 0u) != thumb) continue;
        if (candidate_count) ++*candidate_count;
        const NdsStaticValidation* validation = table[i].validation;
        if (validation && (!validation_live ||
                           !validation_live(validation, pc, thumb, user))) {
            if (inactive_candidate) *inactive_candidate = &table[i];
            continue;
        }
        const uint32_t rank = nds_dispatch_candidate_rank(&table[i]);
        if (nds_dispatch_rank_wins(best, best_rank, rank)) {
            best = &table[i];
            best_rank = rank;
        }
    }
    return best;
}

inline NdsDispatchLookupResult nds_dispatch_lookup_chain(
        const NdsDispatchBankView* banks, unsigned bank_count, uint32_t pc,
        bool thumb, NdsDispatchValidationFn validation_live, void* user) {
    NdsDispatchLookupResult result{};
    uint32_t best_rank = 0u;
    for (unsigned i = 0u; i < bank_count; ++i) {
        const NdsDispatchEntry* hit = nds_dispatch_lookup_one(
            banks[i].table, banks[i].len, pc, thumb,
            &result.candidate_count, &result.inactive,
            validation_live, user);
        if (!hit) continue;
        const uint32_t rank = nds_dispatch_candidate_rank(hit);
        if (nds_dispatch_rank_wins(result.selected, best_rank, rank)) {
            result.selected = hit;
            best_rank = rank;
        }
    }
    return result;
}

inline uint64_t nds_dispatch_entry_key(const NdsDispatchEntry* entry) {
    return (uint64_t{entry->addr} << 1u) |
        uint64_t{entry->thumb != 0u};
}

inline void nds_dispatch_index_add(
        std::vector<const NdsDispatchEntry*>& index,
        const NdsDispatchEntry* table, unsigned len) {
    std::vector<const NdsDispatchEntry*> merged;
    merged.reserve(index.size() + len);
    std::size_t old_index = 0u;
    unsigned new_index = 0u;
    while (old_index < index.size() && new_index < len) {
        const NdsDispatchEntry* old_entry = index[old_index];
        const NdsDispatchEntry* new_entry = &table[new_index];
        // Existing rows sort first for an equal key, so the run at a key is in
        // registration order. Selection is NOT positional: the lookup walks the
        // whole run and applies the rank contract above (largest owned span
        // wins; first-registered breaks a tie). Registration order therefore
        // only decides ties, which is why it must stay stable here.
        if (nds_dispatch_entry_key(old_entry) <=
            nds_dispatch_entry_key(new_entry)) {
            merged.push_back(old_entry);
            ++old_index;
        } else {
            merged.push_back(new_entry);
            ++new_index;
        }
    }
    merged.insert(merged.end(), index.begin() + old_index, index.end());
    for (; new_index < len; ++new_index) merged.push_back(&table[new_index]);
    index.swap(merged);
}

inline NdsDispatchLookupResult nds_dispatch_lookup_index(
        const std::vector<const NdsDispatchEntry*>& index,
        uint32_t pc, bool thumb,
        NdsDispatchValidationFn validation_live, void* user) {
    NdsDispatchLookupResult result{};
    const uint64_t wanted = (uint64_t{pc} << 1u) | uint64_t{thumb};
    auto it = std::lower_bound(
        index.begin(), index.end(), wanted,
        [](const NdsDispatchEntry* entry, uint64_t value) {
            return nds_dispatch_entry_key(entry) < value;
        });
    uint32_t best_rank = 0u;
    for (; it != index.end() && nds_dispatch_entry_key(*it) == wanted; ++it) {
        const NdsDispatchEntry* entry = *it;
        ++result.candidate_count;
        if (entry->validation && validation_live &&
            !validation_live(entry->validation, pc, thumb, user)) {
            result.inactive = entry;
            continue;
        }
        const uint32_t rank = nds_dispatch_candidate_rank(entry);
        if (nds_dispatch_rank_wins(result.selected, best_rank, rank)) {
            result.selected = entry;
            best_rank = rank;
        }
    }
    return result;
}

// ---- Tier-3 coverage filing rule (beads-yjp.56 / beads-yjp.62) ------------
//
// Filing a Tier-3 observation is what puts its page on the live compiler's
// work list, so both reasons to withhold one belong in a single rule, and
// their ORDER is load-bearing:
//
//   1. DORMANT wins. Compiled output for the page already exists; it merely
//      could not be activated in whatever scene was up when it was
//      preflighted. Commissioning the compiler for it yields a byte-identical
//      shard that is deferred identically -- which is how a fresh install
//      spent twelve compile runs reproducing shards its own cache held.
//   2. Otherwise skip only if a bank row at this address is LIVE-VALID.
//
// The second clause is the one that is easy to get wrong, and getting it
// wrong is silent in both directions. "A row exists at this address" is NOT
// coverage: a row whose expected bytes belong to a different overlay
// generation owns the address without being able to execute one instruction
// of what is actually resident. An owned-but-STALE address is real,
// uncovered work and MUST file, or the compiler is handed empty batches while
// the interpreter carries the whole scene. nds_dispatch_lookup_index()
// already draws exactly this line -- `selected` is only ever a candidate the
// validation predicate accepted, and `inactive` is where an owning-but-stale
// row goes -- so the caller asks for a SELECTED row, never for presence.
enum class NdsTier3FileDecision : uint8_t {
    File,
    SkipDormant,
    SkipLiveBank,
};

inline NdsTier3FileDecision nds_tier3_file_decision(bool dormant_covered,
                                                    bool live_valid_bank) {
    if (dormant_covered) return NdsTier3FileDecision::SkipDormant;
    if (live_valid_bank) return NdsTier3FileDecision::SkipLiveBank;
    return NdsTier3FileDecision::File;
}

inline NdsDispatchMissDecision nds_dispatch_miss_decision(
        bool mapped_writable_ram, bool has_write_provenance) {
    return (mapped_writable_ram && has_write_provenance)
        ? NdsDispatchMissDecision::Tier3
        : NdsDispatchMissDecision::Fatal;
}
