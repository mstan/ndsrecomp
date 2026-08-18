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
    for (unsigned i = lo; i < len && table[i].addr == pc; ++i) {
        if ((table[i].thumb != 0u) != thumb) continue;
        if (candidate_count) ++*candidate_count;
        const NdsStaticValidation* validation = table[i].validation;
        if (validation && (!validation_live ||
                           !validation_live(validation, pc, thumb, user))) {
            if (inactive_candidate) *inactive_candidate = &table[i];
            continue;
        }
        return &table[i];
    }
    return nullptr;
}

inline NdsDispatchLookupResult nds_dispatch_lookup_chain(
        const NdsDispatchBankView* banks, unsigned bank_count, uint32_t pc,
        bool thumb, NdsDispatchValidationFn validation_live, void* user) {
    NdsDispatchLookupResult result{};
    for (unsigned i = 0u; i < bank_count; ++i) {
        const NdsDispatchEntry* hit = nds_dispatch_lookup_one(
            banks[i].table, banks[i].len, pc, thumb,
            &result.candidate_count, &result.inactive,
            validation_live, user);
        if (!hit) continue;
        if (!hit->validation) {
            result.selected = hit;
            return result;
        }
        result.selected = hit;
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
        // Existing rows sort first for an equal key. A subsequent lookup walks
        // the complete run and therefore selects the newest matching identity.
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
    for (; it != index.end() && nds_dispatch_entry_key(*it) == wanted; ++it) {
        const NdsDispatchEntry* entry = *it;
        ++result.candidate_count;
        if (!entry->validation) {
            result.selected = entry;
            return result;
        }
        if (validation_live &&
            !validation_live(entry->validation, pc, thumb, user)) {
            result.inactive = entry;
            continue;
        }
        result.selected = entry;
    }
    return result;
}

inline NdsDispatchMissDecision nds_dispatch_miss_decision(
        bool mapped_writable_ram, bool has_write_provenance) {
    return (mapped_writable_ram && has_write_provenance)
        ? NdsDispatchMissDecision::Tier3
        : NdsDispatchMissDecision::Fatal;
}
