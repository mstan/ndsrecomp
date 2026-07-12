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

// melonDS-faithful timeline (docs/scheduler_design.md, Commit A). g_sys_timestamp
// is in SYSTEM cycles (= ARM7 cycles = ARM9 cycles >> kArm9ClockShift).
// g_slot[0].cycles is the ARM9 timestamp (ARM9 cycles); g_slot[1].cycles the ARM7.
uint64_t           g_sys_timestamp = 0;
constexpr int      kArm9ClockShift = 1;    // ARM9 runs at 2x the system clock
constexpr uint64_t kIterCap        = 64;   // system cycles per outer iteration
// Next scheduled hardware-event time in SYSTEM cycles. Commit A has no event
// table yet, so the kIterCap always dominates; Commit B+ returns the next
// LCD/timer/SPI/DMA deadline here (and lets an MMIO write shorten it).
uint64_t next_scheduled_event_time() { return UINT64_MAX; }

void save_current() {
    if (g_cur < 0) return;
    g_slot[g_cur].state = g_cpu;
    g_slot[g_cur].crs_depth = runtime_call_stack_depth();
    const uint32_t* src = runtime_call_stack_data();
    for (uint32_t i = 0; i < g_slot[g_cur].crs_depth; ++i)
        g_slot[g_cur].crs[i] = src[i];
}

void switch_to(int cpu) {
    if (g_cur == cpu) return;
    // Save outgoing: register file AND call-return stack (a preempted
    // spin may be deep in a call chain whose returns must survive).
    save_current();
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
        // Debug-server event break: stop this slice at the dispatched-block
        // boundary right after the armed event fired, so run_to_event lands AT
        // the Nth event. The other CPU's slice this round breaks immediately
        // too (flag stays set until run_to_event disarms), so neither core
        // free-runs past the sync point.
        if (nds_event_break_hit()) break;
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
    g_sys_timestamp = 0;
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

void scheduler_run_round() {
    // melonDS-faithful interleave (docs/scheduler_design.md, Commit A). Each
    // outer iteration is capped at kIterCap SYSTEM cycles (or the next scheduled
    // event, whichever is sooner). ARM9 runs FIRST up to its target and may
    // overshoot atomically on its final instruction; ARM7 then CATCHES UP to
    // ARM9's ACTUAL resulting timestamp (run-until->=, not force-equal), so the
    // two timelines stay tightly aligned and cross-CPU writes land in the order
    // real parallel hardware produces. Replaces the fixed 2048/1024 round, whose
    // ~1024-ARM7-cycle within-round lead desynced the IPCSYNC handshake.
    // NOTE: with the current naive cycle model (ARM9 ~1.8 vs melonDS ~6-8
    // cyc/insn) the ARM9 still retires too many instructions per iteration, so
    // the boot is EXPECTED to still deadlock until the ARM9 memory-timing model
    // lands (Commits B-C). Commit A's acceptance is the invariants, not the menu.
    uint64_t planned = g_sys_timestamp + kIterCap;
    const uint64_t ev = next_scheduled_event_time();
    if (ev < planned) planned = ev;

    // ARM9 first, to its target in ARM9 cycles; do NOT clamp its overshoot.
    const uint64_t arm9_target = planned << kArm9ClockShift;
    if (g_slot[0].started && !g_slot[0].halted && g_slot[0].cycles < arm9_target)
        run_slice(0, static_cast<uint32_t>(arm9_target - g_slot[0].cycles));

    // Rendezvous = ARM9's ACTUAL (possibly overshot) timestamp, normalized to
    // system cycles. The ARM7 catches up to THIS, not to `planned`.
    const uint64_t rendezvous = g_slot[0].cycles >> kArm9ClockShift;

    // Update display/timer clocks at the ARM9 timestamp, so the VBlank/timer
    // IRQs they raise are visible to the ARM7 as it catches up.
    nds_tick_hw(g_slot[0].cycles);

    // ARM7 catches up to the rendezvous (run-until->=; it too may overshoot its
    // final instruction). Bail if it makes no progress (terminally halted or a
    // debug/insn break is armed) to avoid a busy spin.
    while (g_slot[1].started && !g_slot[1].halted && g_slot[1].cycles < rendezvous) {
        const uint64_t before = g_slot[1].cycles;
        run_slice(1, static_cast<uint32_t>(rendezvous - g_slot[1].cycles));
        if (g_slot[1].cycles == before) break;
    }

    g_sys_timestamp = rendezvous;
    // run_due_system_events(rendezvous);  // Commit B+
}

SchedResult scheduler_run(uint64_t budget) {
    // ARM9 issues ~2 cycles per ARM7 cycle (67 vs 33 MHz). The quantum is
    // a balance: small enough that a polled IPCSYNC/FIFO write by one core
    // is seen by the other promptly, large enough to amortize the
    // context-switch (state + call-return-stack save/restore).
    uint64_t rounds = 0;
    uint64_t last9 = 0, stall = 0;
    while (g_slot[0].cycles < budget &&
           !(g_slot[0].halted && g_slot[1].halted)) {
        scheduler_run_round();
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
    save_current();

    SchedResult r{};
    for (int c = 0; c < 2; ++c) {
        r.halted[c] = g_slot[c].halted;
        r.reason[c] = g_slot[c].reason;
        r.cycles[c] = g_slot[c].cycles;
    }
    r.rounds = rounds;
    return r;
}

const ArmCpuState& scheduler_cpu_state(int cpu) {
    save_current();
    return g_slot[cpu].state;
}

uint64_t scheduler_cpu_cycles(int cpu) {
    return g_slot[cpu & 1].cycles;
}
