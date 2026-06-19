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
- **Bus so far:** main RAM, shared WRAM, ARM7 WRAM (mirrored), ITCM/DTCM,
  BIOS regions, I/O stub + always-on access ring.
- **NEXT (execution-driven):** both cores are blocked polling **IPCSYNC
  (0x04000180)**, so model the IPC sync/FIFO + POSTFLG + IME/IE/IF
  handshake FIRST, THEN the SPI firmware-flash device for the ARM7 LLE
  firmware boot. Then the dirty-RAM interpreter (Tier 3) for the copied
  firmware menu code, and the melonDS oracle (task #8).
- Still ahead this phase: WRAMCNT split, VRAM banks A–I, palette/OAM, the
  2D engines, the SDL host shell.

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
