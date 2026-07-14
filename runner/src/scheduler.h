// scheduler.h — dual-CPU interleave for the DS runner.
//
// The DS has two cores on one timeline: ARM9 (~67 MHz) + ARM7 (~33 MHz),
// sharing main RAM. The generated code operates on the single active
// `g_cpu`, so the scheduler context-switches it (and g_nds_active) between
// CPUs at slice boundaries. The call-return stack is empty at every slice
// boundary (the cooperative yield unwinds the host stack, each frame
// cancelling its pushed return), so only the ArmCpuState is swapped.

#pragma once

#include <cstdint>

void scheduler_init();
void scheduler_reset_cpu(int cpu, uint32_t pc, uint32_t cpsr);

struct SchedResult {
    bool        halted[2];
    const char* reason[2];
    uint64_t    cycles[2];
    uint64_t    rounds;
};

// Interleave both CPUs until ARM9 reaches `arm9_cycle_budget` or both CPUs
// have terminally halted. ARM9 runs ~2× the cycles per round (clock ratio).
SchedResult scheduler_run(uint64_t arm9_cycle_budget);
void scheduler_run_round();

// Inspect a CPU's saved state after a run (for reporting).
const struct ArmCpuState& scheduler_cpu_state(int cpu);
uint64_t scheduler_cpu_cycles(int cpu);
uint64_t scheduler_system_timestamp();
uint64_t scheduler_next_event_timestamp();
bool scheduler_cpu_terminal_halted(int cpu);
const char* scheduler_cpu_halt_reason(int cpu);
