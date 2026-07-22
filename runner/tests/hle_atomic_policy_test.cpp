#include "hle_atomic_policy.h"

#include <cstdint>
#include <cstdio>

namespace {

int expect(bool condition, const char* message) {
    if (condition) return 0;
    std::fprintf(stderr, "%s\n", message);
    return 1;
}

}  // namespace

int main() {
    int failures = 0;
    failures += expect(
        nds_hle_atomic_policy_allows(0u, 0u, 100u, 0u, UINT32_MAX),
        "unlimited clean window was rejected");
    failures += expect(
        !nds_hle_atomic_policy_allows(1u, 0u, 100u, 1000u, 1u),
        "runtime blocker was accepted");
    failures += expect(
        !nds_hle_atomic_policy_allows(0u, 1u, 100u, 1000u, 1u),
        "deferred cycle debt was accepted");
    failures += expect(
        !nds_hle_atomic_policy_allows(0u, 0u, 1000u, 1000u, 0u),
        "window at its cycle cap was accepted");
    failures += expect(
        !nds_hle_atomic_policy_allows(0u, 0u, 1001u, 1000u, 0u),
        "window beyond its cycle cap was accepted");
    failures += expect(
        !nds_hle_atomic_policy_allows(0u, 0u, 900u, 1000u, 101u),
        "window with insufficient cycle room was accepted");
    failures += expect(
        nds_hle_atomic_policy_allows(0u, 0u, 900u, 1000u, 100u),
        "window with exactly enough cycle room was rejected");
    return failures == 0 ? 0 : 1;
}
