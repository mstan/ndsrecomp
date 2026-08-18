#include "dispatch_lookup.h"

#include <cstdio>
#include <cstring>

namespace {

void old_body() {}
void new_body() {}
void root_body() {}
void callee_body() {}

const uint8_t old_bytes[8] = {};
const uint8_t new_bytes[8] = {1, 2, 3, 4, 5, 6, 7, 8};
const uint8_t callee_bytes[8] = {8, 7, 6, 5, 4, 3, 2, 1};

const NdsStaticValidation old_validation{0x02000000u, sizeof(old_bytes),
                                         old_bytes};
const NdsStaticValidation new_validation{0x02000000u, sizeof(new_bytes),
                                         new_bytes};
const NdsStaticValidation callee_validation{0x02000020u,
                                            sizeof(callee_bytes),
                                            callee_bytes};

struct ValidationState {
    const NdsStaticValidation* live[4] = {};
    unsigned count = 0u;
};

bool validation_live(const NdsStaticValidation* validation, uint32_t pc,
                     bool thumb, void* user) {
    auto* state = static_cast<ValidationState*>(user);
    if (!nds_dispatch_validation_owns_entry(validation, pc, thumb))
        return false;
    for (unsigned i = 0u; i < state->count; ++i) {
        if (state->live[i] == validation) return true;
    }
    return false;
}

struct ByteState {
    uint8_t bytes[0x40] = {};
};

bool validation_live_bytes(const NdsStaticValidation* validation, uint32_t pc,
                           bool thumb, void* user) {
    if (!nds_dispatch_validation_owns_entry(validation, pc, thumb))
        return false;
    auto* state = static_cast<ByteState*>(user);
    if (!validation->dependencies || validation->dependency_count == 0u)
        return false;
    for (uint32_t i = 0u; i < validation->dependency_count; ++i) {
        const NdsStaticValidationRange& range = validation->dependencies[i];
        if (range.addr < 0x02000000u ||
            uint64_t{range.addr - 0x02000000u} + range.size >
                sizeof(state->bytes) ||
            std::memcmp(state->bytes + (range.addr - 0x02000000u),
                        range.expected, range.size) != 0) {
            return false;
        }
    }
    return true;
}

bool expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

}  // namespace

int main() {
    const NdsDispatchEntry old_rows[] = {
        {0x02000000u, 0u, old_body, &old_validation},
    };
    const NdsDispatchEntry new_rows[] = {
        {0x02000000u, 0u, new_body, &new_validation},
    };
    const NdsDispatchBankView chain[] = {
        {old_rows, 1u},
        {new_rows, 1u},
    };
    ValidationState state{{&old_validation, &new_validation}, 2u};
    NdsDispatchLookupResult result = nds_dispatch_lookup_chain(
        chain, 2u, 0x02000000u, false, validation_live, &state);
    if (!expect(result.selected == &new_rows[0],
                "newest matching duplicate candidate should win"))
        return 1;
    if (!expect(result.candidate_count == 2u,
                "duplicate candidates across banks should be counted"))
        return 1;

    state = ValidationState{{&old_validation}, 1u};
    result = nds_dispatch_lookup_chain(
        chain, 2u, 0x02000000u, false, validation_live, &state);
    if (!expect(result.selected == &old_rows[0],
                "older candidate should run when newer identity is stale"))
        return 1;
    if (!expect(result.inactive == &new_rows[0],
                "stale newer candidate should be reported as inactive"))
        return 1;

    // Generation A -> B -> A must remain a candidate-chain selection, not a
    // destructive replacement of whichever implementation was seen first.
    state = ValidationState{{&new_validation}, 1u};
    result = nds_dispatch_lookup_chain(
        chain, 2u, 0x02000000u, false, validation_live, &state);
    if (!expect(result.selected == &new_rows[0],
                "generation B should select the B candidate"))
        return 1;
    state = ValidationState{{&old_validation}, 1u};
    result = nds_dispatch_lookup_chain(
        chain, 2u, 0x02000000u, false, validation_live, &state);
    if (!expect(result.selected == &old_rows[0],
                "returning to generation A should immediately reuse A"))
        return 1;

    std::vector<const NdsDispatchEntry*> indexed;
    nds_dispatch_index_add(indexed, old_rows, 1u);
    nds_dispatch_index_add(indexed, new_rows, 1u);
    state = ValidationState{{&new_validation}, 1u};
    result = nds_dispatch_lookup_index(
        indexed, 0x02000000u, false, validation_live, &state);
    if (!expect(result.selected == &new_rows[0] &&
                    result.candidate_count == 2u,
                "flat runtime index should retain the candidate chain"))
        return 1;
    state = ValidationState{{&old_validation}, 1u};
    result = nds_dispatch_lookup_index(
        indexed, 0x02000000u, false, validation_live, &state);
    if (!expect(result.selected == &old_rows[0],
                "flat runtime index should reuse an older live identity"))
        return 1;

    const NdsDispatchEntry root_rows[] = {
        {0x02000000u, 0u, root_body, &old_validation},
        {0x02000020u, 0u, callee_body, &callee_validation},
    };
    const NdsDispatchBankView root_bank[] = {{root_rows, 2u}};
    state = ValidationState{{&old_validation}, 1u};
    result = nds_dispatch_lookup_chain(
        root_bank, 1u, 0x02000000u, false, validation_live, &state);
    if (!expect(result.selected == &root_rows[0],
                "matching root candidate should be callable"))
        return 1;
    result = nds_dispatch_lookup_chain(
        root_bank, 1u, 0x02000020u, false, validation_live, &state);
    if (!expect(result.selected == nullptr &&
                result.inactive == &root_rows[1],
                "stale direct callee must reject instead of running native"))
        return 1;

    const uint8_t changed_callee[8] = {9, 7, 6, 5, 4, 3, 2, 1};
    const NdsStaticValidationRange closure_a_ranges[] = {
        {0x02000000u, sizeof(old_bytes), old_bytes},
        {0x02000020u, sizeof(callee_bytes), callee_bytes},
    };
    const NdsStaticValidationRange closure_b_ranges[] = {
        {0x02000000u, sizeof(old_bytes), old_bytes},
        {0x02000020u, sizeof(changed_callee), changed_callee},
    };
    const NdsStaticValidation closure_a{
        0x02000000u, sizeof(old_bytes), old_bytes,
        closure_a_ranges, 2u};
    const NdsStaticValidation closure_b{
        0x02000000u, sizeof(old_bytes), old_bytes,
        closure_b_ranges, 2u};
    const NdsDispatchEntry closure_a_rows[] = {
        {0x02000000u, 0u, old_body, &closure_a},
    };
    const NdsDispatchEntry closure_b_rows[] = {
        {0x02000000u, 0u, new_body, &closure_b},
    };
    const NdsDispatchBankView closure_chain[] = {
        {closure_a_rows, 1u}, {closure_b_rows, 1u},
    };
    ByteState byte_state{};
    std::memcpy(byte_state.bytes + 0x20u, changed_callee,
                sizeof(changed_callee));
    result = nds_dispatch_lookup_chain(
        closure_chain, 2u, 0x02000000u, false,
        validation_live_bytes, &byte_state);
    if (!expect(result.selected == &closure_b_rows[0] &&
                    result.inactive == &closure_a_rows[0],
                "matching root must reject a candidate whose direct callee changed"))
        return 1;
    std::memcpy(byte_state.bytes + 0x20u, callee_bytes,
                sizeof(callee_bytes));
    result = nds_dispatch_lookup_chain(
        closure_chain, 2u, 0x02000000u, false,
        validation_live_bytes, &byte_state);
    if (!expect(result.selected == &closure_a_rows[0],
                "restoring the callee generation should reuse its closure"))
        return 1;

    result = nds_dispatch_lookup_chain(
        root_bank, 1u, 0x90900004u, false, validation_live, &state);
    if (!expect(result.selected == nullptr && result.inactive == nullptr &&
                result.candidate_count == 0u,
                "bogus unmapped target should not resolve to a candidate"))
        return 1;

    if (!expect(nds_dispatch_miss_decision(true, true) ==
                    NdsDispatchMissDecision::Tier3,
                "written mapped RAM miss should enter Tier 3"))
        return 1;
    if (!expect(nds_dispatch_miss_decision(true, false) ==
                    NdsDispatchMissDecision::Fatal,
                "clean mapped RAM miss should remain fatal"))
        return 1;
    if (!expect(nds_dispatch_miss_decision(false, false) ==
                    NdsDispatchMissDecision::Fatal,
                "unmapped bogus miss should remain fatal"))
        return 1;

    std::puts("PASS: dispatch lookup resolves live candidate chains safely");
    return 0;
}
