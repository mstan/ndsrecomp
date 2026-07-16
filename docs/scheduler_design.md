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

---

# Cycle-model design — ARM9 memory timing (2nd ChatGPT consult, 2026-07-11)

Follow-up consult after running Steps 1-2. STEP 1: the ARM7 is NOT stuck-halted at
the deadlock (IRQ mode, IME7=1, servicing VBlank/Timer; recv FIFO empty so IF18=0
is correct) — HALT-wake works; blocker is the ARM9 not sending the FIFO word.
STEP 2 (timestamps at equal retired index, added cyc9/cyc7 to both servers):
**ARM7 native ~1.85 cyc/insn vs melonDS ~1.63 (+13%); ARM9 native ~1.8 vs melonDS
~6-8 (-75%).** So the naive ~1 cyc/insn model is the problem.

## Verdict: need BOTH the scheduler AND an ARM9 cycle model

`Correct CPU semantics + correct cycles-per-CPU + correct timestamp-aligned
interleave = correct cross-CPU observation order.` The 64-cycle cap bounds how LONG
a timing error persists but can't fix the ARM9 executing the wrong NUMBER of
instructions per 64 cycles. Scheduler alone will reproduce the wrong order in
smaller intervals. **Pin the exact melonDS commit the oracle uses** — match THAT
effective timeline.

Unit check: melonDS stores ARM9Timestamp in ARM9-clock units (ARM9ClockShift=1),
compares `ARM9Timestamp>>1` vs ARM7/system. Native cyc9 is ARM9 ticks (2048/1024
quantum ratio), so the ~4-5x mismatch is real.

## The 8.6-cyc/insn fingerprint (early ARM9 boot = uncached BIOS code fetch)

```
BIOS 32-bit nonseq base   1 system cycle
ARM9 nonseq CPU penalty   +3
ARM9 clock shift          x2
------------------------------------
code fetch                8 raw ARM9 cycles   (+~0.6 data/internal => ~8.6)
```
The ~8.6 → ~6 drop is uncached BIOS + CP15 cacheability/TCM changes + branch/mix,
NOT cache warmup. First high-value fix = charge melonDS's uncached BIOS code-fetch
timing correctly.

## melonDS ARM9 region timings (N/S, system cycles, before the x2 shift)

| Region | Bus | Base N / S |
|---|---|---|
| BIOS | 32-bit | 1 / 1  (+ ARM9 nonseq penalty) |
| Main RAM | 16-bit | 8 / 1 |
| Shared / ARM9 WRAM | 32-bit | 1 / 1  (+ nonseq penalty) |
| I/O | 32-bit | 1 / 1  (+ nonseq penalty) |
| VRAM / palette | 16-bit | 1 / 1  (+ nonseq penalty) |
| ITCM / DTCM | internal | ~1 |

melonDS adds a **+3 ARM9 CPU nonseq penalty** outside main RAM (main RAM's nonseq
already includes its larger penalty), and **splits 32-bit accesses on 16-bit buses**.

## Minimal ARM9 model (port effective behavior, NOT full hardware)

1. Per-region non-seq/seq bus timings. 2. ARM9 clock shift. 3. ITCM/DTCM ranges.
4. CP15 MPU/cacheability state. 5. Code-fetch timing. 6. **Data timing by the
DYNAMIC data address, width, sequential status** (not PC region!). 7. Instruction
timing-class combine (Code / Code+Internal / Code+Data / Code+Data+Internal).
8. Branch/pipeline-refill.

**No real cache** — melonDS uses averaged cache timing: `code-cache=3, data-cache=3`
(3 cyc at a branch or 32-byte code boundary, 1 otherwise; real ICacheLookup() is
commented out). A real cache would be LESS timeline-compatible.

**Code/data overlap** (not simple addition):
`combine = max(code + data - 6, max(code, data))`.

Do NOT do per-PC-region average CPI — it fails on `ldr r0,[r1]` where PC is fast
WRAM but the data address is slow main RAM, or `ldmia` (first=N, rest=S). Data cost
is data-address-dependent.

## Cheap in a recompiler

PC / opcode class / ARM-Thumb / branch type are **compile-time constants** (bake
into codegen). Memory helpers already know the runtime data address + width. CP15
writes rebuild a page-timing table / bump a timing epoch. Per instruction, inline:
```c
uint32_t code = arm9_code_cycles(cpu, pc, branch_or_refill);  // mostly compile-time
uint32_t data = cpu->last_data_cycles;                        // set by the mem helper
cpu->timestamp += arm9_combine_cd(code, data);
```

## ARM7: fix ARM9 first, but ARM7's +13% still matters

`(1.85-1.63) * 18198 = ~4000 system cycles drift` over the ARM7 boot ≈ 62 of the
64-cycle intervals — enough to move an IPC poll / timer / VBlank across an ARM9
write. ARM7 needs its own N/S accesses, branch/internal categories, code/data
overlap. Priority: (1) ARM9 region/CP15/TCM/code-fetch, (2) ARM9 data timing +
combine, (3) ARM7 correction, (4) IPC/event convergence.

## Calibration via an oracle per-retired-instruction record

Emit from the oracle (to validate the model instruction-by-instruction + fit
unclear constants): retired index, PC, ARM/Thumb, timestamp delta, timing class
(C/CI/CD/CDI), branch-taken/refill, 32-byte code boundary, code cacheable/ITCM,
data address, r/w + width, first/sequential transfer, data cacheable/DTCM, CP15
timing epoch.

## Implementation sequencing (separate commits)

- **A — timestamp units + scheduler structure.** Validate the invariants below even
  though firmware WILL still deadlock. Acceptance = scheduler invariants +
  deterministic synthetic IPC ordering (NOT "reaches menu").
- **B — ARM9 region timings + CP15/TCM map.**
- **C — ARM9 instruction/data timing combination.**
- **D — ARM7 timing correction.**
- **E — enable all → firmware-boot acceptance.**

Commit-A scheduler invariants:
```
ARM9 target = system target << 1
ARM9 runs first
ARM9 may atomically overshoot target
ARM7 catches up to ARM9's actual normalized timestamp
ARM7 may overshoot on its final instruction
events run after catch-up
iteration capped at 64 system cycles
rescheduling may shorten an active target
```

## Success criteria (don't judge by global average CPI)

1. Timestamp delta at the same retired index stops growing linearly.
2. Per-instruction cycle deltas match by timing class.
3. At every IPCSYNC/FIFO read+write, normalized CPU timestamps are close to the oracle.
4. The merged IPC transcript has the same write/read observation order.
5. ARM9 sends the missing FIFO word naturally.
6. ARM7 receives it and exits the wait without any IPC-specific yielding.

---

# EXACT melonDS ARM9 timing + the combine refactor (2026-07-12, research agents A+B)

Two Sonnet agents extracted melonDS's exact ARM9 model and mapped the recompiler's
cycle emission. This is the implementation spec for the proper combine.

## melonDS ARM9 combine (ARM.h:266-303, verbatim; numC=(R15&2)?0:CodeCycles)
- AddCycles_C  (branch/simple ALU-imm): `Cycles += numC`
- AddCycles_CI (reg-shift ALU, MUL, MCR/MRC): `Cycles += numC + numI`  (ADD, no overlap)
- AddCycles_CD (store) AND AddCycles_CDI (load): `Cycles += max(numC+numD-6, max(numC,numD))`
  -- byte-identical; LOADS SKIP THE INTERNAL CYCLE ("ARM9 seems to skip" per source).
- Taken branch / PC-write: instruction's own AddCycles_* runs first, THEN JumpTo adds
  2x the TARGET region's CodeCycles (pipeline refill).

## numC (code fetch) — ARM.h:254 "all code accesses are forced nonseq 32bit"
Region N32 (post +3 penalty on every region EXCEPT main RAM, post <<1 x2 shift):
BIOS/WRAM/IO/OAM=8, main RAM=18, palette/VRAM=10, ITCM=1. NO sequential discount.
Pinned to region N32 at JumpTo; reused straight-line. Thumb odd-half (R15&2): numC=0.
CACHE: per-PU-region cacheable flag `pu&0x40` (NOT C1 bit12) -> flat kCodeCacheTiming=3
on branch/32-byte-boundary, else 1 (un-shifted). ICacheLookup is DEAD CODE on master.
Data cache regions -> kDataCacheTiming=3 flat.

## numD (data) — CP15.cpp DataRead/Write
8/16-bit single = always N (MemTimings[..][1]); 32-bit first word = N ([2]),
subsequent (LDM/STM/LDRD) = S ([3], ACCUMULATES). ITCM/DTCM = flat 1 any width.
LDM/STM: numD = N + (n-1)*S summed, ONE combine call.

## numI (internal) — ARMInterpreter*.cpp
reg-specified-shift ALU = +1; MUL/MLA/etc ARM9 = flat 1 (S=0) or 3 (S=1) -- NOT the
ARM7 operand-dependent count; MCR=2, MRC=3; loads/stores/branches = 0 (folded).

## Recompiler cycle-emission map (agent B) + the edit set
- instr_cycle_base (arm_ir.cpp:99-166) FOLDS the code fetch into the GBA base
  (branch=3="2S+1N", LDR=2, ...) -- this is why ARM7 is byte-exact, and why it does
  NOT map to melonDS's separate numC/numD/numI.
- Codegen builds one _cyc = base + shift/PC surcharges + Sum runtime_mem_cycles.
  14 runtime_tick sites, 4 mem_cycles sites (LDM loop = N per register), 6
  mul_cycles sites. (The per-insn hook no longer carries any timing: since the
  inline-counter emission it is `++g_insn_count[g_nds_active]` + an armed-gated
  runtime_insn_slow() payload call; runtime_insn_fp remains only as the
  whole-hook for pre-inline banks.)
- STATUS (2026-07-16): the PROPER combine below is IMPLEMENTED — _cyc is split
  into _code/_data/_int, every tick emits arm9_cycle_combine/arm7_cycle_combine,
  runtime_mul_cycles models ARM7 early-termination (incl. Thumb T_MUL_REG
  C-destroy), and tier3's RtBus overrides access_cycles. One landmine fixed
  2026-07-16: the post-call/fall-through RESYNC tick (all accumulators zeroed)
  must cost 0 — arm7_cycle_combine's numD==0 path returned seqC-1, charging a
  spurious +1 for ARM-mode code on the 16-bit main-RAM bus (first exposed by
  the SM64DS ARM7 runtime-RAM bank; found via insn-ring cycle diff at
  insn7=39,596,066).
- PROPER combine (as designed, now landed): split _cyc into
  _code=runtime_code_cycles(pc) / _data=Sum raw mem_cycles / _int=internal-only;
  at each tick emit runtime_tick(combine(_code,_data,_int,class)); branch
  emitters add the target refill at the transfer site. ARM7 path stays
  behaviorally identical (it's exact) -- combine branches on CPU.
- Regen: nds_recompile.exe --config bios/biosnds{9,7}.toml --bin ... --out generated
  --bank arm{9,7}_bios ; then rebuild recompiler+runner ; check dispatch_misses.log.

## Diagnostic (2026-07-12): the handshake is cycle-gated + EXTREMELY sensitive
Merged IPCSYNC transcript: both sides reach SYNC 202/202 (6th write) with cyc9 only
+2% apart, then native's ARM9 freezes while the oracle advances. The +2% is enough
to flip the ARM7's cross-CPU read (byte-exact until insn7=154719, then diverges to a
garbage PC -> ARM9 spins). The +2% is dominated by the BIOS-spin OVERCHARGE (native
9.2 vs oracle 6.0 cyc/insn in that window) -> the per-PU-region cache model is NOT
cosmetic; the ARM9 spins in the BIOS during the handshake wait. => convergence needs
BOTH the combine AND the BIOS cache, i.e. cyc9 accurate to ~1%.
