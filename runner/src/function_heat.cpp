#include "function_heat.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "runtime_arm.h"

extern "C" {
uint64_t g_nds_function_heat_sample_mask = 0u;
uint64_t g_nds_function_heat_sample_phase = 0u;
}

namespace {

#if defined(NDS_PROFILE_FUNCTION_HEAT)
struct FunctionHeatStats {
    const NdsFunctionHeatDescriptor* descriptor = nullptr;
    unsigned cpu = 0;
    uint64_t samples = 0;
};

std::vector<FunctionHeatStats> g_function_heat;
unsigned g_function_heat_log2 = 10u;

unsigned configured_log2() {
    const char* text = std::getenv("NDS_FUNCTION_HEAT_LOG2");
    if (!text || !*text) return 10u;  // deterministic 1/1024 sampling
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (*end != '\0' || parsed > 20u) {
        std::fprintf(stderr,
            "invalid NDS_FUNCTION_HEAT_LOG2 (expected 0..20); using 10\n");
        return 10u;
    }
    return static_cast<unsigned>(parsed);
}

uint64_t configured_phase(unsigned log2) {
    const uint64_t stride = uint64_t{1} << log2;
    const char* text = std::getenv("NDS_FUNCTION_HEAT_PHASE");
    if (!text || !*text) return 0u;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (*end != '\0' || parsed >= stride) {
        std::fprintf(stderr,
            "invalid NDS_FUNCTION_HEAT_PHASE for current log2; using 0\n");
        return 0u;
    }
    return static_cast<uint64_t>(parsed);
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

}  // namespace

extern "C" void runtime_function_heat_sample(
        const NdsFunctionHeatDescriptor* descriptor) {
#if defined(NDS_PROFILE_FUNCTION_HEAT)
    const unsigned cpu = static_cast<unsigned>(g_nds_active) & 1u;
    for (auto& stats : g_function_heat) {
        if (stats.descriptor == descriptor && stats.cpu == cpu) {
            ++stats.samples;
            return;
        }
    }
    g_function_heat.push_back(FunctionHeatStats{descriptor, cpu, 1u});
#else
    (void)descriptor;
#endif
}

void nds_function_heat_reset() {
#if defined(NDS_PROFILE_FUNCTION_HEAT)
    g_function_heat.clear();
    g_function_heat_log2 = configured_log2();
    g_nds_function_heat_sample_mask =
        (uint64_t{1} << g_function_heat_log2) - 1u;
    g_nds_function_heat_sample_phase =
        configured_phase(g_function_heat_log2);
#endif
}

std::string nds_function_heat_json() {
#if !defined(NDS_PROFILE_FUNCTION_HEAT)
    return "{\"enabled\":false,\"functions\":[]}";
#else
    std::vector<const FunctionHeatStats*> ranked;
    ranked.reserve(g_function_heat.size());
    uint64_t total_samples = 0u;
    for (const auto& stats : g_function_heat) {
        ranked.push_back(&stats);
        total_samples += stats.samples;
    }
    std::sort(ranked.begin(), ranked.end(),
        [](const FunctionHeatStats* a, const FunctionHeatStats* b) {
            if (a->samples != b->samples) return a->samples > b->samples;
            const int bank = std::string(a->descriptor->bank).compare(
                b->descriptor->bank);
            if (bank != 0) return bank < 0;
            if (a->descriptor->address != b->descriptor->address)
                return a->descriptor->address < b->descriptor->address;
            return a->cpu < b->cpu;
        });

    const uint64_t stride = uint64_t{1} << g_function_heat_log2;
    std::string out = "{\"enabled\":true,\"sample_log2\":" +
        std::to_string(g_function_heat_log2) + ",\"sample_phase\":" +
        std::to_string(g_nds_function_heat_sample_phase) +
        ",\"sample_stride\":" + std::to_string(stride) +
        ",\"total_samples\":" + std::to_string(total_samples) +
        ",\"functions\":[";
    bool first = true;
    for (const FunctionHeatStats* stats : ranked) {
        if (!first) out.push_back(',');
        first = false;
        const NdsFunctionHeatDescriptor& descriptor = *stats->descriptor;
        out += "{\"name\":";
        append_json_string(out, descriptor.name);
        out += ",\"bank\":";
        append_json_string(out, descriptor.bank);
        out += ",\"cpu\":" + std::to_string(stats->cpu) +
               ",\"address\":" + std::to_string(descriptor.address) +
               ",\"end_address\":" +
                   std::to_string(descriptor.end_address) +
               ",\"thumb\":" + std::to_string(descriptor.thumb) +
               ",\"content_sha1\":";
        append_json_string(out, descriptor.content_sha1);
        out += ",\"samples\":" + std::to_string(stats->samples) +
               ",\"estimated_instructions\":" +
                   std::to_string(stats->samples * stride) + "}";
    }
    out += "]}";
    return out;
#endif
}
