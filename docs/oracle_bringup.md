# Oracle bring-up (nds_oracle melonDS reference)

The oracle is a headless melonDS frontend (`oracle/shim/`) that boots the real
BIOS + firmware and serves the `TCP.md` debug protocol on `127.0.0.1:19843`, so
the diff harness can sync the native runtime against melonDS on counted
hardware events. melonDS is GPLv3, kept as a separate binary (tool-use).

## Status: shim boots to the firmware menu

The shim builds cleanly out-of-tree against the `1.0rc` melonDS clone
(frontend/OGL/GDB/JIT/LTO off), runs the real BIOS + firmware path, and reaches
the interactive DS firmware menu with the known-good local dumps:

- ARM9 BIOS SHA-1: `bfaac75f101c135e32e2aaf541de6b1be4c8c62d`
- ARM7 BIOS SHA-1: `24f67bdea115a2c847c8813a262502ee1607b7df`
- Firmware SHA-1: `ae22de59fbf3f35ccfbeacaeba6fa87ac5e7b14b`

Verified framebuffer state at `run_to_event vblank9 120` shows the firmware
date/time screen on engine A and the main menu on engine B. Touch, keys, regs,
memory, event counts, and framebuffer commands all answer over TCP.

## Debug protocol notes

- `run_to_event vblank9 N` is exact for this oracle: one VBlank per CPU per
  `RunFrame`.
- `event_counts` tracks VBlank, DMA, timer, IPCSYNC writes, and IPC FIFO sends.
- `io_state` snapshots ARM9/ARM7 `IME`, `IE`, `IF`, `POSTFLG`, `IPCSYNC`,
  `CPUStop`, `NumFrames`, and event counts.
- `read_io` performs exact-width I/O reads. Use it for registers; byte-wise
  `read_mem` is a CPU bus view and can be misleading for registers that only
  implement 16-bit/32-bit access paths.

## Correct interpretation of the apparent ARM7 halt

The previously documented "deadlock" was a false positive. At menu time:

- ARM7 often sits at BIOS `SVC_Halt` (`0x000011C4`) from the firmware wait loop.
- ARM7 `IE/IME` are enabled and VBlank IRQs continue to fire.
- The installed ARM7 IRQ dispatcher at `0x037FD798` clears `IF`, dispatches the
  VBlank handler through the table at `0x03805230`, and that handler sets bit 0
  in the shared IRQ flag word at `0x0380FFF8`.
- The firmware wait helper at `0x037FF124` consumes that bit and clears it before
  waiting for the next event.

Seeing ARM7 parked in `SVC_Halt` with `0x0380FFF8` bit 0 clear after a full
frame is therefore normal idle behavior, not a boot failure.

## Native next steps

The native runner is now the blocker, not the dumps or the oracle setup:

1. Bring the native firmware boot path to the same visible menu state as the
   oracle.
2. Add the native TCP debug server on `127.0.0.1:19842` so
   `oracle/find_first_diverge.py` can compare native vs oracle on hardware
   event counts.
3. Implement the user-facing SDL window(s), input plumbing, and persistent
   firmware save writes only after the native core reaches the oracle menu.

## Build / run quickref

    bash oracle/setup-melonds.sh
    export PATH="/c/msys64/mingw64/bin:$PATH"
    cmake -B oracle/shim/build -S oracle/shim -DMELONDS_DIR=third_party/melonDS -G Ninja
    cmake --build oracle/shim/build --target nds_oracle
    oracle/shim/build/nds_oracle.exe --bios9 bios/biosnds9.rom \
        --bios7 bios/biosnds7.rom --firmware bios/firmware.bin --boot firmware --port 19843
