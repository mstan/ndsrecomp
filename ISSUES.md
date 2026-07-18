# ISSUES.md — open work queue

Written 2026-07-16 as a handoff surface for the next agent (Codex).
`PLAN.md` stays the roadmap source of truth; this file is the actionable
queue with full context per issue. Discipline lives in `CLAUDE.md` /
`PRINCIPLES.md` / `DEBUG.md` — read those first; they override
convenience. Update this file as issues close (move to a ✅ line with
numbers, like PLAN.md does).

## State snapshot (2026-07-16)

- HEADs, all pushed: framework `ndsrecomp` **0d5eb09** ·
  game repo `supermario64dsrecomp` **f25c2ff** · `ndsref` **04ca532**.
- Both worktrees are clean. The runner links separate content-validated
  ARM9 boot/title (`85e24929…`, 18,174 functions) and gameplay
  (`53f37d3f…`, 40,314 functions) generations plus the merged ARM7 bank.
- Standing gates (run after EVERY change, defined in PLAN.md):
  - **G1** firmware traversal: `py -3 oracle\firmware_traversal.py
    --scenario <name>` for all 8 scenarios in
    `oracle/firmware_traversal.json`, fresh ndsref+runner server pair
    PER scenario (no `--rom`). Expected: pass, `tier3_insns9 =
    tier3_insns7 = 0`, audio sample-exact.
  - **G2** host soak: 2400-frame `NDS_FRONTEND_REQUIRE_AUDIO=1`
    interactive run, underruns=0, frame FNV pair
    `(e333837761ca0d1c, d61d2eb50e96b61d)` unchanged unless explained.
    On failure check `key_presses`/`touch_presses` first (a stray
    input, not a regression, is the common cause).
  - **G3** SM64DS parity: `py -3 oracle\probe_gx_state.py --nav
    sm64ds-title --start 100000000 --step 100000000 --count 7`
    (with `--rom`) — byte-locked vs fresh ndsref at insn9=100M..700M
    including BOTH framebuffers.
- Perf picture (all measured, interleaved A/B min-of-N, same machine):
  - Tier-3 interpretation is **eliminated as a cost class**: firmware
    surface tier3 = 0 across all 8 scenarios; game boot→title→700M
    tier3_insns9 = 20,503 / tier3_insns7 = 13 (~0.003% of executed
    instructions). WS-A banked ~22% (A2), B3 inline bus fast path
    ~14% (game serve), B2 ~8-9%, B4 GPU2D OBJ 8.29s→6.11s per soak.
  - The remaining gap to 60 FPS is **static/native-path host cost**.
    User reports ~40 FPS in castle-grounds gameplay. Title-screen
    profile: display bucket 3.46 µs/round vs 0.91 at the 2D firmware
    menu — the 3D software rasterizer is the dominant suspect.
  - Measurement discipline: this machine's wall clock drifts ~2×
    across sessions. Only same-binary (or same-interleave) A/B
    min-of-N numbers are valid. Never single boot-window runs.

---

## ISSUE-1 — [CORRECTNESS LANDED; PERF SAMPLE PENDING] gameplay banks

Landed and pushed as framework `0d5eb09` + game `f25c2ff`. The first
boot-window acceptance correctly stopped a bad promotion:
`tier3_insns9=122,456,468` (pre-merge 21,274). Root cause was not
missing coverage: the monotonic address merge replaced the bank's only
source image, while gameplay had replaced title overlays at
`0x020A0000..0x020CFFFF`; 5,232 / 10,315 old roots had different first
opcodes. The fix preserves the boot/title and gameplay byte generations
as separate content-validated banks. The capture helper now requires a
new `--variant` when an existing source identity differs, preventing a
repeat.

Final correctness evidence:

- G3 100M..700M: byte-locked at every stop including both framebuffers;
  ARM9 Tier-3 **20,503**, ARM7 **13**, `clean_ram_rejects=0`.
- G1: all 8 scenarios pass from fresh pairs, firmware Tier-3/rejects
  zero, audio sample-exact. G2: 2,400 frames, underruns zero, FNV pair
  `(e333837761ca0d1c,d61d2eb50e96b61d)`.
- Interactive navigation reached live castle-grounds gameplay with the
  split banks.

Only the FPS/profile sample remains, and it is now ISSUE-2/P-1 rather
than unlanded correctness work. The first attempt was invalid: 48–54
unrelated build processes were active; provisional samples had heavy
underruns (menu 59.5 FPS, title 34.9, castle 31.0→26.3). Do not use
those as baseline evidence. Repeat menu/title/castle
`frontend_stats` + `profile` min-of-N on a quiet host before P-2.

## ISSUE-2 — [PIVOT — PRIMARY WORKSTREAM] locked 60 FPS with headroom

**User decisions (2026-07-16, expanded 2026-07-17):** the LLE floor is proven (firmware
tier3=0, all gates byte-locked vs the independent oracle), which is
the precondition the project policy set for building layers above it.
The project now pivots to a **default-on host-side optimization
layer** to reach a locked 60 FPS *with headroom* — headroom matters
because widescreen/ultrawide enhancements (WS-E) come next and 3D
rasterization cost scales linearly with pixel count. Primary target
architecture: x86-64. The practical headroom target is now roughly
**2x real-time throughput** (about 8.3 ms of host work per 16.7 ms
frame), to absorb lower-end hosts and later 21:9 + anti-aliasing/
supersampling cost. This is the engineering aspiration, not a binary
acceptance boundary. Success still requires sustained locked 60 FPS plus
measured, comfortable margin above 100% real time; merely touching 60 FPS is
not enough, while falling short of 2x is not by itself a failed optimization.

### Policy tiers (agreed with the user — respect these boundaries)

1. **Parity-safe host optimizations** — guest-visible state and both
   framebuffers byte-identical, provable via G3 with the optimization
   FORCED (the B3 precedent). These may be **always-on / default-on**
   with zero policy tension. *This tier is where the 60 FPS is
   expected to come from.*
2. **Performance HLE** — the user explicitly authorized modular
   whole-subsystem and whole-routine replacements, including host floating-point
   shortcuts, when they produce material gains. The proven faithful LLE path
   remains linked, selectable, and authoritative in verify mode. Each item is
   separately forced, differential-tested, measured, and promoted; an
   accuracy-affecting item needs a declared error contract and explicit user
   approval before becoming default-on. See `HLE_ARCHITECTURE.md`.
3. **Enhancements** (widescreen, ultrawide, higher internal resolution,
   AA/supersampling) — WS-E, opt-in, later. A native-resolution compute renderer
   used purely as a performance HLE belongs to tier 2, not this tier.

### What NOT to do

Do not copy PS2 FPU opcode handlers literally: the NDS has no FPU
(ARM946E-S is integer-only; SM64DS uses fixed-point, the GX geometry
engine, and memory-mapped division/square-root hardware). Do copy the
successful process: retain the faithful implementation, identify hot bounded
seams, add mechanically keyed native replacements, collect misses, run
same-input and mixed-tier differential tests, reject non-winners, and promote
validated winners while preserving a force-floor mode. Host floating point and
SIMD are valid implementation tools inside a modular HLE when profile evidence
and the item's accuracy contract justify them.

### P-1: measured gameplay baseline (falls out of ISSUE-1 step 3)

Measured 2026-07-17 on the validated split gameplay banks, quiet host,
normal priority. Firmware menu held 59.84 FPS with zero underruns; title
ran 54.03--55.88 FPS; live castle-grounds player control ran
37.51--40.76 FPS, with a stable 60-second sample at **39.41 FPS**
(2,366 frames / 1,453 underruns). The castle scheduler sample was
5.218 us/round: ARM9 2.717, ARM7 1.444, devices 0.731, display 0.256;
the sparse 1/1009 sampler usually misses the once-per-frame VCount215
raster call. GPU2D itself was only 0.862 ms/frame. P-2 is justified by
the direct FPS gap and the verified VCount215 soft-rasterizer path.

### P-2: threaded 3D software rasterizer — COMPLETE (2026-07-17)

Thread the vendored melonDS soft renderer the way upstream does —
scanline-partitioned worker rendering with **identical pixel output**
(upstream ships this default-on as `Threaded3D`).

Verified facts (2026-07-16, this session):

- The vendored copy **retains the full upstream threading machinery**:
  `SoftRenderer::SetThreaded / SetupRenderThread / EnableRenderThread /
  StopRenderThread / RenderThreadFunc` are all present in
  `runner/vendor/melonds/GPU3D_Soft.cpp` (see lines ~30-140, 1739+).
  Nothing needs to be ported from `third_party/melonDS`.
- The runner's `melonDS::Platform` shim already implements **real**
  `Thread_Create/Wait/Free` (std::thread) and
  `Semaphore_Create/Free/Reset/Wait/Post` (mutex+cv) —
  `runner/src/gpu3d.cpp:95-148`.
- The old runner policy deliberately left this disabled. It is now
  wired only through the public API: interactive defaults on,
  serve/batch defaults off, and `NDS_3D_THREADED=0/1` provides
  same-binary A/B and forced-on proof. No vendor files changed.
- Reusable debug reset first disables/joins the worker, resets all
  devices, then restores the selected policy. Five forced-threaded
  reset→1,000-VBlank→framebuffer-read stress cycles passed.
- Gates after the lifecycle fix: all eight G1 scenarios pass with
  zero Tier-3/rejects; G2 2,400 frames has zero underruns/audio errors
  and FNV pair `(e333837761ca0d1c,d61d2eb50e96b61d)`; normal and
  FORCED-threaded G3 byte-lock both pass at 100M..700M, both screens.
- Serve nav→700M interleaved A/B (seconds, lower is better): off
  `187.164 / 174.689 / 188.972`; on
  `150.810 / 151.957 / 154.126`. Min-of-3 improves **13.7%**;
  medians improve **18.8%**. Every sample retained Tier-3
  `(20,503,13)` and rejects `(0,0)`.
- Live castle-control same-binary windows: off
  `35.76 / 36.28 / 33.96 FPS`; on
  `42.62 / 41.61 / 41.66 FPS` (**16.5% median gain**). P-2 is a
  material default-on win but is still far short of locked 60 and the
  aspirational 2x-capacity target.

### P-3: B5 — scheduler-round overhead [JUDG]

P-2 still leaves castle near 42 FPS, so this is material. First add
direct per-frame timing around 3D RenderFrame/worker wait/GetLine (the
1/1009 scheduler sampler misses the once-per-frame raster call) and use
a diagnostic-only null/clear renderer to establish the zero-raster
ceiling. Do not ship that diagnostic renderer.

Direct bridge timing landed/measured 2026-07-17 on the same title path
to 700M (4,286 rendered frames): unthreaded VCount215 spends
**7.399 ms/frame** in the soft render. Threaded mode reduces the main
critical path to 0.0136 ms submission + 0.0260 ms GetLine waits +
0.0062 ms VCount144 sync. Wall time falls 188.399→154.296 seconds,
or 7.96 ms/frame, matching the hidden raster cost. Therefore the
remaining ~23.5 ms castle critical path is primarily guest
CPU/runtime/scheduler; renderer work still matters for worker-core
contention and future high-resolution HLE, but cannot alone meet the
8.3 ms whole-frame target.

First exact CPU result (2026-07-17): `arm9_cycle_combine` is now a
header-inline pure integer helper. This lets each CPU-specific generated
bank fold the constant instruction class and removes almost every
per-instruction combine ABI call without changing generated sources.
Static audit found identical generated-symbol/relocation coverage,
`.text` shrinking 235,726,176→223,701,472 bytes (**-5.10%**), and only
201 residual helper calls from 31,994 combine sites in a representative
gameplay shard. Clean nav→700M interleaved A/B seconds were baseline
`124.174 / 115.842 / 110.705 / 112.840 / 110.373` versus inline
`118.894 / 120.594 / 109.483 / 109.845 / 109.595`: **0.8% min-of-5**
and **2.7% median** improvement. Live castle-control windows improved
from `53.89 / 54.22 / 53.12` to `54.46 / 54.54 / 54.76 FPS`
(**+1.2% median**). This is a small additive/code-footprint win, not a
60-FPS solution. Gates: G1 8/8 with zero Tier-3/rejects; G2 2,400 frames,
zero underruns/audio errors and exact FNV pair; always-on/forced G3
byte-lock passed at 100M..700M on both framebuffers.

Second exact CPU result (2026-07-17): the runner now maintains an exact
`IME & IE & IF` cache and a rare-condition yield-poll hint. Generated banks
keep the existing `runtime_tick` / `runtime_should_yield` ABI and the full
faithful checks remain available as the slow path. Every transition that can
make a CPU halt, DMA stall, instruction/event break, static guard
invalidation, terminal halt, or interrupt pending sets/recomputes the
corresponding runner state; break-PC and cycle-cap remain dynamically tested
on every poll. `NDS_CPU_FAST_POLL=0/1` provides same-binary reference/forced
testing, with the exact fast path default-on.

Nav-to-700M same-binary A/B seconds were OFF
`118.375 / 115.639 / 116.337 / 132.648 / 138.057` versus ON
`120.148 / 112.707 / 110.558 / 132.487 / 106.690`. The host slowed sharply
during the latter samples, so retain both views: min-of-5 improves
**8.4% throughput** (115.639 -> 106.690 seconds) and medians improve
**5.0%** (118.375 -> 112.707 seconds). All ten samples had identical
700M instruction, static-coverage, Tier-3 `(20,503,13)`, and reject `(0,0)`
counters. Forced-ON gates pass: G1 8/8 with byte-exact frames/audio and zero
Tier-3/rejects; G2 2,400 frames with zero underruns/errors/input and exact FNV
pair `(e333837761ca0d1c,d61d2eb50e96b61d)`; G3 byte-lock at 100M..700M on
both framebuffers with the established RAM_COUNT sequence.

A broader tick-centric generated yield-call gate was also tried and rejected.
It built and passed the focused unit tests and a menu soak, but both forced-OFF
and forced-ON title navigation halted at ARM9 instruction 54,096,928 with
`call-return overflow`. Because the control experiment failed identically, it
was not performance evidence and none of that experiment remains in the tree.

Then rank exact CPU work from evidence. An initial runner-scoped GCC LTO/IPO
ceiling attempt produced a 663 MB intermediate archive but no linked runner;
it is incomplete and provides no performance evidence. Do not keep paying its
whole-program build cost as the selection gate. Generated instructions still
make out-of-line calls to `runtime_should_yield`, `runtime_code_cycles`,
`runtime_tick`, and memory-timing helpers. The next parity-safe CPU experiment
is therefore a narrow generated-code specialization of those exact common
paths, retaining the out-of-line faithful fallback. The earlier fast-poll result
makes a >=5% gain plausible; reject the experiment if quiet interleaved A/B is
below that threshold or code growth/i-cache cost erases the saved calls. Retain raw
`switch_ns`, `switches`, `crs_words`, and `next_event_ns` before
touching context copies or deadline calculation. Same discipline:
measure, change, G-gates, measure, commit.

### P-4: bus/runtime residuals [MECH-ish]

The B3 follow-up candidate from PLAN.md: inline `runtime_mem_cycles`'
hot regions (flagged as a separate melonDS-mirroring risk class —
mirror the melonDS timing model exactly, prove with G3). Also any
remaining slow-path bus hits visible in the profile. Only if material
after P-2/P-3.

### P-5: modular performance-HLE escalation (tier 2)

To pursue comfortable above-real-time headroom and the aspirational 2x target
after P-2..P-4, use the measured evidence to rank modular HLE candidates,
including floating-point or
approximate raster math. Keep faithful LLE selectable, expose explicit
A/B control, quantify framebuffer/visual differences, and never
silently remove fidelity for speed.

Current best architecture candidate is upstream melonDS
`ComputeRenderer` behind the existing `Renderer3D` seam, with untouched
`SoftRenderer` as faithful fallback. An opt-in experimental integration now
exists behind build option `NDS_ENABLE_COMPUTE_RENDERER=ON` and runtime force
`NDS_3D_RENDERER=compute`; soft remains the runtime default. The runner supplies
real 512-byte flat-VRAM dirty granules, an OpenGL 4.3 hidden context, shader/GL
failure checks, and the accelerated capture/readback hook required by CPU
GPU2D `GetLine`. The runner owns the CPU readback buffer, checks PBO map and
unmap success, preserves upstream `AbortFrame`/`RenderXPos` line semantics, and
turns runtime failures into persistent terminal state plus nonzero process
status. Imported implementation files remain byte-identical to the pinned
melonDS source; the compatibility shims are explicitly runner-owned.

Current forced-compute evidence is deliberately insufficient for promotion:

- all 33 compute programs compile on an NVIDIA RTX 3080 Ti and a 240-frame
  forced smoke completes cleanly;
- paired soft/compute GX state passes through ARM9 700M with the established
  RAM-count sequence and identical execution/static-coverage counters;
- framebuffer output is not parity-safe: early title samples differed on the
  3D screen by 54--56 pixels, and castle-route samples differed by
  1,144--7,311 3D-screen pixels with maximum channel deltas up to 243; the
  bottom screen remained exact in those samples;
- synchronized native-resolution GPU readback costs roughly 0.8--1.0 ms/frame
  in profiled heavy-route intervals;
- no performance gain is claimed yet. The available apparent gain came from a
  profiled, non-interleaved compute run, and a later unprofiled run overlapped
  unrelated compiler load. Both are contaminated.

Rejected integration experiment: replacing the mapped PBO readback with
`glGetBufferSubData` made failure observation straightforward but reduced the
same 240-frame early-boot smoke to **33.72 FPS**, versus **44.40--47.14 FPS**
after restoring a runner-owned checked map/unmap path (adjacent soft smoke
**45.47 FPS**). These are smoke numbers, not castle performance evidence; they
were sufficient to reject and remove the slower readback implementation.

The fresh-process castle-cutscene harness has validated two soft and two
compute replays to VBlank 2600: per-backend observations are deterministic,
guest/GX/coverage state agrees, and the bottom-screen route signature is
`34e37c06faeccc75e67127191c11805de5de4932f88d9e2901d8dda4057b9f8b`.
Balanced `ABBAAB` timing remains pending: an unrelated orphaned devkitPro
Python process was consuming most of one CPU core, so starting performance
sampling would have violated the quiet-host rule. A later full attempt began
after that process exited, but intermittent host pressure made even host-only
queries stall and the harness hit its 20-minute outer timeout during checkpoint
revalidation, before producing any timing sample. That run is inconclusive and
records no renderer gain; CPU-HLE heat profiling now advances while the
validated renderer benchmark waits for a genuinely quiet window.

The compute-enabled binary's retained soft floor is green after the integration
and hardening: G1 passes all eight fresh-pair firmware scenarios with byte-exact
frames/audio and zero Tier-3/rejects; G2 completes 2,400 frames with zero audio
errors/underruns and locked FNV pair
`(e333837761ca0d1c,d61d2eb50e96b61d)` and zero input; default-soft G3 byte-locks
both screens at every 100M--700M checkpoint. Focused decode/cycle tests pass.
The changed sources also compile with compute support disabled. A final
post-hardening 240-frame forced-compute smoke compiles all 33 programs, reports
the compute backend, raises no GL error, and matches a fresh soft run at that
checkpoint.

Therefore ComputeRenderer remains a useful tier-2 experiment, opt-in and
unpromoted. Next run same-binary fresh-process interleaved soft/compute trials
over one uninterrupted castle interval on a quiet host, with profiling absent
from timing runs. Then characterize slow-frame tails, audio underruns, resets,
display capture, representative visual effects, and AMD/Intel behavior. If
readback erases the renderer gain, the next renderer seam is direct GPU
composition rather than more CPU-readback tuning. Classic GL remains a
lower-accuracy compatibility fallback and expects raw VRAM banks the runner
shim does not expose. Before promotion, also add a non-guest-mutating drain for
a finite run that stops with one compute frame pending, harden teardown after a
catastrophic context-loss/reacquire failure, add focused `RenderXPos` boundary
and `AbortFrame` tests, and measure the remaining GL-error poll cost.

The CPU-HLE heat probe is implemented as a candidate-only generated wrapper
keyed by the selected bank's exact validation identity, not a bare-PC runtime
hook and not whole-bank instrumentation. Its strict title manifest selects
static ARM9 `MulVec3Mat4x3` at `0x02052858..0x02052914`; the generator verifies
the program SHA, exact unsplit 188-byte function, independently derives content
SHA-1 `4d9db01dcbcbd3b05ce22dfd9ab1eb06ecaa6616`, and proves 47 ARM instructions,
no calls/branches/SWIs, and one unconditional return. Profiling builds count
every content-qualified start/interior segment and time a configurable phase of
every `2^N` segments around the unchanged private LLE body. Clean normal and
unwind segments are accepted separately; IRQ, invalid-length, nesting/depth,
guard, and PC contamination remain separate counters. Queries are cumulative
and passive; measure by subtracting two stopped-route snapshots. Profiler-OFF
emits the original public body directly; a manual `-O2 -g0` candidate-shard
object comparison is byte-identical.

Castle VBlank 2600--4400 triage rejects `MulVec3Mat4x3` as a standalone HLE
lever. Three dispersed 1/64 phases each saw the exact same 149,864 segments and
79,240 logical starts. Their systematic instrumented body-cost estimates were
**0.582%, 0.566%, and 0.586%** of route wall time. An all-segment census then
sampled all 149,864 segments, accepted 149,837, explicitly rejected 24
IRQ-contaminated and three invalid-length samples, and estimated **0.566%**.
There were no guard, PC, depth, or nested-entry failures. This is a heat proxy,
not a formal zero-cost speedup ceiling: timed clock/bookkeeping overhead biases
the numerator up, unsampled profiler overhead dilutes the denominator, and an
atomic HLE can also change dispatch/resume cost outside the wrapper. Even with
those caveats, four concordant measurements put this routine's wrapper-local
heat below 1% in this castle route window; they do not measure other scenes or
external dispatch/resume savings. That is no evidence for the 4.76% removable
share needed for a 5% speedup, so a standalone replacement is deprioritized
unless broader measurements change its rank. Profiling-ON forced G3 passed
GX state and both framebuffer byte-locks at every 100M--700M checkpoint. The
default Profiler-OFF path is object-identical to the no-manifest candidate body
(SHA-256 `778d8b34b5601bca56e26a575784988b490211083de74b405f5d17c9f379fc8b`)
and passed G1 8/8, G2 at 2,400 frames with zero underruns/errors and the locked
FNV pair, and G3 GX/both-screen byte-lock at 100M--700M. The measurement seam is
green; no CPU-HLE speedup is claimed.

A parity-safe generated retirement-call fusion was also tried and rejected on
2026-07-18. It replaced each adjacent `runtime_tick(cycles)` plus
`runtime_unwinding()` ABI pair with one exact `runtime_retire(cycles)` helper;
the helper preserved cycle/deferred-debt commitment, IRQ observation, and
unwind ordering. Regenerated banks linked and focused decode/cycle tests passed.
In a representative gameplay shard `.text` shrank 1,580,224 -> 1,534,304 bytes
(**-2.91%**), with 6,249 tick relocations and 6,249 unwind relocations replaced
by 6,249 retire relocations. Fresh-process castle-cutscene validation proved the
untouched and fused binaries identical at both VBlank 2,600 and 4,400: both
framebuffers plus event/instruction, GX, scheduler, static-coverage, and Tier-3
state all matched (endpoint ARM9/ARM7 instructions
`585,396,047 / 178,255,829`). Two timing attempts were contaminated by unrelated
PSX/SNES compiler work and are not performance evidence. The complete attempt's
raw, invalid ABBAAB seconds were baseline
`35.902 / 37.218 / 36.406` and fused
`36.353 / 37.019 / 40.030`; even the lower envelope did not indicate a 5% win.
Because promotion requires affirmative >=5% quiet evidence, the experiment
failed the retention gate and was removed without spending full G1/G2/G3 time.
This rejects only the extra call-boundary fusion, not the larger exact work
inside code-fetch or memory-timing helpers.

An out-of-line static ARM9 code-fetch specialization was then tried and
rejected on 2026-07-18. Generated ARM9 ARM/Thumb own-fetch sites called
mode-specific helpers while ARM7, refills, Tier-3, and the original generic
helper remained faithful fallbacks; `NDS_CPU_STATIC_FETCH=0/1` selected the
same experimental binary's generic/specialized path. The deployed ARM common
path was a 15-instruction leaf, OFF/invalid state tail-jumped to the original
helper, a representative optimized shard retained exactly 1,580,224 bytes of
`.text`, and relocations changed exactly as intended (gameplay shard:
31,974 ARM + 4 Thumb specialized calls, zero generic own-fetch calls; ARM7
stayed generic). Untouched A, experimental OFF, and ON were byte/state exact at
castle VBlank 2,600 and 4,400: both framebuffers, event/instruction, GX,
scheduler, static/Tier-3 state, and rejects matched; endpoint ARM9/ARM7 counts
were `585,396,047 / 178,255,829` with rejects `(0,0)`.

The shared host repeatedly launched unrelated workloads, so no sequential
timing batch was claimable. A diagnostic-only simultaneous A/O/N harness pinned
each fresh process to a disjoint two-core-plus-sibling set and rotated all three
sets across rounds. Its A/O control ratios were `0.9991 / 1.0113 / 0.9947`;
ON/A ratios were `1.0140 / 1.0175 / 0.9993`, and ON/O ratios were
`1.0149 / 1.0061 / 1.0046`. One round overlapped a compiler and all results are
exploratory, not promotion evidence. Nevertheless every observed signal was
below 1.8%, far from the required 5%; absent affirmative material evidence,
the standalone seam failed the retention gate and was removed without full
G1/G2/G3. This does not reject combining yield/count/hook/fetch work behind one
exact prologue ABI, which removes a larger universally executed boundary.

That larger exact combined-prologue experiment was implemented and rejected on
2026-07-18. For statically compiled ARM9 ARM instructions it combined the
yield poll, retired-instruction count/hook, and own-code fetch behind one host
ABI boundary; ARM7, Thumb, unusual/refill paths, and an authoritative generic
fallback stayed unchanged. The runner kept a private snapshot of the already
latched ARM9 code-region timing and still tested live ITCM placement. Generated
R15 publication and all yield/retirement/fetch ordering remained faithful.
Focused decode/codegen and cycle tests passed. A representative gameplay shard
contained 31,974 combined ARM sites and only four legacy Thumb sites; `.text`
fell from 9,259,712 to 7,070,784 bytes (**-23.64%**). Untouched and combined
binaries were exact at castle VBlank 2,600 and 4,400: both framebuffers plus
event/instruction, GX, scheduler, static-coverage, and Tier-3 state matched.

Six compiler-clean simultaneous forward/reverse affinity rotations produced
baseline/candidate throughput ratios
`1.04496 / 1.06125 / 1.04158 / 1.05493 / 1.03007 / 1.04306`.
Each binary visited each affinity set equally. The balanced geometric mean was
**1.04593**, the median **1.04401**, and only two of six observations reached
the predeclared `1.05` retention threshold. A frameless tail-jump refinement
removed the helper's call frame but yielded only
`1.03804 / 1.05613 / 1.04437` (median **1.04437**), providing no incremental
gain; it was reverted first. A final sequential `ABBAAB` confirmation could not
produce quiet evidence: its first sample overlapped C++ builds and a PSP recomp
test (41.3% host CPU busy), and process enumeration timed out during sample two.
That failed batch is operationally inconclusive and cannot rescue the miss.
The combined seam is therefore recorded as a genuine roughly **4.4--4.6%**
exploratory gain, but it failed the >=5% retention gate and was removed without
spending full G1/G2/G3 time. The result reinforces that broad universally hot
generated/runtime boundaries matter; it does not justify retaining this added
ABI and timing-snapshot maintenance surface on its own.

The adversarial review rejected the earlier general wrapper-local inclusive
timer: slice unwinds can destroy a wrapper host frame and resume in a descendant
or even at the start PC. A correct non-leaf profiler needs per-CPU logical guest
frames and call-return-depth lifecycle hooks. That larger mechanism is deferred
until measured evidence requires a non-leaf candidate. Windows WPR/xperf was
also tried as zero-code corroboration but this session lacks the system-profile
privilege it requires. A broad `gprof` build was stopped and rejected after it
began rebuilding hundreds of unrelated generated firmware TUs; its `mcount`
overhead, whole-lifetime attribution, multithread denominator, and lack of a
graceful serve flush make it unsuitable as the selection gate. Neither attempt
produced performance numbers.

The CPU/title seam and promotion contract are now specified in
`HLE_ARCHITECTURE.md`. Do not hook candidates by bare PC in
`runtime_dispatch`: immutable banks can make direct generated calls and future
castle bank generations can reuse the same addresses. Emit candidate-only
wrappers that retain the original generated body, and select the first math
replacement from content-qualified dynamic call/time data.

### Acceptance for ISSUE-2

- Sustained locked 60 FPS interactive through title, attract, AND
  castle-grounds gameplay, `underruns=0`, normal process priority
  (extends the WS-B acceptance line in PLAN.md).
- **Required headroom**: measured uncapped throughput must sit comfortably
  above 100% real time across the representative windows, with slow-frame
  tails reported; a fragile 60.0 FPS average is not acceptance.
- **Aspirational headroom**: continue toward roughly 2x real-time throughput,
  i.e. average host frame work ≤ ~8.3 ms of the 16.7 ms frame, so lower-end
  CPUs and future 21:9 widescreen render (~1.75× pixels at equal height,
  before AA/supersampling) retain usable budget. Report the measured margin;
  missing 2x alone does not classify an otherwise useful optimization as a
  failure.
- All three gates green; every tier-1 optimization proven parity-safe
  with its forced-on G3 run; tier-2 backends get explicit visual/error
  and guest-state validation; per-change perf numbers in commits.

## ISSUE-3 — [ROUTINE, RECURRING] gameplay coverage merges

Castle interior, painting levels, bosses load overlays not yet
captured → tier-3 returns there. The ~15-min routine (monotonic,
gate-checked): play `--interactive --rom --discover-static-misses`
into the new area → run the ARM9 capture tool with `--skip-nav
--variant <area_name>` and the ARM7 tool with `--skip-nav`
(`supermario64dsrecomp/tools/capture_arm{9,7}_ram_bank.py`) against
the play surface → regen banks → rebuild → gates → commit with
counts. The user can play with discovery on to feed this.

## ISSUE-4 — WS-C: in-game audio validation (PLAN.md priority 2)

Untouched. C1 gameplay audio traversal (scripted-input scenarios,
`audio_samples` sample-exact vs ndsref at matched ordinals), C2 SPU
capture completion (SNDCAPxCNT source-select/add-mode inert; SM64DS
routes reverb through capture — mirror melonDS SPU.cpp), C3 gameplay
soak. Becomes more urgent once gameplay perf work (ISSUE-2) has
people actually playing.

## ISSUE-5 — correctness backlog (opportunistic; details in PLAN.md WS-D)

- D1 2D windows (Win0/Win1/OBJ-window/WINOUT unimplemented).
- D2 main-memory display FIFO (0x04000068, DMA mode 4, capture srcB).
- D3 save persistence + per-game chip types (today: 8 KiB EEPROM,
  RAM-only).
- D4 small fidelity: engine-B 0x04001064 latch, io.cpp -Wnarrowing,
  TCP.md long-tail command docs.
- D5 GXFIFO stall path implemented but unexercised by SM64DS —
  validate on a GXFIFO-heavy title before trusting.
- Informational: 27 residual boot-window ARM7 entries + BIOS
  mid-function PCs.
- Game repo `ndsrecomp.pin` is current at framework `0d5eb09`.

---

## Operating rules (non-negotiable, condensed — full text in CLAUDE.md/PRINCIPLES.md/DEBUG.md)

- **LLE-faithful floor is untouchable.** No stubbed SWIs, no direct
  boot, tier-3 interpreter only on guest-written RAM. HLE/perf layers
  sit ON TOP of the proven floor per the tier policy above.
- **Never edit `generated/**`.** Fix recompiler/runtime/config, regen,
  rebuild, re-measure. Vendored `runner/vendor/melonds/` files: treat
  as upstream — drive via public APIs; verbatim upstream restores
  only, documented.
- **Gates G1/G2/G3 after every change.** Parity is the only gate mode
  (`--rtc-host`, `NDS_DEEP_TRACE` stay off/default).
- **Ring buffers, query-after-the-fact.** Never arm-then-capture,
  never pause/step to compare. Play mode rejects `run_to_*` by
  design — sample counters twice instead.
- **Server hygiene:** kill stale `ndsref`/`nds_runner` by PORT before
  any probe (`netstat -ano | Select-String ':1984[23]\s.*LISTENING'` —
  exactly one pid per port; stale servers fabricate phantom
  divergences). Kill before rebuilding (exe lock). Fresh server pair
  PER traversal scenario.
- **Build via PowerShell** with explicit `C:\msys64\mingw64\bin\cmake.exe`
  (PATH cmake is devkitPro MSYS and mangles Windows paths; git-bash
  builds silently fail). Running exes from bash is fine.
- **Perf numbers:** interleaved A/B min-of-N on a quiet machine, in
  every commit message. Cross-session absolutes drift ~2×.
- **Experiment ledger:** document failed, rejected, and inconclusive trials,
  including the hypothesis, control, exact failure or contamination, observed
  numbers when valid, and final disposition. Do not silently discard a
  non-winner, and do not label a useful gain "failed" merely because it did
  not reach an aspirational aggregate target such as 2x.
- **Use Terra subagents throughout the project.** Parallelize concrete,
  independent audits/probes when that shortens the critical path. For every
  material architecture, correctness, or performance conclusion, assign an
  adversarial review of the evidence or proposed change and explicitly
  critique the returns before adopting them. Subagent agreement is not a gate;
  reproducible measurements and the project oracles remain authoritative.
- The game repo's `ndsrecomp` junction targets the LIVE framework
  checkout; `ndsrecomp.pin` is documentation. Per-title work goes in
  `supermario64dsrecomp`, framework work in `ndsrecomp`, framework
  commits first, push when green.

## Build / run crib

```powershell
# rebuild recompiler / regen game banks / rebuild runner
& C:\msys64\mingw64\bin\cmake.exe --build "F:/Projects/ndsrecomp/ndsrecomp/recompiler/build"
& C:\msys64\mingw64\bin\cmake.exe --build "F:/Projects/ndsrecomp/supermario64dsrecomp/build-native" --target sm64ds_recompiled_banks
& C:\msys64\mingw64\bin\cmake.exe --build "F:/Projects/ndsrecomp/ndsrecomp/runner/build-sm64-native" --target nds_runner

# G2 soak
$env:NDS_FRONTEND_STATS='1'; $env:NDS_FRONTEND_MAX_FRAMES='2400'; $env:NDS_FRONTEND_REQUIRE_AUDIO='1'
& "F:\Projects\ndsrecomp\ndsrecomp\runner\build-sm64-native\nds_runner.exe" "F:\Projects\ndsrecomp\ndsrecomp\bios" --interactive

# servers (kill first; ONE listener per port 19842/19843)
Stop-Process -Name ndsref,nds_runner -Force -ErrorAction SilentlyContinue
& ".\runner\build-sm64-native\nds_runner.exe" ".\bios" --serve --rom "F:\Projects\ndsrecomp\supermario64dsrecomp\Super Mario 64 DS.nds"   # +--discover-static-misses for coverage
& "F:\Projects\ndsrecomp\ndsref\build-native\ndsref.exe" --bios9 ...\bios\biosnds9.rom --bios7 ...\bios\biosnds7.rom --firmware ...\bios\firmware.bin --rom "...\Super Mario 64 DS.nds" --boot firmware --port 19843

# gates
py -3 oracle\firmware_traversal.py --scenario <name>          # G1, 8 names in oracle/firmware_traversal.json, fresh pair each
py -3 oracle\probe_gx_state.py --nav sm64ds-title --start 100000000 --step 100000000 --count 7   # G3

# play-mode driving/measuring (interactive serves TCP on 19842; protocol in TCP.md)
& ".\runner\build-sm64-native\nds_runner.exe" ".\bios" --interactive --rom "..." [--discover-static-misses]
# then: touch/keys/framebuffer/frontend_stats/profile/static_coverage/tier3_coverage

# live captures against the play surface (or a stopped serve)
py -3 supermario64dsrecomp\tools\capture_arm9_ram_bank.py --skip-nav   # and the arm7 variant
```
