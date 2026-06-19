# oracle/shim/ — the `nds_oracle` melonDS reference shim

Headless melonDS frontend that boots the **real** BIOS + firmware to the menu
and serves the `../../TCP.md` debug protocol on `127.0.0.1:19843`, so the diff
harness (`../diff_*.py`, `../find_first_diverge.py`) can sync the native
runtime against melonDS on **hardware events** (never frame indices).

melonDS is GPLv3 and stays a **separate binary** — never linked into the native
runner. This is tool-use, not distribution of melonDS.

## Files

- `platform_oracle.cpp` — headless `melonDS::Platform::*` backing (stdio file
  I/O, `std` threads/mutex/semaphore, time; Net/MP/Camera/Addon/save-writeback
  stubbed). The core ships no Platform impl; every frontend provides its own.
- `oracle_hooks.h` — the always-on hardware-event counters (`OracleCounters`)
  that back `event_counts` / `run_to_event`.
- `nds_oracle.cpp` — arg parsing, dump loading, the `OracleNDS` subclass
  (IO-write overrides counting IPCSYNC/FIFO traffic), the TCP server, and the
  command dispatch. Boots firmware-mode and seeds battery/RTC/lid + firmware
  checksums (the post-`Reset()` setup the qt_sdl frontend does).
- `CMakeLists.txt` — out-of-tree build: pulls the melonDS clone in as a
  subdirectory with frontend/OGL/GDB/JIT/LTO disabled, defines
  `MELONDS_ORACLE_HOOKS`, and links the `core` static lib into `nds_oracle`.

## Build

These sources are tracked here, not in the gitignored melonDS clone. Only one
melonDS edit (the `NDS::SetIRQ` event-counter hook) ships as a patch in
`../patches/`, applied by `../setup-melonds.sh`. Then, from the repo root
(mingw64 g++ + Ninja on PATH):

    bash oracle/setup-melonds.sh
    cmake -B oracle/shim/build -S oracle/shim -DMELONDS_DIR=third_party/melonDS -G Ninja
    cmake --build oracle/shim/build --target nds_oracle

    oracle/shim/build/nds_oracle.exe --bios9 bios/biosnds9.rom \
        --bios7 bios/biosnds7.rom --firmware bios/firmware.bin \
        --boot firmware --port 19843

## Status

**Shim complete and working as a protocol server.** Builds clean; every
`../../TCP.md` command verified against the live oracle (ping / regs / read_mem
/ read_region / event_counts / run_to_event / framebuffer / touch / keys).
`run_to_event vblank9` is exact (one VBlank/CPU per frame); the `SetIRQ` hook
feeds vblank/dma/timer counters and the IO-write overrides feed ipcsync/fifo.

**Open issue — melonDS does not reach the firmware menu with these dumps.**
Both CPUs deadlock in BIOS IntrWait within frame 1: the ARM7 firmware-boot
loads the ARM9 section (code appears at `0x021F0000`) but the ARM7 section
(`0x0380CC00`) never loads, so the ARM7 BIOS aborts mid-load into a sleep/halt
with `IE=0`, and the ARM9 spins polling `IPCSYNC` for a handshake that never
comes. Verified retail BIOSes (melonDS CRC32-confirms both native) and bootable
firmware; battery-okay / RTC / lid-open / `UpdateChecksums()` all set and none
break the deadlock. See `../../docs/oracle_bringup.md`.
