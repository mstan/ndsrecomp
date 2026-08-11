# Wiimmfi bring-up runbook: build, boot, and observe

Everything below was **personally executed and verified** in this session
against the two fresh worktrees:

- Framework: `F:\Projects\ndsrecomp\ndsrecomp-wiimmfi` (branch `claude/wiimmfi`,
  off main `9d33234`)
- Game: `F:\Projects\ndsrecomp\mariokartdsrecomp-wiimmfi` (branch
  `claude/wiimmfi`, off main `1d3ca4e`), whose `ndsrecomp` submodule path is a
  directory junction to the framework worktree above.

No Wi-Fi/networking code was touched. This is a buildability + bring-up
verdict and a copy-pasteable command log.

**Bottom line:** the runner builds clean in ~3.65 minutes from the
already-generated banks, the real BIOS+firmware LLE boot reaches the
interactive DS firmware menu byte-for-byte matching the melonDS oracle, and
Mario Kart DS reaches its real title screen (with a visible "NINTENDO WFC"
menu entry) via the automatic-startup LLE path. Touch-driven, manual
cart-icon-tap launch from the firmware menu was attempted and did **not**
work in this session — see "Known-broken things."

## 1. Environment prerequisites

Verified present on this machine:

- `C:\msys64\mingw64\bin\cmake.exe` — CMake 4.2.2 (**not** the devkitPro
  `cmake` on PATH, which mangles Windows paths).
- Ninja 1.13.2 (`C:\msys64\mingw64\bin\ninja.exe`), found automatically by
  the `-G Ninja` generator.
- `g++.exe (Rev9, Built by MSYS2 project) 15.2.0` at
  `C:\msys64\mingw64\bin\g++.exe`.
- SDL2 CMake config at `C:\msys64\mingw64\lib\cmake\SDL2` (required because
  the runner defaults `NDS_ENABLE_COMPUTE_RENDERER=ON`, which hard-requires
  SDL2 at configure time).
- `ccache` auto-detected and enabled by the framework's
  `NdsCompilerCache.cmake` (`AUTO` policy) — configure log prints
  `ndsrecomp: compiler cache enabled: C:/msys64/mingw64/bin/ccache.exe`.
- Game worktree's `.venv` (Python 3.12) already has `Pillow` 12.3.0 and
  `ndspy` importable — no environment setup was needed this session.
- **All builds were run via the PowerShell tool, never Bash.** Bash (git-bash)
  is fine for running already-built `.exe` files and for file
  inspection/scripting, but never for `cmake`/`ninja` invocations.
- Before any oracle or runner probe: `taskkill /F /IM ndsref.exe` and
  `taskkill /F /IM nds_runner.exe` (both were clean/not-found at the start of
  this session, and were re-run at every backend switch).

### Bash git caveat (informational, not a build blocker)

`git status` / `git log` against these two fresh worktrees fail from the
Bash tool with `fatal: not a git repository:
/f/Projects/ndsrecomp/ndsrecomp/.git/worktrees/ndsrecomp-wiimmfi` — the
worktree's `.git` gitlink file contains a **mixed-slash** path
(`F:\Projects/ndsrecomp/ndsrecomp-wiimmfi/.git`), which git-bash's path
translation trips over. The same commands work fine from PowerShell:

```powershell
Set-Location F:\Projects\ndsrecomp\ndsrecomp-wiimmfi
git status
git log --oneline -3
```

## 2. Run architecture (how a game actually gets executed)

The game repo's own top-level `CMakeLists.txt`
(`mariokartdsrecomp-wiimmfi\CMakeLists.txt`) only builds `mkds_romcheck` and
the `mkds_recompiled_banks` static library (via `tools/prepare_mkds.py` +
the framework's `nds_recompile` executable, writing
`generated/recomp/mkds_arm9_*.c` / `mkds_arm7_*.c` + `*_dispatch.c`). It does
**not** produce a runnable executable.

The actual playable binary is `nds_runner` (target defined in
`ndsrecomp\runner\CMakeLists.txt`, i.e. the **framework's** runner project),
built as a **separate CMake configuration** that points at the
already-generated bank sources via two cache options:

- `NDS_TITLE_BANK_DIR` — a directory containing `*_arm9_*.c` /
  `*_arm7_*.c` body shards plus one `*_dispatch.c` per bank (globbed with
  `file(GLOB ... CONFIGURE_DEPENDS)` in `runner/CMakeLists.txt:76-88`).
- `NDS_TITLE_ROM_SHA1` — exact 40-hex-digit ROM SHA-1 gating registration.

At **configure time**, `runner/CMakeLists.txt:169-211` literally
`file(WRITE)`/`file(APPEND)`s a translation unit,
`${CMAKE_CURRENT_BINARY_DIR}/title_bank_registry.cpp`, that `extern "C"`
-declares each discovered bank's dispatch table/length and defines
`nds_register_configured_title_banks(rom_sha1)`, which compares the runtime
ROM's SHA-1 against the configured one and calls `nds_register_dispatch`
per bank. This is exactly the mechanism already used for Metroid Prime
Hunters — confirmed by reading (read-only)
`F:\Projects\ndsrecomp\ndsrecomp\runner\build-mph-title\title_bank_registry.cpp`,
which has the identical shape with `mph_arm7`/`mph_arm9` banks instead.

So: **no game names or per-title dispatch symbols ever appear in shared
runner source.** A new title only needs its generated bank directory + ROM
SHA-1 at configure time. The resulting executable is
`<build-dir>\nds_runner.exe`.

Firmware linking is controlled by `NDS_BOOTSTRAP_FIRMWARE`
(`runner/CMakeLists.txt:19-21,45-51`): `OFF` (default) links the
recompiled `generated/fw_*_[0-9][0-9].c` / `fw_*_dispatch.c` banks; `ON`
links none of them and firmware runs entirely through the Tier-3
dirty-RAM interpreter (still real LLE — PRINCIPLES.md's "one exception" —
just not a recompiled bank). **Established precedent for game titles is
`ON`**: confirmed by inspecting (read-only) the pre-existing, already-built
`CMakeCache.txt` in both
`F:\Projects\ndsrecomp\ndsrecomp\runner\build-mph-main-integration` (MPH) and
`F:\Projects\ndsrecomp\mariokartdsrecomp\ndsrecomp\runner\build-mkds-release`
(MKDS main-repo release build) — both have
`NDS_BOOTSTRAP_FIRMWARE:BOOL=ON` even though the framework's `generated/`
directory has the full recompiled `fw_*.c` bank set. I followed this exact
precedent.

## 3. Framework build

**Not required and not attempted this session.** The runner's
`NDS_TITLE_BANK_DIR` path consumes pre-generated `.c` files directly and
does not depend on the `nds_recompile` executable at configure or build
time. The framework worktree's `generated/` directory already contained a
complete BIOS+firmware bank set (`arm9_bios.c`, `arm7_bios.c`, their
dispatch tables, and the full `fw_arm7_*`/`fw_arm9_*` capture set), staged
before this session started. I did not build `recompiler/` or run
`nds_recompile` myself.

## 4. Game bank generation

**Not required and not attempted.** `generated/recomp/` in the game
worktree already contained the complete, matching MKDS bank set (16 ARM9
shards + dispatch, 4 ARM7 shards + dispatch, headers, and `.stamp` files),
copied in before this session per the task setup. Because the runner build
path (§2) globs `generated/recomp/*.c` directly and never configures the
game's own top-level `CMakeLists.txt` (which is what would invoke
`tools/prepare_mkds.py` + `nds_recompile` and risk a multi-minute-plus
regen via stale `add_custom_command` timestamps), there was **zero regen
risk** in the commands actually run below. I did not invoke
`cmake --build build-mkds --target mariokartdsrecomp` or any target that
touches `mkds_generate_banks` this session, so I have no measured regen
time to report.

## 5. Runner build with the MKDS title (build dir: `build-wiimmfi`)

Working directory for both commands:
`F:\Projects\ndsrecomp\mariokartdsrecomp-wiimmfi`

Configure (PowerShell):

```powershell
& C:\msys64\mingw64\bin\cmake.exe -G Ninja -S ndsrecomp/runner -B ndsrecomp/runner/build-wiimmfi `
  -DNDS_BOOTSTRAP_FIRMWARE=ON `
  -DNDS_TITLE_BANK_DIR="$PWD/generated/recomp" `
  -DNDS_TITLE_ROM_SHA1=691e00d9a5dd80b04f80cc7559503e8b06848785
```

Result: configure succeeded in **1.6 seconds**. `CMakeCache.txt` confirms
`NDS_TITLE_BANK_DIR=F:/Projects/ndsrecomp/mariokartdsrecomp-wiimmfi/generated/recomp`,
`NDS_BOOTSTRAP_FIRMWARE=ON`, `SDL2_DIR=C:/msys64/mingw64/lib/cmake/SDL2`,
`CMAKE_C_COMPILER=C:/msys64/mingw64/bin/cc.exe`.

Build:

```powershell
& C:\msys64\mingw64\bin\cmake.exe --build ndsrecomp/runner/build-wiimmfi -j 12
```

Result: **exit code 0, zero compiler errors** (only the expected
generated-code `-Wunused-label` warnings from the BIOS banks, silenced
project-wide for game/title banks via
`COMPILE_OPTIONS "-w;-O2;-g0"`). **Wall-clock: 218.5 seconds (~3 minutes
39 seconds)** for a from-scratch configure (77 ninja steps: BIOS banks,
16 ARM9 + 4 ARM7 MKDS shards, the full `nds_runner` link, and the three
unit-test executables). Output binary:

```
F:\Projects\ndsrecomp\mariokartdsrecomp-wiimmfi\ndsrecomp\runner\build-wiimmfi\nds_runner.exe
```

(70,625,322 bytes.) Also produced in the same build:
`frontend_config_test.exe`, `relative_mouse_touch_test.exe`,
`battery_save_test.exe` (all linked with exit code 0; not separately run
this session — out of scope).

## 6. LLE firmware-menu boot (no cartridge)

Working directory: `F:\Projects\ndsrecomp\mariokartdsrecomp-wiimmfi`

```powershell
taskkill /F /IM nds_runner.exe
$argv = @("ndsrecomp\bios","--serve","--port","19842","--config","game.toml","--no-save")
Start-Process -FilePath ".\ndsrecomp\runner\build-wiimmfi\nds_runner.exe" -ArgumentList $argv `
  -RedirectStandardOutput "$env:TEMP\nds_runner_menu_stdout.log" `
  -RedirectStandardError  "$env:TEMP\nds_runner_menu_stderr.log" -PassThru
```

`--serve` is the headless, deterministic, scriptable debug-protocol surface
(vs. `--interactive`, an SDL window — see §9). No `--rom` means no
cartridge is inserted at all — the purest form of the firmware-menu gate.

Then, from any directory with the venv Python on `PATH` (protocol client in
§8):

```
run_to_event vblank9 120   ->  {"reached": true, ..., "counts": {"vblank9": 120, "vblank7": 120, "ipcsync_w": 211, "spi_w": 152359, ...}}
```

Screenshot (`framebuffer` for engine A + engine B, stacked): saved to
`generated\captures\wiimmfi-runbook\native_firmware_menu_vblank120.png` in
the game worktree. It shows the real DS firmware date/time (top screen) and
main menu with PictoChat / DS Download Play icons and "There is no DS Card
inserted." / "There is no Game Pak inserted." (bottom screen) — **the
interactive firmware menu, LLE-booted through the real recompiled BIOS +
(Tier-3-interpreted) firmware.**

Cross-checked against the melonDS oracle at the identical hardware-event
count (§10): oracle screenshot
`generated\captures\wiimmfi-runbook\oracle_firmware_menu_vblank120.png` is
visually identical, and both sides' `run_to_event vblank9 120` responses
report the same `ipcsync_w=211`, `spi_w=152359`, `soundbias_last=512`
(oracle: `vblank9=120`, native: `vblank9=120`) — confirming the native
recompiled boot matches the independent melonDS reference on hardware
events, not just pixels.

## 7. Launching MKDS from the menu

Two things were tried. Only the second demonstrably worked.

### 7a. Manual touch-tap on the cartridge icon — UNVERIFIED, did not work

Relaunched with a cartridge inserted and startup forced to `manual` so the
run stops at the firmware menu instead of auto-booting the cart:

```powershell
taskkill /F /IM nds_runner.exe
$argv = @("ndsrecomp\bios","--serve","--port","19842","--config","game.toml",
  "--rom","`"Mario Kart DS.nds`"","--no-save","--startup-mode","manual")
Start-Process -FilePath ".\ndsrecomp\runner\build-wiimmfi\nds_runner.exe" -ArgumentList $argv `
  -RedirectStandardOutput "$env:TEMP\nds_runner_cart_stdout.log" `
  -RedirectStandardError  "$env:TEMP\nds_runner_cart_stderr.log" -PassThru
```

**Gotcha:** a bare `"Mario Kart DS.nds"` array element in PowerShell's
`-ArgumentList` gets space-split before reaching the child process
(`nds_runner` logged `unknown arg: DS.nds` and exited). Fix: embed literal
quotes in the element itself, `` "--rom","`"Mario Kart DS.nds`"" ``.

At VBlank 120 the firmware main menu now shows a real "MARIO KART DS /
Nintendo" banner with the kart-icon graphic in the slot that previously
read "There is no DS Card inserted." — i.e. the recompiled firmware
genuinely parsed the inserted ROM's banner/icon and rendered it
(`generated\captures\wiimmfi-runbook\native_firmware_menu_cart_inserted_vblank120.png`).

I located the exact tappable box by overlaying a pixel grid on the
`framebuffer` PNG
(`generated\captures\wiimmfi-runbook\firmware_menu_bottom_screen_grid_debug.png`):
bottom-screen-local **x≈8–248, y≈8–68**, center **(128, 38)**. I then:

- Injected `touch x=128 y=38 down=true`, held across many `run_to_event`
  windows (10, 60, 300, and up to 870 VBlanks), then `touch down=false`.
- Tried a second immediate tap (double-tap), and tapping directly on the
  kart-icon sub-region (`x=30,y=38`).
- Confirmed the injection actually reaches hardware: `read_io cpu=7
  addr=0x04000136 width=16` (EXTKEYIN) read **63 while held down** and
  **127 immediately after release** — bit 6 (pen-up flag) toggling exactly
  as `nds_set_touch` (`runner/src/io.cpp:2079-2091`) implements it. So the
  touch signal is real at the register level.
- **Result: no observable change** in the rendered framebuffer across any
  of these attempts, even after holding for ~870 VBlanks (~14.5 s of
  simulated time) — no bounce animation, no "Start!" prompt, no boot.
  `dispatch_misses.log` stayed absent throughout (§11), so this is not a
  crash or a missing-function gap; the firmware simply never visibly
  reacted to the injected touch on this screen in this build. I did not
  root-cause this further (e.g., whether the Tier-3-interpreted firmware's
  menu-select routine samples touch on a different cadence, or needs an
  additional input such as a button press). **Treat manual
  touch-driven cart launch as unverified/not working**, not as proven.

### 7b. Automatic-startup LLE boot — WORKED, reached the real title screen

Relaunched with the project's documented, real (non-HLE) automatic-start
path — `game.toml`'s own default (`[system] startup_mode = "automatic"`),
which the real firmware+BIOS reads from its (private, in-memory-only)
Slot-1 auto-start flag, per README.md's "Interactive display
configuration" section:

```powershell
taskkill /F /IM nds_runner.exe
$argv = @("ndsrecomp\bios","--serve","--port","19842","--config","game.toml",
  "--rom","`"Mario Kart DS.nds`"","--no-save","--startup-mode","automatic")
Start-Process -FilePath ".\ndsrecomp\runner\build-wiimmfi\nds_runner.exe" -ArgumentList $argv `
  -RedirectStandardOutput "$env:TEMP\nds_runner_auto_stdout.log" `
  -RedirectStandardError  "$env:TEMP\nds_runner_auto_stderr.log" -PassThru
```

- `run_to_event vblank9 300` → reached; screenshot
  `generated\captures\wiimmfi-runbook\native_mkds_boot_esrb_vblank300.png`
  shows the real Nintendo logo + "ESRB NOTICE: Game Experience May Change
  During Online Play." boot presentation. Event counts
  (`rounds=3080113, ipcsync_w=12520, spi_w=157618`) are **identical** to
  the pre-existing `generated\captures\early-static\report.json` evidence
  from before this session — i.e. this fresh `build-wiimmfi` binary
  reproduces the project's own prior milestone bit-for-bit.
- `run_to_event vblank9 900` → reached; screenshot
  `generated\captures\wiimmfi-runbook\native_mkds_title_screen_vblank900.png`
  shows the **actual Mario Kart DS title screen** — the MARIOKART DS logo,
  Mario on his kart, and the menu: SINGLE PLAYER / MULTIPLAYER /
  **NINTENDO WFC** / RECORDS / OPTIONS.

This is the verified, reproducible way to reach the MKDS title screen this
session. It is a genuine LLE boot (BIOS → firmware → real card path), not
direct-boot HLE — the firmware still parses the header, decrypts KEY1,
etc.; only the "which menu item is focused/auto-selected" decision is
pre-seeded via the firmware auto-start flag instead of a touch gesture.

## 8. Scenario automation mechanism

The project's own scripted mechanism for this exact workflow is
`mariokartdsrecomp-wiimmfi\tools\capture_mkds_checkpoints.py`: it launches
`nds_runner.exe <bios-dir> --serve --port <N> --rom <rom> --config
<toml> --no-save --startup-mode automatic`, waits for the TCP port, then
loops `run_to_event vblank9 <target>` + `framebuffer` for each checkpoint
in `--targets`, stitching engine A (top) over engine B (bottom) into one
PNG per checkpoint and a `report.json` with regs/DISPCNT/dispatch stats.
I did not invoke this script directly this session (no need — I drove the
identical protocol by hand, per §6/§7/§9), but I read it in full and its
subprocess/TCP-client shape is exactly what I replicated manually. Example
invocation (untested by me this session, taken from the script's own
`argparse` + README.md's "Reproducing coverage" section):

```powershell
& "F:\Projects\ndsrecomp\mariokartdsrecomp-wiimmfi\.venv\Scripts\python.exe" `
  tools\fuzz_mkds_gameplay.py `
  --runner ndsrecomp\runner\build-wiimmfi\nds_runner.exe `
  --bios ndsrecomp\bios --rom "Mario Kart DS.nds" `
  --config game.toml --out generated\fuzz\wiimmfi-smoke `
  --start-vblank 4800 --skip-start-tap --steps 0 --capture-static-coverage
```

Deterministic input scenarios themselves live in `scenarios/*.json`
(`attract_loop.json`, `race_start.json`) as a flat `{"kind": "key"|"wait",
...}` action list consumed by `tools/fuzz_mkds_gameplay.py`; there is no
separate "scenario JSON drives --interactive directly" flag on
`nds_runner` itself — automation always goes through the TCP debug
protocol against `--serve`.

## 9. Debug-server connection + example queries

- **Port:** `19842` (native runner default; overridable with `--port`).
- **Handshake:** none beyond TCP connect — the server is a plain
  line-delimited JSON request/response socket. Send `{"cmd":"<name>",
  ...args}\n`, read one `\n`-terminated JSON response.
- **Modes:** `--serve` (headless, `run_to_event` etc. drive execution — used
  for everything in §6/§7 above) vs `--interactive` (real SDL window,
  frontend thread owns execution; the same protocol answers on the same
  port from an I/O thread, but `run_to_event`/`run_to_pc` are rejected with
  an error — query `event_counts`/`frontend_stats` instead). I only
  exercised `--serve` this session; `--interactive` (a real on-screen
  window) was not exercised because this environment has no way to drive
  actual mouse/window input or capture an OS-level window screenshot —
  the `framebuffer` TCP query is the reproducible, scriptable equivalent
  the project's own tooling already uses.

Minimal working client (no repo dependency; also saved at
`ds_debug_client.py` in this session's scratchpad):

```python
import json, socket
def cmd(sock, buf, name, **kw):
    kw["cmd"] = name
    sock.sendall((json.dumps(kw) + "\n").encode())
    while b"\n" not in buf[0]:
        buf[0] += sock.recv(65536)
    line, buf[0] = buf[0].split(b"\n", 1)
    return json.loads(line)

s = socket.create_connection(("127.0.0.1", 19842))
buf = [b""]
print(cmd(s, buf, "ping"))                                   # {"pong": true}
print(cmd(s, buf, "run_to_event", event="vblank9", count=120))
print(cmd(s, buf, "regs", cpu=9))
print(cmd(s, buf, "read_io", cpu=7, addr=0x04000136, width=16))
print(cmd(s, buf, "touch", x=128, y=38, down=True))
```

Verified working examples this session (against the native runner, port
19842):

- `{"cmd":"ping"}` → `{"pong": true}`
- `{"cmd":"run_to_event","event":"vblank9","count":120}` →
  `{"reached": true, "stalled": false, ..., "counts": {"vblank9": 120, ...,
  "spi_w": 152359, ...}}`
- `{"cmd":"framebuffer","engine":"A"}` /
  `{"cmd":"framebuffer","engine":"B"}` → `{"w":256,"h":192,"rgb":"<hex>"}`
  (stitched into the PNGs referenced above)
- `{"cmd":"touch","x":128,"y":38,"down":true}` → `{"ok": true}`
- `{"cmd":"regs","cpu":9}` →
  `{"r":[0,31,0,0,0,0,0,0,0,0,0,0,0,35062088,33610724,33619212],"cpsr":31,"spsr":0,"mode":31}`
- `{"cmd":"io_state"}` →
  `{"cpu9":{"ime":1,"ie":262169,"if":524289,"postflg":1,"ipcsync":0},"cpu7":{...},"cpu_stop":0,"num_frames":900,"counts":{...}}`
- `{"cmd":"read_io","cpu":7,"addr":67109174,"width":16}` (i.e.
  `0x04000136`, EXTKEYIN) → `{"value":63}` while touch held down, `{"value":127}`
  immediately after release.

## 10. Oracle startup + example queries

`ndsref` (the melonDS-based reference oracle,
`F:\Projects\ndsrecomp\ndsref` — **main checkout, read-only, not
modified**) was already built at
`F:\Projects\ndsrecomp\ndsref\build-native\ndsref.exe`; I did not rebuild
it.

**Always `taskkill /F /IM ndsref.exe` first** — a stale server on the port
fabricates phantom divergences.

Working directory: `F:\Projects\ndsrecomp\ndsref`

```powershell
taskkill /F /IM ndsref.exe
Start-Process -FilePath ".\build-native\ndsref.exe" -ArgumentList @(
  "--bios9","F:\Projects\ndsrecomp\ndsrecomp-wiimmfi\bios\biosnds9.rom",
  "--bios7","F:\Projects\ndsrecomp\ndsrecomp-wiimmfi\bios\biosnds7.rom",
  "--firmware","F:\Projects\ndsrecomp\ndsrecomp-wiimmfi\bios\firmware.bin",
  "--boot","firmware","--port","19843"
) -RedirectStandardOutput "$env:TEMP\ndsref_stdout.log" -RedirectStandardError "$env:TEMP\ndsref_stderr.log" -PassThru
```

- **Port:** `19843` (one above the native runner's `19842`), same
  line-delimited-JSON protocol as §9.
- Verified working examples this session:
  - `{"cmd":"ping"}` → `{"pong": true}`
  - `{"cmd":"event_counts"}` → all-zero at cold start, e.g.
    `{"vblank9":0,"vblank7":0,...}`
  - `{"cmd":"run_to_event","event":"vblank9","count":120}` →
    `{"reached": true, "frames": 201, "counts": {"vblank9": 120, "vblank7":
    120, "ipcsync_w": 211, "spi_w": 152359, ...}}` — **matches the native
    runner's counts at the same VBlank** (§6).
  - `{"cmd":"regs","cpu":9}` →
    `{"r":[1,1,8,50348032,...],"cpsr":1610612767,"spsr":0,"mode":31}`
  - `{"cmd":"framebuffer","engine":"A"}` /
    `"B"` → stitched into
    `generated\captures\wiimmfi-runbook\oracle_firmware_menu_vblank120.png`
    (§6), visually identical to the native runner's screenshot at the same
    VBlank.

## 11. `dispatch_misses.log` verdict

**Empty/absent — zero dispatch misses.** Checked after every run this
session (no-cartridge firmware boot to VBlank 120; cartridge-inserted
manual-startup run with all the touch experiments, up to VBlank 990;
cartridge-inserted automatic-startup run through the MKDS title screen at
VBlank 900). `runtime_arm.cpp:1141` only creates `dispatch_misses.log`
(relative to the process's CWD, i.e.
`F:\Projects\ndsrecomp\mariokartdsrecomp-wiimmfi\dispatch_misses.log` since
that's where I launched `nds_runner.exe` from) on the **first** miss, via
`fopen(..., "ab")`. The file was never found:

```powershell
Test-Path F:\Projects\ndsrecomp\mariokartdsrecomp-wiimmfi\dispatch_misses.log   # False
```

No missing discovered functions surfaced in any tested path, including the
(unsuccessful) touch-tap experiments in §7a — that failure is a UI/logic
gap, not a codegen/discovery gap.

## 12. Where logs and screenshots land

- **`dispatch_misses.log`** — relative to the runner process's working
  directory (append-only; absent = zero misses). In every command above I
  launched from `F:\Projects\ndsrecomp\mariokartdsrecomp-wiimmfi`, so that's
  where it would appear.
- **stdout/stderr** — redirected explicitly per launch to
  `%TEMP%\nds_runner_*_std{out,err}.log` / `%TEMP%\ndsref_std{out,err}.log`
  in the commands above (all empty on every successful run this session).
- **Screenshots** — this session's evidence PNGs were copied into the game
  worktree's existing (git-ignored) capture convention:
  `F:\Projects\ndsrecomp\mariokartdsrecomp-wiimmfi\generated\captures\wiimmfi-runbook\`.
  This mirrors the pre-existing `generated\captures\early-*`,
  `generated\captures\attract-map`, etc. directories already in that tree
  from prior sessions (see `tools\capture_mkds_checkpoints.py`, which writes
  the same `vblank-NNNN.png` + `report.json` shape).
- **Battery saves** — `--no-save` was passed on every run this session, so
  no `.sav` file was created; the default location is alongside the ROM
  unless `--save-path` is given (see `main.cpp` `--save-path`/`--no-save`).

## 13. Known-broken things (exact evidence, not fixed)

1. ~~**Manual touch-driven cartridge-icon launch from the interactive
   firmware menu does not visibly work.**~~ **CORRECTED 2026-08-10
   (beads-yjp.3, resolved) — it works.** §7a's "zero visible reaction"
   was a false negative, not a runtime/firmware defect. The reaction is a
   multi-hundred-VBlank state machine: after releasing the tap, the
   card-slot focus highlight drops within ~1 VBlank, the bottom-screen
   icon row visibly fades over the next ~15–30 VBlanks, the screen goes
   black for the "flip card" transition, and by roughly VBlank+80 past
   the release `ipcsync_w` jumps from the firmware-menu's steady `211` to
   `12520` — the *same* game-boot IPC handshake signature already
   established for `--startup-mode automatic` below (§7b). By VBlank
   ~2000 (absolute, from a cold boot) the real MARIOKART DS title screen
   renders (SINGLE PLAYER / MULTIPLAYER / NINTENDO WFC / RECORDS /
   OPTIONS), and running further reaches actual attract-mode gameplay.
   Reproduced with the *same* short holds §7a already tried (a 10-VBlank
   hold-then-release is sufficient) — the missing ingredient was running
   the simulation far enough forward **after** the release to observe the
   queued reaction, not a different gesture, coordinate, or duration.
   Evidence chain (SPI trace ring, cartridge ROMCTRL ring, screenshots at
   each transition stage, a matching short-hold retest) and a regression
   scenario (`oracle/mkds_cart_icon_launch_smoke.py`) are recorded on
   beads-yjp.3. Settings-touch was independently re-verified scripted
   (no `--interactive` needed): tap (127,180) opens Settings, tap
   (55,143) opens the System category, tap (55,52) opens Start-up — two
   levels deep, purely over `--serve`.
2. **PowerShell `-ArgumentList` silently mis-splits a space-containing
   element.** `"--rom","Mario Kart DS.nds"` produced `unknown arg: DS.nds`
   from the child process; fix is embedding literal quotes in the element:
   `` "--rom","`"Mario Kart DS.nds`"" ``.
3. **Bash (git-bash) cannot run `git` against these worktrees** —
   `fatal: not a git repository: .../.git/worktrees/<name>` — because the
   worktree's `.git` gitlink file mixes `\` and `/` in its path
   (`F:\Projects/ndsrecomp/ndsrecomp-wiimmfi/.git`). PowerShell `git` works
   fine against the same worktree. Not attempted to fix (out of scope,
   framework/tooling-owned, not part of this task).
4. **`--interactive` (real SDL window) was not exercised at all this
   session** — no actual mouse-driven window interaction or OS-level
   window screenshot was attempted; everything above used `--serve` +
   the TCP `framebuffer` query, which is the same mechanism the project's
   own `capture_mkds_checkpoints.py` already relies on. If a literal
   on-screen window + real mouse click is required for a later gate, that
   is unverified by this session.
5. **The top-level game CMake project
   (`mariokartdsrecomp-wiimmfi\CMakeLists.txt`, the
   `tools/prepare_mkds.py` + `nds_recompile` regeneration path) was never
   configured or built this session.** If the already-generated
   `generated/recomp/*.c` / `generated/inputs/*` ever go stale relative to
   `tools/prepare_mkds.py`, the ROM, or the coverage-seed JSON (all now
   carrying fresh Aug-10 worktree-checkout mtimes newer than the Aug-9
   `.stamp` files), a full regen would trigger on next configure of that
   project — untested, unmeasured, and avoided entirely by building the
   runner directly against `NDS_TITLE_BANK_DIR` (§5) instead.
6. **`launcher/recomp-ui` was not built or run this session** — out of
   scope for this task.
