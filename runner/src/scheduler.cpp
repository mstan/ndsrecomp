// scheduler.cpp — see scheduler.h.

#include "scheduler.h"

#include <cstdio>
#include <cstring>

#include "state.h"
#include "runtime_arm.h"
#include "io.h"

namespace {

struct CpuSlot {
    ArmCpuState state;        // saved register file when not active
    uint32_t    crs[1024];   // saved call-return stack (preserved across
    uint32_t    crs_depth = 0;//   preemption — a spin may be mid-call)
    uint64_t    cycles = 0;   // this CPU's accumulated cycles
    bool        halted = false;
    const char* reason = nullptr;
    bool        started = false;
};

CpuSlot g_slot[2];
int     g_cur = -1;           // currently-loaded CPU (-1 = none)

void switch_to(int cpu) {
    if (g_cur == cpu) return;
    if (g_cur >= 0) {
        // Save outgoing: register file AND call-return stack (a preempted
        // spin may be deep in a call chain whose returns must survive).
        g_slot[g_cur].state = g_cpu;
        g_slot[g_cur].crs_depth = runtime_call_stack_depth();
        const uint32_t* src = runtime_call_stack_data();
        for (uint32_t i = 0; i < g_slot[g_cur].crs_depth; ++i)
            g_slot[g_cur].crs[i] = src[i];
    }
    g_cpu = g_slot[cpu].state;                       // load incoming
    runtime_call_stack_restore(g_slot[cpu].crs, g_slot[cpu].crs_depth);
    g_nds_active = (cpu == 0) ? NDS_ARM9 : NDS_ARM7;
    g_cur = cpu;
}

// Run `cpu` for up to `quantum` cycles (its own clock), or until it
// terminally halts. A guest spin simply burns the quantum and yields —
// not a fault (the other core may unblock it).
void run_slice(int cpu, uint32_t quantum) {
    if (g_slot[cpu].halted) return;
    switch_to(cpu);
    g_runtime_cycles = g_slot[cpu].cycles;
    const uint64_t cap = g_slot[cpu].cycles + quantum;
    nds_slice_begin(cap);

    // runtime_dispatch returns at a natural boundary or a backward-branch
    // slice-yield — in both cases R15 is a dispatch entry, so re-dispatch
    // is clean. Keep stepping until this slice's cycle cap is reached.
    long guard = 0;
    while (g_runtime_cycles < cap && !g_slot[cpu].halted) {
        uint32_t pc = g_cpu.R[15];
        uint32_t t  = (g_cpu.cpsr & CPSR_T_BIT) ? 1u : 0u;
        nds_clear_unwinding();   // a fresh dispatch is a real entry, not an unwind
        runtime_dispatch(pc | t);
        if (g_nds_terminal) {
            g_slot[cpu].halted = true;
            g_slot[cpu].reason = g_nds_halt_reason;
            break;
        }
        if (++guard > 20'000'000) {     // host-loop backstop
            g_slot[cpu].halted = true;
            g_slot[cpu].reason = "slice guard (no progress)";
            break;
        }
    }
    g_slot[cpu].cycles = g_runtime_cycles;
}

}  // namespace

void scheduler_init() {
    g_slot[0] = CpuSlot{};
    g_slot[1] = CpuSlot{};
    g_cur = -1;
}

void scheduler_reset_cpu(int cpu, uint32_t pc, uint32_t cpsr) {
    std::memset(&g_slot[cpu].state, 0, sizeof(ArmCpuState));
    g_slot[cpu].state.cpsr = cpsr;
    g_slot[cpu].state.R[15] = pc;
    g_slot[cpu].cycles = 0;
    g_slot[cpu].halted = false;
    g_slot[cpu].reason = nullptr;
    g_slot[cpu].started = true;
}

SchedResult scheduler_run(uint64_t budget) {
    // ARM9 issues ~2 cycles per ARM7 cycle (67 vs 33 MHz). The quantum is
    // a balance: small enough that a polled IPCSYNC/FIFO write by one core
    // is seen by the other promptly, large enough to amortize the
    // context-switch (state + call-return-stack save/restore).
    const uint32_t kQ9 = 2048, kQ7 = 1024;

    uint64_t rounds = 0;
    uint64_t last9 = 0, stall = 0;
    while (g_slot[0].cycles < budget &&
           !(g_slot[0].halted && g_slot[1].halted)) {
        if (g_slot[0].started) run_slice(0, kQ9);   // ARM9
        if (g_slot[1].started) run_slice(1, kQ7);   // ARM7
        nds_tick_hw(g_slot[0].cycles);              // display/timer clocks
        ++rounds;
        // Diagnostic: if ARM9 stops advancing for many rounds while ARM7
        // keeps running, report and stop (a cross-CPU wait isn't resolving).
        if (g_slot[0].cycles == last9) {
            if (++stall == 2000000u) {
                std::fprintf(stderr, "[sched] ARM9 stalled %llu rounds: "
                    "ARM9 pc=0x%08X cyc=%llu halt=%d | ARM7 pc=0x%08X cyc=%llu halt=%d\n",
                    (unsigned long long)stall, g_slot[0].state.R[15],
                    (unsigned long long)g_slot[0].cycles, g_slot[0].halted,
                    g_slot[1].state.R[15], (unsigned long long)g_slot[1].cycles,
                    g_slot[1].halted);
                break;
            }
        } else { stall = 0; last9 = g_slot[0].cycles; }
    }
    // Park final live state back into its slot.
    if (g_cur >= 0) g_slot[g_cur].state = g_cpu;

    SchedResult r{};
    for (int c = 0; c < 2; ++c) {
        r.halted[c] = g_slot[c].halted;
        r.reason[c] = g_slot[c].reason;
        r.cycles[c] = g_slot[c].cycles;
    }
    r.rounds = rounds;
    return r;
}

const ArmCpuState& scheduler_cpu_state(int cpu) { return g_slot[cpu].state; }
