# CLAUDE.md — Project Rules

## RULE SOURCE

All rules live in `PRINCIPLES.md` and `DEBUG.md`. This file does not
redefine them; it states the failure modes, the build loop, and the
file boundaries.

## FAILURE MODE — your response is INVALID if you

- guess instead of tracing the writer / scheduler / decoder
- skip the first divergence
- trust unvalidated tool output
- edit generated C to fix a symptom
- stub a BIOS SWI, skip the firmware boot, or hand-wave a "direct
  boot" instead of executing the real BIOS copy path
- make any runtime exec path depend on the ARM interpreter, **except**
  the bounded dirty-RAM interpreter running the guest's own copied
  bytes (see PRINCIPLES.md "The one exception")
- free-run one CPU past a cross-CPU sync point (IPC / shared WRAM /
  observed I/O), or pause/step the two CPUs to "compare" them
- add load-bearing HLE to make the menu work by accident

→ restart from `DEBUG.md`.

## PROJECT OVERVIEW

Static Nintendo DS recompilation. **Two** CPUs:
ARM9 (ARMv5TE) + ARM7 (ARMv4T, the GBA core), sharing main RAM,
interleaved on one event scheduler. Target: boot the BIOSes +
firmware to the **interactive menu** (melonDS oracle).

Fixes belong in:
- the recompiler (`recompiler/`)
- the runtime (`runner/`)
- per-target config (firmware/BIOS `.toml`)

**Never** in `generated/`.

## BUILD LOOP

1. Build the recompiler (`recompiler/`).
2. Run it over the BIOS dumps + firmware parts + config → fresh
   `generated/*.c` (banks: `arm9_bios`, `arm7_bios`, `fw_arm9`,
   `fw_arm7`, + their dispatch tables).
3. Build the runner (`runner/`).
4. Run. The runtime hash-verifies all three dumps and refuses to start
   otherwise.
5. **Check `dispatch_misses.log`** (per CPU). Non-empty = silent
   game-breaking bug: add discovered functions to config, regen,
   rebuild, re-run, until empty.

## FIRMWARE-MENU GATE

No game/cartridge work, no "what happens if I load a ROM" until the
console boots to the **interactive firmware menu**, visually +
in-memory flawless vs the melonDS oracle, with touch (mouse on the
bottom screen) navigating settings. See PRINCIPLES.md "Firmware menu
must be flawless before any game".

## DUAL-CPU RULE

The ARM9 and ARM7 are two cooperating cores on one scheduler. Sync via
**hardware events** (IPCSYNC writes, IPC FIFO send/recv counts, VBlank
IRQ count, DMA completion, timer overflow, BIOS-IRQ-return), never raw
frame numbers. Resync at every cross-CPU boundary. The earliest
divergence is the only one with a root cause.

Early-boot debug priority:
1. ARM7/ARM9 BIOS reset + CPSR/mode/SP setup
2. Firmware header parse + part copy/relocate (LLE)
3. IPCSYNC / IPC FIFO handshake between the cores
4. CP15 setup on ARM9 (TCM/MPU/cache)
5. IRQ vectors + IE/IF/IME (per CPU)
6. DMA + timer programming
7. POWCNT / DISPCNT / engine A+B BG+OBJ initial writes
8. VRAM bank mapping (VRAMCNT_A..I), palette/OAM population

## FILES

Editable: `recompiler/**`, `runner/**`, `tools/**`, `*.toml` config,
`runner/src/main.*`.
Never: `generated/**`.

## OBSERVABILITY

Always-on ring buffers (per-CPU bus accesses, IPC traffic, dispatch
entries), a frame-record ring, and a dispatch-miss log, queried
retroactively over the TCP debug server. Never arm-then-capture; never
pause/step to synchronize observers. See `DEBUG.md`.
