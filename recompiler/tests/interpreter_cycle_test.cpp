#include <cstdint>
#include <cstdio>

#include "interpreter.h"
#include "thumb_decode.h"

using namespace armv4t;

namespace {

struct NullBus final : Bus {
    uint8_t read8(uint32_t) override { return 0; }
    uint16_t read16(uint32_t) override { return 0; }
    uint32_t read32(uint32_t) override { return 0; }
    void write8(uint32_t, uint8_t) override {}
    void write16(uint32_t, uint16_t) override {}
    void write32(uint32_t, uint32_t) override {}
};

int check_thumb_mul(uint32_t old_rd, uint32_t rs, uint32_t want_cycles) {
    // 0x434D: MUL r5,r1. ARM7 Thumb timing keys off the original r5.
    const Instr in = ThumbDecoder::decode(0x434Du, 0x1000u);
    CPUState cpu{};
    cpu.R[1] = rs;
    cpu.R[5] = old_rd;
    cpu.R[15] = 0x1000u;
    cpu.cpsr.t = true;
    cpu.cpsr.mode = static_cast<uint8_t>(Mode::System);
    cpu.thumb = true;
    NullBus bus;
    uint32_t cycles = 0;
    const auto result = Interpreter::step(cpu, bus, in, &cycles);
    const uint32_t want_result = old_rd * rs;
    if (result != Interpreter::Result::Normal || cycles != want_cycles ||
        cpu.R[5] != want_result || cpu.R[15] != 0x1002u) {
        std::printf("FAIL Thumb MUL old=%08X rs=%08X cycles=%u want=%u "
                    "result=%08X want_result=%08X pc=%08X\n",
                    old_rd, rs, cycles, want_cycles, cpu.R[5], want_result,
                    cpu.R[15]);
        return 1;
    }
    return 0;
}

}  // namespace

int main() {
    int failures = 0;
    failures += check_thumb_mul(0x0000007Fu, 3u, 2u);
    failures += check_thumb_mul(0x00000100u, 3u, 3u);
    failures += check_thumb_mul(0x00010000u, 3u, 4u);
    failures += check_thumb_mul(0xFFFFFF00u, 3u, 5u);
    if (failures) return 1;
    std::puts("[interpreter_cycle_test] PASS");
    return 0;
}
