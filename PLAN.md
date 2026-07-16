# ndsrecomp — Plan & Status

Single source of truth for the roadmap and where we are. Detailed
findings live in `docs/`; this file ties them together. Discipline lives
in `CLAUDE.md` / `PRINCIPLES.md` / `DEBUG.md`.

## Mission

Static recompiler for the Nintendo DS. **Milestone:** boot the ARM7+ARM9
BIOSes and the Nintendo **firmware menu** to an *interactive* state
(mouse = touchscreen on the bottom screen; menu/settings respond),
visually + in-memory flawless vs the **melonDS** oracle. No commercial
game until that gate is met.

## Locked decisions

- **Two CPUs, one scheduler.** ARM9 (ARMv5TE, ~67 MHz) + ARM7 (ARMv4T =
  the GBA core, ~33 MHz), sharing 4 MB main RAM, handshaking via IPC
  FIFO/IPCSYNC. Interleave on one event scheduler; resync at every
  cross-CPU boundary.
- **Recompiler is C++**, cloned from `gbarecomp/src/armv4t`, extended
  with ARMv5TE + CP15 for ARM9.
- **LLE boot.** The recompiled ARM7 BIOS does the real firmware header
  parse + part decompress/copy. Offline decompression is NOT a boot
  prerequisite.
- **Three-tier dispatch** (psxrecomp model — see
  `docs/dispatch_architecture.md`): Tier 1 recompiled native banks →
  Tier 2 dirty-RAM JIT shard (deferred) → Tier 3 dirty-RAM ARM
  interpreter (the correctness floor running the firmware's own copied
  bytes). The firmware menu runs interpreted at first, promoted to banks
  later.
- **Host:** SDL2, single window stacked 256×384 (engine A top / B
  bottom), software rendering; mouse → ARM7 TSC touch.

## Inputs (verified, in `bios/`, git-ignored)

ARM9 BIOS `bfaac75f…` @0xFFFF0000 · ARM7 BIOS `24f67bde…` @0x0 ·
firmware `ae22de59…` (retail, MACP). GBA BIOS kept for later.

## Roadmap & status

### Phase 0 — scaffold + ground truth ✅ DONE
Repo skeleton, discipline docs, `bios/` verified. Firmware header decoded
(`tools/fw_inspect.py`, `docs/firmware_boot.md`): ARM9 boot → RAM
0x021F0000, ARM7 boot → 0x0380CC00; decode confirmed vs DeSmuME.
Oracle infra scaffolded (`oracle/`, `TCP.md`) — melonDS build deferred to
Phase 2 (nothing to diff until a runtime exists; task #8).

### Phase 0.5 — BIOS analysis + symbol import ✅ DONE
- Ghidra (MCP on port 2222): both BIOSes analyzed; boot / SWI-dispatch /
  SWI-table labeled (`docs/bios_analysis.md`).
- Symbols: PikalaxALT/ndsbios reassembles SHA-1-identical to our dumps
  (`make compare` OK). `tools/import_bios_symbols.py` →
  `bios/biosnds7.toml` (145 named funcs) + `bios/biosnds9.toml` (39) with
  ARM/Thumb modes + SWI jump tables. These are the recompiler's
  authoritative per-BIOS config.

### Phase 1 — ARM core port ✅ DONE
Recompiler builds + runs on the real BIOSes; `--audit` gives the
execution-driven gap list (`docs/phase1_audit.md`). All NEXT items done:
1. **MCR/MRC/CDP** decode → IR → codegen (`runtime_coproc_*`); ARM9
   discovery un-truncated.
2. **ARMv5TE completeness** — BLX(reg+imm), CLZ, QADD/QSUB/QDADD/QDSUB
   (CPSR.Q), the signed-multiply family, LDRD/STRD, PLD. The saturating +
   signed-mul ops had been *silently* mis-decoding as DP ops — now
   intercepted. Test: `recompiler/tests/armv5te_decode_test.cpp`.
3. **C bank emission** (`--out --bank`) → `generated/<bank>.{c,h}` +
   dispatch table; both BIOS banks emit + compile clean as C11.
4. Both BIOS audits: **zero codegen gaps**.
   (CP15 *model* landed in Phase 2, where the bus consumes it.)

### Phases 2-6 — dual-CPU runtime through the firmware menu ✅ DONE
The runner links the generated banks on a full DS runtime
(`docs/runner_bringup.md`, `docs/scheduler_design.md`): dual-CPU
melonDS-faithful scheduler, CP15/TCM, complete bus, IPC, IRQ/timers/DMA,
SPI (flash/TSC/power), RTC, 2D engines A+B, SPU with capture, Wi-Fi
power/beacon surface, SDL host (stacked screens, mouse→TSC, native-cadence
audio). All eight firmware scenarios execute from provenance-validated
firmware banks, byte-identical to melonDS per VBlank including audio
(`docs/accuracy_burndown.md`, `docs/firmware_release_evidence.json`).
The host audio/input soak gate passed 2026-07-16 (prebuffered start +
glide-floor pacing; `NDS_FRONTEND_REQUIRE_AUDIO=1`, underruns=0, frame
FNV pair pinned).

### Phase 7 — SM64DS bring-up: cartridge, game boot, 3D ✅ DONE (2026-07-16)
Physical-card LLE boot (KEY1, secure area) into the recompiled game banks
(sibling repo `supermario64dsrecomp`: 3,090 ARM9 fns + ARM7 boot closure).
The melonDS 1.0rc GPU3D geometry engine + software rasterizer are vendored
as the runner's 3D device model (`runner/vendor/melonds/`, GPL — see
`THIRD_PARTY_ATTRIBUTION.md`), driven from the scheduler exactly as
melonDS's RunSystem drives it; BG0-as-3D compositing, DISPCAPCNT display
capture, GXFIFO DMA/stall/IRQ, and the AUXSPI backup chip (8 KiB EEPROM)
are implemented. **Gate: SM64DS boot→title→attract runs byte-locked vs a
fresh ndsref through insn9=700M — every event/cycle counter, GXSTAT, and
BOTH framebuffers pixel-identical.** User-confirmed interactively playable
into the castle grounds. Key invariants discovered (enforced in code
comments): melonDS `ARM9Timestamp` is live per-instruction and GXSTAT
reads run the engine against it; the engine's drain state is
Run()-cadence-dependent, so cross-implementation comparison uses the
`gx_run_sample` cadence rings; fine-grained `run_to_event` stepping
perturbs that cadence (compare free-running or coarse-stop runs only).

## Roadmap forward

Priority order set 2026-07-16. Every work item ends at the same regression
floor — the three standing gates — plus its own acceptance test:

- **G1 firmware traversal:** `oracle/firmware_traversal.py` all scenarios
  pass vs fresh ndsref (audio sample-exact).
- **G2 host soak:** 2400-frame `NDS_FRONTEND_REQUIRE_AUDIO=1` run,
  underruns=0, frame FNV pair unchanged (unless explained).
- **G3 SM64DS parity:** boot→title→attract byte-locked vs ndsref at
  insn9 ordinals incl. both framebuffers
  (`oracle/probe_gx_state.py --nav sm64ds-title` + the wide-parity probe).

Delegability tags for farming work out (Codex / parallel agents / future
sessions): **[MECH]** mechanical, fully specified, gates catch mistakes;
**[SPEC]** well-specified device mirroring with melonDS source as the
reference; **[JUDG]** oracle-driven debugging judgment — keep in a
first-divergence-disciplined session.

### WS-A — static coverage promotion (priority 1a)
Measured at SM64DS boot→title (insn9=200M): ARM9 93.5% static / 6.5%
tier-3; ARM7 68.7% / 31.3%. The interpreter is the correctness floor, not
the performance plan.
- **A1 [MECH→SPEC] ARM7 game payload → static banks.** The whole SM64DS
  ARM7 program (sound engine, cart/input services) is copied to RAM at
  boot and runs tier-3 (~18.5M insns by 200M). Pipeline: run with
  `--discover-static-misses` through boot + representative gameplay →
  `tier3_coverage` regions → finder over the RAM image (the game repo's
  bank pipeline; per-title config lives in `supermario64dsrecomp`) →
  regen → gates. Acceptance: `tier3_insns7 ≈ 0` through the G3 window;
  gameplay spot-checks clean. Judgment part: multi-entry/landing-pad
  config for the sound engine's dispatch style.
- **A2 [MECH] ARM9 overlays → static banks.** Same pipeline for the ~6.5%
  copied ARM9 code. Overlays may be region-swapped: bank per overlay ID,
  provenance-validated like the firmware banks.
- **A3 [MECH] 12 extended firmware bank sets.** The menu-interaction
  tier-3 debt (824k/536k insns). The promotion runbook already exists;
  needs `--discover-static-misses` runs per scenario, regen, G1.
- **A4 [MECH] re-pin `supermario64dsrecomp`** to the current framework
  HEAD once A1/A2 land (it pins 87f7ad4, many commits behind).

### WS-B — throughput to a locked 60 FPS (priority 1b)
Boot is ~1.3× realtime (masked by the audio prebuffer); gameplay budget
will tighten with 3D scenes. Measurement discipline is mandatory: the
machine's wall clock varies 2× — interleaved A/B, min-of-N, or the soak's
phase/underrun stats; never single boot-window runs.
- **B1 [MECH] profile first.** `NDS_PROFILE_SCHED`, `NDS_PROFILE_GPU`,
  soak phase stats before/after every change; keep numbers in commits.
- **B2 [SPEC] inline the per-insn counter in emission.** The
  `runtime_insn_fp` call chain is the largest per-instruction constant;
  fold counting/timing into emitted code. Recompiler change → regen all
  (~156 bank TUs) → full gates. High leverage, medium risk.
- **B3 [SPEC] bus fast paths.** Inline main-RAM/WRAM load-store fast
  paths ahead of the general bus dispatch.
- **B4 [MECH] GPU2D remaining passes.** OBJ line pass is next (~2.6s of
  the 2400-frame boot's GPU time); same per-tile treatment as the text-BG
  work (commit 984ac08 as the template). FNV pins the output.
- **B5 [JUDG] scheduler round overhead.** Round-granular costs (per-round
  Run(), timer catch-up) if B1 shows them material.
- Acceptance: sustained 60 FPS interactive through title+attract with
  underruns=0 at normal process priority; boot ≤1× realtime.

### WS-C — in-game audio validation (priority 2)
- **C1 [MECH harness / JUDG divergences] gameplay audio traversal.**
  Extend the traversal manifest pattern with scripted input scenarios
  (file-select → castle grounds …) comparing `audio_samples` sample-exact
  vs ndsref at matched ordinals, like the firmware scenarios.
- **C2 [SPEC] SPU capture completion.** SNDCAPxCNT source-select/add-mode
  bits are stored but inert (`spu.cpp` set_cnt vs run/flush); SM64DS-era
  games route reverb/echo through capture. Mirror melonDS SPU.cpp.
- **C3 [MECH] gameplay host soak** with music+SFX heavy scenes.

### WS-D — correctness backlog (scheduled opportunistically; all gate-safe)
- **D1 [SPEC] 2D windows.** Win0/Win1/OBJ-window/WINOUT masks are
  unimplemented (`CalculateWindowMask` in melonDS GPU2D.cpp is the
  reference; the 3D layer already routes its window bit). First game that
  uses windows will show in G3-style framebuffer diffs.
- **D2 [SPEC] main-memory display FIFO.** 0x04000068 feed + DMA start
  mode 4 + FIFO as capture source B (currently zero-filled).
- **D3 [SPEC] save persistence + chip types.** Host `.sav` read/write
  (melonDS `Platform::WriteNDSSave` analog), and per-game chip selection
  (vendor melonDS ROMList or per-game TOML). Today: fixed 8 KiB EEPROM,
  RAM-only.
- **D4 [MECH] small fidelity items.** Engine-B 0x04001064 latch vs
  melonDS drop; io.cpp pre-existing -Wnarrowing/init warnings; TCP.md
  full 30+ command reference.
- **D5 [JUDG] GXFIFO stall stress.** The stall path (CPUStop_GXStall
  analog) is implemented but SM64DS's title window never fills the FIFO;
  validate against a GXFIFO-heavy title before trusting it.

### WS-E — enhancements (priority 3; strictly opt-in, parity default)
All enhancements live behind flags, default off; gates always run in
parity mode. Enhancement builds must never alter guest-visible state.
- **E1 [SPEC] widescreen 3D.** Wider aspect via GPU3D projection/viewport
  adjustment at the device-model boundary (16:9 render + 2D letterboxing
  policy). Straightforward in the vendored soft renderer.
- **E2 [JUDG] increased 3D internal resolution.** The vendored soft
  rasterizer is 256×192-wired; 2× requires scanline-width generalization
  (melonDS's GL renderer is the alternative but drags in a GL stack and
  the compositor split). Scope carefully before committing.
- **E3** (later) texture filtering/replacement, MSAA-style edge smoothing.

### WS-F — host shell / UX (priority 3, parallel-friendly)
- **F1 [MECH] detached top/bottom screen windows.** SDL multi-window with
  per-window scale/rotation and layouts (stacked / side-by-side / focus
  one screen). Host-only; FNV + soak gate it.
- **F2 [MECH] input rebinding + config file;** touch-on-gamepad mapping.
- **F3** (later) save states — `Savestate.h` is already vendored for the
  3D engine; whole-runner serialization is its own project.

### WS-G — real-hardware validation loop (as needed)
A DS + flash carts are available for homebrew probes over the network.
Reserve for cases where melonDS itself is the suspect (its GPU2D/3D
sources carry explicit `TODO/checkme` items we inherited — e.g. capture
alpha=0 semantics, RDLINES 0x320 constant). Build the libnds probe
harness the first time one of these matters; ring-buffer discipline
applies there too.

### WS-H — release hygiene (before any public artifact)
GPL-3.0-or-later posture for the runner binary (already documented in
`THIRD_PARTY_ATTRIBUTION.md`), license texts in-tree, README refresh,
and a reproducible release-evidence run (G1-G3 outputs archived like
`docs/firmware_release_evidence.json`).

## Build / run

```
# recompiler (mingw64 g++ 15.2 + ninja on PATH)
cmake -G Ninja -B recompiler/build recompiler && cmake --build recompiler/build
recompiler/build/nds_recompile --config bios/biosnds7.toml --bin bios/biosnds7.rom --audit
recompiler/build/nds_recompile --config bios/biosnds9.toml --bin bios/biosnds9.rom --audit

# BIOS symbol re-import (after ndsbios changes)
make -C third_party/ndsbios all && python tools/import_bios_symbols.py

# firmware header
python tools/fw_inspect.py bios/firmware.bin
```

## Open / deferred

- **task #8** — build the melonDS oracle + author its TCP patch
  (`oracle/`); deferred to Phase 2 (needs a runtime to diff against).
- Ghidra MCP (port 2222) requires a program open in CodeBrowser to expose
  its tools (see `memory/ghidra-mcp-setup.md`).

## Doc map

`docs/firmware_boot.md` · `docs/bios_analysis.md` ·
`docs/dispatch_architecture.md` · `docs/phase1_audit.md` ·
`oracle/README.md` · `ghidra/README.md` · `TCP.md`.
