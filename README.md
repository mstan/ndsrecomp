# ndsrecomp

Static recompiler for the **Nintendo DS**. In the same family as
`nesrecomp`, `snesrecomp`, `psxrecomp`, `segagenesisrecomp`, and
`gbarecomp` — it lifts the console's real code to C and runs it
natively, rather than interpreting it.

This is **not** an emulator. The DS firmware and BIOS bytes are
decoded ahead of time into C functions; a thin runtime supplies the
memory bus, the two CPUs' scheduling, and the host I/O (SDL).

## Current target: the DS firmware menu + BIOS

There is no commercial game in scope yet. The milestone is to boot
the console exactly as real hardware does — through the ARM7 + ARM9
BIOSes and the Nintendo **firmware menu** (Health & Safety → main
menu) — and reach it **interactively**: the mouse drives the
touchscreen on the bottom display, and the menu responds.

## The DS is two CPUs

This is the structural difference from every prior project in the
family (which are effectively single-CPU):

| | core | ISA | clock | role |
|---|---|---|---|---|
| **ARM9** | ARM946E-S | ARMv5TE | ~67 MHz | main; caches, MPU, TCM, CP15; runs the menu GUI |
| **ARM7** | ARM7TDMI | ARMv4T | ~33 MHz | sub; touch/SPI, sound, RTC, wifi |

They share 4 MB of main RAM and hand off through the IPC FIFO /
IPCSYNC. The runtime interleaves both on one event scheduler.

**ARM7 is the same core as the GBA**, so the ARM core is ported from
`gbarecomp/src/armv4t`; ARM9 is that core plus the ARMv5TE delta and
the CP15 system-control coprocessor.

## Inputs (user-provided, hash-verified at load)

Place your own dumps in `bios/`:

| file | sha-1 | role |
|---|---|---|
| `biosnds9.rom` | `bfaac75f101c135e32e2aaf541de6b1be4c8c62d` | ARM9 BIOS (4 KB, maps at `0xFFFF0000`) |
| `biosnds7.rom` | `24f67bdea115a2c847c8813a262502ee1607b7df` | ARM7 BIOS (16 KB, maps at `0x00000000`) |
| `firmware.bin` | `ae22de59fbf3f35ccfbeacaeba6fa87ac5e7b14b` | flash: header + boot parts + **menu** + settings (256 KB) |

(`BIOSGBA.ROM` is kept for future GBA-mode and is out of scope now.)

## Oracle

**melonDS** is the behavioral / cycle oracle; DeSmuME is a secondary
cross-check. Disassembly (GBATEK, no$gba, public RE) is reference for
structure only — never an execution oracle.

## Layout

```
recompiler/   ARM/Thumb/ARMv5 decode -> IR -> C codegen; function finder; config
runner/       dual-CPU scheduler, bus, CP15, I/O, 2D engines, SDL host
generated/    recompiled banks (arm9_bios, arm7_bios, fw_arm9, fw_arm7) + dispatch
bios/         your dumps (git-ignored)
tools/        firmware unpacker, oracle bridge, probes
docs/         firmware-boot notes, memory map, references
```

## Rules

See `PRINCIPLES.md`, `CLAUDE.md`, `DEBUG.md`. In short: no HLE BIOS,
no stubs, never edit generated C (fix the recompiler and regen),
always-on ring buffers (never arm-then-capture), dispatch-miss log
clean every run.
