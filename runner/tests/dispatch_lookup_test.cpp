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
    // beads-lqa.40: two candidates that co-validate and own the SAME span are
    // a rank tie, and a tie keeps the FIRST-registered row. The old contract
    // (last registered wins) is gone: it made bank registration order, not the
    // proven span, decide which body serves an address.
    ValidationState state{{&old_validation, &new_validation}, 2u};
    NdsDispatchLookupResult result = nds_dispatch_lookup_chain(
        chain, 2u, 0x02000000u, false, validation_live, &state);
    if (!expect(result.selected == &old_rows[0],
                "co-validating candidates of equal span keep the "
                "first-registered row"))
        return 1;
    if (!expect(result.candidate_count == 2u,
                "duplicate candidates across banks should be counted"))
        return 1;
    const NdsDispatchBankView reversed_chain[] = {
        {new_rows, 1u},
        {old_rows, 1u},
    };
    result = nds_dispatch_lookup_chain(
        reversed_chain, 2u, 0x02000000u, false, validation_live, &state);
    if (!expect(result.selected == &new_rows[0],
                "the tie-break must follow registration order, not identity"))
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

    // ---- beads-lqa.40: rank by owned span, not by registration order -------
    //
    // MPH's capture-derived runtime banks hold two-instruction landing pads
    // whose expected bytes are copied from the same guest image as the main
    // closure's whole-function rows. Both validate at the same instant on
    // ~88k addresses, and under the old last-registered-wins walk the FRAGMENT
    // won: execution left through the dispatcher every two instructions.
    static const uint8_t function_bytes[64] = {};
    static const uint8_t fragment_bytes[8] = {};
    static const NdsStaticValidation function_validation{
        0x02010000u, sizeof(function_bytes), function_bytes};
    static const NdsStaticValidation fragment_validation{
        0x02010020u, sizeof(fragment_bytes), fragment_bytes};
    const NdsDispatchEntry function_rows[] = {
        {0x02010020u, 0u, root_body, &function_validation},
    };
    const NdsDispatchEntry fragment_rows[] = {
        {0x02010020u, 0u, callee_body, &fragment_validation},
    };
    ValidationState both{{&function_validation, &fragment_validation}, 2u};

    const NdsDispatchBankView fragment_last[] = {
        {function_rows, 1u}, {fragment_rows, 1u},
    };
    result = nds_dispatch_lookup_chain(
        fragment_last, 2u, 0x02010020u, false, validation_live, &both);
    if (!expect(result.selected == &function_rows[0],
                "a co-validating fragment registered LAST must not shadow the "
                "whole function"))
        return 1;
    const NdsDispatchBankView fragment_first[] = {
        {fragment_rows, 1u}, {function_rows, 1u},
    };
    result = nds_dispatch_lookup_chain(
        fragment_first, 2u, 0x02010020u, false, validation_live, &both);
    if (!expect(result.selected == &function_rows[0],
                "a co-validating fragment registered FIRST must not shadow the "
                "whole function either"))
        return 1;

    // The runner resolves through the flat index, so pin it there too, in the
    // exact MPH registration order (main closure, then the capture bank).
    std::vector<const NdsDispatchEntry*> span_index;
    nds_dispatch_index_add(span_index, function_rows, 1u);
    nds_dispatch_index_add(span_index, fragment_rows, 1u);
    result = nds_dispatch_lookup_index(
        span_index, 0x02010020u, false, validation_live, &both);
    if (!expect(result.selected == &function_rows[0] &&
                    result.candidate_count == 2u,
                "the flat index must select the largest owned span"))
        return 1;
    // ... and the fragment is still the answer when the function's identity is
    // stale, so ranking costs no coverage.
    ValidationState fragment_only{{&fragment_validation}, 1u};
    result = nds_dispatch_lookup_index(
        span_index, 0x02010020u, false, validation_live, &fragment_only);
    if (!expect(result.selected == &fragment_rows[0] &&
                    result.inactive == &function_rows[0],
                "a fragment must still run when the larger span is not live"))
        return 1;

    // ---- beads-lqa.40: an unvalidated row is live but proves nothing -------
    //
    // Immutable BIOS/ROM banks leave validation null. Such a row is
    // unconditionally live yet declares no span, so it ranks below every live
    // validating candidate and wins only when none of them is live.
    const NdsDispatchEntry immutable_rows[] = {
        {0x02010020u, 0u, old_body, nullptr},
    };
    std::vector<const NdsDispatchEntry*> null_index;
    nds_dispatch_index_add(null_index, immutable_rows, 1u);
    nds_dispatch_index_add(null_index, function_rows, 1u);
    result = nds_dispatch_lookup_index(
        null_index, 0x02010020u, false, validation_live, &both);
    if (!expect(result.selected == &function_rows[0],
                "a live validating candidate must outrank an unvalidated row"))
        return 1;
    ValidationState none{{}, 0u};
    result = nds_dispatch_lookup_index(
        null_index, 0x02010020u, false, validation_live, &none);
    if (!expect(result.selected == &immutable_rows[0] &&
                    result.inactive == &function_rows[0],
                "an unvalidated row must serve the address when nothing "
                "validates"))
        return 1;
    // Registration order must not change that verdict either way.
    std::vector<const NdsDispatchEntry*> null_last_index;
    nds_dispatch_index_add(null_last_index, function_rows, 1u);
    nds_dispatch_index_add(null_last_index, immutable_rows, 1u);
    result = nds_dispatch_lookup_index(
        null_last_index, 0x02010020u, false, validation_live, &both);
    if (!expect(result.selected == &function_rows[0],
                "an unvalidated row registered last must not shadow a live "
                "validating candidate"))
        return 1;

    // ---- beads-lqa.40: equal spans resolve deterministically ---------------
    ValidationState tie_state{{&old_validation, &new_validation}, 2u};
    std::vector<const NdsDispatchEntry*> tie_index;
    nds_dispatch_index_add(tie_index, old_rows, 1u);
    nds_dispatch_index_add(tie_index, new_rows, 1u);
    for (unsigned repeat = 0u; repeat < 4u; ++repeat) {
        result = nds_dispatch_lookup_index(
            tie_index, 0x02000000u, false, validation_live, &tie_state);
        if (!expect(result.selected == &old_rows[0],
                    "an equal-span tie must resolve to the first-registered "
                    "row on every lookup"))
            return 1;
    }

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

    // ---- beads-yjp.56 / .62: what counts as COVERED for coverage filing ----
    //
    // A Tier-3 observation is filed only when nobody already holds usable
    // output for the address. "Somebody has a row here" is not that test: a
    // row whose expected bytes belong to a different overlay generation owns
    // the address and can execute none of what is actually resident, so an
    // owned-but-STALE address is real work and must file. Getting this wrong
    // hands the live compiler empty batches while the interpreter carries the
    // whole scene -- runs that finish clean with +0 shards.
    //
    // The lookup already draws the line: a stale owner lands in `inactive`
    // and never in `selected`. These cases pin that the FILING rule consumes
    // `selected` (live-valid) and not presence, and that dormancy outranks it.
    std::vector<const NdsDispatchEntry*> filing_index;
    nds_dispatch_index_add(filing_index, old_rows, 1u);

    // (a) The row is present and owns 0x02000000, but its bytes are not the
    //     bytes in guest memory. Uncovered: FILE.
    ValidationState stale_state{{}, 0u};
    result = nds_dispatch_lookup_index(
        filing_index, 0x02000000u, false, validation_live, &stale_state);
    if (!expect(result.selected == nullptr &&
                result.inactive == &old_rows[0] &&
                result.candidate_count == 1u,
                "an owning row whose bytes are stale must be reported "
                "inactive, never selected"))
        return 1;
    if (!expect(nds_tier3_file_decision(false, result.selected != nullptr) ==
                    NdsTier3FileDecision::File,
                "an owned-but-STALE address is uncovered work and must file, "
                "or the live compiler is commissioned with empty batches "
                "while the interpreter carries the scene"))
        return 1;

    // (b) Same row, now byte-identical with what is resident. Covered: SKIP.
    ValidationState live_state{{&old_validation}, 1u};
    result = nds_dispatch_lookup_index(
        filing_index, 0x02000000u, false, validation_live, &live_state);
    if (!expect(result.selected == &old_rows[0],
                "a live-valid owning row must be selected"))
        return 1;
    if (!expect(nds_tier3_file_decision(false, result.selected != nullptr) ==
                    NdsTier3FileDecision::SkipLiveBank,
                "an address a live-valid row serves must not be filed again"))
        return 1;

    // (c) Dormancy outranks both: compiled output already exists for the
    //     page, it just could not be activated in the scene that was up when
    //     it was preflighted. Filing it commissions a byte-identical shard
    //     that will be deferred identically.
    if (!expect(nds_tier3_file_decision(true, false) ==
                    NdsTier3FileDecision::SkipDormant &&
                nds_tier3_file_decision(true, true) ==
                    NdsTier3FileDecision::SkipDormant,
                "a dormant candidate must suppress filing whether or not a "
                "live-valid row also covers the address"))
        return 1;

    std::puts("PASS: dispatch lookup resolves live candidate chains safely; "
              "Tier-3 filing skips only dormant or live-valid coverage");
    return 0;
}
