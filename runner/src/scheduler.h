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

struct NdsSchedulerProfile {
    uint64_t next_event_ns;
    uint64_t arm9_ns;
    uint64_t arm7_ns;
    uint64_t devices_ns;
    uint64_t sampled_round_ns;
    uint64_t sampled_rounds;
    // Sub-buckets. switch_ns is sampled (same 1-in-1009 rounds); switches and
    // crs_words are exact whole-run counters (an increment is too cheap to
    // gate). The devices split shares the sampled rounds.
    uint64_t switch_ns;
    uint64_t switches;
    uint64_t crs_words;
    uint64_t display_ns;
    uint64_t spu_ns;
    uint64_t wifi_ns;
    uint64_t rtc_ns;
    uint64_t sysev_ns;
};

// Opt-in coarse sampler used by the release profiler. It samples one complete
// scheduler round out of every 1009 so profiling does not materially change
// the 64-cycle interleave it is measuring. Zeroes are returned unless
// NDS_PROFILE_SCHED is present in the environment.
void scheduler_profile_reset();
void scheduler_profile(NdsSchedulerProfile* out);

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
