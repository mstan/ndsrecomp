#include "function_heat.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "runtime_arm.h"

extern "C" {
uint64_t g_insn_count[2] = {};
}
NdsCpu g_nds_active = NDS_ARM9;

namespace {

#if defined(NDS_PROFILE_FUNCTION_HEAT)
void set_environment(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

bool contains(const std::string& text, const char* fragment) {
    if (text.find(fragment) != std::string::npos) return true;
    std::fprintf(stderr, "missing JSON fragment: %s\nJSON: %s\n",
                 fragment, text.c_str());
    return false;
}
#endif

}  // namespace

int main() {
#if !defined(NDS_PROFILE_FUNCTION_HEAT)
    const std::string json = nds_function_heat_json();
    return json == "{\"enabled\":false,\"functions\":[]}" ? 0 : 1;
#else
    set_environment("NDS_FUNCTION_HEAT_LOG2", "2");
    set_environment("NDS_FUNCTION_HEAT_PHASE", "1");
    nds_function_heat_reset();

    const NdsFunctionHeatDescriptor bank_a{
        "shared_pc", "ram_capture_a", 0x02001000u, 0x02001100u, 0u,
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    const NdsFunctionHeatDescriptor bank_b{
        "shared_pc", "ram_capture_b", 0x02001000u, 0x02001100u, 0u,
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"};

    // Ordinals 1, 5, and 9 match phase 1 at stride 4. Calling the same
    // descriptor non-contiguously models slice unwind and interior resume.
    for (uint64_t ordinal = 1; ordinal <= 9; ++ordinal) {
        g_insn_count[0] = ordinal;
        const NdsFunctionHeatDescriptor* descriptor =
            ordinal == 5 ? &bank_b : &bank_a;
        runtime_function_heat_retire(descriptor);
    }
    g_nds_active = NDS_ARM7;
    g_insn_count[1] = 1;
    runtime_function_heat_retire(&bank_a);

    const std::string json = nds_function_heat_json();
    bool ok = true;
    ok &= contains(json, "\"enabled\":true");
    ok &= contains(json, "\"sample_log2\":2");
    ok &= contains(json, "\"sample_phase\":1");
    ok &= contains(json, "\"sample_stride\":4");
    ok &= contains(json, "\"total_samples\":4");
    ok &= contains(json, "\"bank\":\"ram_capture_a\"");
    ok &= contains(json, "\"bank\":\"ram_capture_b\"");
    ok &= contains(json, "\"content_sha1\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"");
    ok &= contains(json, "\"content_sha1\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"");
    ok &= contains(json, "\"samples\":2,\"estimated_instructions\":8");
    ok &= contains(json, "\"cpu\":1");
    return ok ? 0 : 1;
#endif
}
