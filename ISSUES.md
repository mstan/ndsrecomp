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
frame), not merely clearing 60 FPS, to absorb lower-end hosts and later
21:9 + anti-aliasing/supersampling cost.

### Policy tiers (agreed with the user — respect these boundaries)

1. **Parity-safe host optimizations** — guest-visible state and both
   framebuffers byte-identical, provable via G3 with the optimization
   FORCED (the B3 precedent). These may be **always-on / default-on**
   with zero policy tension. *This tier is where the 60 FPS is
   expected to come from.*
2. **Accuracy-affecting perf HLE** — the user explicitly authorized
   pursuing modular floating-point/approximate shortcuts when they offer
   gains comparable to the strongest parity-safe options. The proven
   faithful LLE renderer must remain intact and selectable; every HLE
   shortcut must be separately gated, accuracy-diffed against LLE, and
   measured before any default-on decision. Do not mix several
   approximations into one unattributable change.
3. **Enhancements** (widescreen, ultrawide, GL/compute renderer,
   higher internal resolution) — WS-E, opt-in, later. Not this issue.

### What NOT to do

The user's colleague's PS2recomp win came from per-operation FPU
emulation fast paths (ADD.S/MADD.S/DIV.S/RSQRT.S timings). **That
class does not exist here**: the NDS has no FPU (ARM946E-S is
integer-only; SM64DS uses fixed-point + the GX geometry engine + the
memory-mapped hardware div/sqrt device we already emulate), and our
recompiled code is already native x86 — there is no per-op emulation
layer to optimize. Do not import that playbook. SIMD belongs inside
rasterizer inner loops if profiling justifies it, nowhere else.

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
  2x-capacity target.

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

Then rank exact CPU work from evidence. The leading low-risk probe is
runner-scoped LTO/IPO: generated instructions currently make
out-of-line calls to `runtime_should_yield`, `runtime_code_cycles`,
`runtime_tick`, ARM9 cycle combine, and memory timing helpers. If full
LTO establishes a useful ceiling, prefer small CPU-specialized inline
fast paths with exact fallback over one giant opaque change. Retain raw
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

If P-2..P-4 cannot reach the 2x-headroom target, use the measured
evidence to rank modular HLE candidates, including floating-point or
approximate raster math. Keep faithful LLE selectable, expose explicit
A/B control, quantify framebuffer/visual differences, and never
silently remove fidelity for speed.

Current best architecture candidate is upstream melonDS
`ComputeRenderer` behind the existing `Renderer3D` seam, with untouched
`SoftRenderer` as faithful fallback. It is not drop-in: the runner must
produce real 512-byte flat-VRAM dirty granules for its texture cache,
add the accelerated capture/readback hook required by CPU GPU2D
`GetLine`, and provide a GL 4.3 context/frontend path. Classic GL is a
lower-accuracy compatibility fallback and also expects raw VRAM banks
the runner shim does not expose. Scalar floating-point interpolation
shortcuts are lower priority: review estimates only 1.2--1.8x
renderer-local and potentially <=10--25% whole-runtime, with edge/depth
error. Use the null-render ceiling before investing in either path.

### Acceptance for ISSUE-2

- Sustained locked 60 FPS interactive through title, attract, AND
  castle-grounds gameplay, `underruns=0`, normal process priority
  (extends the WS-B acceptance line in PLAN.md).
- **Headroom**: target roughly 2x real-time throughput, i.e. average
  host frame work ≤ ~8.3 ms of the 16.7 ms frame, so lower-end CPUs
  and future 21:9 widescreen render (~1.75× pixels at equal height,
  before AA/supersampling) still have usable budget.
  Report the measured margin, whatever it is.
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
