# Oracle bring-up (nds_oracle melonDS reference)

The oracle is a headless melonDS frontend (`oracle/shim/`) that boots the real
BIOS + firmware and serves the `TCP.md` debug protocol on `127.0.0.1:19843`, so
the diff harness can sync the native runtime against melonDS on counted
hardware events. melonDS is GPLv3, kept as a separate binary (tool-use).

## Status: shim works; melonDS boot-to-menu blocked on these dumps

The shim itself is complete and verified: it builds clean (out-of-tree against
the `1.0rc` clone, frontend/OGL/GDB/JIT/LTO off), boots melonDS in firmware
mode, and answers every protocol command correctly. Confirmed live:

- `run_to_event vblank9 N` is exact (one VBlank per CPU per `RunFrame`).
- `event_counts` — vblank9/7, dma_done, timer_ovf come from the patched
  `NDS::SetIRQ` hook; ipcsync_w / fifo9to7 / fifo7to9 from the `OracleNDS`
  IO-write overrides. (timer_ovf ticks during boot, proving the hook fires.)
- `regs` (both CPUs, with banked SPSR), `read_mem` (per-CPU bus view),
  `read_region` (mainram 4 MB, wram7 64 KB, vram, pal, oam, itcm, dtcm),
  `framebuffer` (256×192 RGB), `touch`, `keys`.

## The blocker: identical IPCSYNC deadlock in melonDS AND the native runner

With `bios/biosnds9.rom`, `bios/biosnds7.rom`, `bios/firmware.bin`, melonDS
does **not** reach the firmware menu. State (stable from frame 1 onward):

| | ARM9 | ARM7 |
|---|---|---|
| PC | `0xFFFF07F4` (BIOS WFI/IntrWait poll loop) | `0x000011C4` (SWI Sleep/Halt: `STRB #0xC0 → HALTCNT 0x04000301`) |
| POSTFLG | 1 (init done) | 0 (early) |
| IE / IF / IME | 0 / 0 / 1 | 0 / 0 / 1 |

- `IPCSYNC9 = IPCSYNC7 = 0`, `ipcsync_w = fifo* = 0` — no handshake ever occurs.
- ARM9 finished init and polls IPCSYNC (R10=`0x04000180`) for the ARM7.
- ARM7 wrote `HALTCNT=0xC0` (sleep) from boot context `LR=0x2E10` and waits for
  an IRQ — but `IE=0`, so nothing (not even the VBlank that does fire every
  frame) can wake it. Permanent circular wait.
- **Load evidence:** the ARM9 boot section partially loads — a jump stub
  appears at `0x021F0000` (`LDR r0,[pc]; BX r0`) and ~320 KB of main RAM is
  written in frame 0 — but the ARM7 section at `0x0380CC00` stays **all zero**.
  So the ARM7 BIOS firmware-boot aborts *after* the ARM9 part, *before* the
  ARM7 part, and falls into IntrWait instead of completing the hand-off.

This matches the native runner's blocker exactly (ARM7 waits on IF bit 18
IPC-recv; ARM9 jumps null on empty boot params; menu parts to `0x021F0000` /
`0x0380CC00` not copied). Two independent emulations failing the same way.

## What was ruled out

melonDS confirms the inputs are valid, so the deadlock is not a "bad BIOS / not
bootable" case:

- `NDS::NeedsDirectBoot()` returns **false** → `Firmware::IsBootable()` true AND
  **both BIOSes CRC32-match the retail dumps** (`ARM7BIOSNative`/`ARM9BIOSNative`
  are exact-CRC checks in `NDS.cpp`). The dumps are genuine retail.
- Post-`Reset()` setup mirrors the qt_sdl frontend and none of it breaks the
  deadlock: `SPI.GetPowerMan()->SetBatteryLevelOkay(true)`,
  `RTC.SetDateTime(2024,1,1,12,0,0)` (fixed for determinism),
  `SetLidClosed(false)` (lid is open by default anyway —
  `KeyInput` bit 23 clear), and `Firmware::UpdateChecksums()`.

## Next moves (for whoever continues)

1. **Isolate dumps vs. setup.** Boot this exact `firmware.bin` + BIOSes in the
   stock melonDS Qt build (the project's oracle reference). If Qt also stalls →
   the firmware's ARM7 boot section is bad/unusual → get a known-good dump. If
   Qt reaches the menu → the headless shim is still missing a setup step the Qt
   frontend does (compare `EmuInstance::createConsole`/`reset` line by line:
   `SetNDSCart(nullptr)`, GBA slot, renderer/GPU init, threading).
2. If the firmware is the problem, that explains the native runner too — the
   firmware-menu gate can't be met until the firmware boot-section load works.
3. Once melonDS reaches the menu, run `oracle/find_first_diverge.py` to localize
   the native-vs-oracle divergence (default `--event-name vblank9`).

## Build / run quickref

    bash oracle/setup-melonds.sh            # clone @1.0rc + apply patches/*.patch
    export PATH="/c/msys64/mingw64/bin:$PATH"
    cmake -B oracle/shim/build -S oracle/shim -DMELONDS_DIR=third_party/melonDS -G Ninja
    cmake --build oracle/shim/build --target nds_oracle
    oracle/shim/build/nds_oracle.exe --bios9 bios/biosnds9.rom \
        --bios7 bios/biosnds7.rom --firmware bios/firmware.bin --boot firmware --port 19843
    # probe:  cd oracle && python -c "import _client; ..."  (see find_first_diverge.py)
