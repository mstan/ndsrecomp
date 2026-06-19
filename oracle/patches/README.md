# oracle/patches/ — melonDS nds_oracle patch

`*.patch` files here are applied by `../setup-melonds.sh` after the
melonDS clone. They add an `nds_oracle` frontend implementing the
`../../TCP.md` protocol. They are authored **against the actual melonDS
source** (pinned tag `1.0RC`) — not blind — so they are added once the
clone exists, exactly as `gbarecomp/oracle/patches` were authored
against mGBA. This README records the hook points so the patch is a
mechanical write against known APIs, not guesswork.

## Hook points (melonDS, tag 1.0RC)

- **Boot**: construct the core, set `bios9`/`bios7`/`firmware`, boot in
  **firmware mode** (we want the menu, not direct cart boot). Reference:
  `NDS::Reset()` + the firmware-boot path that runs the real BIOS from
  reset (do NOT call the direct-boot setup).
- **Stepping**: drive `NDS::RunFrame()` (or finer `NDS::Run` slices) and
  service TCP between slices.
- **Event counters** (the heart of `run_to_event`): increment on
  - VBlank IRQ raise per CPU — hook the IRQ-raise for `IRQ_VBlank` in
    `NDS::SetIRQ` for ARM9 vs ARM7,
  - IPCSYNC writes — `NDS::IPCSyncWrite9/7`,
  - IPC FIFO send — `NDS::IPCFIFOSend9/7`,
  - DMA completion — the DMA channel finish path,
  - timer overflow — the timer reload path.
- **State reads**: `read_region` maps to melonDS memory arrays
  (`NDS::MainRAM`, `NDS::ARM7WRAM`, the GPU VRAM banks, `GPU2D` palette,
  OAM). `read_mem cpu=9|7` must go through the **per-CPU bus**
  (`NDS::ARM9Read*` / `NDS::ARM7Read*`) so the view matches hardware.
- **Framebuffer**: `GPU::Framebuffer` for engine A/B → 256×192 RGB.
- **Input**: `touch` → `NDS::TouchScreen(x,y)` / `NDS::ReleaseScreen()`;
  `keys` → `NDS::SetKeyMask`.

## API reality at tag `1.0rc` (clone verified 2026-06-18)

melonDS `1.0rc` is the **instance-based** API, NOT the global `NDS::`
namespace this doc originally assumed. The hook methods all exist — they
are just members of `class NDS` (in `namespace melonDS`, `src/NDS.h`):

- `class NDS { u8* MainRAM; u32 MainRAMMask; u8* ARM7WRAM; ... }` — region
  reads are `nds->MainRAM` / `nds->ARM7WRAM` (+ GPU/VRAM/palette/OAM on the
  GPU members).
- `void NDS::SetIRQ(u32 cpu, u32 irq)` — hook here for per-CPU IRQ event
  counters (VBlank etc.); also `SetIRQ2`.
- `virtual u32 NDS::ARM9Read32(u32)` / `ARM7Read32` — per-CPU bus reads for
  `read_mem cpu=9|7`.
- Construct via `NDSArgs` (`src/NDS.h:52`); see the qt_sdl frontend's
  `EmuInstance`/`ROMManager` for how it wires BIOS9/BIOS7/firmware and boots
  **firmware mode** (do NOT call the direct-boot setup).

Build shape: `src/CMakeLists.txt` builds `add_library(core STATIC ...)`.
The shim is a separate `add_executable(nds_oracle ...)` linking `core`;
configure `-DBUILD_QT_SDL=OFF` (no Qt/SDL) and `-DENABLE_OGLRENDERER=OFF`
(the oracle reads the framebuffer array directly). Core deps are minimal
(zlib; libslirp only for networking, which we don't need).

NOTE: melonDS also ships an in-tree **GDB stub** (`-DENABLE_GDBSTUB=ON`,
default). A GDB-client probe is a lighter alternative for the register/memory
diff, but it lacks the hardware-event counters `TCP.md`'s `run_to_event`
needs — so the custom shim is still preferred for event-synced diffing.

## Status

Clone present (`third_party/melonDS` @ `1.0rc`). The shim sources now live in
`../shim/` (tracked) rather than encoded as patches for new files — patches
here cover only **edits to existing melonDS source** (the `NDS::SetIRQ`
event-counter hook + the root-CMake `BUILD_ORACLE_SHIM` option). Shim authoring
is in progress (`platform_oracle.cpp` + `oracle_hooks.h` done; `nds_oracle.cpp`,
the CMake target, and these patches still to come). The protocol clients
(`../diff_*.py`) are complete and version-independent of this patch.
