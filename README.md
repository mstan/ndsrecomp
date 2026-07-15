# ndsrecomp

> ## Status: very early pre-alpha (v0.0.1)
>
> This is an experimental developer snapshot, not a ready-to-use emulator or
> a stable framework. It has demonstrated one specific, hash-verified Nintendo
> DS firmware path through the Health & Safety screen to an interactive menu,
> but it has no commercial game target, no compatibility promise, no stable
> API, and no turnkey clean-clone runner build yet. Internals and instructions
> may change without notice.

ndsrecomp is a static recompiler for the **Nintendo DS**. In the same family
as `nesrecomp`, `snesrecomp`, `psxrecomp`, `segagenesisrecomp`, and
`gbarecomp`, it lifts guest ARM code to C ahead of time and runs that code
natively rather than interpreting the immutable banks.

This is **not** a general-purpose emulator. A thin runtime supplies the memory
bus, two-CPU scheduling, hardware models, and optional SDL presentation. Code
copied into RAM by the guest currently uses a bounded interpreter tier.

## Current demonstrated target

There is no commercial game in scope. The current milestone is the original
DS firmware menu: boot through the ARM7 and ARM9 BIOSes, pass the Health &
Safety screen, reach the main menu, and interact with it through mouse-driven
touch input. The retained release evidence covers only the tested dump hashes
and scripted paths; it is not a general firmware-compatibility claim.

Current source release: **[v0.0.1](https://github.com/mstan/ndsrecomp/releases/tag/v0.0.1)**.

## What currently works

- ARM7TDMI (ARMv4T) and ARM946E-S (ARMv5TE) decode and C emission.
- A dual-CPU, event-aligned scheduler and the hardware paths exercised by the
  tested firmware-menu traversal.
- Interactive SDL video, touch, keyboard, and paced stereo audio in the tested
  developer build.
- A separate melonDS-based accuracy oracle and machine-readable traversal
  evidence for differential testing.

Important limitations:

- No DS game is supported or currently targeted.
- The checked-in tree intentionally omits generated recompiled banks, because
  they contain code derived from user-provided Nintendo dumps.
- Building the full firmware-menu runner requires local RAM captures and a
  developer-oriented capture/regeneration workflow that is not yet turnkey
  from a clean clone.
- Documentation outside this README is primarily internal development material.

## The DS is two CPUs

| | core | ISA | clock | role |
|---|---|---|---|---|
| **ARM9** | ARM946E-S | ARMv5TE | ~67 MHz | main; caches, MPU, TCM, CP15; runs the menu GUI |
| **ARM7** | ARM7TDMI | ARMv4T | ~33 MHz | sub; touch/SPI, sound, RTC, Wi-Fi |

They share 4 MB of main RAM and communicate through IPC FIFO and IPCSYNC. The
runtime interleaves both CPUs on one event scheduler.

ARM7 uses the same core family as the GBA, so the portable ARM core began as a
port of the sibling `gbarecomp/src/armv4t` implementation. ARM9 adds the
ARMv5TE instructions and CP15 system-control behavior. See
[`THIRD_PARTY_ATTRIBUTION.md`](THIRD_PARTY_ATTRIBUTION.md) for provenance and
licensing boundaries.

## User-provided inputs

You must dump these files from hardware you own and place them in `bios/`.
They are hash-verified at load and are ignored by Git.

| file | SHA-1 | role |
|---|---|---|
| `biosnds9.rom` | `bfaac75f101c135e32e2aaf541de6b1be4c8c62d` | ARM9 BIOS (4 KB, maps at `0xFFFF0000`) |
| `biosnds7.rom` | `24f67bdea115a2c847c8813a262502ee1607b7df` | ARM7 BIOS (16 KB, maps at `0x00000000`) |
| `firmware.bin` | `ae22de59fbf3f35ccfbeacaeba6fa87ac5e7b14b` | 256 KB flash image used by the demonstrated path |

`BIOSGBA.ROM` is reserved for possible future GBA-mode work and is out of
scope. More detail is in [`bios/README.md`](bios/README.md).

## Oracle

The accuracy oracle is a separately built process based on
[melonDS](https://github.com/melonDS-emu/melonDS), pinned to tag `1.0rc` for
the retained evidence. It is optional and never links into the native runner.
DeSmuME is used as a secondary behavioral cross-check. See
[`oracle/README.md`](oracle/README.md) and
[`THIRD_PARTY_ATTRIBUTION.md`](THIRD_PARTY_ATTRIBUTION.md).

## Layout

```text
recompiler/   ARM/Thumb/ARMv5 decode -> IR -> C codegen; function finder
runner/       dual-CPU scheduler, bus, CP15, I/O, 2D engines, SDL host
generated/    local recompiled banks and dispatch tables (ignored)
bios/         user-provided dumps (ignored) and tracked address configs
oracle/       separate melonDS oracle glue, patches, and comparison tools
tools/        capture, inspection, symbol, and release-verification tools
docs/         internal accuracy, architecture, and bring-up notes
```

## Build from source

The recompiler and its current tests build from a clean clone with CMake 3.20+
and a C++20 compiler. Ninja is used below:

```sh
cmake -G Ninja -B recompiler/build recompiler
cmake --build recompiler/build
./recompiler/build/armv5te_decode_test
./recompiler/build/interpreter_cycle_test
```

After supplying your own dumps, the two immutable BIOS banks can be emitted
locally:

```sh
./recompiler/build/nds_recompile --config bios/biosnds9.toml \
  --bin bios/biosnds9.rom --out generated --bank arm9_bios
./recompiler/build/nds_recompile --config bios/biosnds7.toml \
  --bin bios/biosnds7.rom --out generated --bank arm7_bios
```

The full runner additionally expects firmware RAM-bank captures matching the
configs under `bios/firmware_banks/`. The current capture and regeneration
tools are `tools/capture_firmware_images.py` and
`tools/export_firmware_bank_configs.py`, but this bootstrap workflow still
assumes an active developer setup. Once every required bank exists under
`generated/`, configure and build the runner:

```sh
cmake -G Ninja -B runner/build runner
cmake --build runner/build
```

SDL2 is optional at configure time; without it, the runner is headless and
interactive presentation is unavailable.

## Copyright and licensing

This repository intentionally contains no Nintendo BIOS, firmware, ROM,
screenshot, save data, generated recompiled code, or binary embedding those
materials. The reachable Git history is intended to be source-only as well.

There is not yet a project-wide license grant. Copyright remains with the
respective authors, and making the source public does not by itself grant
permission to reuse it. Ported components and the optional oracle have their
own terms; see [`THIRD_PARTY_ATTRIBUTION.md`](THIRD_PARTY_ATTRIBUTION.md).

## Development rules

See [`PRINCIPLES.md`](PRINCIPLES.md), [`DEBUG.md`](DEBUG.md), and
[`CLAUDE.md`](CLAUDE.md). These are working documents for active development,
not a stable contributor guide.

---

<p align="center">
  <sub><b>R.A.I.D. — Retro AI Development</b> · a Discord community for AI-assisted retro reverse-engineering, decomp &amp; recomp · <a href="https://discord.gg/Ad9BwSzctP">Join the server</a></sub>
</p>
