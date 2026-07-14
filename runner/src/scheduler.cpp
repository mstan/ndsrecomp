// scheduler.cpp — see scheduler.h.

#include "scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "state.h"
#include "runtime_arm.h"
#include "io.h"
#include "spu.h"
#include "wifi.h"

namespace {

struct CpuSlot {
    uint32_t    deferred_cycles = 0; // uncommitted ARM::Cycles HALT debt
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

NdsSchedulerProfile g_profile{};
uint64_t g_profile_rounds = 0;

bool profiling() {
    static const bool enabled = std::getenv("NDS_PROFILE_SCHED") != nullptr;
    return enabled;
}

using ProfileClock = std::chrono::steady_clock;

void profile_add(uint64_t& dst, ProfileClock::time_point start) {
    dst += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            ProfileClock::now() - start).count());
}

// melonDS-faithful timeline (docs/scheduler_design.md, Commit A). g_sys_timestamp
// is in SYSTEM cycles (= ARM7 cycles = ARM9 cycles >> kArm9ClockShift).
// g_slot[0].cycles is the ARM9 timestamp (ARM9 cycles); g_slot[1].cycles the ARM7.
uint64_t           g_sys_timestamp = 0;
constexpr int      kArm9ClockShift = 1;    // ARM9 runs at 2x the system clock
constexpr uint64_t kIterCap        = 64;   // system cycles per outer iteration
// Power-on periodic scheduler events in SYSTEM cycles. NextTarget scans RTC,
// SPU and LCD even before their IRQs are enabled. They phase the 64-cycle CPU
// rendezvous grid, so omitting a deadline can move an IPCSYNC write across the
// peer's poll despite each CPU's local instruction timing being exact.
uint64_t next_scheduled_event_time() {
    constexpr uint64_t kLine = 2130;
    constexpr uint64_t kHblankStart = 1584;
    const uint64_t line_base = (g_sys_timestamp / kLine) * kLine;
    const uint64_t pos = g_sys_timestamp - line_base;
    const uint64_t lcd = (pos < kHblankStart) ? (line_base + kHblankStart)
                                               : (line_base + kLine);

    const uint64_t spu = (g_sys_timestamp / 1024u + 1u) * 1024u;

    // RTC ScheduleTimer carries the 33,513,982/32,768 fractional remainder,
    // so deadline N is floor(N*33513982/32768), starting at N=1 (1022).
    constexpr uint64_t kRtcNumerator = 33513982u;
    constexpr uint64_t kRtcDenominator = 32768u;
    uint64_t rtc_n = (g_sys_timestamp * kRtcDenominator) / kRtcNumerator + 1u;
    uint64_t rtc = (rtc_n * kRtcNumerator) / kRtcDenominator;
    while (rtc <= g_sys_timestamp) {
        ++rtc_n;
        rtc = (rtc_n * kRtcNumerator) / kRtcDenominator;
    }
    return std::min(nds_next_system_event_time(),
                    std::min(nds_wifi_next_event_time(),
                             std::min(rtc, std::min(spu, lcd))));
}

void save_current() {
    if (g_cur < 0) return;
    g_slot[g_cur].state = g_cpu;
    g_slot[g_cur].crs_depth = runtime_call_stack_depth();
    const uint32_t* src = runtime_call_stack_data();
    for (uint32_t i = 0; i < g_slot[g_cur].crs_depth; ++i)
        g_slot[g_cur].crs[i] = src[i];
    g_slot[g_cur].deferred_cycles = runtime_deferred_cycles();
}

void switch_to(int cpu) {
    if (g_cur == cpu) return;
    // Save outgoing: register file AND call-return stack (a preempted
    // spin may be deep in a call chain whose returns must survive).
    save_current();
    g_cpu = g_slot[cpu].state;                       // load incoming
    runtime_call_stack_restore(g_slot[cpu].crs, g_slot[cpu].crs_depth);
    runtime_deferred_cycles_set(g_slot[cpu].deferred_cycles);
    g_nds_active = (cpu == 0) ? NDS_ARM9 : NDS_ARM7;
    g_cur = cpu;
}

// Run `cpu` for up to `quantum` cycles (its own clock), or until it
// terminally halts. A guest spin simply burns the quantum and yields —
// not a fault (the other core may unblock it).
void run_slice(int cpu, uint32_t quantum) {
    if (g_slot[cpu].halted) return;

    // DMA owns the CPU's bus slot while active.  melonDS runs DMA up to the
    // same per-round CPU target and does not execute a guest instruction in
    // that outer iteration even when the transfer completes early.
    if (nds_dma_cpu_stalled(cpu)) {
        switch_to(cpu);
        g_runtime_cycles = g_slot[cpu].cycles;
        const uint64_t cap = g_slot[cpu].cycles + quantum;
        nds_dma_run(cpu, cap);
        g_slot[cpu].cycles = g_runtime_cycles;
        return;
    }

    // A guest HALT is not a terminal slot halt. With no enabled interrupt the
    // hardware simply consumes the requested timeline without retiring an
    // instruction. On wake, enter IRQ before the next instruction exactly as
    // ARM::Execute does; ARM7 may wake with IME clear and then resume normally.
    if (nds_cpu_halted(cpu) && !nds_halt_wake_pending(cpu)) {
        g_slot[cpu].cycles += quantum;
        return;
    }
    switch_to(cpu);
    g_runtime_cycles = g_slot[cpu].cycles;
    const uint64_t cap = g_slot[cpu].cycles + quantum;
    nds_slice_begin(cap);
    if (nds_cpu_halted(cpu)) {
        nds_cpu_wake(cpu);
        if (!(g_cpu.cpsr & CPSR_I_BIT) && nds_irq_pending(cpu))
            runtime_irq(g_cpu.R[15]);
    }

    // runtime_dispatch returns at a natural boundary or a backward-branch
    // slice-yield — in both cases R15 is a dispatch entry, so re-dispatch
    // is clean. Keep stepping until this slice's cycle cap is reached.
    long guard = 0;
    while (g_runtime_cycles < cap && !g_slot[cpu].halted) {
        uint32_t pc = g_cpu.R[15];
        uint32_t t  = (g_cpu.cpsr & CPSR_T_BIT) ? 1u : 0u;
        nds_clear_unwinding();   // a fresh dispatch is a real entry, not an unwind
        runtime_dispatch(pc | t);
        // An exact-index stop reached in Tier 3 may have unwound through a
        // nested static IRQ dispatch. Restore the captured guest state after
        // those host frames return so the observer lands on the true PC.
        nds_restore_unwind_state();
        if (g_nds_terminal) {
            g_slot[cpu].halted = true;
            g_slot[cpu].reason = g_nds_halt_reason;
            break;
        }
        if (nds_dma_cpu_stalled(cpu)) {
            // ARM::Execute breaks before committing the instruction that
            // enabled DMA (Halted==2), without snapping to the slice target.
            // Recover that pre-instruction timestamp and carry its full cost
            // until the first instruction after DMA completion.
            const uint64_t enter = nds_dma_entry_cycle(cpu);
            const uint64_t final_cost =
                g_runtime_cycles > enter ? g_runtime_cycles - enter : 0u;
            g_runtime_cycles = enter;
            runtime_deferred_cycles_set(static_cast<uint32_t>(final_cost));
            break;
        }
        if (nds_cpu_halted(cpu)) {
            // melonDS notices HALT after executing the store, snaps the CPU
            // timestamp to this Execute target, but breaks before committing
            // the instruction's pending ARM::Cycles. Carry that debt across
            // sleep and commit it with the first resumed instruction.
            const uint64_t enter = nds_halt_entry_cycle(cpu);
            const uint64_t final_cost =
                g_runtime_cycles > enter ? g_runtime_cycles - enter : 0u;
            // An exact instruction break truncates melonDS's live ARM target
            // to the pre-instruction timestamp. Normal execution instead
            // snaps to the full slice target. Neither path commits the debt.
            g_runtime_cycles = nds_event_break_hit() ? enter : cap;
            runtime_deferred_cycles_set(static_cast<uint32_t>(final_cost));
            break;
        }
        // Debug-server event break: stop this slice at the dispatched-block
        // boundary right after the armed event fired, so run_to_event lands AT
        // the Nth event. The other CPU's slice this round breaks immediately
        // too (flag stays set until run_to_event disarms), so neither core
        // free-runs past the sync point.
        if (nds_event_break_hit()) break;
        // A device write may schedule an earlier system event and shorten the
        // live cap through nds_reschedule_slice().  Stop this dispatch loop at
        // that revised boundary; the scheduler will process/catch up around
        // the event instead of repeatedly redispatching with an expired cap.
        if (nds_slice_over()) break;
        if (++guard > 20'000'000) {     // host-loop backstop
            g_slot[cpu].halted = true;
            g_slot[cpu].reason = "slice guard (no progress)";
            break;
        }
    }
    g_slot[cpu].cycles = g_runtime_cycles;
    g_slot[cpu].deferred_cycles = runtime_deferred_cycles();
}

}  // namespace

void scheduler_init() {
    g_slot[0] = CpuSlot{};
    g_slot[1] = CpuSlot{};
    g_cur = -1;
    g_sys_timestamp = 0;
    scheduler_profile_reset();
}

void scheduler_reset_cpu(int cpu, uint32_t pc, uint32_t cpsr) {
    std::memset(&g_slot[cpu].state, 0, sizeof(ArmCpuState));
    g_slot[cpu].state.cpsr = cpsr;
    g_slot[cpu].state.R[15] = pc;
    // melonDS ARM::Reset calls JumpTo(ExceptionBase) after zeroing the CPU's
    // pending-cycle accumulator. The first Execute therefore commits one
    // reset-vector pipeline refill before the first guest instruction's own
    // cost (ARM9 BIOS: 16 cycles; ARM7 BIOS: 2). Seed the per-CPU timeline
    // with that same refill so cross-CPU edges start in the same phase.
    g_slot[cpu].cycles = (cpu == 0) ? arm9_refill_cycles(pc)
                                     : arm7_refill_cycles(pc);
    g_slot[cpu].deferred_cycles = 0;
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
    const bool sample = profiling() && ((g_profile_rounds++ % 1009u) == 0u);
    const auto round_start = sample ? ProfileClock::now()
                                    : ProfileClock::time_point{};
    auto phase_start = round_start;

    uint64_t planned = g_sys_timestamp + kIterCap;
    const uint64_t ev = next_scheduled_event_time();
    if (sample) profile_add(g_profile.next_event_ns, phase_start);
    // melonDS deliberately snaps to an event up to seven cycles beyond the
    // normal 64-cycle cap (kIterationCycleMargin=8).
    if (ev < planned + 8u) planned = ev;

    // ARM9 first, to its target in ARM9 cycles; do NOT clamp its overshoot.
    const uint64_t arm9_target = planned << kArm9ClockShift;
    if (sample) phase_start = ProfileClock::now();
    if (g_slot[0].started && !g_slot[0].halted && g_slot[0].cycles < arm9_target)
        run_slice(0, static_cast<uint32_t>(arm9_target - g_slot[0].cycles));
    nds_tick_timers(0, g_slot[0].cycles);
    if (sample) profile_add(g_profile.arm9_ns, phase_start);

    // Rendezvous = ARM9's ACTUAL (possibly overshot) timestamp, normalized to
    // system cycles. The ARM7 catches up to THIS, not to `planned`.
    const uint64_t rendezvous = g_slot[0].cycles >> kArm9ClockShift;

    // ARM7 catches up to the rendezvous (run-until->=; it too may overshoot its
    // final instruction). Bail if it makes no progress (terminally halted or a
    // debug/insn break is armed) to avoid a busy spin.
    if (sample) phase_start = ProfileClock::now();
    while (g_slot[1].started && !g_slot[1].halted &&
           !nds_event_break_hit() && g_slot[1].cycles < rendezvous) {
        const uint64_t before = g_slot[1].cycles;
        run_slice(1, static_cast<uint32_t>(rendezvous - g_slot[1].cycles));
        nds_tick_timers(1, g_slot[1].cycles);
        if (g_slot[1].cycles == before) break;
    }
    if (sample) profile_add(g_profile.arm7_ns, phase_start);

    // PowerMan register 0 bit 6 stops the entire console immediately from the
    // ARM7 SPI write. melonDS exits RunFrame without running any later system
    // event at this rendezvous. ARM9 has already executed first for this outer
    // iteration, so retain both live timestamps and terminate both slots here.
    if (nds_powered_off()) {
        g_slot[0].halted = true;
        g_slot[1].halted = true;
        g_slot[0].reason = "power off";
        g_slot[1].reason = "power off";
        g_sys_timestamp = rendezvous;
        return;
    }

    if (sample) phase_start = ProfileClock::now();
    nds_tick_display(rendezvous);
    nds_tick_spu(rendezvous);
    nds_wifi_run_events(rendezvous);
    nds_tick_rtc(rendezvous);
    nds_run_system_events(rendezvous);
    if (sample) {
        profile_add(g_profile.devices_ns, phase_start);
        profile_add(g_profile.sampled_round_ns, round_start);
        ++g_profile.sampled_rounds;
    }

    // melonDS overwrites its local target with ARM9Timestamp>>shift before
    // ARM7 catch-up, then RunSystem(target) advances SysTimestamp to that
    // ACTUAL normalized rendezvous (including one-instruction overshoot).
    g_sys_timestamp = rendezvous;
    // run_due_system_events(rendezvous);  // Commit B+
}

void scheduler_profile_reset() {
    g_profile = NdsSchedulerProfile{};
    g_profile_rounds = 0;
}

void scheduler_profile(NdsSchedulerProfile* out) {
    if (out) *out = g_profile;
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

uint64_t scheduler_system_timestamp() { return g_sys_timestamp; }
uint64_t scheduler_next_event_timestamp() { return next_scheduled_event_time(); }
bool scheduler_cpu_terminal_halted(int cpu) { return g_slot[cpu & 1].halted; }
const char* scheduler_cpu_halt_reason(int cpu) {
    return g_slot[cpu & 1].reason ? g_slot[cpu & 1].reason : "";
}
