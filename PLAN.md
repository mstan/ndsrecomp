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
- **A1 ✅ DONE (2026-07-16).** The relocated ARM7 runtime code (sound
  engine in shared-WRAM/WRAM at 0x037F8000..0x03805818+, helpers in the
  0x027Cxxxx main-RAM mirror alias) is a content-validated bank
  (`supermario64dsrecomp` config/sm64ds_arm7_ram.toml + tools/
  capture_arm7_ram_bank.py, firmware-capture model, code_copy rows for
  the WRAM window and all main-RAM mirror aliases).
  `tier3_insns7` through the 700M G3 window: 123,815,925 → 135;
  clean_ram_rejects=0; G1/G2/G3 green. Promotion exposed and fixed a
  general +1-cycle arm7_cycle_combine resync bug for ARM-mode
  main-RAM-bus code (found via insn-ring cycle diff — see that commit).
  Remaining: gameplay-window discovery passes (in-level sound paths) can
  merge more entries monotonically via the same tool.
- **A2 ✅ DONE (2026-07-16).** ARM9 runtime code (ITCM-resident fast paths
  at 0x01FF8188.., overlays over/past the static image at 0x0200Fxxx /
  0x02043xxx / 0x020AAxxx / 0x0232xxxx / 0x023FEE00) is a content-validated
  bank (`supermario64dsrecomp` config/sm64ds_arm9_ram.toml + tools/
  capture_arm9_ram_bank.py, fw_arm9 composite model: 32 KiB ITCM high
  mirror + 4 MiB main RAM contiguous at load 0x01FF8000, no code_copy
  needed). Overlay-generation check (checkpoint hashes at every 100M
  insn9): all regions byte-stable 100M..700M except one flip in the
  0x0232xxxx window (~502 tier-3 hits) — a single 700M capture suffices;
  no per-generation banks needed. `tier3_insns9` through the 700M G3
  window: 359,809,505 → 21,274 (entries 21.3M → 4,733); tier3_insns7=135
  unchanged; clean_ram_rejects=0; counters bit-exact across runs and
  binaries. Serve-mode nav+title→700M interleaved A/B min-of-3:
  321.0s → 249.9s (~22%). G1/G2/G3 green. Residual: gameplay-window
  discovery passes merge monotonically via the same tool.
- **A3 ✅ DONE (2026-07-16).** All extended firmware bank sets rebuilt
  from fresh per-scenario LLE captures (traversal --capture-static-dir
  on a discovery server, fresh server pair per scenario — consecutive
  scenarios on one pair desync the audio ordinal streams). Image sha1s
  reproduced the committed identities exactly; entries grew monotonically
  (from-scratch discovery is more complete than the incrementally-grown
  originals). Discovery surfaced ARM7 roots in three scenarios that
  previously had none → 3 NEW configs+banks (fw_arm7_{profile_save,
  system_options_save,shutdown}), now registered: 16 extended sets
  total, all new-emission, gate NDS_HAVE_FW_EXTENDED_BANKS on.
  **G1: all 8 scenarios pass with tier3_insns9=tier3_insns7=0** (was
  824k/536k) and clean_ram_rejects=0 — the interpreter is fully idle
  across the entire firmware surface. G2 FNV pinned, G3 byte-locked.
- **A4 [MECH] re-pin `supermario64dsrecomp`** to the current framework
  HEAD once A1/A2 land (it pins 87f7ad4, many commits behind).

### WS-B — throughput to a locked 60 FPS (priority 1b)
Boot is ~1.3× realtime (masked by the audio prebuffer); gameplay budget
will tighten with 3D scenes. Measurement discipline is mandatory: the
machine's wall clock varies 2× — interleaved A/B, min-of-N, or the soak's
phase/underrun stats; never single boot-window runs.
- **B1 ✅ DONE (2026-07-16).** Profile reporting now prints from every
  exit path (nds_profile_report; it was batch-only, so profiled soaks
  yielded nothing) and the OBJ bucket includes the previously-untimed
  compose_line6 call. Post-B2/B4 profiled soak: GPU2D 6.11s
  (A 4.12 / B 1.99 / OBJ 2.43) per 2400 frames; scheduler round 5.37µs
  (ARM9 2.13, ARM7 1.18, devices 1.68 of which display 1.11).
- **B2 ✅ DONE (2026-07-16).** Per-insn counter inlined in emission
  (`++g_insn_count[g_nds_active]` + armed-gated runtime_insn_slow);
  ~8-9%% emu-phase win on the firmware soak (interleaved A/B). Old
  emission stays ABI-valid; the 12 extended firmware bank sets remain
  old-emission until their captures are regenerated.
- **B3 ✅ DONE (2026-07-16).** Inline bus fast path: static-inline
  bus_read_*/bus_write_* in runtime_arm.h serve main RAM / WRAM / ARM9
  TCM directly (resolve()-exact mapping incl. WRAMCNT split + TCM
  shadowing, bounds-checked; writes replicate written[]/generation/
  static-guard provenance byte-for-byte) while the deep-trace policy is
  off — interactive only; --serve/batch keep the fully-ringed slow path
  bit-identical, `NDS_DEEP_TRACE=0` opts a serve server into fast-path
  mode for equivalence proofs and honest perf A/B. No emission change,
  no regen: every call site (old/new banks, tier-3, devices) routes via
  the header. Proven: G3 byte-lock passes with the fast path FORCED
  (700M, both framebuffers); G1/G2/G3 green in normal modes. Perf (same
  binary, interleaved): game-path serve nav+title→700M default 150.3s →
  fast 129.1s (~14%); firmware soak emu 21.06s → 20.06s min-of-3
  (~4.7%); slow path unregressed (min-of-2 within noise). Follow-up
  candidate: inline runtime_mem_cycles' hot regions (separate
  melonDS-mirroring risk class, not done here).
- **B4 ✅ DONE (2026-07-16).** OBJ line pass per-tile for regular
  tile-mode sprites: GPU2D 8.29s → 6.11s, OBJ 3.61s → 2.43s on the
  profiled soak; FNV/G3 pinned. Affine/bitmap/window stay per-pixel.
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
