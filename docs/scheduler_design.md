# Dual-CPU scheduler design — melonDS-faithful rewrite

Design call from the ChatGPT Recomp-project consult (2026-07-11), grounded in a
fresh read of melonDS `src/NDS.cpp` / `ARM.cpp`. Context: both CPUs are proven
byte-exact vs the melonDS oracle (ARM7 for 18198 insns, ARM9 for 426830) via the
insn7/insn9 fp-stream microscope; the boot deadlocks at ipcsync_w=95 in the
IPCSYNC handshake. The remaining fault is in **cross-component state production or
observation** — CPU interleave, timestamp drift, late/missed IF, HALT wake, event
order, IPCSYNC/FIFO propagation, or a stale-MMIO/compiled-loop-preemption bug —
not necessarily interleave alone.

## Headline

Replace the fixed 2048/1024 rounds with melonDS's timestamp-aligned scheduler.
Copy its ordering closely for bring-up. **Do NOT add IPC write-yields.** Treat
HALT wake as a first-class check.

## The detail my quantum experiments missed

melonDS does **not** interleave only at hardware events. Current master **caps
each outer iteration at ~64 system cycles** (with an ~8-cycle margin that can pull
a nearby event into the current iteration). It runs **ARM9 first**, uses ARM9's
**actual resulting timestamp** (post-overshoot) as the ARM7 catch-up target, then
processes scheduled events. That hard cap is why fixed-quantum tuning was
non-monotonic/misleading.

## Scheduler skeleton

```
while (running) {
    uint64_t planned = min(next_scheduled_event_time(), sys_timestamp + CAP); // CAP ~ 64 sys cyc
    arm9_target = planned << arm9_clock_shift;
    run_arm9_until_target_or_reschedule();
    // Do NOT clamp back to `planned`: ARM9 may overshoot atomically on its last insn.
    uint64_t rendezvous = arm9_timestamp >> arm9_clock_shift;
    update_arm9_timers_and_devices(arm9_timestamp);
    while (arm7_timestamp < rendezvous) {
        arm7_target = rendezvous;
        if (arm7_dma_active) run_arm7_dma();
        else                 run_arm7_until_target_or_reschedule();
        update_arm7_timers(arm7_timestamp);
    }
    sys_timestamp = rendezvous;
    run_due_system_events(rendezvous);
}
```

Nuances:
- "Catch up to ARM9" = run **until `arm7_timestamp >= target`**, not force equality
  (ARM7 can overshoot on its final instruction too).
- **Mid-slice rescheduling:** if a running CPU schedules an event earlier than its
  current target (e.g. an MMIO write creates an earlier deadline), shorten the
  target and make the active compiled block return.

Replicate all of: common timestamp units + ARM9 clock shift; ARM9-first
tie-breaking; instruction overshoot (no clamp); ARM7 runs to >= ARM9's normalized
timestamp; immediate IPC/MMIO side effects; HALT timestamp advancement; event &
timer processing order; mid-slice rescheduling.

## Cross-CPU writes are NOT a yield boundary (why yield-on-write regressed)

Under ARM9-first ordering the ARM7 may **never observe an intermediate nibble** —
it starts its interval only after ARM9 reached the rendezvous. Yield-on-write
injected observations the oracle never made (phantom handshake steps in a polling
loop) — that's why it reached only 62. Rule: a cross-CPU write is **immediately
visible in shared state but not an automatic CPU-selection boundary.** An IPC op
forces an early return ONLY when it changes a real scheduler condition (wakes a
halted CPU, or creates an earlier deadline).

## Event set (no heap needed)

`min(next LCD phase, next relevant timer overflow, next SPI/firmware transfer
completion, cap)`. A small fixed event table + presence bitmask suffices; no
heap-based queue. Do NOT model IPCSYNC/FIFO as queued events — they're synchronous
side effects: update peer-visible reg/FIFO immediately → set IF bit → wake halted
CPU → return the active block if scheduler state changed. Critical pieces for THIS
deadlock: LCD/VBlank timing; ARM7 FIFO-receive IF generation; HALT wake semantics;
timer advancement; the 64-cyc cap; correct same-timestamp ordering.

## ARM7 HALT (high priority — possibly a bug independent of the scheduler)

melonDS does not burn guest instructions while halted; with no wake condition it
fast-forwards the halted CPU's timestamp to target. **A pending FIFO-receive IRQ
wakes ARM7 even though IME=0** — HALT wake is `IE & IF`, NOT gated by IME. (Our
observed state: ARM7 IE bit18 enabled, IME disabled — exactly this case.)

Wake predicate:
```
if (arm7_halted && (IE7 & IF7)) {
    arm7_halted = false;
    if ((IME7) && !(CPSR7 & CPSR_I)) enter_irq();   // else just resume, no vector
}
```
Deadlock probe — inspect ARM7 halted? / IE7.18 / IF7.18 / FIFO-recv nonempty:
- IE18=1, IF18=1, still halted → HALT-wake predicate wrong.
- FIFO nonempty but IF18=0 → FIFO IRQ generation wrong/late.
- wakes but vectors despite IME=0 → IRQ acceptance wrong.
- wakes without vectoring, continues → expected.
"Spinning HALT" is only harmless if it advances timestamp WITHOUT retiring guest
instructions.

## Cycle accuracy — match melonDS's EFFECTIVE model, not silicon

Event-bounding limits but doesn't eliminate cycle-error damage: a small error is
harmless only while it doesn't move a peer read across a peer write. Upgrade the
microscope to compare **timestamps at equal retired-instruction indices** (CPU,
retired index, oracle ts, recomp ts, delta, PC), well before the first IPCSYNC
divergence:
- timestamps stay close, only global interleave differs → scheduler rewrite is
  likely enough.
- substantial drift at the same index → scheduler alone won't converge; improve
  the cycle model (ARM9<->ARM7 conversion, branch taken/not costs, load/store+MMIO
  costs, RAM/WRAM/TCM distinctions, multiply timing, atomic overshoot, DMA ts,
  HALT fast-forward).
Also log oracle "sync slack" = reader_ts − most-recent-peer-writer_ts; if reads
land within 1–2 sys cycles of writes, the handshake is highly timing-sensitive.

## Pre-rewrite verification (do these FIRST)

1. **ARM7 HALT wake** on `IE18 & IF18` with IME disabled — highest priority.
2. **Timestamps at equal retired counts** (does state-equality hide cycle drift?).
3. **Merged IPC transcript:** per IPCSYNC read/write log CPU, PC, retired index,
   CPU ts, sys ts, old IPC9/7, value, new IPC9/7. At the first bad read classify:
   peer write never occurred / occurred later / earlier-but-overwritten / present
   in shared reg but reader got stale data.
4. **Event lateness:** per LCD/timer/SPI/DMA event log scheduled ts, processed ts,
   lateness, CPU states (the 2048/1024 sched likely services events 100s–1000s of
   cycles late).
5. **MMIO visibility:** generated C must re-read shared regs, not hoist them into a
   local (no stale-MMIO caching).
6. **Compiled-loop preemption:** every backward branch / bounded compiled run must
   eventually test the active-CPU target; an IPC write that shortens the deadline
   must make the loop return.

## Recommended sequence

1. Verify ARM7 HALT wake on `IE18 & IF18` with IME disabled.
2. Compare normalized timestamps at equal retired counts.
3. Produce the merged IPC/IF/HALT transcript.
4. Replace fixed rounds with the 64-sys-cycle, ARM9-first, actual-timestamp
   catch-up model.
5. Match event/timer ordering + dynamic target shortening.
6. Only then improve instruction timing where the timestamp comparison proves drift.

Final call: implement the melonDS scheduler structure now; no IPC write-yields;
HALT wake is a first-class check.
