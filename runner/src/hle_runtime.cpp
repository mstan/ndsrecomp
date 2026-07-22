#include "hle_runtime.h"

#include "runtime_arm.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "io.h"
#include "state.h"

namespace {

enum class HlePolicy : uint8_t { Off, On, Force, Verify };

struct HleConfig {
    HlePolicy master = HlePolicy::Off;
    HlePolicy math = HlePolicy::Off;
    HlePolicy particle = HlePolicy::Off;
    HlePolicy effective_math = HlePolicy::Off;
    HlePolicy effective_particle = HlePolicy::Off;
};

enum class HleDecision : uint8_t {
    Disabled,
    Handled,
    Unsupported,
    MissingHandler,
    MissingVerifier,
    VerifyMatch,
    VerifyMismatch,
    VerifyUnsupported,
    ForceFailure,
};

struct HleStats {
    const NdsHleProfileDescriptor* descriptor = nullptr;
    const NdsHleHandler* handler = nullptr;
    uint64_t attempts = 0;
    uint64_t hits = 0;
    uint64_t verifies = 0;
    uint64_t mismatches = 0;
    uint64_t fallbacks_disabled = 0;
    uint64_t fallbacks_unsupported = 0;
    uint64_t fallbacks_missing_handler = 0;
    uint64_t fallbacks_missing_verifier = 0;
    uint64_t force_failures = 0;
    uint64_t handler_host_ns = 0;
};

struct HleDiagnostic {
    uint64_t sequence = 0;
    const NdsHleProfileDescriptor* descriptor = nullptr;
    const NdsHleHandler* handler = nullptr;
    uint64_t cycles = 0;
    uint64_t instructions = 0;
    uint64_t vblank = 0;
    uint32_t pc = 0;
    uint8_t cpu = 0;
    uint8_t mode = 0;
    HleDecision decision = HleDecision::Disabled;
};

constexpr size_t kDiagnosticCapacity = 128;
HleConfig g_config;
std::vector<HleStats> g_stats;
std::array<HleDiagnostic, kDiagnosticCapacity> g_diagnostics{};
uint64_t g_diagnostic_sequence = 0;

const char* policy_name(HlePolicy policy) {
    switch (policy) {
        case HlePolicy::Off: return "off";
        case HlePolicy::On: return "on";
        case HlePolicy::Force: return "force";
        case HlePolicy::Verify: return "verify";
    }
    return "off";
}

const char* decision_name(HleDecision decision) {
    switch (decision) {
        case HleDecision::Disabled: return "disabled";
        case HleDecision::Handled: return "handled";
        case HleDecision::Unsupported: return "unsupported";
        case HleDecision::MissingHandler: return "missing_handler";
        case HleDecision::MissingVerifier: return "missing_verifier";
        case HleDecision::VerifyMatch: return "verify_match";
        case HleDecision::VerifyMismatch: return "verify_mismatch";
        case HleDecision::VerifyUnsupported: return "verify_unsupported";
        case HleDecision::ForceFailure: return "force_failure";
    }
    return "disabled";
}

bool parse_policy(const char* variable, const char* value, bool allow_force,
                  HlePolicy* out) {
    if (!value || !*value) {
        *out = HlePolicy::Off;
        return true;
    }
    if (std::strcmp(value, "off") == 0) *out = HlePolicy::Off;
    else if (std::strcmp(value, "on") == 0) *out = HlePolicy::On;
    else if (std::strcmp(value, "verify") == 0) *out = HlePolicy::Verify;
    else if (allow_force && std::strcmp(value, "force") == 0)
        *out = HlePolicy::Force;
    else {
        std::fprintf(stderr,
            "invalid %s (expected %s)\n", variable,
            allow_force ? "off, on, force, or verify" : "off, on, or verify");
        return false;
    }
    return true;
}

bool string_has_prefix(const char* text, const char* prefix) {
    return text && prefix &&
           std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

bool string_contains(const char* text, const char* needle) {
    return text && needle && std::strstr(text, needle) != nullptr;
}

HlePolicy effective_policy_for(const NdsHleProfileDescriptor* descriptor,
                               const NdsHleHandler* handler) {
    if (string_has_prefix(descriptor ? descriptor->id : nullptr,
                          "sm64ds.particle") ||
        string_contains(handler ? handler->id : nullptr, "particle")) {
        return g_config.effective_particle;
    }
    return g_config.effective_math;
}

HleStats& stats_for(const NdsHleProfileDescriptor* descriptor,
                    const NdsHleHandler* handler) {
    for (HleStats& stats : g_stats) {
        if (stats.descriptor == descriptor && stats.handler == handler)
            return stats;
    }
    g_stats.push_back({descriptor, handler});
    return g_stats.back();
}

uint64_t host_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<
        std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void record_decision(const NdsHleProfileDescriptor* descriptor,
                     const NdsHleHandler* handler, HleDecision decision) {
    const uint64_t sequence = g_diagnostic_sequence++;
    HleDiagnostic& entry = g_diagnostics[sequence % kDiagnosticCapacity];
    entry.sequence = sequence;
    entry.descriptor = descriptor;
    entry.handler = handler;
    entry.cycles = g_runtime_cycles;
    entry.instructions = g_insn_count[g_nds_active];
    entry.vblank = g_nds_active == NDS_ARM9
        ? nds_event_counts().vblank9 : nds_event_counts().vblank7;
    entry.pc = g_cpu.R[15];
    entry.cpu = static_cast<uint8_t>(g_nds_active);
    entry.mode = static_cast<uint8_t>(g_cpu.cpsr & 0x1fu);
    entry.decision = decision;
}

bool force_failure(HleStats& stats,
                   const NdsHleProfileDescriptor* descriptor,
                   const NdsHleHandler* handler, const char* reason) {
    ++stats.force_failures;
    record_decision(descriptor, handler, HleDecision::ForceFailure);
    std::fprintf(stderr,
        "[hle] force failure: candidate=%s bank=%s handler=%s reason=%s "
        "cpu=%s pc=0x%08X\n",
        descriptor && descriptor->id ? descriptor->id : "<null>",
        descriptor && descriptor->bank ? descriptor->bank : "<null>",
        handler && handler->id ? handler->id : "<null>", reason,
        g_nds_active == NDS_ARM9 ? "arm9" : "arm7", g_cpu.R[15]);
    nds_halt("HLE force failure");
    return true;
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

}  // namespace

bool nds_hle_configure_from_environment() {
    HleConfig parsed;
    if (!parse_policy("NDS_HLE", std::getenv("NDS_HLE"), false,
                      &parsed.master) ||
        !parse_policy("NDS_HLE_MATH", std::getenv("NDS_HLE_MATH"), true,
                      &parsed.math) ||
        !parse_policy("NDS_HLE_PARTICLE", std::getenv("NDS_HLE_PARTICLE"),
                      true, &parsed.particle)) {
        return false;
    }

    if (parsed.master == HlePolicy::Off || parsed.math == HlePolicy::Off) {
        parsed.effective_math = HlePolicy::Off;
    } else if (parsed.master == HlePolicy::Verify) {
        parsed.effective_math = HlePolicy::Verify;
    } else {
        parsed.effective_math = parsed.math;
    }
    if (parsed.master == HlePolicy::Off ||
        parsed.particle == HlePolicy::Off) {
        parsed.effective_particle = HlePolicy::Off;
    } else if (parsed.master == HlePolicy::Verify) {
        parsed.effective_particle = HlePolicy::Verify;
    } else {
        parsed.effective_particle = parsed.particle;
    }

#if !defined(NDS_ENABLE_HLE)
    if (parsed.effective_math != HlePolicy::Off ||
        parsed.effective_particle != HlePolicy::Off) {
        std::fprintf(stderr,
            "HLE requested but this runner was built without NDS_ENABLE_HLE\n");
        return false;
    }
#endif
    g_config = parsed;
    return true;
}

void nds_hle_print_policy() {
    std::fprintf(stderr,
        "[hle] built=%s master=%s math=%s particle=%s "
        "effective_math=%s effective_particle=%s "
        "LLE=fallback+verify-oracle\n",
#if defined(NDS_ENABLE_HLE)
        "yes",
#else
        "no",
#endif
        policy_name(g_config.master), policy_name(g_config.math),
        policy_name(g_config.particle), policy_name(g_config.effective_math),
        policy_name(g_config.effective_particle));
}

void nds_hle_reset_diagnostics() {
    g_stats.clear();
    g_diagnostics = {};
    g_diagnostic_sequence = 0;
}

extern "C" bool runtime_hle_try(
        const NdsHleProfileDescriptor* descriptor, NdsHleLleFn lle,
        const NdsHleHandler* handler) {
    HleStats& stats = stats_for(descriptor, handler);
    ++stats.attempts;

    const HlePolicy policy = effective_policy_for(descriptor, handler);
    if (policy == HlePolicy::Off) {
        ++stats.fallbacks_disabled;
        record_decision(descriptor, handler, HleDecision::Disabled);
        return false;
    }

    if (!descriptor || !lle || !handler || !handler->run) {
        ++stats.fallbacks_missing_handler;
        if (policy == HlePolicy::Force)
            return force_failure(stats, descriptor, handler,
                                 "missing descriptor, LLE body, or handler");
        record_decision(descriptor, handler, HleDecision::MissingHandler);
        return false;
    }

    if (policy == HlePolicy::Verify) {
        if (!handler->verify) {
            ++stats.fallbacks_missing_verifier;
            record_decision(descriptor, handler,
                            HleDecision::MissingVerifier);
            return false;
        }
        ++stats.verifies;
        if (handler->minimum_atomic_cycles != 0u &&
            !runtime_hle_atomic_allowed(handler->minimum_atomic_cycles)) {
            ++stats.fallbacks_unsupported;
            record_decision(descriptor, handler,
                            HleDecision::VerifyUnsupported);
            lle();
            return true;
        }
        const uint64_t begin = host_ns();
        const NdsHleVerifyResult result = handler->verify(descriptor, lle);
        stats.handler_host_ns += host_ns() - begin;
        if (result == NDS_HLE_VERIFY_MATCH) {
            ++stats.hits;
            record_decision(descriptor, handler, HleDecision::VerifyMatch);
        } else if (result == NDS_HLE_VERIFY_MISMATCH) {
            ++stats.mismatches;
            record_decision(descriptor, handler, HleDecision::VerifyMismatch);
            std::fprintf(stderr,
                "[hle] verify mismatch: candidate=%s bank=%s handler=%s\n",
                descriptor->id ? descriptor->id : "<null>",
                descriptor->bank ? descriptor->bank : "<null>",
                handler->id ? handler->id : "<null>");
        } else {
            ++stats.fallbacks_unsupported;
            record_decision(descriptor, handler,
                            HleDecision::VerifyUnsupported);
        }
        // The verification adapter ran LLE and left its state authoritative.
        return true;
    }

    if (handler->minimum_atomic_cycles != 0u &&
        !runtime_hle_atomic_allowed(handler->minimum_atomic_cycles)) {
        ++stats.fallbacks_unsupported;
        if (policy == HlePolicy::Force)
            return force_failure(stats, descriptor, handler,
                                 "insufficient atomic cycle budget");
        record_decision(descriptor, handler, HleDecision::Unsupported);
        return false;
    }

    const uint64_t begin = host_ns();
    const NdsHleRunResult result = handler->run(descriptor);
    stats.handler_host_ns += host_ns() - begin;
    if (result == NDS_HLE_RUN_HANDLED) {
        ++stats.hits;
        record_decision(descriptor, handler, HleDecision::Handled);
        return true;
    }

    ++stats.fallbacks_unsupported;
    if (policy == HlePolicy::Force)
        return force_failure(stats, descriptor, handler, "unsupported input");
    record_decision(descriptor, handler, HleDecision::Unsupported);
    return false;
}

std::string nds_hle_status_json() {
    std::string out = "{\"built\":";
#if defined(NDS_ENABLE_HLE)
    out += "true";
#else
    out += "false";
#endif
    out += ",\"master\":\"" + std::string(policy_name(g_config.master)) +
           "\",\"math\":\"" + std::string(policy_name(g_config.math)) +
           "\",\"particle\":\"" +
           std::string(policy_name(g_config.particle)) +
           "\",\"effective_math\":\"" +
           std::string(policy_name(g_config.effective_math)) +
           "\",\"effective_particle\":\"" +
           std::string(policy_name(g_config.effective_particle)) +
           "\",\"candidates\":[";
    bool first = true;
    for (const HleStats& stats : g_stats) {
        if (!first) out.push_back(',');
        first = false;
        out += "{\"id\":";
        append_json_string(out, stats.descriptor ? stats.descriptor->id : nullptr);
        out += ",\"bank\":";
        append_json_string(out,
                           stats.descriptor ? stats.descriptor->bank : nullptr);
        out += ",\"handler\":";
        append_json_string(out, stats.handler ? stats.handler->id : nullptr);
        out += ",\"attempts\":" + std::to_string(stats.attempts) +
               ",\"hits\":" + std::to_string(stats.hits) +
               ",\"verifies\":" + std::to_string(stats.verifies) +
               ",\"mismatches\":" + std::to_string(stats.mismatches) +
               ",\"fallbacks_disabled\":" +
                   std::to_string(stats.fallbacks_disabled) +
               ",\"fallbacks_unsupported\":" +
                   std::to_string(stats.fallbacks_unsupported) +
               ",\"fallbacks_missing_handler\":" +
                   std::to_string(stats.fallbacks_missing_handler) +
               ",\"fallbacks_missing_verifier\":" +
                   std::to_string(stats.fallbacks_missing_verifier) +
               ",\"force_failures\":" +
                   std::to_string(stats.force_failures) +
               ",\"handler_host_ns\":" +
                   std::to_string(stats.handler_host_ns);
        if (stats.handler && stats.handler->diagnostics_json) {
            const char* details = stats.handler->diagnostics_json();
            out += ",\"handler_detail\":";
            out += (details && *details) ? details : "null";
        }
        out += "}";
    }
    out += "],\"diagnostics\":[";
    const uint64_t retained = g_diagnostic_sequence < kDiagnosticCapacity
        ? g_diagnostic_sequence : kDiagnosticCapacity;
    const uint64_t oldest = g_diagnostic_sequence - retained;
    for (uint64_t sequence = oldest; sequence < g_diagnostic_sequence;
         ++sequence) {
        const HleDiagnostic& entry =
            g_diagnostics[sequence % kDiagnosticCapacity];
        if (sequence != oldest) out.push_back(',');
        out += "{\"sequence\":" + std::to_string(entry.sequence) +
               ",\"id\":";
        append_json_string(out,
                           entry.descriptor ? entry.descriptor->id : nullptr);
        out += ",\"bank\":";
        append_json_string(out,
                           entry.descriptor ? entry.descriptor->bank : nullptr);
        out += ",\"handler\":";
        append_json_string(out, entry.handler ? entry.handler->id : nullptr);
        out += ",\"decision\":\"" +
               std::string(decision_name(entry.decision)) +
               "\",\"cpu\":" + std::to_string(entry.cpu ? 7u : 9u) +
               ",\"mode\":" + std::to_string(entry.mode) +
               ",\"pc\":" + std::to_string(entry.pc) +
               ",\"cycles\":" + std::to_string(entry.cycles) +
               ",\"instructions\":" +
                   std::to_string(entry.instructions) +
               ",\"vblank\":" + std::to_string(entry.vblank) + "}";
    }
    return out + "]}";
}
