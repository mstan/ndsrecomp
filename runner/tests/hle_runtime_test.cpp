#include "hle_runtime.h"

#include "io.h"
#include "runtime_arm.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
ArmCpuState g_cpu = {};
unsigned long long g_runtime_cycles = 0;
uint64_t g_insn_count[2] = {};
}
NdsCpu g_nds_active = NDS_ARM9;
bool g_atomic_allowed = true;

extern "C" bool runtime_hle_atomic_allowed(uint32_t) {
    return g_atomic_allowed;
}

namespace {

NdsEventCounts g_events{};
int g_halt_calls = 0;
const char* g_halt_reason = nullptr;
#if defined(NDS_ENABLE_HLE)
int g_lle_calls = 0;
int g_handler_calls = 0;
int g_verify_calls = 0;
int g_guest_result = 0;

const NdsHleProfileDescriptor kDescriptor{
    "test.math", "test_bank", 0x02001000u, 0x02001010u, 0u, 4u,
    "0000000000000000000000000000000000000000", nullptr};
const NdsHleProfileDescriptor kParticleDescriptor{
    "sm64ds.particle_delta", "test_bank", 0x02002000u, 0x02002010u, 0u, 4u,
    "0000000000000000000000000000000000000000", nullptr};
#endif

void set_environment(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

#if defined(NDS_ENABLE_HLE)
void configure(const char* master, const char* math,
               const char* particle = nullptr) {
    set_environment("NDS_HLE", master);
    set_environment("NDS_HLE_MATH", math);
    set_environment("NDS_HLE_PARTICLE", particle);
    if (!nds_hle_configure_from_environment()) {
        std::fprintf(stderr,
                     "failed to configure NDS_HLE=%s NDS_HLE_MATH=%s "
                     "NDS_HLE_PARTICLE=%s\n",
                     master ? master : "<unset>",
                     math ? math : "<unset>",
                     particle ? particle : "<unset>");
        std::exit(2);
    }
    nds_hle_reset_diagnostics();
    g_halt_calls = 0;
    g_halt_reason = nullptr;
    g_lle_calls = 0;
    g_handler_calls = 0;
    g_verify_calls = 0;
    g_guest_result = 0;
    g_atomic_allowed = true;
}

void lle_body() {
    ++g_lle_calls;
    g_guest_result = 11;
}

NdsHleRunResult handled(const NdsHleProfileDescriptor*) {
    ++g_handler_calls;
    g_guest_result = 22;
    return NDS_HLE_RUN_HANDLED;
}

NdsHleRunResult unsupported(const NdsHleProfileDescriptor*) {
    ++g_handler_calls;
    return NDS_HLE_RUN_UNSUPPORTED;
}

NdsHleVerifyResult verify_and_retain_lle(
        const NdsHleProfileDescriptor*, NdsHleLleFn lle) {
    ++g_verify_calls;
    ++g_handler_calls;
    g_guest_result = 22;
    lle();
    return g_guest_result == 11 ? NDS_HLE_VERIFY_MISMATCH
                                : NDS_HLE_VERIFY_MATCH;
}

NdsHleVerifyResult verify_match_and_retain_lle(
        const NdsHleProfileDescriptor*, NdsHleLleFn lle) {
    ++g_verify_calls;
    ++g_handler_calls;
    g_guest_result = 22;
    lle();
    return NDS_HLE_VERIFY_MATCH;
}

const NdsHleHandler kHandled{"test_handler", handled, nullptr, 0u, nullptr};
const NdsHleHandler kUnsupported{
    "test_handler", unsupported, nullptr, 0u, nullptr};
const NdsHleHandler kVerified{
    "test_handler", handled, verify_and_retain_lle, 0u, nullptr};
const NdsHleHandler kVerifiedMatch{
    "test_handler", handled, verify_match_and_retain_lle, 0u, nullptr};
const NdsHleHandler kPreflightHandled{
    "test_handler", handled, nullptr, 47u, nullptr};
const NdsHleHandler kPreflightVerified{
    "test_handler", handled, verify_match_and_retain_lle, 47u, nullptr};

void generated_wrapper(const NdsHleHandler* handler) {
    if (!runtime_hle_try(&kDescriptor, lle_body, handler)) lle_body();
}

void generated_particle_wrapper(const NdsHleHandler* handler) {
    if (!runtime_hle_try(&kParticleDescriptor, lle_body, handler)) lle_body();
}

bool contains(const std::string& text, const char* fragment) {
    return text.find(fragment) != std::string::npos;
}
#endif

int expect(bool condition, const char* message) {
    if (condition) return 0;
    std::fprintf(stderr, "%s\n", message);
    return 1;
}

}  // namespace

const NdsEventCounts& nds_event_counts() { return g_events; }

extern "C" void nds_halt(const char* reason) {
    ++g_halt_calls;
    g_halt_reason = reason;
}

int main() {
    int failures = 0;

#if !defined(NDS_ENABLE_HLE)
    set_environment("NDS_HLE", nullptr);
    set_environment("NDS_HLE_MATH", nullptr);
    failures += expect(nds_hle_configure_from_environment(),
                       "disabled build rejected the default-off policy");
    set_environment("NDS_HLE", "on");
    set_environment("NDS_HLE_MATH", "on");
    set_environment("NDS_HLE_PARTICLE", "on");
    failures += expect(!nds_hle_configure_from_environment(),
                       "disabled build accepted an HLE runtime request");
    set_environment("NDS_HLE", nullptr);
    set_environment("NDS_HLE_MATH", nullptr);
    return failures == 0 ? 0 : 1;
#else
    configure(nullptr, nullptr);
    generated_wrapper(&kHandled);
    failures += expect(g_lle_calls == 1 && g_handler_calls == 0 &&
                           g_guest_result == 11 && g_halt_calls == 0,
                       "default policy did not execute only LLE");
    failures += expect(
        contains(nds_hle_status_json(), "\"fallbacks_disabled\":1"),
        "default policy did not report its LLE fallback");

    configure("on", "on");
    generated_wrapper(&kHandled);
    failures += expect(g_lle_calls == 0 && g_handler_calls == 1 &&
                           g_guest_result == 22 && g_halt_calls == 0,
                       "enabled handled input did not execute only HLE");
    failures += expect(contains(nds_hle_status_json(), "\"hits\":1"),
                       "handled HLE input was not reported");

    configure("on", "off", "on");
    generated_particle_wrapper(&kHandled);
    failures += expect(g_lle_calls == 0 && g_handler_calls == 1 &&
                           g_guest_result == 22 && g_halt_calls == 0,
                       "particle policy did not independently enable handler");

    configure("on", "on", "off");
    generated_particle_wrapper(&kHandled);
    failures += expect(g_lle_calls == 1 && g_handler_calls == 0 &&
                           g_guest_result == 11 && g_halt_calls == 0,
                       "disabled particle policy did not fall back to LLE");

    configure("on", "on");
    generated_wrapper(&kUnsupported);
    failures += expect(g_lle_calls == 1 && g_handler_calls == 1 &&
                           g_guest_result == 11 && g_halt_calls == 0,
                       "unsupported handler did not fall back to LLE");
    failures += expect(
        contains(nds_hle_status_json(), "\"fallbacks_unsupported\":1"),
        "unsupported handler fallback was not reported");

    configure("on", "on");
    g_atomic_allowed = false;
    generated_wrapper(&kPreflightHandled);
    failures += expect(g_lle_calls == 1 && g_handler_calls == 0 &&
                           g_guest_result == 11 && g_halt_calls == 0,
                       "atomic preflight miss did not cheaply fall back to LLE");

    configure("verify", "on");
    g_atomic_allowed = false;
    generated_wrapper(&kPreflightVerified);
    failures += expect(g_verify_calls == 0 && g_handler_calls == 0 &&
                           g_lle_calls == 1 && g_guest_result == 11 &&
                           g_halt_calls == 0,
                       "verify preflight miss did not retain one LLE result");
    failures += expect(
        contains(nds_hle_status_json(), "\"fallbacks_unsupported\":1"),
        "verify preflight fallback was not reported");

    configure("on", "on");
    generated_wrapper(nullptr);
    failures += expect(g_lle_calls == 1 && g_handler_calls == 0 &&
                           g_guest_result == 11 && g_halt_calls == 0,
                       "missing handler did not fall back to LLE");

    configure("on", "force");
    generated_wrapper(&kUnsupported);
    failures += expect(g_lle_calls == 0 && g_handler_calls == 1 &&
                           g_halt_calls == 1 && g_halt_reason &&
                           std::strcmp(g_halt_reason,
                                       "HLE force failure") == 0,
                       "force miss did not halt without executing LLE");
    failures += expect(
        contains(nds_hle_status_json(), "\"force_failures\":1"),
        "force miss was not reported");

    configure("verify", "on");
    generated_wrapper(&kVerified);
    failures += expect(g_verify_calls == 1 && g_handler_calls == 1 &&
                           g_lle_calls == 1 && g_guest_result == 11 &&
                           g_halt_calls == 0,
                       "verify mode did not retain exactly one LLE result");
    failures += expect(
        contains(nds_hle_status_json(), "\"mismatches\":1"),
        "verify mismatch was not reported");

    configure("verify", "on");
    generated_wrapper(&kVerifiedMatch);
    failures += expect(g_verify_calls == 1 && g_handler_calls == 1 &&
                           g_lle_calls == 1 && g_guest_result == 11 &&
                           g_halt_calls == 0,
                       "verify match did not retain exactly one LLE result");
    failures += expect(
        contains(nds_hle_status_json(), "\"decision\":\"verify_match\""),
        "verify match was not reported");

    configure("verify", "on");
    generated_wrapper(&kHandled);
    failures += expect(g_verify_calls == 0 && g_handler_calls == 0 &&
                           g_lle_calls == 1 && g_guest_result == 11 &&
                           g_halt_calls == 0,
                       "missing verifier did not fall back to LLE");
    failures += expect(
        contains(nds_hle_status_json(), "\"fallbacks_missing_verifier\":1"),
        "missing verifier fallback was not reported");

    set_environment("NDS_HLE", nullptr);
    set_environment("NDS_HLE_MATH", nullptr);
    set_environment("NDS_HLE_PARTICLE", nullptr);
    return failures == 0 ? 0 : 1;
#endif
}
