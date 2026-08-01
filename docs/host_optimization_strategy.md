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

- **D1. Superblock emission — COMPLETE for validated ARM9 RAM banks**:
  adjacent, same-mode function bodies are coalesced only within one 4 KiB
  validation region and only when no earlier registered bank can own the
  next entry. Every dispatch row still enters through ordered live-byte
  validation, but all member rows select the shared leader and union
  descriptor; internal fall-throughs become local gotos. Every instruction
  retains its normal PC update/yield poll and every interior PC remains a
  resume-switch target. This removes the dominant artificial boundary
  without host recursion or a per-boundary cache.
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
- 2026-08-01 **validated ARM7 fallthrough trampoline extension**: REJECTED
  and reverted. The extension enabled the same loop only for the exact live
  `CachedStaticLookup` hit carrying validation metadata; that metadata is
  currently coupled to `--validate-live-bytes` and therefore to generated
  direct calls being disabled. Unvalidated ARM7 ROM/BIOS bodies retained the
  recursive reference path. G3 remained exact through 700M, but quiet target
  B/A/B whole-route FPS was **54.089 / 54.133 / 54.135**. Settled-Yoshi
  emulation time was **16.665 / 16.530 / 16.439 ms/frame**: the candidate mean
  was **0.13% slower** than baseline, with other gameplay phases also flat.
  Large dynamic call count did not translate into removable frame time; do
  not broaden the trampoline to ARM7 without new profile evidence.
- 2026-08-01 **post-trampoline target-mode RIP profile**: detached screens +
  adaptive top, settled Yoshi, 1 ms sampling. The main thread contributed
  18,601 runner samples. `lookup_static_cached` remains first at
  **2,805 (15.08%)**, followed by `runtime_dispatch` **1,236 (6.64%)**,
  adaptive framebuffer composition **975 (5.24%)**, `runtime_should_yield`
  **713 (3.83%)**, executable-page generation lookup **660 (3.55%)**,
  `runtime_code_cycles` **642 (3.45%)**, `runtime_tick` **615 (3.31%)**,
  and `runtime_mem_cycles` **535 (2.88%)**. The separate soft-render worker
  contributed 8,161 samples, dominated by polygon scanlines (4,771).
  Instrumented phase timing was 17.107 ms emulation + 1.631 ms presentation;
  use the RIP shares for ranking, not that instrumented absolute.
- 2026-08-01 **inlined dispatch-cache hit path**: REJECTED and reverted.
  The common key/generation check was forced inline into the dispatch loop
  while ordered search/refill stayed out-of-line and unchanged. G3 remained
  exact. Quiet target B/A/B whole-route FPS was
  **54.462 / 54.048 / 54.840** (candidate mean **+1.12%**); settled-Yoshi
  emulation was **16.148 / 16.506 / 16.193 ms/frame** (candidate mean
  **2.03% lower**). Only the transient new-game load crossed 5%. The sampled
  lookup symbol is mostly necessary live-generation validation, not removable
  call overhead; the result fails the retention gate.
- 2026-08-01 **File A settled-gameplay coverage closure**: RETAINED for
  correctness, performance-neutral. The new profile exposed 69,323 ARM9
  Tier-3 instructions / 8,968 entries in settled Yoshi. Discovery found 424
  unique PCs; 406 were in the gameplay generation, 124 were new configured
  boundaries, and the 32 hottest sites matched the existing captured image
  byte-for-byte over their sampled 64-byte windows. Regenerating the same
  content-validated bank reduced the identical phase to
  **Tier-3 `(0,0)` instructions and entries** without relaxing validation.
  Target B/A/B whole-route FPS was **54.151 / 54.096 / 54.104**, and
  settled-Yoshi emulation was **16.470 / 16.505 / 16.413 ms/frame**: flat.
  G1 passes 8/8 exact with firmware Tier-3 zero; G2 passes 2,400 frames with
  zero underruns/errors/input and exact FNV pair
  `e333837761ca0d1c,d61d2eb50e96b61d`; G3 is exact through 700M on both
  screens.
- 2026-08-01 **fallthrough guard-to-cache handoff**: REJECTED and reverted.
  The candidate reused a completed body's exact positive validation snapshot
  only when the queued fallthrough target's cache entry matched its page
  addresses, generation pointers, and generation values. G3 remained exact
  through 700M on both screens. In quiet detached-screen/adaptive-top B/A/B,
  whole-route FPS was **53.818 / 53.564 / 53.042** (candidate mean
  **0.25% lower**). Settled-Yoshi FPS was
  **53.176 / 54.147 / 52.684** (candidate mean **2.25% lower**) and emulation
  time was **16.763 / 16.346 / 16.912 ms/frame** (candidate mean
  **3.01% worse**). The comparisons and cache-entry copy cost more than the
  avoided generation reads; do not pursue snapshot handoff as another
  per-dispatch fast path.
- 2026-08-01 **fixed-target validated fallthrough links**: REJECTED and
  reverted. The candidate emitted 56,244 ARM9 RAM/gameplay link sites with a
  per-site exact-byte/generation cache and retained normal ordered dispatch
  whenever the linked bank's target validation was not live. G3 remained
  exact through 700M on both screens. The executable grew
  300,346,754 -> 312,843,850 bytes (**+12.50 MB / +4.16%**).

  Quiet detached-screen/adaptive-top B/A/B whole-route FPS was
  **54.367 / 54.244 / 53.893** (candidate mean **0.21% lower**).
  Settled-Yoshi emulation was **16.135 / 16.472 / 16.342 ms/frame**
  (candidate mean **1.42% lower**) and FPS was
  **54.763 / 53.958 / 53.969** (candidate mean **+0.76%**). This was not a
  coverage failure: the final candidate leg recorded **44,085,767 linked
  hits and 7 misses** in settled Yoshi. A fixed target removes hash/key/bank
  selection but still reads the executable-page generation at each boundary,
  so its result agrees with the prior 1-2% cache-hit-inline experiments.
  Further work must eliminate validation boundaries (selection-safe
  page/superblock coalescing), not add another per-boundary cache.
- 2026-08-01 **selection-safe 4 KiB page leases**: REJECTED and reverted.
  Static owner-order closure qualified 88.38% of runtime-bank and 53.31% of
  gameplay-bank fallthrough targets. The generated candidate used 409 shared
  exact-page descriptors across 56,244 sites, proved the ordinary ordered-bank
  winner once per site, and carried a page-generation snapshot through the
  nonrecursive tail loop. Same-page linked boundaries then performed no lookup
  and no generation read. At the 700M G3 endpoint the forced candidate recorded
  **264,350,851 lease reuses**, **40,386,777 establishments**, and
  **17,585,638 safe fallbacks**; both screens remained byte-exact at every
  100M..700M stop.

  The first timing candidate accidentally repeated a full 4 KiB
  provenance/byte comparison for every rejected boundary. It was negative and
  is retained only as implementation evidence: whole-route FPS was
  **56.112 / 56.195 / 55.551** in candidate/baseline/candidate order, while
  settled-Yoshi emulation was **15.657 / 14.906 / 15.079 ms/frame**.
  A corrected tri-state page cache memoized both success and rejection for the
  current generation and passed G3 again.

  The corrected quiet detached-screen/adaptive-top B/A/B still lost:
  whole-route FPS was **55.535 / 57.879 / 57.042** (candidate mean
  **2.75% lower**), and settled-Yoshi emulation was
  **13.979 / 13.213 / 15.036 ms/frame** (candidate mean **9.80% worse**).
  Castle/water emulation was **14.773 / 14.068 / 14.500 ms/frame** (candidate
  mean **4.04% worse**), and everyone-present was
  **17.713 / 16.699 / 17.268 ms/frame** (**4.74% worse**). The executable grew
  300,344,569 -> 316,419,486 bytes (**+16.07 MB / +5.35%**). Dynamic coverage
  was ample; the distributed call-site and code-footprint cost outweighed the
  eliminated lookup/generation work. Do not pursue generated per-site page
  linking again without a substantially smaller representation.
- 2026-08-01 **selection-safe same-page ARM9 RAM superblocks**: RETAINED.
  Before generation, the runtime-RAM bank had **15,719 / 17,988 (87.39%)**
  eligible adjacent edges and the gameplay-RAM bank had
  **19,621 / 38,256 (51.29%)**. The landed conservative pass merged 15,669
  and 19,615 edges respectively. It requires live-byte validation, rejects
  HLE banks, requires unique direct-mapped entries, never crosses a mode or
  4 KiB page boundary, and rejects a target present in any preceding dispatch
  table. All rows in a block still participate in ordinary ordered dispatch
  and share one exact union validation descriptor; only a validated block's
  internal artificial fall-throughs become local gotos. The executable
  shrank 300,344,569 -> 287,654,876 bytes
  (**-12,689,693 bytes / -4.22%**).
  Consequently, dispatch/trace counters no longer observe those removed
  boundaries. Union validation is also deliberately stricter than validating
  one member alone: modification of any member makes the whole block fall back
  through ordinary dispatch. The retained scenario remained Tier-3-free.

  At the 700M G3 endpoint, ARM9 dispatch entries fell from 400,476,423 in the
  prior page-lease instrumentation run to **78,634,647**, while generated
  fall-through entries fell from 324,420,758 to **2,578,982**. Both screens
  remained byte-exact at every 100M..700M stop.

  Quiet detached-two-window/adaptive-top B/A/B whole-route FPS was
  **59.476 / 57.930 / 59.458** (candidate mean **+2.65%**, substantially
  cap-limited). Emulation time improved in every phase by
  **10.92–16.22%**. Settled Yoshi was
  **11.442 / 12.993 / 11.377 ms/frame** (**12.19% more headroom**);
  castle/water was **12.462 / 14.042 / 12.555 ms/frame**
  (**10.92% more headroom**); and the worst file-select transition was
  **16.431 / 19.436 / 16.134 ms/frame** (**16.22% more headroom**).
  Scenario artifacts are
  `20260801-superblocks-{B1,A1,B2}`.

  G1 is 8/8. G2 completed 2,400 headed frames at 57.858 FPS with zero audio
  underruns/errors/input and exact FNV pair
  `e333837761ca0d1c,d61d2eb50e96b61d`. G3 is exact through 700M on both
  screens. Decode, interpreter-cycle, HLE-manifest, memory-timing,
  static-fetch, frontend-config, and battery-save tests pass. This is a
  material headroom win, but it is not ISSUE-2 acceptance yet: the worst
  transition remains about **16.28 ms emulation/frame**, above the 12.8 ms
  goal, and initial attract still presents at about 56 FPS.
- 2026-08-01 **post-superblock RIP re-profile**: the prior ranking is no
  longer current. In `adventure_to_file_select`, the main-thread leaders are
  `lookup_static_cached` **8.79%**, `runtime_code_cycles` **6.30%**,
  GPU2D `render_engine_line` **5.84%**, `runtime_should_yield` **5.42%**,
  `runtime_tick` **4.20%**, `runtime_dispatch` **4.19%**, and
  `runtime_mem_cycles` **4.14%**. In settled Yoshi, lookup is **11.90%**,
  dispatch **5.51%**, code cycles **4.14%**, yield **3.95%**, executable-page
  generation **3.81%**, tick **3.78%**, and memory cycles **3.63%**. Artifacts:
  `20260801-superblocks-reprofile-{file-select,yoshi}`. Dispatch is no longer
  the ~30% leading cluster; residual lookup/validation, per-instruction timing,
  GPU2D, and ARM7 now require separate evidence.
- 2026-08-01 **ARM7 runtime-RAM superblock extension**: REJECTED and
  reverted. The already validated ARM7 RAM capture offered only **258**
  eligible same-page edges among 7,247 emitted functions. G3 remained exact
  at every 100M..700M stop on both screens, but those edges were cold:
  file-select ARM7 fall-throughs changed only
  **28,427.1 -> 28,411.0/frame (0.06%)**, and settled Yoshi was statistically
  unchanged (**24,378.6 -> 24,380.2/frame**).

  One quiet detached-two-window/adaptive-top candidate/baseline pair was
  sufficient to reject the no-coverage extension. Whole-route FPS was
  **59.471 / 59.518**. File-select emulation was
  **16.452 / 16.349 ms/frame** (**0.63% worse**), settled Yoshi was
  **11.620 / 11.530 ms/frame** (**0.78% worse**), and the nine phase results
  ranged from 3.43% better to 1.28% worse without a coherent signal. The
  candidate was only 107,074 bytes smaller. Artifacts:
  `20260801-arm7-superblocks-{B1,A1}`. The large residual ARM7 fall-through
  count comes from nonadjacent finder/entry-point structure, not the contiguous
  edge class handled by D1; changing that structure needs a different design.
- 2026-08-01 **combined-prologue revisit after D1**: REJECTED as a build
  experiment; no new runtime claim. The archived exact seam was reimplemented
  against the superblock emitter with its same-binary forced-OFF legacy path:
  ARM9 ARM and ARM7 could fuse yield polling, retirement bookkeeping, and
  code-fetch timing, while ARM9 Thumb and special/NV paths stayed generic.
  Structural emission tests and the decoder test passed.

  The first runner build applied the experimental definition to every generated
  firmware/title bank and failed to link within ten minutes, with four O3
  firmware-shard compilers still active. A second version correctly restricted
  the definition to SM64DS sources, but the first attempt had invalidated the
  monolithic runner's firmware objects; rebuilding that set again failed to
  produce a binary within another ten-minute ceiling. Both builds were stopped
  by verified process tree, all source and generated-bank changes were reverted,
  and the exact retained D1 binary was restored. The prior 2026-07-18
  **4.4–4.6%** measurement remains the only performance evidence. Revisit only
  with an isolated title-bank build or a substantially smaller representation,
  not by invalidating the full firmware-bank matrix.

- 2026-08-01 **post-D1 GPU2D component census**: passive
  `NDS_PROFILE_GPU=1` attribution over the full detached-two-window/adaptive-top
  route measured total GPU2D work at **1.59--2.32 ms/frame**. Engine B led the
  castle intervals at up to **1.84 ms/frame**; OBJ was **0.26--0.39 ms/frame**
  and is already included in the engine totals. The renderer already has
  direct VRAM chunk mappings, batched tile rows, an OBJ row fast path, a
  no-background path, and a no-effects compositor. A 5% whole-emulation win
  would require removing roughly **27--44%** of the entire measured renderer,
  so no bounded scanline micro-optimization is justified by this census.
  Temporal reuse remains a larger candidate, but it first needs classification
  counters plus fine-grained dirty/generation evidence for aliased VRAM,
  palettes, OAM, registers, display capture, 3D, and mid-frame writes.
  Artifact: `20260801-superblocks-gpu2d-census`; its wall timing is
  instrumented and is not a performance claim.
- 2026-08-01 **post-D1 dispatch-cache validation census**: REJECTED cache
  micro-optimization direction. Temporary diagnostic counters in the exact
  target mode found that **98.99--99.88%** of positive ARM9 cache hits across
  the route were one-page live-code validations. Immutable hits were only
  **0.06--0.38%**, two-page validations **0.07--0.79%**, and stale generations
  were negligible. Full searches were also rare: the retained run's cache
  fast-hit rates were **99.77%** in file select and **98.50%** in settled
  Yoshi. Thus the sampled `lookup_static_cached` share is primarily the exact
  required generation predicate, not collision/search overhead. Hash size,
  associativity, another inline hit path, or sticky RAM selection cannot
  credibly clear the 5% gate. Artifact:
  `20260801-superblocks-lookup-census`. All diagnostic source was reverted and
  the canonical runner restored.
- 2026-08-01 **same-superblock literal-B goto coverage probe**: REJECTED
  before implementing the optimization. An adversarial review identified one
  conservative extension: a non-link `B` to an exact, same-mode, unique,
  unshadowed entry label inside the already-active validated ARM9 RAM
  superblock could use the existing local-goto sequence without another
  dispatch or generation read. Static census found **5,544 / 10,524**
  literal-B sites eligible, but site count overstated dynamic heat.

  A temporary coverage-only build tagged those branches while preserving the
  ordinary dispatcher. Eligible traffic was only **71.82%** of literal-B
  dispatches in initial attract, **46.68%** in the title/menu transition,
  **47.42%** in file select, and **41.70%** in the following new-game load.
  Given post-D1 lookup+dispatch shares, the idea needed approximately
  **88--94%** of literal-B traffic to make a 5% whole-emulation win plausible.
  It therefore fails the evidence gate without paying for an optimized
  candidate or correctness gates. The probe used diagnostic `-O0` title banks,
  so its FPS is intentionally discarded. Two earlier release-link attempts
  hit the established ten-minute ceiling after a first shared-header design
  invalidated the firmware matrix; both exact build process trees were stopped.
  All probe code was reverted, canonical banks regenerated, and the exact
  retained runner SHA-256
  `938F8B97BF8461ACD598A12C379F213E0CEC05BC3CAA952BD185AEB53BCABA6A`
  restored. Artifact: `20260801-superblock-branch-coverage-file`.
- 2026-08-01 **target-mode health confirmation after D1**: one fresh,
  non-interleaved detached-two-window/adaptive-top run on the canonical runner
  completed the full route without a runtime failure. Presentation ranged
  **56.03--59.81 FPS** and emulation cost **11.38--16.14 ms/frame**; the
  sustained castle/Yoshi intervals in this leg were about **14--15 ms/frame**.
  This is a health check, not a replacement for the retained quiet B/A/B:
  it confirms the shipping defaults still need additional headroom and should
  not be described as comfortably locked 60. Artifact:
  `20260801-superblocks-target-confirm`.
- 2026-08-01 **ComputeRenderer target-mode constraint**: no
  soft/compute A/B was run because the current runner deliberately refuses
  `NDS_3D_RENDERER=compute` when adaptive widescreen is active. Compute remains
  a useful tier-2 renderer experiment in native presentation, but it is not a
  candidate for the user's detached/adaptive-top acceptance mode until the
  adaptive path can consume GPU-composited output without synchronized CPU
  readback. Do not quote a compute gain for this workstream.

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
2026-08-01 retained D1 B/A/B state in separate-window/adaptive-top mode:
settled Yoshi **11.41 ms emulation** by the two candidate-leg mean and
castle/water **12.51 ms**, both inside the 12.8 ms headroom goal; the
file-select transition remains about **16.28 ms** and initial attract still
presents near 56 FPS. A later single health leg showed 14--15 ms sustained
gameplay under that host state, so acceptance is not yet robust. Presentation
is no longer the dominant gap; CPU emulation and host variance remain the
primary burndown targets.
