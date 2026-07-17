# ISSUES.md — open work queue

Written 2026-07-16 as a handoff surface for the next agent (Codex).
`PLAN.md` stays the roadmap source of truth; this file is the actionable
queue with full context per issue. Discipline lives in `CLAUDE.md` /
`PRINCIPLES.md` / `DEBUG.md` — read those first; they override
convenience. Update this file as issues close (move to a ✅ line with
numbers, like PLAN.md does).

## State snapshot (2026-07-16)

- HEADs, all pushed: framework `ndsrecomp` **14482c0** ·
  game repo `supermario64dsrecomp` **0d7851a** · `ndsref` **04ca532**.
- **Uncommitted** (game repo): `config/sm64ds_arm9_ram.toml` +
  `config/sm64ds_arm7_ram.toml` — the gameplay-window bank merges
  (ISSUE-1). The built runner
  `runner/build-sm64-native/nds_runner.exe` already links these merged
  banks and is **unvalidated**.
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
    tier3_insns9 = 21,274 / tier3_insns7 = 135 (~0.003% of executed
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

## ISSUE-1 — [IN-FLIGHT, LAND FIRST] gameplay bank-merge validation

The two RAM-bank configs were re-captured live during real castle-
grounds gameplay (title → ADVENTURE → FILE A → castle grounds as
Yoshi, ~1 min varied movement) over the play-mode TCP surface:
ARM9 config 10,315 → 28,536 entries (capture image sha1 `53f37d3f…`),
ARM7 5,117 → 5,243 (`c21758bd…`). Regenerated banks: ARM9 RAM
18,174 → 40,314 functions; ARM7 RAM 7,247. Runner rebuilt clean.
**Zero measurements taken since** — validation was paused for a
user check-in (approved to proceed).

Capture bins (git-ignored, required for regen) are at
`supermario64dsrecomp/generated/capture/sm64ds_arm{9,7}_ram.bin`.

Steps (all user-approved):

1. **Boot-window acceptance.** Kill stale servers by port first
   (see rules below). Launch a serve pair, run the nav→700M timed
   path (scratchpad helper `a2_timed_run.py` if still present, else
   the G3 probe run), read `tier3_insns9` — compare vs the pre-merge
   21,274. Expect *some* movement: the gameplay captures replaced the
   boot-window captures, and where boot-vs-gameplay overlay content
   differs, rows now validate against gameplay bytes (content
   validation keeps correctness; the count is what we're measuring).
   A large regression (≫100k) means a capture problem — stop and
   diagnose, don't commit.
2. **G1 (8 scenarios, fresh pair each) + G2 + G3.** All must be green.
   Bank promotion keeps `clean_ram_rejects=0` and passes
   `--validate-live-bytes`.
3. **FPS re-probe** (this doubles as the ISSUE-2 baseline — record
   everything). Launch `--interactive --rom`, drive over TCP
   (port 19842): menu tap (128,48) → title tap (128,120) →
   ADVENTURE (128,162) → FILE A (52,55) → Start-skip cutscenes →
   castle grounds. At menu / title / castle grounds: sample
   `frontend_stats` twice for fps, and `profile`
   (NDS_PROFILE_GPU/SCHED) for the bucket breakdown. Scratchpad
   helper `fps_probe.py` did this; recreate if lost.
4. **Commit** the two game-repo configs with the numbers, re-pin
   `ndsrecomp.pin` to the framework HEAD, update PLAN.md status +
   memory. Framework commits first if any framework change was
   needed.

## ISSUE-2 — [PIVOT — PRIMARY WORKSTREAM] locked 60 FPS with headroom

**User decision (2026-07-16):** the LLE floor is proven (firmware
tier3=0, all gates byte-locked vs the independent oracle), which is
the precondition the project policy set for building layers above it.
The project now pivots to a **default-on host-side optimization
layer** to reach a locked 60 FPS *with headroom* — headroom matters
because widescreen/ultrawide enhancements (WS-E) come next and 3D
rasterization cost scales linearly with pixel count. Primary target
architecture: x86-64.

### Policy tiers (agreed with the user — respect these boundaries)

1. **Parity-safe host optimizations** — guest-visible state and both
   framebuffers byte-identical, provable via G3 with the optimization
   FORCED (the B3 precedent). These may be **always-on / default-on**
   with zero policy tension. *This tier is where the 60 FPS is
   expected to come from.*
2. **Accuracy-affecting perf HLE, default-on** — would amend the
   standing parity-default rule (PRINCIPLES.md / ENHANCEMENTS Rule 1).
   **Requires explicit user sign-off per item**, and only if tier 1
   measurably falls short. Do not start tier-2 work unprompted.
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

The 40 FPS report and the display-bucket numbers predate the merged
banks. The ISSUE-1 fps/profile probe at menu / title / castle grounds
is the baseline that ranks the buckets. Every subsequent P-item is
justified (or dropped) by this measurement, not by the inference.

### P-2: threaded 3D software rasterizer (expected main win)

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
- The runner deliberately never calls `SetThreaded(true, …)` — the
  comment at `runner/src/gpu3d.cpp:90-93` says single-threaded was
  chosen for deterministic, oracle-comparable execution. That concern
  is what the gating pattern below answers.

Plan:

1. Wire `SetThreaded(true, gpu)` from the runner bridge
   (`runner/src/gpu3d.cpp` / frontend init). **No vendor-file edits
   needed** — it's a public vendored API. If a vendor file genuinely
   must change, restore upstream code verbatim and document the delta;
   never hand-roll divergent logic inside `runner/vendor/melonds/`.
2. Mirror the **B3 gating pattern** exactly: interactive/play mode
   defaults threaded; `--serve`/batch/oracle modes default
   single-threaded and bit-identical; an env var (suggest
   `NDS_3D_THREADED=1`) forces threaded mode on a serve server for
   equivalence proofs and honest same-binary perf A/B.
3. **Prove parity**: G3 byte-lock (100M..700M, both framebuffers)
   with threading FORCED, plus G1/G2/G3 green in normal modes.
   Threading is host-side render-order only; if any guest-visible
   divergence appears (GXSTAT, RDLINES-class state, capture
   contents), that's a real finding — stop and root-cause, don't
   fudge the gate.
4. **Measure**: same-binary interleaved A/B min-of-3 on the serve
   nav→700M path (threaded vs not), and the interactive fps probe at
   castle grounds vs the P-1 baseline. Watch the sync boundary: the
   2D compositor reads 3D scanlines as they complete
   (`RenderThreadRendering` / semaphore handshake); a naive
   full-frame join wastes the parallelism.
5. Commit with numbers.

### P-3: B5 — scheduler-round overhead [JUDG]

Only if P-1 shows the scheduler buckets material after P-2 (title
profile had scheduler round 5.37 µs: ARM9 2.13, ARM7 1.18, devices
1.68). Round-granular costs: per-round Run() dispatch, timer catch-up.
Same discipline: measure, change, G-gates, measure, commit.

### P-4: bus/runtime residuals [MECH-ish]

The B3 follow-up candidate from PLAN.md: inline `runtime_mem_cycles`'
hot regions (flagged as a separate melonDS-mirroring risk class —
mirror the melonDS timing model exactly, prove with G3). Also any
remaining slow-path bus hits visible in the profile. Only if material
after P-2/P-3.

### P-5: escalation (tier 2) — STOP AND ASK

If, after P-2..P-4, castle-grounds fps is still short of locked 60 at
normal process priority, compile the measured evidence (bucket
breakdown, what each P-item bought) and **ask the user** before any
accuracy-affecting work. Do not silently degrade fidelity for speed.

### Acceptance for ISSUE-2

- Sustained locked 60 FPS interactive through title, attract, AND
  castle-grounds gameplay, `underruns=0`, normal process priority
  (extends the WS-B acceptance line in PLAN.md).
- **Headroom**: average frame work comfortably under budget —
  target ≤ ~11 ms of the 16.7 ms frame so a future 16:9 widescreen
  render (~1.33× pixels) still fits without tier-2 concessions.
  Report the measured margin, whatever it is.
- All three gates green; every optimization proven parity-safe with
  its forced-on G3 run; per-change perf numbers in commit messages.

## ISSUE-3 — [ROUTINE, RECURRING] gameplay coverage merges

Castle interior, painting levels, bosses load overlays not yet
captured → tier-3 returns there. The ~15-min routine (monotonic,
gate-checked): play `--interactive --rom --discover-static-misses`
into the new area → run both capture tools with `--skip-nav`
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
- Game repo `ndsrecomp.pin` is one commit behind framework HEAD —
  routine re-pin post-gates (part of ISSUE-1 step 4).

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
