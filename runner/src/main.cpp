// main.cpp — DS runner driver.
//
// Loads + SHA-1-verifies the three dumps (CLAUDE.md: refuse to start
// otherwise), maps both BIOSes, resets both cores, and interleaves them on
// the scheduler until ARM9 reaches the cycle budget or both terminally
// halt. Reports where each core got — the execution-driven signal for the
// next pieces (SPI/firmware boot for ARM7, IPC handshake between them).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "state.h"
#include "scheduler.h"
#include "runtime_arm.h"
#include "io.h"
#include "sha1.h"

// Generated per-CPU dispatch tables (C linkage).
extern "C" const DispatchEntry g_dispatch_arm9_bios[];
extern "C" const unsigned g_dispatch_arm9_bios_len;
extern "C" const DispatchEntry g_dispatch_arm7_bios[];
extern "C" const unsigned g_dispatch_arm7_bios_len;

namespace {

std::vector<uint8_t> read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

bool verify(const std::vector<uint8_t>& data, const char* want,
            const char* label) {
    if (data.empty()) { std::fprintf(stderr, "[load] %s: missing/empty\n", label); return false; }
    std::string got = gba::sha1(data.data(), data.size()).hex();
    if (got != want) {
        std::fprintf(stderr, "[load] %s: SHA-1 mismatch\n  got  %s\n  want %s\n",
                     label, got.c_str(), want);
        return false;
    }
    std::fprintf(stderr, "[load] %s: %zu bytes, SHA-1 ok\n", label, data.size());
    return true;
}

void dump_cpu(const char* name, const ArmCpuState& c, uint64_t cycles) {
    std::fprintf(stderr, "  %s: PC=%08X CPSR=%08X SP=%08X LR=%08X "
                 "R0=%08X R12=%08X  cycles=%llu\n",
                 name, c.R[15], c.cpsr, c.R[13], c.R[14], c.R[0], c.R[12],
                 (unsigned long long)cycles);
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir = (argc > 1) ? argv[1] : "bios";
    uint64_t budget = (argc > 2) ? std::strtoull(argv[2], nullptr, 0)
                                 : 4000000ull;

    auto a9 = read_file(dir + "/biosnds9.rom");
    auto a7 = read_file(dir + "/biosnds7.rom");
    auto fw = read_file(dir + "/firmware.bin");
    bool ok = verify(a9, "bfaac75f101c135e32e2aaf541de6b1be4c8c62d", "arm9 bios")
            & verify(a7, "24f67bdea115a2c847c8813a262502ee1607b7df", "arm7 bios")
            & verify(fw, "ae22de59fbf3f35ccfbeacaeba6fa87ac5e7b14b", "firmware");
    if (!ok) { std::fprintf(stderr, "refusing to start: dump verification failed\n"); return 1; }

    bus_init();
    bus_load_arm9_bios(a9.data(), (uint32_t)a9.size());
    bus_load_arm7_bios(a7.data(), (uint32_t)a7.size());
    cp15_reset();
    nds_io_reset();
    nds_io_load_firmware(fw.data(), (uint32_t)fw.size());
    runtime_init(nullptr);
    runtime_trace_reset();

    nds_register_dispatch(NDS_ARM9, g_dispatch_arm9_bios,
                          g_dispatch_arm9_bios_len, 0xFFFF0000u);
    nds_register_dispatch(NDS_ARM7, g_dispatch_arm7_bios,
                          g_dispatch_arm7_bios_len, 0x00000000u);

    // Reset both cores: SVC mode, IRQ+FIQ masked, ARM state, reset vector.
    const uint32_t reset_cpsr = 0x13u | CPSR_I_BIT | CPSR_F_BIT;
    scheduler_init();
    scheduler_reset_cpu(0, 0xFFFF0000u, reset_cpsr);  // ARM9
    scheduler_reset_cpu(1, 0x00000000u, reset_cpsr);  // ARM7

    std::fprintf(stderr, "[run] dual-CPU from reset, ARM9 budget=%llu cycles\n",
                 (unsigned long long)budget);
    SchedResult r = scheduler_run(budget);

    std::fprintf(stderr, "\n== result (%llu rounds) ==\n",
                 (unsigned long long)r.rounds);
    dump_cpu("ARM9", scheduler_cpu_state(0), r.cycles[0]);
    std::fprintf(stderr, "        %s\n",
                 r.halted[0] ? r.reason[0] : "running (reached budget / idle)");
    dump_cpu("ARM7", scheduler_cpu_state(1), r.cycles[1]);
    std::fprintf(stderr, "        %s\n",
                 r.halted[1] ? r.reason[1] : "running (reached budget / idle)");
    std::fprintf(stderr, "  CP15: control=%08X DTCM(en=%d base=%08X sz=%u)\n",
                 g_cp15.control, g_cp15.dtcm_enable, g_cp15.dtcm_base,
                 g_cp15.dtcm_size);
    nds_dump_irq();
    std::fprintf(stderr, "\n== recent execution trace (last-scheduled CPU, tail) ==\n");
    runtime_trace_dump_recent(24);
    return 0;
}
