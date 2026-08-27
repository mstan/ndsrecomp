# Per-instruction machinery ledger (beads-0vo / beads-yjp.42)

Every experiment aimed at the **per-instruction helper cluster** — the work
the runtime does at each retired guest instruction, on both the recompiled
and the interpreted path — is recorded here, retained or rejected, in the
entry format of `docs/host_optimization_strategy.md` §Ledger. That document
remains the contract (selector rule, 5% retention gate, G1/G2/G3); this one
is the running record for this workstream so a rejection is as findable as a
win.

Read `docs/interp_perf_baseline.md` first for the attribution that motivates
the whole line of work: on two titles, ~80% of the *native* path is ABI
helpers and dispatch, not recompiled guest arithmetic.

---

## Entries

- 2026-08-26 **Phase 0 forced-interpreter rig + pinned baseline** (commit
  `5024b61`): RETAINED, measurement only. A forced-interpreter selector at
  the single dispatch chokepoint `lookup_static_cached`, reachable via
  `--force-tier3`, `NDS_FORCE_TIER3=1` and the debug-server `force_tier3`
  command, with `dispatch_stats.forced_tier3` + `forced_tier3_misses` so a
  harness can verify the tier rather than assume it. The forced miss is
  applied *after* the real lookup so the per-instruction poll cost stays in
  the measurement; BIOS banks stay native because Tier 3 cannot execute
  bytes without write provenance. **Measured, MPH adventure, 3 faithful + 3
  forced, all six runs passing both tier witnesses:** forced/faithful
  emulation time medians **1.66x / 1.70x / 1.67x** (settle/walk/steady),
  min-of-3 **1.73x / 1.69x / 1.71x**, 60 FPS held in every run. Attribution:
  the forced path is 20.3% decode, 19.6% Tier-1 takeover poll, 12.3%
  register publication, 11.6% fetch provenance, 8.2% coverage capture — only
  28.9% is the interpreter proper. The faithful path is ~80%
  per-instruction ABI helpers and dispatch (`runtime_code_cycles` 21.5%,
  `runtime_should_yield` 14.3%, `lookup_static_cached_impl` 13.3%). Full
  analysis and disproved theories: `docs/interp_perf_baseline.md`.

- 2026-08-26 **Deadline-bounded per-instruction machinery, native path**
  (commit `aa7b564`): **RETAINED**. `g_nds_fast_limit` publishes the
  earliest `g_runtime_cycles` at which the active CPU could need any
  service; below it `runtime_should_yield` and `runtime_tick` reduce to an
  inlined compare with no event derivation, and `runtime_unwinding` becomes
  a load over exported data. At or over the limit the original faithful
  bodies run unchanged (`*_slow`). The limit is the scheduler's live
  `g_cycle_cap` and nothing else, which is the entire cross-CPU safety
  argument: the bound is exactly the bound `runtime_should_yield` already
  enforced. Selector `NDS_CYCLE_FAST_LIMIT=0` runs the faithful path in the
  same binary; `dispatch_stats.cycle_fast_limit` +
  `fast_limit_publishes` are the witnesses.

  **Measured** — interleaved A1 B1 A2 B2 A3 B3, same binary, quiet host,
  MPH adventure, emulation ms/frame:

  | Phase | A (faithful) | B (deadline) | median | min-of-3 |
  |---|---|---|---|---|
  | settle | 5.55 / 5.65 / 5.52 | 4.75 / 4.42 / 4.54 | **-18.3%** | -20.0% |
  | walk | 6.14 / 6.27 / 6.15 | 4.92 / 4.81 / 5.04 | **-20.0%** | -21.6% |
  | steady | 7.47 / 7.53 / 7.59 | 6.10 / 6.03 / 6.17 | **-19.0%** | -19.2% |

  All nine pairwise comparisons favour B (+14.5% to +23.3%); the legs do not
  overlap at any phase. The 2,400-frame soak agrees independently
  (13.983 s -> 11.716 s emulation, -16.2%). The attract route agrees in
  direction but drifted badly in *both* legs late in the block and is not
  quoted. Well clear of the 5% retention gate. Cost: the runner grows
  224,395,976 -> 250,814,426 bytes (**+11.8%**), ~13 bytes at each of ~2M
  inlined sites. SIZE UP / SPEED UP.

  **Gates:** instruction-anchored byte-lock 100M..700M, both selector
  states — both framebuffers, both CPUs' full register file + CPSR/SPSR/
  mode, and the entire `event_counts` block identical at all seven stops
  (0 deadlines published off, 15,694,588 on). 2,400-frame soak both states:
  underruns 0, queue errors 0, exact pinned FNV pair
  `834260ebdafba128,b54ecba81220bc28`. ctest 18/18.

  **Two things that were nearly missed, recorded so they are not repeated:**
  * With plain `static inline`, GCC -O3 declined to inline the fast paths
    into the huge generated bank bodies and emitted a *local out-of-line
    copy per TU* — `nm` showed `t runtime_tick` calling `U runtime_tick_slow`
    across 2,186 call sites in one MPH bank. The per-instruction call the
    deadline exists to remove was still there. `always_inline` is required,
    and the guard test now pins it. This is the most likely explanation for
    why the 2026-07-31 A1 header-inline experiment scored only 1.3–3.4%.
  * `publish_fast_limit` originally required `g_yield_poll_hint` to be
    clear. On the bank path that check was dead (the hint is provably zero
    at both call sites), but Tier 3 does not maintain that flag at all, so
    it silently pinned the deadline at zero for every fully interpreted
    stretch. Caught by the publish counter in forced-tier3 mode, not by any
    correctness gate.

  **The specified determinism gate was replaced, for cause.** "Identical
  framebuffer SHA pair from `measure_mph_scenario.py --route adventure`"
  does not discriminate on this host: that route is navigated at real time
  through the live frontend and is not reproducible run to run. Four
  control runs — two with the deadline on, two with it forced off —
  produced three different total `insn9` counts and two different
  top-screen digests, with the *same* digest appearing in both legs. The
  nondeterminism is present with the deadline forced off, where the runtime
  is the unmodified faithful path, so it belongs to the harness.
  `tools/probe_machinery_bytelock.py` replaces the wall-clock anchor with a
  guest anchor (`run_to_event insn9`) and is the gate reported above.
  Evidence: `perf-results/det-{A-faithful,A-faithful-2,B-normal,B-normal-2}`.

- 2026-08-26 **Deadline-bounded Tier-3 exit polling + the publish bug it
  exposed** (commit `5195a21`): **RETAINED, marginally.** `tier3_run()`'s
  six cross-TU exit predicates per interpreted instruction are bounded by
  the same `g_nds_fast_limit`, and the loop republishes it through
  `runtime_publish_fast_limit()` after its own all-clear scan. No new state.

  **The bug this exposed, recorded because no correctness gate could have
  caught it:** `publish_fast_limit` required `g_yield_poll_hint` to be
  clear. On the bank path that check was dead (the hint is provably zero at
  both call sites), but Tier 3 does not maintain that flag, so it pinned the
  deadline at zero for every fully interpreted stretch. Measured in
  forced-tier3 mode over 100M ARM9 instructions: 1,198,214 publishes (all
  from the still-native BIOS banks) instead of 5,726,635. Found by reading
  the publish counter — which exists exactly because a selector flag that
  converts nothing proves nothing. The predicate now checks directly the
  four conditions the hint stood for (`g_nds_insn_stop`,
  `nds_event_break_hit`, `nds_cpu_halted`, `nds_dma_cpu_stalled`) and the
  guard test asserts the hint is *not* consulted.

  **Measured** — forced-tier3 rig (every non-BIOS instruction interpreted on
  both CPUs), interleaved A1 B1 A2 B2 A3 B3, all six runs passing both tier
  witnesses (ARM9 tier3 share 0.994–1.003), emulation ms/frame:

  | Phase | A (faithful) | B (deadline) | median | min-of-3 | pairwise median |
  |---|---|---|---|---|---|
  | settle | 9.09 / 8.86 / 9.27 | 9.01 / 8.61 / 8.49 | +5.4% | +4.2% | +2.9% |
  | walk | 10.23 / 10.08 / 10.43 | 9.62 / 9.86 / 9.60 | **+6.0%** | +4.7% | +6.0% |
  | steady | 12.36 / 11.88 / 12.17 | 11.51 / 11.35 / 11.72 | **+5.4%** | +4.4% | +4.4% |

  All nine pairwise comparisons favour B (+1.0% to +8.4%), so the direction
  is unambiguous, but the magnitude **straddles the 5% gate**: it passes on
  medians for the two sustained phases and fails on min-of-N (4.4–4.7%).
  Retained on three stated grounds rather than on a comfortable number:
  it adds no state and ~10 lines (the ABI/complexity side of the gate is
  empty); it carries the publish fix, which the native half needs to reach
  its 19% in interpreted stretches at all; and its value is the fallback
  floor, not this route — MPH adventure runs 0.2% interpreted, and the
  titles this helps are the ones without bank coverage.

  **Gates:** byte-lock 100M..700M native path and 20M..100M with
  `--force-tier3`, both selector states, identical at every stop (the forced
  run also independently confirms the interpreted and recompiled paths
  produce the same framebuffer at 100M); 2,400-frame soak both states,
  underruns 0, exact pinned FNV pair; ctest 18/18.

  **C0 is NOT in this commit, deliberately.** Stopping the per-instruction
  `nds_has_bank` poll is the largest single item on the interpreted path
  (measured 19.6%), but the formulation in `interp_perf_baseline.md` — poll
  only on control transfer, epoch change and page crossing — silently
  *delays Tier-1 takeover* for straight-line execution that runs into a bank
  entry mid-page. That is semantically harmless (Tier 3 executes the same
  guest bytes under the same cycle model) but it surfaces Tier-3 activity,
  which the strategy contract's clause 3 treats as a regression in itself. A
  safe formulation exists and is the recommended design: a per-CPU "no
  dispatch rows in this 4 KiB page" filter keyed on `dispatch_epoch`, which
  returns the *exact same answer* and would short-circuit almost every poll,
  because runtime-copied code has no static bank by definition. It touches
  dispatch registration and deserves its own design pass.

### Prior art carried forward from `host_optimization_strategy.md`

Recorded here because this workstream's result only makes sense against it:

- 2026-07-31 **A1/A2 header-inline tick/yield/unwind fast paths**:
  **REJECTED and reverted.** Same cluster, same idea, measured on SM64DS:
  weighted emulation 15.119 -> 14.923 ms/frame (**1.30%**), 2.07% in the
  separate-window acceptance mode, +1,629,616 bytes. Missed the 5% gate.
  The 2026-08-26 entry above is the same target scoring 19%, and the two
  most likely reasons are both recorded there: A1 inlined *calls* without
  the deadline that removes the *work*, and plain `static inline` does not
  actually inline into these bank bodies at all.
- 2026-08-01 **inlined dispatch-cache hit path**: REJECTED, 2.03%.
- 2026-08-01 **A3 combined prologue (archived)**: 4.4–4.6%, removed at the
  5% gate; superseded by the header inlining if that lands.


---

## Queued, not yet attempted

From `docs/interp_perf_baseline.md` §6, the Tier-3-only candidates. These
were ranked from a measured profile of the forced-interpreter leg and are
deliberately **not** carried into this pass:

- **C1** publish only `{R15, R14, cpsr}` on the Tier-3 hot path (measured
  12.3% of interpreted cost; `sync_out` writes 45 words per instruction and
  only three are read mid-instruction).
- **C2** memoize `bus_range_has_write_provenance` per (cpu, page, write
  epoch) (11.6%); the in-tree precedent is `coverage_note_exec`.
- **C3** direct-mapped recent-key filter in front of `coverage_note` (8.2%).
- **C4** decoded-instruction cache keyed by (page, offset, generation)
  (20.3% as a class; the largest and the largest change).
- **C5** skip the call-return-stack scan on a plain `B` (1.4%; fold into
  whichever change touches that line).
- **C6** devirtualize the bus for the runner's concrete `RtBus` (3.9%;
  crosses into `external/arm-recomp-core`).
- **C7** collapse the six per-instruction cross-TU polls into one armed word
  (4.8%; the candidate most able to perturb guest-visible timing).

The standing caveat from the Phase 0 analysis applies to all of them: on a
route that runs 0.2% interpreted, ~52% of interpreted cost is ~0.1% of frame
time. They should be gated on measuring a genuinely interpreter-heavy route
(MKDS local multiplayer is the known case) rather than done because they are
well-evidenced.

- 2026-08-27 **MPH superblock coalescing (exp 1)**: RETAINED as flat-cost
  simplification. --coalesce-fallthroughs was never passed to MPH bank
  generation; wired for the ARM9/ARM7 main closures + both ARM7 alias banks
  with preceding-dispatch lists mirroring the runner's per-CPU sorted-glob
  registration order. Census: 17,056 + 2,409 + 261 + 13 merged edges.
  Interleaved 3+3 A/B (adventure + attract) timing-flat (every delta inside
  one side's own spread); ARM9 literal_fallthrough fell monotonically in all
  10 phases (-0.24..-6.45%); ARM7 unchanged (registration-order blocked).
  Binary 250,815,513 -> 212,182,455 (-15.4%). Correctness: cross-binary
  byte-lock identical at 100M..700M; soak FNV pair exact; 81-bank inventory.
- 2026-08-27 **ARM7 WRAM-alias registration unlock (exp 2)**: RETAINED,
  owner decision (candidate adds the registration-order mechanism; on the
  dev i9 the timing win is 2.4% adventure / 1.8% attract, below the strict
  5% complexity gate, but the mechanism is decisive and the field population
  is dispatch-bound at ~2.5x the dev per-cycle cost). registration-order.txt
  in the title bank dir lets a whole-module bank register ahead of
  page-generation banks; MPH names mph_arm7_wram_alias, unlocking its
  coalescing 261 -> 3,198 edges. Measured: ARM7 dispatch_total -25.9%
  gameplay / -59.1% boot, literal_fallthrough -33.5% / -86.7% (700M window:
  -44.4% / -61.6%); ARM9 counters bit-identical; adventure emu -2.4%
  (3/3 pairwise), attract -1.8%, soak emu -3.7%; one FMV window
  (attract_2400_3000) +1.2%, consistent with the extra failed validation
  while a materialized generation is live. Correctness gates identical to
  exp 1, all green.
