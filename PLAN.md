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

### Phase 2 — dual-CPU runtime ⟳ IN PROGRESS

**Current convergence checkpoint (2026-07-14):** the historical IPCSYNC
blocker is resolved. LLE firmware execution is exact against melonDS through
ARM9 instruction 120,000,000 / ARM7 instruction 53,068,580 (VBlank 1,127),
including CPU cycles and all shared event counters. RTC progression and the
full SPU mixer/sound-capture path exercised by firmware are implemented; the
first 4 KiB capture buffer is byte-identical. WRAMCNT, VRAM A-I, palette, OAM,
and both published RGB framebuffers are also byte-identical at that ruler; the
top-screen proof includes an animated affine-OBJ frame and the exact
VBlank-to-frame-finish front/back lifecycle. See
`docs/accuracy_burndown.md` for the live evidence and remaining axes.
All editable Settings pages and their save paths are now deterministic
every-frame scenarios. PictoChat is likewise exact through lobby entry,
joining Room A, typed and drawn messages, room exit, Quit cancellation, and
console power-off; its exercised WiFi power/beacon/TX behavior and terminal
GPU/SPU stop boundary match melonDS.
Download Play, the main-menu brightness/clock controls, both empty cartridge
targets, and their cancel/terminal paths are also covered by full per-VBlank
scenarios. This closes the navigable firmware surface for the configured
no-cartridge state; the remaining release work is static provenance/banking,
continuous host audio/SDL integration, unexercised hardware-mode closure, and
performance/determinism gates.
The runner (`runner/`) links the generated banks on a DS runtime
(`docs/runner_bringup.md`). **Done:**
- **M1 — ARM9 boots:** SHA-1-verify the 3 dumps, run the recompiled ARM9
  BIOS from reset through CP15/TCM setup + hardware init to the ARM7-wait
  idle. Zero gaps. CP15 model (control / ITCM / DTCM / MPU) in
  `runner/src/cp15.cpp`; the bus honors TCM placement.
- **M2 — dual-CPU:** both banks link (bank-prefixed symbols), scheduler
  interleaves two `ArmCpuState` (ARM9 ~2× ARM7), 7434 rounds, **zero
  dispatch misses**. Cooperative preemption (preempt only at backward
  branches; preserve + per-CPU save/restore the call-return stack; finder
  seeds call-return addresses) — the hard part, solved.
- **Bus so far:** main RAM, accurate shared/ARM7 WRAM ownership, ITCM/DTCM,
  BIOS regions, physical VRAM A-I, palette/OAM, and always-on access rings.
- **NEXT (execution-driven):** complete the unexercised 2D modes and
  per-scanline state lifecycle, then add deterministic touch/key scripting so
  the first-divergence ruler can traverse every firmware page. Profile and
  flatten renderer VRAM access: the first exact 1,208-frame soak is only
  ~26 FPS and does not meet the release performance gate. In parallel, promote
  validated firmware code to static ARM9/ARM7 banks and harden Tier 3 with
  dirty-page provenance; do not begin game work before the menu release gate.
- Still ahead this phase: complete 2D coverage, SDL video/audio/input,
  scripted menu traversal, static firmware promotion, and release soak.

### Phase 3 — recompile BIOSes + firmware as banks, LLE boot
Recompile both BIOSes + firmware ARM9/ARM7 parts as banks. LLE the BIOS
firmware-boot copy path.

### Phase 4 — minimum hardware for the menu
IPC FIFO/SYNC, both IRQ controllers, timers, DMA. SPI: firmware flash,
touchscreen TSC, power mgmt. RTC. 2D engines A&B (text/affine/extended
BG + OAM) software-rendered to two 256×192 framebuffers via VRAM bank
mapping + vblank/hblank.

### Phase 5+6 — host shell + reach/verify the menu
SDL2 stacked window, mouse→TSC, keyboard→KEYINPUT. Boot to Health&Safety
→ main menu, touch navigates settings. Frame-diff vs melonDS; dispatch
miss log clean.

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
