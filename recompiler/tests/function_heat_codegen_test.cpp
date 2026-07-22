#include <cstdio>
#include <string>

#include "arm_codegen.h"
#include "arm_decode.h"

using namespace armv4t;

int main() {
    const Instr mov = ArmDecoder::decode(0xE1A00000u, 0x02001000u);
    bool not_implemented = false;

    CodegenCtx disabled;
    const std::string plain = ArmCodegen::emit_instr(
        mov, disabled, &not_implemented);
    if (not_implemented ||
        plain.find("NDS_PROFILE_FUNCTION_HEAT") != std::string::npos ||
        plain.find("runtime_function_heat_retire") != std::string::npos) {
        std::fprintf(stderr, "disabled codegen emitted function heat work\n");
        return 1;
    }

    CodegenCtx profiled;
    profiled.function_heat_descriptor = "g_function_heat_test_7";
    const std::string instrumented = ArmCodegen::emit_instr(
        mov, profiled, &not_implemented);
    if (not_implemented ||
        instrumented.find("#if defined(NDS_PROFILE_FUNCTION_HEAT)") ==
            std::string::npos ||
        instrumented.find(
            "runtime_function_heat_retire(&g_function_heat_test_7)") ==
            std::string::npos) {
        std::fprintf(stderr, "profiled codegen omitted descriptor sample\n");
        return 1;
    }

    Instr never{};
    never.pc = 0x02002000u;
    never.cond = Cond::NV;
    const std::string condition_failed = ArmCodegen::emit_instr(
        never, profiled, &not_implemented);
    if (condition_failed.find(
            "runtime_function_heat_retire(&g_function_heat_test_7)") ==
            std::string::npos) {
        std::fprintf(stderr, "condition-failed retirement was not sampled\n");
        return 1;
    }
    return 0;
}
