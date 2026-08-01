# Host optimization strategy — static-path overhead (ISSUE-2)

Status doc for the "locked 60 FPS with headroom" workstream. Written
2026-07-31 against framework `728d12b` + the forward-goto emission
generalization; update it as knobs land. Companion evidence:
`supermario64dsrecomp/docs/performance_baseline.md` (2026-07-31
re-baseline + profile) and the experiment ledger in `ISSUES.md` (P-2..P-5).

## The contract (do not violate)

1. **Semantics are bit-exact.** Every knob here removes HOST bookkeeping
   cost only. Guest-visible state (registers, memory, cycle counts, IRQ
   delivery points, framebuffers, audio ordinals) must be byte-identical
   before and after. G3 byte-lock (100M..700M, both framebuffers), G1
   (8 firmware scenarios, tier3=0), and G2 (2,400-frame soak,
   underruns=0, FNV pair) gate every change.
2. **The LLE floor stays linked and forceable.** Fast paths are layered:
   the out-of-line reference implementation remains in the binary and
   selectable (B3 precedent: `NDS_DEEP_TRACE=0` proof mode; fast-poll:
   `NDS_CPU_FAST_POLL=0`). An inline fast path must be provably the SAME
   COMPUTATION, not a model of it.
3. **Tier-3 stays an emergency fallback.** Coverage is closed on the
   SM64DS path (zero interpreted instructions in gameplay/menu profile
   windows). Any change that surfaces tier-3 activity is a regression.
4. **Binary size is an accepted cost.** User decision 2026-07-31: a
   large exe is an acceptable trade for speed (the runner is already
   ~300 MB; +tens of MB for inlining/alignment/PGO is fine). i-cache
   pressure is still real — measure, don't assume; hot loops touch a
   small working set of the giant .text.
5. **Measurement discipline.** Quiet host only (zero compiler processes;
   discard contaminated runs), interleaved A/B, min-of-N + medians.
   Quick iteration signal: worst-phase (adventure_to_file_select) and
   yoshi_settled windows via `tools/measure_sm64ds_scenario.py` /
   `tools/profile_sm64ds_worst_phase.py --phase <label>`. Retention gate
   for added complexity/ABI surface: >=5% quiet interleaved win. Pure
   simplifications and neutral generalizations may be retained at flat
   cost with recorded numbers.

## Evidence (2026-07-31, quiet host, headed)

Main-thread RIP profile, consistent across menu/attract/gameplay:

| Class | Share | What it is |
|---|---:|---|
| Dispatch machinery | 29.7–31.7% | `runtime_dispatch`, `lookup_static_cached` (up to 16.6% alone), `bus_exec_page_generation` |
| Per-insn ABI helpers | 14.8–19.3% | `runtime_should_yield`, `runtime_code_cycles`, `runtime_tick`, `runtime_mem_cycles`, `runtime_unwinding` |
| GPU2D scanlines | 5.6–9.8% | `render_engine_line`, text/OBJ decode |
| Everything else | rest | guest arithmetic (generated tail), bus, timers, SPU; 3D bridge ≤1.3 ms/f (worker thread renders in parallel) |

Per guest instruction the generated code makes 4 out-of-line calls
(5 with a memory access); each opaque call also forces GCC to spill and
reload cached guest state. Every BL to a non-same-shard target, every
cross-unit branch, every generated body fallthrough, and every unmatched
return funnels through `runtime_dispatch` (hash probe +
content-generation validation done TWICE per hit + indirect call). ARM9
banks are all `--validate-live-bytes`, so no direct C-to-C calls exist
anywhere on ARM9 (confirmed: zero in all sampled shards).

## Knob inventory (all semantics-identical)

### A. Per-instruction ABI cluster (~15–19% + spill tail)

- **A1. Header-inline helper fast paths** (`runtime_should_yield`,
  `runtime_tick`, `runtime_unwinding`; later `runtime_mem_cycles` /
  `runtime_code_cycles` ARM9 fast classes). Move the exact fast-path
  computation into `recompiler/armv4t/runtime_arm.h` as static-inline
  over exported globals; slow tails (`runtime_irq`, full yield scan,
  region misses) stay out-of-line. No emission change — banks recompile
  against the header. SIZE UP / SPEED UP. This is the B3 move; B3 bought
  ~14% on the game path.
  Secondary effect: GCC keeps guest registers live across instruction
  boundaries and coalesces the per-insn `g_cpu.R[15]` double-store when
  provably unobserved between boundaries.
- **A2. Branch-hint annotations in emission**: `__builtin_expect(...,0)`
  on the per-insn `runtime_should_yield()`/`runtime_unwinding()` returns
  and the `g_insn_hook_armed` branch. Pure code-layout win in huge
  functions. Tiny, do with A1.
- **A3. Combined prologue (archived)**: one fused call replacing the
  yield/count/fetch boundary. Measured 4.4–4.6% in 07-18 (removed at the
  5% gate; design preserved on branch `archive/combined-prologue-stash`).
  Superseded by A1 if A1 lands (inlining subsumes call fusion); revisit
  only if A1 disappoints.

### B. Dispatch/transfer cluster (~30%)

- **B0. Composition counters — COMPLETE.** Cheap always-on counters splitting
  dispatch volume by cause: BL-call dispatch, cross-unit direct branch,
  generated body fallthrough, return CRS-hit vs CRS-miss, computed
  (BX/LDR-pc/LDM-pc), IRQ/SWI vectoring, slice-yield exits, and cache
  hit/absent/slow-lookup. Queryable via debug server; printed by
  `nds_profile_report`. The first residual-only draft was rejected in
  review because it mislabeled dynamic PC writes, fallthrough, and
  exceptions as literal branches; source-tagged emission replaced it.
- **B1. Guard-snapshot dedup — COMPLETE**: `StaticGuardScope::call`
  copies the page/generation snapshot that `cached_lookup_live` just
  validated instead of recomputing it through `arm_static_guard`. The
  reference builder remains as the safe fallback for any future cache slot
  without a complete snapshot. Runtime-only; no generated-bank change.
- **B2. Validated direct calls for BL**: B0 measured only
  ~1.6K–3.1K literal BL dispatches/frame on ARM9, so this is no longer
  projected as the dominant dispatch win. A safe implementation must
  preserve ordered overlapping-bank selection and install the same nested
  static guard as `runtime_dispatch`; a same-bank byte/generation check is
  insufficient. Keep plain `B` out of this design: it is a tail transfer,
  and `callee(); return` can grow the host stack around cross-function
  cycles and skip dispatch preemption. Reconsider after B1/A1 and the
  fallthrough work are measured.
- **B3. Per-callsite monomorphic inline cache** for computed transfers
  (BX reg / LDR pc / LDM pc): a static per-site slot {target, fn,
  generation snapshot}; hit = compare + call, miss = dispatch + refill.
  SIZE UP (+~24 B/site + check code) / SPEED UP. Needs B0 to confirm
  computed transfers matter.
- **B4. Larger RAM-bank split units**: RAM banks are generated with
  `--max-function-bytes 512`; every unit-crossing branch dispatches.
  Any unit ≤4 KiB still spans ≤2 4-KiB pages (the guard limit), so the
  cap can rise ~8x, converting cross-unit dispatches into intra-unit
  gotos and removing artificial body fallthroughs (the forward-goto
  generalization already handles both directions). B0 makes this more
  interesting for fallthrough reduction than for literal branches;
  first split the dynamic counts by bank and re-check capture-tool
  assumptions.
- **B5. Dispatch entry for with_exchange**: `runtime_dispatch_with_exchange`
  fires two gated trace-event calls per invocation (its own + the
  delegated one); fold when tracing is off. Micro; bundle with B1.

### C. Compiler-level (zero source change)

- **C1. Bank optimization level**: confirm what `ndsrecomp_limit_generated_compiles`
  sets for bank TUs in the sm64-native runner build and evaluate -O3 /
  `-falign-loops` for bank TUs. SIZE UP / SPEED UP, free semantics.
- **C2. PGO** (`-fprofile-generate` → play the scenario → `-fprofile-use`):
  the strongest whole-binary layout knob; build-time cost is high and
  profile freshness must be maintained. Worth one measured experiment
  after A1/B1 land.
- **C3. `-mtune=native`** for local builds (keep generic for release).
- (Rejected precedent: whole-runner GCC LTO produced a 663 MB archive
  and no linked binary — do not retry blindly; scoped LTO of the
  runtime core TUs is plausible but unproven.)

### D. Structural (bigger projects, later)

- **D1. Superblock emission**: extend straight-line emission across
  fall-through chains / hot traces within one validated region to
  amortize entry/exit overhead further. B0 promoted this: generated body
  fallthrough accounts for ~79–87% of ARM9 dispatch entries in the
  measured phases. Any implementation must avoid unbounded host recursion,
  retain slice preemption, and preserve ordered-bank validation/guarding.
- **D2. GPU2D per-scanline cost** (5.6–9.8%) and ARM7 share (~26% of
  scheduler samples): separate workstreams once the two machinery
  clusters are paid down.
- **D3. Renderer seam** (tier 2, user-authorized 2026-07-31): melonDS
  ComputeRenderer promotion (parity characterization + uncontaminated
  A/B pending; stash `compute readback overlap exact candidate` exists),
  SDL3/GL/Vulkan output path when frames become GPU-composited,
  sized for adaptive 21:9 widescreen (~1.75x pixels).

## Ledger of results in this workstream

- 2026-07-31 **forward-goto generalization** (emitter): intra-function
  forward branches now take the same local `goto` as backward ones
  (was dispatch). G3 byte-locked. Interleaved 3-pair A/B: FLAT
  (-2.1%..+1.9% per phase, overall 56.86→57.20 FPS ≈ noise). Static
  conversion was only ~90 sites/shard (ROM banks) and 0 (RAM banks —
  finder splits at branch targets). Retained as a neutral uniformity
  fix; NOT a perf win. Lesson: measure dispatch composition (B0) before
  the next dispatch swing.
- 2026-07-31 **B0 source-aware dispatch composition**: the first draft's
  residual attribution was rejected in adversarial review. The landed
  counters tag literal B, literal BL, generated fallthrough, exception,
  resume, exchange, slice-yield, CRS, and cache outcomes at their sources.
  One quiet headed full-path run (`20260731-dispatch-composition-v3`) found
  ARM9 generated fallthrough at **32.8K–115.7K/frame**, literal B at
  **3.2K–14.6K/frame**, and literal BL at **1.6K–3.1K/frame**. Thus
  fallthrough is ~79–87% of ARM9 dispatch entries and the earlier
  “38K–125K literal B/BL” conclusion is retracted. ARM7 fallthrough was
  15.8K–30.2K/frame. G1 8/8, G2 2,400 frames with zero underruns and the
  pinned FNV pair, and G3 100M..700M with exact GX state and both screens
  pass. Instrumentation-cost A/B remains pending: one quiet control run
  completed, but three matching B attempts were discarded when unrelated
  compiler processes appeared. Do not use the instrumented binary for a
  headline FPS claim until that A/B closes. Re-ranked next work:
  **B1 guard-snapshot dedup → A1/A2 helper inlining → validated
  nonrecursive fallthrough/body-coalescing design**, then re-profile.
- 2026-07-31 **SM64DS adaptive sky polygon widening**: REJECTED and
  reverted. Treating opaque polygon ID 2 as one screen-filling backdrop,
  widening its geometry, and scaling texture S by the same 448/256 ratio
  did not remove either the black voids or visible sky stretching in the
  castle courtyard. The sky model is not a single safely scalable surface;
  revisit only with model/draw-aware capture or a title-side skybox patch.
  This result is visual-correctness evidence, not a performance result.
- 2026-07-31 **B1 guard-snapshot dedup**: four quiet interleaved full-path
  pairs, with the last pair fully warmed and launch order counterbalanced,
  produced weighted emulation-time wins of **10.16%, 23.78%, 1.57%, and
  4.80%**. The four-run medians were 16.694→15.478 ms/frame (**7.29%**);
  fastest-vs-fastest was 15.563→14.816 (**4.80%**). Worst-phase median was
  21.889→20.786 (**5.04%**); Yoshi median was 14.766→14.383 (**2.59%**),
  with fastest Yoshi 14.111→13.361 (**5.32%**). Retained conservatively as
  an ABI-neutral simplification that removes a provably duplicated snapshot,
  not as a double-digit headline win. Gates: G1 8/8 exact with Tier-3 zero;
  G2 2,400 headed frames at 57.863 FPS, zero underruns/queue errors/input,
  exact FNV pair `e333837761ca0d1c,d61d2eb50e96b61d`; G3 byte-lock exact at
  every 100M..700M stop on both screens.
- 2026-07-31 **A1/A2 header-inline tick/yield/unwind fast paths**:
  REJECTED and reverted. The candidate exported the exact fast-poll state,
  kept `NDS_CPU_FAST_POLL=0` on the original out-of-line path, and added
  unlikely hints to the emitted unwind/hook branches. It increased the
  runner by 1,629,616 bytes (0.54%). A clean stacked/native pair reduced
  weighted emulation time only 15.119→14.923 ms/frame (**1.30%**, +0.196 ms
  headroom). In the new separate-window/adaptive-top acceptance mode it
  reduced emulation time 17.231→16.874 ms (**2.07%**) and end-to-end time
  22.964→22.192 ms (**3.36%**). Both default and forced-reference G3 passed
  exactly through 700M, but the speedup missed the 5% ABI/complexity gate.
  The acceptance-mode run also exposed a larger target: presentation
  averaged 4.807 ms/frame and reached 9.25 ms in settled gameplay.
- 2026-08-01 **adaptive sky-repair disable diagnostic**: REJECTED as a
  performance change. In the separate-window/adaptive-top target mode, the
  same-binary repair-on/off pair reduced weighted presentation only
  4.691->4.478 ms/frame and adaptive composition 3.834->3.689 ms/frame.
  Settled Yoshi improved by about 0.49 ms of adaptive time, but the run had
  broad emulation variance and the repair toggle remained below the 5%
  retention gate. Keep the repair enabled; its remaining visual defects are
  a correctness workstream rather than the dominant presentation cost.
- 2026-08-01 **frame-coherent adaptive 3D snapshot**: RETAINED. Attribution
  split weighted presentation into adaptive composition (3.83 ms), texture
  upload (0.07 ms), draw (0.004 ms), and two-window swap (0.79 ms). Temporary
  inner timers then showed HUD/OBJ rasterization, sky repair, and final blend
  accounted for less than 0.8 ms combined; the missing time was rereading the
  threaded renderer. The lifecycle was also frame-incoherent: native 2D had
  consumed 3D frame N, while VCount 215 had already started rendering N+1
  before host presentation reread the shared wide buffer. The fix snapshots
  each wide color/attribute scanline into the matching 2D back-buffer slot
  when native 2D consumes it, then adaptive presentation reads that completed
  slot without waiting on or sampling N+1.

  Quiet headed B->A->B in the acceptance mode
  (`separate` + adaptive top, sky repair on) measured weighted presentation
  at **1.449 / 5.144 / 1.642 ms/frame** and emulation+presentation at
  **19.138 / 22.978 / 20.404 ms/frame**. The candidate median was 19.771 ms,
  a **13.96% total frame-time reduction**; presentation fell **69.96%**.
  Settled Yoshi presentation fell 7.973->1.710/1.789 ms and FPS rose
  42.79->51.37/49.14. A separate post-gate adaptive-surface capture through
  the castle confirmed coherent widened world output without new corruption;
  the already-known sky seam/stretch remains. Gates: G1 8/8 exact with
  Tier-3 zero; G2 2,400 frames, zero underruns/queue errors/input, exact FNV
  pair `e333837761ca0d1c,d61d2eb50e96b61d`; G3 exact at every 100M..700M stop
  on both screens; decode/cycle/manifest tests pass.
- 2026-08-01 **B4 larger RAM split units**: REJECTED before build. The
  512-byte cap adds only 178 bodies to the ARM9 runtime bank
  (19,082 finder functions -> 19,260 emitted) and 147 to the gameplay bank
  (40,167 -> 40,314). The remaining boundaries come from finder/entry-point
  structure, so raising the cap cannot materially reduce the measured
  fallthrough volume. Do not pay a full regeneration/gate cycle until
  dispatch attribution is split by selected bank.
- 2026-08-01 **C1 generated game banks at `-O3`**: REJECTED as non-viable in
  the current shard layout. A clean build completed only 53/76 objects after
  ten minutes, with three gameplay-shard compiler processes still active; no
  candidate binary linked, so no performance claim is made. The one-line
  option change was reverted and the cached `-O2` runner restored in 16
  seconds at the prior 300,213,538-byte size. Revisit only with smaller
  compilation units or scoped hot-bank evidence.
- 2026-08-01 **fallthrough selected-bank attribution**: one diagnostic-only
  headed scenario tagged the bank selected after live-byte validation; its
  wall timings were discarded because unrelated builds were active. ARM9
  title/attract fallthroughs resolved **100% to the runtime-RAM bank**.
  Settled Yoshi split **50.25% runtime RAM / 49.75% gameplay RAM**, with
  title ROM and system banks both zero. ARM7 resolved **100% to its
  runtime-RAM bank** in every phase. About 1.5% ARM9 / 3.1% ARM7 remained
  untagged because slice-yield exits occur before bank selection. The
  diagnostic counter/API changes were reverted after capture. Conclusion:
  D1 must coalesce RAM finder/entry-point bodies or provide a nonrecursive
  RAM-bank trampoline; ROM tuning and a larger max-size cap cannot address
  the dominant fallthrough class.
- 2026-08-01 **ARM9 fallthrough tail-dispatch trampoline**: RETAINED. A
  generated literal fallthrough now returns its raw target to the active
  `runtime_dispatch` invocation, which iterates instead of recursively
  creating another host dispatch frame. Calls, exchanges, dynamic branches,
  ARM7, miss/Tier-3 paths, per-target yielding/counters/tracing, and ordered
  live-byte validation are unchanged. Each iteration destroys its
  `StaticGuardScope` before consuming the queued target, so nested dispatch
  and guard invalidation retain their prior lifetime semantics.

  Quiet-host headed B/A/B used the shipping target mode (detached screens +
  adaptive top). Whole-route FPS was candidate **52.685 / 53.415** versus
  baseline **50.882** (candidate mean **+4.26%**). The sustained gameplay
  target clears the retention threshold: settled-Yoshi emulation time was
  **17.168 / 16.698 ms/frame** versus **18.170** (**6.81% mean reduction**)
  and FPS was **51.942 / 53.233** versus **49.420** (**+6.41% mean**).
  Castle/water, first-character arrival, and everyone-present emulation time
  improved **8.26% / 8.85% / 7.67%** by the same two-leg mean; boot/menu was
  flat. This is meaningful headroom but not ISSUE-2 acceptance: the automated
  target-mode route still falls below 60 FPS and reports audio underruns.

  Correctness is green: G3 byte-locks both screens at every 100M..700M stop;
  G1 passes all eight fresh-pair firmware scenarios with exact screens/audio
  and zero Tier-3; G2 completes 2,400 interactive frames with zero
  underruns/errors/input and exact FNV pair
  `e333837761ca0d1c,d61d2eb50e96b61d`. Decode, interpreter-cycle,
  HLE-manifest, frontend-config, and battery-save tests pass.

## Reproduction crib

```powershell
# quick phase A/B (headed, quiet host, alternate exes)
py -3 tools/measure_sm64ds_scenario.py --repetitions 1 --exe <exe> --output perf-results/<tag>
# targeted RIP sample of any phase
py -3 tools/profile_sm64ds_worst_phase.py --phase yoshi_settled --output perf-results/<tag>
# gates
py -3 oracle\probe_gx_state.py --nav sm64ds-title --start 100000000 --step 100000000 --count 7   # G3
..\run_firmware_scenario.ps1 -Scenario <name>                                                    # G1 x8
$env:NDS_FRONTEND_STATS='1'; $env:NDS_FRONTEND_MAX_FRAMES='2400'; $env:NDS_FRONTEND_REQUIRE_AUDIO='1'; nds_runner --interactive  # G2
```

Headroom metric: `phase_ms_per_frame.emu` from the scenario harness for core
emulation, plus `phase_ms_per_frame.present` when evaluating enhanced display
modes.
16.7 ms = break-even, **12.8 ms = 1.3x (goal), 8.3 ms = 2x (aspiration)**.
2026-07-31 state: Yoshi 59.8 FPS at ~14.6 ms (locked but thin); worst
menu phase 43.5 FPS at 22.4 ms.
2026-08-01 separate-window/adaptive-top state after the coherent snapshot:
weighted emulation+presentation 19.14/20.40 ms across two candidate runs;
settled Yoshi 19.11/20.22 ms (49.1-51.4 FPS). Presentation is no longer the
dominant gap; CPU emulation is again the primary burndown target.
