# Tier-3 interpreter performance — Phase 0 baseline (beads-yjp.42)

Status: **Phase 0 complete — rig built, baseline pinned, attribution
profiled, candidates ranked. No optimization implemented.**

Companion to `docs/host_optimization_strategy.md`, whose contract and
ledger conventions this workstream inherits. Read that first; this
document adds only what is specific to the Tier-3 interpreter.

Worktree: `ndsrecomp-interp-perf`, branch `interp-perf`, from main
`e580805`. Title under test: Metroid Prime Hunters
(`90164d1ac127ee5f9815ea4ae7de798c7b5fc629`), adventure route.

---

## 0. The headline, stated up front

Forcing **every** non-BIOS guest instruction on **both** CPUs through the
Tier-3 interpreter costs about **1.7x emulation time** on the MPH
adventure route and **still holds 60 FPS at ~0.99x guest speed**.

That is not the result this campaign was set up to find. The brief
anticipated "brutal FPS"; the honest measurement says the interpreter is
roughly a 5 ms/frame tax on a 16.7 ms budget that had ~10 ms of slack.

The mechanism is **not** that guest CPU is a minority of frame time —
that was my first guess and the scheduler shares refute it (CPU is ~66%
of emulation here). It is that the interpreter costs only **~2.05x the
recompiled path per unit of guest CPU work**, because both tiers pay most
of the same per-instruction tax: profiling the faithful leg over the same
window shows ~80% of the *native* path is ABI helpers and dispatch
machinery, not recompiled guest arithmetic. The interpreter's own
overhead is layered on a baseline that is already mostly overhead.

Section 7 draws the consequence, which is uncomfortable for this
campaign: interpreter-only micro-optimization has a real but bounded
ceiling on a route that runs 0.2% interpreted, and Phase 1 should be
gated on measuring an interpreter-heavy route rather than assumed.

The result is reported as measured. Both witnesses agree the tier was
actually flipped (Section 3), which is the failure mode that invalidated
a comparable psxrecomp session and the one thing this rig was built to
rule out.

---

## 1. The selector

One binary contains both paths and chooses between them explicitly, per
the psxrecomp `HOST_OPTIMIZATION_CONTRACT.md` selector rule and this
project's own "LLE floor stays linked and forceable" clause.

| Mode | How | Meaning |
|---|---|---|
| faithful (default) | nothing | normal tiering; zero added cost |
| forced, whole run | `--force-tier3` or `NDS_FORCE_TIER3=1` | interpreted from reset, boot included |
| forced, phase-scoped | debug-server `force_tier3 {on:1|0}` | flip at a route landmark, reversible |

### Where it hooks

`lookup_static_cached()` in `runner/src/runtime_arm.cpp` is the single
chokepoint. Both consumers reach execution through it:

* `runtime_dispatch()` — every native entry (`guard_scope.call(...)`).
* `nds_has_bank()` — Tier 3's per-instruction Tier-1-takeover poll
  (`tier3.cpp` step loop).

So one branch covers both CPUs, static banks, live-overlay shards, calls,
tail dispatches, exception vectors and the interpreter's own re-entry
check. The original function was renamed `lookup_static_cached_impl` and
a thin wrapper added:

```cpp
const CachedStaticLookup* lookup_static_cached(const CpuCtx& c, uint32_t pc,
                                               bool thumb) {
    const CachedStaticLookup* hit = lookup_static_cached_impl(c, pc, thumb);
    if (!g_nds_force_tier3 || !hit || static_bios_pc(pc)) return hit;
    ++g_nds_force_tier3_misses;
    return nullptr;
}
```

### Two design decisions worth defending

**The forced miss is applied AFTER the real lookup, not instead of it.**
Short-circuiting ahead of the cache would have been faster and wrong: the
per-instruction `nds_has_bank` poll — hash, 128-byte cache-line probe,
`cached_lookup_live()` page-generation check — is one of the costs the
campaign exists to measure. Skipping it would have reported a flattering
number for a path no player ever runs. The lookup runs; its result is
discarded. A pleasant side effect is that the mode needs no cache or
epoch invalidation, so the runtime toggle is a single `bool` store and is
safe from the debug-server thread.

**BIOS banks stay native; captured-firmware banks do not.** Tier 3 can
only execute bytes that have write provenance in writable RAM
(`tier3.cpp` refuses otherwise, and `runtime_dispatch` turns such a PC
into a fatal dispatch miss). The BIOS is immutable ROM with neither, so
forcing it would have produced a crash rather than a measurement. It is
also the representative shape: a player who has lost native game banks
still has a native BIOS. Captured-firmware banks live in RAM with
provenance, so they are forced like game code — which is what a real
fallback would face. The predicate is `static_bios_pc(pc)`, the same
range test the rest of the runtime already uses (ARM9 `0xFFFF0000..
0xFFFF1000`, ARM7 `<0x4000`).

Consequence, visible in the data: ARM9 tier3 share reaches ~100% while
ARM7 settles near 94%. The ARM7 residual is its BIOS idle/IRQ path, by
construction, not a leak.

### Cost when unset

One predictable branch on a `bool`, on a path that has already done a
hash and a cache probe. The faithful control measured 5.12/5.62/6.83
ms/frame against the known-good build's 5–7 ms/frame envelope — inside
noise.

---

## 2. The rig

| File | Role |
|---|---|
| `runner/src/state.h` | declares `g_nds_force_tier3`, `g_nds_force_tier3_misses` |
| `runner/src/runtime_arm.cpp` | selector wrapper; counters in `nds_dispatch_stats_json()` |
| `runner/src/main.cpp` | `--force-tier3`, `NDS_FORCE_TIER3`, startup announcement |
| `runner/src/debug_server.cpp` | `force_tier3` command (read / set) |
| `tools/measure_mph_forced_tier3.py` | route harness with the tier gate |
| `tools/profile_mph_forced_tier3.py` | RIP sampler + symbolizer for the forced phase |

The two Python tools are modified copies of the mph-perf harnesses
(`measure_mph_scenario.py`, `profile_mph_worst_phase.py`). The mph-perf
worktree is read-only reference; the change needed was structural, not a
new flag, so the copies live with the selector they drive. **Route
landmarks are copied verbatim** — `TITLE_VBLANK=7800`, the
`adventure_start.json` scenario, and the cumulative-ARM9-instruction
phases `settle 5M / walk 40M / steady 125M`. Instruction landmarks are
what make the two legs comparable: they describe guest work, so host
seconds are the output rather than the input.

### Why the harness had to be copied rather than driven by env alone

The adventure route reaches gameplay through 7800 guest VBlanks of boot,
logos and FMV, then a replayed menu scenario. Interpreting all of that
would have measured menus, at great expense. So **both legs navigate the
route faithfully at native speed**, entering the measured phases from an
identical guest state, and the forced leg flips the selector at the phase
boundary through the debug server. A drain window (250k ARM9
instructions, configurable) then lets recompiled functions already on the
host stack retire, so the first measured phase is not depressed by a
native tail. `NDS_FORCE_TIER3=1` remains available for anyone who wants
the boot path itself interpreted.

---

## 3. Tier verification — the gate, not a footnote

The one lesson imported from the psxrecomp interp-perf session: a number
is worthless until the tier you think you measured is proven to be the
tier that ran. That session chased a phantom 40 FPS figure produced by
shards which had silently failed to compile.

Every phase is checked against two independent witnesses, and a run
failing either is marked invalid and contributes no number:

1. **`dispatch_stats.forced_tier3`** matches the requested mode, and
   `forced_tier3_misses` actually advanced during the phase. A flag that
   is set but converts no lookups proves nothing.
2. **`static_coverage.tier3_insns9` delta / `event_counts.insn9` delta**
   exceeds 0.90 forced, and stays below 0.05 faithful.

Witness 2 is load-bearing: it is measured on the guest instruction stream
itself and cannot be satisfied by a mis-set flag.

### A skew bug this gate caught in its own instrumentation

The first campaign reported ARM9 tier3 shares of **1.0885 / 1.0160 /
1.0049** — impossible values. `event_counts` and `static_coverage` are
two separate TCP round trips and the guest keeps executing between them;
reading them far apart let the coverage counter run ahead. The fix was to
read the pair adjacently at both ends so the skew cancels to first order,
and to take one extra `event_counts` round trip per phase so the
residual is *measured* (`counter_skew_insn9`) rather than assumed away.

This is worth recording because the failure was benign-looking: a share
of 1.0049 would have read as "100%, near enough" if it had not also been
mathematically impossible. The impossible value is what exposed it.

---

## 4. Baseline

Quiet host, headed, `--boot direct`, adaptive/compute renderer `auto`,
scheduler+GPU profiling armed on **both** legs (a few percent, applied
equally — it is what separates "the interpreter is fast enough" from
"guest CPU is a minority of frame time either way").

Two campaigns were run. **A** (`20260826-180557-forced-tier3-phase0`) is
the A/B pair, unprofiled. **B** (`…-forced-tier3-phase0v2`) re-ran the
faithful leg with the counter-skew fix and scheduler profiling armed, to
explain A rather than replace it. B's control reproduces A's control to
within 0.6% (6.83 vs 6.87 ms/frame at steady), which is what licenses
reading the two together.

### The pinned baseline: 3 faithful + 3 forced (campaign P)

`perf-results/20260826-185628-forced-tier3-phase0-pinned`, quiet host,
unprofiled-by-request but with scheduler sampling armed on both legs.
**All six runs passed both tier witnesses**; none was discarded.

Every raw run, emulation ms/frame:

| Phase | faithful 1 / 2 / 3 | forced 1 / 2 / 3 |
|---|---|---|
| settle | 5.347 / 5.521 / 5.071 | 9.341 / 8.896 / 8.759 |
| walk | 6.169 / 5.783 / 5.672 | 10.376 / 9.585 / 9.801 |
| steady | 7.127 / 6.797 / 7.002 | 12.352 / 11.723 / 11.594 |

| Phase | faithful median | forced median | ratio | faithful min | forced min | min ratio |
|---|---|---|---|---|---|---|
| settle | 5.347 | 8.896 | **1.664x** | 5.071 | 8.759 | 1.727x |
| walk | 5.783 | 9.801 | **1.695x** | 5.672 | 9.585 | 1.690x |
| steady | 7.002 | 11.723 | **1.674x** | 6.797 | 11.594 | 1.706x |

60 FPS held in every run of both legs (59.5–60.2), guest speed 0.98–1.00x.
ARM9 tier3 share 0.0018–0.0022 faithful and 0.9915–1.0006 forced.

Forced/faithful emulation-time ratio: **1.66x / 1.70x / 1.67x** on
medians, 1.69–1.73x on min-of-3 — strikingly consistent across three
phases of very different weight, and reproducing campaign A's
single-run 1.67/1.69/1.69 to within 2%.

### The original single-run campaign A, for continuity

| Phase | Mode | emu ms/frame | FPS | guest speed | ARM9 tier3 share |
|---|---|---|---|---|---|
| settle | faithful | 5.12 | 60.33 | 0.913x | 0.0020 |
| settle | **forced** | **8.55** | 59.78 | 0.903x | ~1.00 |
| walk | faithful | 5.62 | 59.79 | 0.983x | 0.0021 |
| walk | **forced** | **9.50** | 59.83 | 0.984x | ~1.00 |
| steady | faithful | 6.83 | 59.85 | 0.992x | 0.0022 |
| steady | **forced** | **11.57** | 59.82 | 0.992x | ~1.00 |

Forced-leg selector counters (`forced_tier3_misses` per phase):
6,348,352 / 41,782,374 / 98,711,795. ARM7 tier3 share 0.94, the residual
being its native BIOS by design.

Forced steady sustained **8.20 M ARM9 instructions/second** interpreted
while holding 60 FPS.

### Reported forced shares, and the correction

Campaign A's raw ARM9 shares were 1.0885 / 1.0160 / 1.0049 — the skew
described in Section 3, not a real excess. Campaign B measures the skew
directly at 95k–125k instructions per extra command round trip, which
accounts for A's excess at each phase size (largest at `settle`, the
smallest window, exactly as an additive skew predicts). The true shares
are at the 0.99–1.00 ceiling. The gate's requirement (>0.90) is met by a
wide margin either way, so no conclusion here depends on the correction.

### Where the time goes (campaign B, faithful leg, scheduler sampling)

| Phase | ARM9 share of scheduler | ARM7 share | CPU total |
|---|---|---|---|
| settle | 0.459 | 0.170 | 0.629 |
| walk | 0.485 | 0.159 | 0.644 |
| steady | 0.491 | 0.168 | 0.659 |

This is the number that turns the headline from a curiosity into a
finding. Guest CPU execution is **~66% of emulation time even on the
faithful path** — this route is CPU-heavy, not device-bound. So the small
forced penalty cannot be explained away by "the CPU barely matters here".

Working it through at `steady`: emulation 6.83 ms, of which CPU ≈ 4.50 ms
and devices ≈ 2.33 ms. Forced emulation is 11.57 ms; the entire +4.74 ms
lands on the CPU portion, taking it 4.50 → 9.24 ms.

> **The Tier-3 interpreter costs about 2.05x the recompiled path per unit
> of guest CPU work on this route.**

Two-ish, not twenty-ish. That single number reframes the campaign, and
Section 6 explains why it is so low — the answer is not that the
interpreter is fast.

### Honest limits of this baseline

* **Repetitions: 3 forced + 3 faithful (campaign P, 2026-08-26).** The
  earlier 1+2 caveat is discharged; medians and min-of-3 are both given
  above and agree. The remaining spread within a leg is ~8% at `settle`
  (the smallest window) and ~5% at `steady`, so ratios are quoted to two
  decimal places and differences below ~5% between legs are noise.
* Campaign A is unprofiled, B profiled; the scheduler shares carry
  sampling overhead (1-in-1009 rounds) and A's ms/frame do not.
* One host, one title, one route. `adventure` is a moving-camera 3D
  workload; menu and FMV phases were not measured forced.
* An earlier run of this campaign was discarded after I misread a slow
  route replay as a hang and killed it. Nothing from that is reported.

---

## 5. Attribution profile

`tools/profile_mph_forced_tier3.py`, 1 ms RIP sampling of the forced
settled-gameplay window (`perf-results/phase0-forced-profile`).

**Tier verified for the sampled window itself:** ARM9 tier3 share
**0.9996**, 85,672,243 ARM9 instructions, 622 frames, 59.81 FPS, 12.01 ms
emu/frame. The profile describes the interpreter, not a mixture.

Sampler totals: 143,108 samples across 14 threads; 6,808 landed inside
the runner image. Shares below are given both ways — of the 2,718
samples in the symbolizer's top-50 (function, source-line) rows, and of
all 6,808 runner-image samples. The top-50 covers 40% of runner samples,
so **the tail is real**: absolute shares are lower bounds, and the
ranking, not the magnitude, is the load-bearing output. `nm`+`addr2line`
split each function across several source lines; these are re-aggregated
by function.

| # | Function | of top-50 | of runner samples |
|---|---|---|---|
| 1 | `lookup_static_cached_impl` | 19.57% | 7.81% |
| 2 | `sync_out` | 12.33% | 4.92% |
| 3 | `ArmDecoder::decode` | 10.85% | 4.33% |
| 4 | `bus_range_has_write_provenance` | 10.45% | 4.17% |
| 5 | `tier3_run` | 6.92% | 2.76% |
| 6 | `coverage_note` | 6.88% | 2.75% |
| 7 | `ArmDecoder::decode_data_processing` | 6.03% | 2.41% |
| 8 | `Interpreter::step` | 4.60% | 1.84% |
| 9 | `Interpreter::cond_passes` | 4.01% | 1.60% |
| 10 | `ArmDecoder::decode_single_data_transfer` | 3.46% | 1.38% |
| 11 | `RtBus::write32` | 2.69% | 1.07% |
| 12 | `nds_dma_cpu_stalled` | 2.47% | 0.98% |
| 13 | `runtime_code_cycles` | 2.35% | 0.94% |
| 14 | `runtime_call_should_return` | 1.36% | 0.54% |
| 15 | `nds_irq_pending` | 1.36% | 0.54% |
| 16 | `coverage_note_generation_entry` | 1.32% | 0.53% |
| 17 | `RtBus::read32` | 1.25% | 0.50% |
| 18 | `resolve` | 1.14% | 0.46% |
| 19 | `nds_cpu_halted` | 0.96% | 0.38% |

Rolled up into cost classes (share of top-50):

| Class | Share | Members |
|---|---|---|
| **Instruction decode** | **20.3%** | `decode` + `decode_data_processing` + `decode_single_data_transfer` |
| **Tier-1 takeover poll** | **19.6%** | `lookup_static_cached_impl` |
| **Register publication** | **12.3%** | `sync_out` |
| **Fetch provenance** | **11.6%** | `bus_range_has_write_provenance` + `resolve` |
| **Coverage capture** | **8.2%** | `coverage_note` + `coverage_note_generation_entry` |
| Interpreter proper | 8.6% | `step` + `cond_passes` |
| Loop body | 6.9% | `tier3_run` |
| Per-insn state polls | 4.8% | `nds_dma_cpu_stalled` + `nds_irq_pending` + `nds_cpu_halted` |
| Bus virtual calls | 3.9% | `RtBus::write32` + `RtBus::read32` |
| Shared cycle model | 2.4% | `runtime_code_cycles` |

**The interpreter proper — decode plus `step` plus `cond_passes` — is
28.9%. The other 71% is bookkeeping around it.** That, not interpreter
quality, is the finding.

### The faithful control profile — why the ratio is only 2.05x

The same sampler over the same window with the selector **off**
(`perf-results/phase0-faithful-profile`; tier3 share 0.0022, 622 frames,
59.80 FPS, 7.48 ms emu/frame, 85,760,994 ARM9 instructions — the same
guest work to within 0.1%):

| # | Function | of top-50 |
|---|---|---|
| 1 | `runtime_code_cycles` | 21.48% |
| 2 | `runtime_should_yield` | 14.32% |
| 3 | `lookup_static_cached_impl` | 13.32% |
| 4 | `runtime_tick` | 10.39% |
| 5 | `runtime_mem_cycles` | 9.15% |
| 6 | `runtime_unwinding` | 6.57% |
| 7 | `runtime_dispatch` | 4.69% |
| 8 | `bus_write_u32` | 2.82% |
| 9 | `runtime_trace_event` | 2.23% |
| 10 | `decode_text_line` (GPU2D) | 2.00% |

**~80% of the native path is per-instruction ABI helpers and dispatch
machinery. Recompiled guest arithmetic barely appears.** This
independently reproduces the 2026-07-31 finding in
`host_optimization_strategy.md` (29.7–31.7% dispatch, 14.8–19.3% per-insn
helpers) on a different title.

That is the mechanism behind Section 4's 2.05x: the interpreter is not
close to native because it is fast, but because **native is already
paying most of the same per-instruction tax.** `runtime_code_cycles`,
`runtime_mem_cycles` and `runtime_tick` — the top of the native profile —
are charged by the Tier-3 loop too.

Two independent corroborations of the timing result fall out of the
sampler: runner-image samples 6,808 forced vs 4,406 faithful (**1.55x**),
and emu 12.01 vs 7.48 ms/frame in these very runs (**1.61x**), against
campaign A's 1.69x. Three different instruments, same answer.

### What this corrected in my own pre-profile ranking

I ranked candidates from code reading before sampling. Two were wrong,
and the profile is why they are not in the plan below:

* I ranked `sync_out` first. It is second (12.3%); the **dispatch poll is
  first** at 19.6%. Reading the code, `nds_has_bank` looked like a cheap
  cache probe. It is the single most expensive function on the path.
* I estimated the call-return-stack scan at 5–12%. It is **1.4%**. The
  reasoning (a whole-depth scan that must miss, on every loop back-edge)
  was mechanically correct and the conclusion was still wrong, because
  the real call depth in this workload is shallow.
* I did not rank coverage capture at all. It is **8.2%** —
  `coverage_note`, an `unordered_map` lookup on every Tier-3 root, call
  and indirect branch, which forcing makes constant.

Keeping the forced miss *after* the real lookup (Section 1) is what made
finding #1 possible. Short-circuiting ahead of the cache would have
deleted the largest single cost from the measurement and reported a
flattering, useless profile.

---

## 6. Ranked candidates

### First, why 2.05x and not 20x

A switch-dispatch interpreter that rebuilds a 92-byte decoded `Instr` per
instruction and reaches memory through virtual calls should not be within
2x of static recompilation. It is, and the reason is not that the
interpreter is fast — it is that **both tiers pay most of the same
per-instruction tax.**

`runtime_code_cycles()`, `runtime_mem_cycles()`, the ARM9/ARM7 cycle
combine, and the retired-instruction hook are charged identically by the
generated banks (via `runtime_insn_fp` / the inline tick sites) and by
the Tier-3 loop. Section 5's faithful control measures the consequence
directly: **~80% of the native path is ABI helpers and dispatch**, with
`runtime_code_cycles` alone at 21.5%. Recompiled guest arithmetic is not
in the top ten.

The interpreter's genuine extra work (decode, the step switch, virtual
bus calls, register sync) is therefore layered on top of a baseline that
is already dominated by shared overhead, which compresses the ratio.

This has a sharp consequence for Phase 1 targeting: **the biggest
interpreter-only costs below are also the ones with no native
counterpart.** Anything shared with the banks belongs in the main
host-optimization workstream, not here, because improving it moves both
legs and does not close the gap.

### The per-instruction path, as audited

`tier3_run()`, `runner/src/tier3.cpp:253-509`, in execution order:

| # | Step | Interpreter-only? |
|---|---|---|
| 1 | `sync_out(ic)` — 45 words, unconditional | **yes** |
| 2 | `nds_event_break_hit()` | yes (cross-TU call) |
| 3 | `g_nds_insn_stop` | yes |
| 4 | `nds_has_bank()` → `lookup_static_cached` | **yes** |
| 5 | `bus_range_has_write_provenance(pc,4)` | **yes**, uncached |
| 6-7 | instruction counters + retire hook | shared |
| 8 | `coverage_note_exec()` | yes, but already cached |
| 9 | `live_overlay_note_tier3()` | yes, early-out |
| 10 | `runtime_code_cycles(pc)` | shared |
| 11 | decode → 92-byte `Instr` | **yes** |
| 12-14 | `cond_passes`, `begin_instruction`, `step()` | **yes** |
| 15 | cycle combine / refill | shared |
| 16 | `nds_cpu_halted` / `nds_dma_cpu_stalled` | yes |
| 17 | CRS push / `runtime_call_should_return` | partly |
| 18-19 | `nds_irq_pending()`, `nds_slice_over()` | yes |

Measured: `sizeof(Instr)` = 92 bytes, `sizeof(CPUState)` = 184 bytes,
`sync_out` writes 45 words.

### Ranked candidates

Ranked by **measured** profile share, not by code-reading intuition —
which Section 5 shows got the order wrong twice. Expected wins are
mechanism estimates against the measured share. Nothing is implemented.

**C0 — Stop polling `nds_has_bank` on every interpreted instruction.**
*Measured 19.6% — the largest single function on the path.*
`tier3.cpp:287` asks "does a static bank now cover this PC?" before every
instruction, and each ask is a hash, a 128-byte cache-line probe, an
epoch compare and `cached_lookup_live()`'s page-generation walk. But the
answer can only change for three reasons: control flow moved to an
address that is a bank entry, a bank was registered or unregistered
(`ctx.dispatch_epoch` bumps), or guest code was rewritten
(`g_coverage_write_epoch` bumps). Straight-line execution within a page
cannot spontaneously become covered. Fix: poll on control transfer, on
either epoch changing, and on crossing a page boundary; skip otherwise.
*Expected: 12–17% of interpreted CPU time.* The epochs it needs already
exist and are already maintained — this is a frequency reduction, not a
new invariant, which is what makes it both the biggest and the safest
win available.

**C1 — Publish only the three architectural fields anything reads.**
*Measured 12.3%.*
`sync_out(ic)` runs unconditionally at the top of every iteration and
writes 16 `R` + 6 `banked_sp` + 6 `banked_lr` + 6 `banked_spsr` + 5
`r8_12_user` + 5 `r8_12_fiq` + packed CPSR = **45 words per interpreted
instruction**. A grep across `bus.cpp`, `io.cpp`, `live_overlay.cpp`,
`gpu3d.cpp`, `spu.cpp` and `vram.cpp` finds that mid-instruction
observers read exactly three things: `g_cpu.cpsr` (6 sites),
`g_cpu.R[15]` (4 sites) and `g_cpu.R[14]` (1 site). The other 42 words
have no reader until the tier exits. Fix: publish `{R15, R14, cpsr}` on
the hot path and keep the full `sync_out` at tier exits, IRQ/SWI entry,
halt/unwind and the return-idiom path (the debug server reads `g_cpu`
only between frames, never mid-instruction, so it is already safe).
*Expected: 8–15%.* Lowest risk of anything here — it removes stores that
provably nobody loads.

**C2 — Cache the fetch-provenance check per page and write epoch.**
*Measured 11.6% (`bus_range_has_write_provenance` + `resolve`).*
It runs per interpreted instruction (`tier3.cpp:297`) and is the most
expensive uncached item on the path: `resolve()` (CP15/TCM/mirror
classification), then `written_for_ptr()` (up to five vector base/size
range compares), then `std::all_of` over the fetch bytes. It is a
per-page invariant that can only change when a guest write lands. The
identical problem is already solved one line away: `coverage_note_exec()`
short-circuits on `(page, g_coverage_write_epoch)`. Fix: give the
provenance check the same `(cpu, page, write-epoch)` memo.
*Expected: 8–11%.* The in-tree precedent means the correctness argument
is already made, and it composes with C0 (both key off the write epoch).

**C3 — Make Tier-3 coverage capture proportional to discovery.**
*Measured 8.2% (`coverage_note` + `coverage_note_generation_entry`).*
Not in my pre-profile list at all. `coverage_note` does an
`unordered_map` lookup for every Tier-3 root, call and indirect branch,
and `tier3_run` notes a root on every entry. Under forcing those events
are continuous. The map deduplicates and only counts hits after the
first, so after a short warm-up **every one of these lookups is a hit
that changes nothing but a counter** — the design assumed Tier 3 was
rare. Fix: a small direct-mapped recent-key filter in front of the map
(the same shape as `coverage_note_exec`'s page memo) so a repeat costs a
compare rather than a hash. *Expected: 5–7%.* Note this is a
**coverage-fidelity-neutral** change only if the filter feeds the same
hit counters; beads-yjp.28 made recording unconditional deliberately, and
that intent must be preserved.

**C4 — Decoded-instruction cache keyed by (page, offset, generation).**
*Measured 20.3% as a class — the largest, and the largest change.*
Every instruction is decoded from scratch into a fresh 92-byte `Instr`
(`zero_instr`, then field fills, then return by value). Straight-line and
looping code re-decodes the same words indefinitely. Fix: a direct-mapped
decode cache validated by the same page write-generation the dispatch
cache already maintains, so self-modifying and overlay code invalidate
correctly by construction. *Expected: 12–18%* (not the full 20.3%: a
cache hit still costs a probe and an `Instr` copy, and shrinking `Instr`
is a separate change). Attempt only after C0–C3 — they are cheaper, and
their landing also makes the decode share legible in a re-profile.

**C5 — Do not scan the call-return stack on a plain `B`.**
*Measured 1.4% — downgraded from my pre-profile estimate of 5–12%.*
`tier3.cpp:419` runs `sync_out(ic)` and then
`runtime_call_should_return()` for **every taken non-call branch**,
including every `B` closing a loop. `runtime_call_should_return`
(`runtime_arm.cpp:1451`) walks the entire CRS depth and, for a plain
branch, is guaranteed to miss — the cost is O(call depth) per loop
back-edge. The generated banks never pay this: their intra-function
branches are local `goto`s. The code already knows the case is special
(`in.op != IrOp::B` guards the *coverage* note two lines later) but the
scan has already happened. Fix: a PC-relative direct `B` is never a
return idiom — skip both the scan and its `sync_out`.
*Expected: ~1%.* Retained because it is nearly free and the reasoning is
sound; **not** because it is worth a session on its own. Fold it into
whichever change touches that code path.

**C6 — Devirtualize the bus interface for the runner's concrete bus.**
*Measured 3.9% (`RtBus::write32` + `RtBus::read32`).*
`armv4t::Bus` is an abstract base; every guest load/store, and every
`access_cycles()` call, is an indirect call through `RtBus`'s vtable,
which also blocks inlining of the `bus_read_*` fast paths that
`runtime_arm.h` provides. `RtBus` is the only implementation the runner
ever instantiates. Fix: template `Interpreter::step` on the bus type, or
give the runner a final-typed bus with a non-virtual fast path, keeping
the virtual interface for the portable core's other users.
*Expected: 2–4%,* concentrated in memory-heavy code. Touches
`external/arm-recomp-core`, so it is a submodule change with wider blast
radius — weigh that against a 3.9% share.

**C7 — Hoist the loop-invariant per-instruction polls.**
*Measured 4.8% (`nds_dma_cpu_stalled` 2.5%, `nds_irq_pending` 1.4%,
`nds_cpu_halted` 1.0%).*
`nds_event_break_hit()`, `g_nds_insn_stop`, `nds_cpu_halted()`,
`nds_dma_cpu_stalled()`, `nds_irq_pending()` and `nds_slice_over()` are
six cross-TU calls per instruction, most reading a value that changes
only at a device or scheduler event. Fix: collapse into one armed
`g_tier3_poll_pending` word maintained by the existing event/IRQ/halt
mutation sites (the `g_insn_hook_armed` pattern in `io.cpp` is the
in-tree precedent), with the slow path taken only when it is set.
*Expected: 3–4%.* Must preserve exact IRQ-delivery and slice-boundary
points — this is the candidate most able to break guest-visible timing,
so it needs the G3 byte-lock more than the others.

### Explicitly disproved — do not re-investigate

* **Deep-trace ring writes are not on the hot path in play mode.**
  `g_runtime_deep_trace` defaults to 1, which looks alarming, but
  `main.cpp:2029` sets it to 0 for `--interactive` — the mode both
  harnesses use. That one flag gates *both* Tier 3's per-instruction
  `trace_push` *and* the inline bus fast path in `runtime_arm.h`. Serve
  mode is the one that needs `NDS_DEEP_TRACE=0` passed explicitly. A
  plausible-looking "unconditional diagnostic writes per instruction"
  finding here would have been wrong.
* **`coverage_note_exec` is not worth attacking.** It is inline and
  already short-circuits on (page, write epoch) — it is the model for C3,
  not a target.
* **`nds_note_insn_retired` is not expensive in play mode.**
  `g_insn_hook_armed` is 0 once deep trace is off, leaving one increment
  and a predictable branch.
* **"Guest CPU is a minority of frame time" is false for this route.**
  It was my first explanation for the small forced penalty; the
  scheduler shares refute it at ~66% CPU. The real explanation is shared
  per-instruction overhead (top of this section).

---

## 7. Ledger

Same conventions as `docs/host_optimization_strategy.md`: every entry
records what was done, what was measured, and what was disproved.

- 2026-08-26 **Phase 0 rig + baseline (beads-yjp.42)**. Added a
  forced-interpreter selector at the single dispatch chokepoint
  (`lookup_static_cached`), reachable three ways (`--force-tier3`,
  `NDS_FORCE_TIER3=1`, debug-server `force_tier3`), with BIOS banks
  deliberately left native and the forced miss applied *after* the real
  lookup so the per-instruction poll cost stays in the measurement.
  Selector state and a converted-lookup counter are exposed in
  `dispatch_stats` so a harness can verify the tier rather than assume
  it. Copied and extended the MPH route harness and worst-phase profiler
  with a two-witness tier gate. **Measured, MPH adventure, 1 forced + 2
  faithful runs:** forced/faithful emulation time **1.67x / 1.69x /
  1.69x** across settle/walk/steady, **60 FPS held in both legs**, guest
  speed ~0.99x, ARM9 tier3 share ~1.00 forced and 0.002 faithful.
  Scheduler sampling puts guest CPU at **~66% of emulation time on the
  faithful path**, from which the interpreter costs **~2.05x the
  recompiled path per unit of guest CPU work**. **Disproved:** deep-trace
  ring writes are not on the play-mode hot path (`--interactive` clears
  `g_runtime_deep_trace`, which also enables the inline bus fast path);
  `coverage_note_exec` and `nds_note_insn_retired` are already cheap;
  "guest CPU is a minority of frame time" is false for this route.
  **Profiled** both legs over the same settled-gameplay window: the
  forced path is 20.3% decode, 19.6% Tier-1 takeover poll, 12.3% register
  publication, 11.6% fetch provenance, 8.2% coverage capture — only 28.9%
  is the interpreter proper. The faithful path is ~80% per-instruction
  ABI helpers and dispatch (`runtime_code_cycles` 21.5%,
  `runtime_should_yield` 14.3%, `lookup_static_cached_impl` 13.3%),
  independently reproducing the 2026-07-31 native-path finding on a
  different title. **Pinned 2026-08-26 at 3+3 repetitions** (campaign P,
  `20260826-185628-forced-tier3-phase0-pinned`): forced/faithful medians
  **1.66x / 1.70x / 1.67x**, min-of-3 **1.73x / 1.69x / 1.71x**, all six
  runs passing both tier witnesses, 60 FPS held throughout.
  Nothing was optimized; no guest-visible behaviour changed; the
  selector is off unless explicitly requested.

### Gates still owed before any Phase 1 change lands

Phase 0 changed no execution path in the default mode, so the standard
gates were not run. Any C1–C6 implementation must clear them, and the
contract's clause 1 (bit-exact semantics) applies with full force —
several candidates touch register publication and IRQ-delivery points,
which are exactly what G3 exists to catch.

* G3 byte-lock (100M..700M, both framebuffers)
* G1 eight firmware scenarios, tier3 = 0
* G2 2,400-frame soak, underruns = 0, pinned FNV pair
* plus, new to this workstream: **a forced-tier3 run of the adventure
  route must still pass the two-witness tier gate afterwards**, since an
  optimization that accidentally lets native banks resolve would look
  like an enormous interpreter win.

### Recommended Phase 1, and the honest case against it

Order: **C0 → C1 → C2 → C3**, re-profile, then decide on **C4**. Those
four are 51.7% of measured interpreted cost, all reuse epochs and memo
patterns the runtime already maintains, and none of them changes what is
computed — only how often. C5 folds into whichever of them touches its
line. C6 crosses into `external/arm-recomp-core` for 3.9%, and C7 is the
one most able to perturb guest-visible timing for 4.8%; hold both.

But the strategy contract's clause 3 says Tier 3 is an emergency fallback
and any change that *surfaces* tier-3 activity is a regression — and the
faithful path already runs at 0.2% tier3 on this route. So the value of
this work is not frame rate on MPH adventure today. It is:

1. **Titles and phases without bank coverage**, where tier3 share is not
   0.2%. MKDS multiplayer and runtime-copied overlay code are the known
   cases (see the local-MP FPS work). The right next measurement is a
   forced-tier3 number on a route that is *already* substantially
   interpreted, not more decimal places on this one.
2. **A real fallback floor.** 2.05x with 60 FPS held means a player who
   loses bank coverage degrades gracefully rather than catastrophically.
   That is worth knowing and worth protecting.

Against that, C0–C3 together are ~52% of interpreted CPU time, which on
*this* route is ~0.2% of frame time. **Phase 1 should be gated on
measuring a genuinely interpreter-heavy route first.** Doing C0–C3
because they are easy and well-evidenced would be optimizing a path this
title does not take.

3. **A third option the profile actually argues for.** The faithful
   control says the *native* path is ~80% per-instruction ABI helpers and
   dispatch, with `lookup_static_cached_impl` at 13.3% there and 19.6%
   forced. C0's mechanism — poll only when an epoch or control flow could
   have changed the answer — may have a native analogue, and
   `runtime_code_cycles` / `runtime_mem_cycles` / `runtime_tick` are
   charged by both tiers. Work aimed at that shared cluster pays on every
   title in every mode, which the interpreter-only candidates do not.

So the ranked list above is the answer to the question asked, and it is
sound. But the most valuable next steps are not on it: point this same
rig at **MKDS local multiplayer** (a route already substantially
interpreted), and take the shared per-instruction cluster to the main
host-optimization workstream.

---

## 8. Reproduction crib

```powershell
# build (from PowerShell; git-bash PATH breaks the mingw -O3 compile)
C:\msys64\mingw64\bin\cmake.exe -S runner -B runner/build-interp-perf -G Ninja `
  -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -g" `
  "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -g" -DNDS_BOOTSTRAP_FIRMWARE=ON `
  -DNDS_ENABLE_COMPUTE_RENDERER=ON `
  "-DNDS_GENERATED_DIR=F:\Projects\ndsrecomp\ndsrecomp\generated" `
  "-DNDS_TITLE_BANK_DIR=F:\Projects\ndsrecomp\metroidprimehuntersrecomp-mph-perf\generated\recomp" `
  "-DNDS_TITLE_ROM_SHA1=90164d1ac127ee5f9815ea4ae7de798c7b5fc629"
C:\msys64\mingw64\bin\cmake.exe --build runner/build-interp-perf

# forced-vs-faithful baseline (ports 19920+)
py -3 tools\measure_mph_forced_tier3.py --tag <tag> --profile `
   --normal-repetitions 3 --forced-repetitions 3

# attribution profile of the forced settled-gameplay phase
py -3 tools\profile_mph_forced_tier3.py --output perf-results\<tag>-profile

# faithful control profile, same window
py -3 tools\profile_mph_forced_tier3.py --no-force --output perf-results\<tag>-profile-control
```

`-g` is added to the Release flags. It changes no codegen at `-O3`; it is
what lets `nm -n -C` and `addr2line` attribute samples to real symbols
instead of `??`.

Do not run two of these at once, and check for other sessions' runners
before trusting a timing number — this machine shares a process
namespace. Never terminate `nds_runner` by image name; match on the
command line and kill only your own PIDs.
