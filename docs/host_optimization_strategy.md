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
cross-unit branch, and every unmatched return funnels through
`runtime_dispatch` (hash probe + content-generation validation done
TWICE per hit + indirect call). ARM9 banks are all `--validate-live-bytes`,
so no direct C-to-C calls exist anywhere on ARM9 (confirmed: zero in all
sampled shards).

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

- **B0. Composition counters FIRST.** Cheap always-on counters splitting
  dispatch volume by cause: BL-call dispatch, cross-unit direct branch,
  return CRS-hit vs CRS-miss, computed (BX/LDR-pc/LDM-pc), IRQ/SWI
  vectoring, cache hit/miss/inactive-revalidation. Queryable via debug
  server; printed by `nds_profile_report`. The 07-31 forward-goto lesson:
  the mechanism was real but the volume estimate was wrong — measure
  composition before optimizing it.
- **B1. Guard-snapshot dedup**: `StaticGuardScope::call` currently
  recomputes (`arm_static_guard`) the same page/generation snapshot that
  `cached_lookup_live` just validated. Copy the snapshot from the
  `CachedStaticLookup` slot instead. Runtime-only, no regen.
- **B2. Validated direct calls for BL** (needs B0 evidence): emit
  `if (inline generation check for callee) callee_sym(); else
  runtime_dispatch(target);` at BL sites with compile-time-known targets,
  including cross-shard (requires cross-shard symbol declarations or a
  per-bank extern table). Preserves the exact validation predicate;
  skips hash probe + indirect call; host stack mirrors guest calls.
  SIZE UP / SPEED UP. The current "no direct calls under
  --validate-live-bytes" rule exists only because calls bypassed
  validation — inlining the validation removes the reason.
- **B3. Per-callsite monomorphic inline cache** for computed transfers
  (BX reg / LDR pc / LDM pc): a static per-site slot {target, fn,
  generation snapshot}; hit = compare + call, miss = dispatch + refill.
  SIZE UP (+~24 B/site + check code) / SPEED UP. Needs B0 to confirm
  computed transfers matter.
- **B4. Larger RAM-bank split units**: RAM banks are generated with
  `--max-function-bytes 512`; every unit-crossing branch dispatches.
  Any unit ≤4 KiB still spans ≤2 4-KiB pages (the guard limit), so the
  cap can rise ~8x, converting cross-unit dispatches into intra-unit
  gotos (the forward-goto generalization already handles both
  directions). Needs B0 to confirm RAM-bank cross-unit volume; also
  re-check capture tool assumptions before changing.
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
  amortize entry/exit overhead further.
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

Headroom metric: `phase_ms_per_frame.emu` from the scenario harness.
16.7 ms = break-even, **12.8 ms = 1.3x (goal), 8.3 ms = 2x (aspiration)**.
2026-07-31 state: Yoshi 59.8 FPS at ~14.6 ms (locked but thin); worst
menu phase 43.5 FPS at 22.4 ms.
