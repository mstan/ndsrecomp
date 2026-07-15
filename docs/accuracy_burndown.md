# Nintendo DS Accuracy Burndown (living document)

Companion to `PLAN.md` and `docs/scheduler_design.md`. `PLAN.md` owns the
product roadmap; this document owns the evidence for every accuracy surface
between the native static recompiler and the vendored melonDS oracle.

Last updated: 2026-07-14.

## Method (binding)

- A subsystem is green only when the native runtime is compared with melonDS
  at the same deterministic hardware or retired-instruction checkpoint.
- Compare the first divergence only. Later differences are consequences until
  the first producing instruction or hardware event is explained.
- CPU registers alone are insufficient. Relevant cycle counts, event counts,
  RAM/device state, pixels, or audio samples must also match.
- Observability is always-on and symmetric. Never pause/step the two CPUs or
  arm a trace after the event of interest.
- Tier 3 may execute guest-written dirty RAM while discovering coverage. It is
  not acceptable as an immutable BIOS/firmware fallback in a release gate.
- No BIOS SWI HLE, direct boot, frame-number synchronization, or generated-bank
  edits are valid fixes.

## Current deterministic ruler

The current boot path is architecturally exact through:

- ARM9 `insn9=120,000,000`
- ARM7 `insn7=53,068,580`
- ARM9 cycles `1,353,203,992`
- ARM7/system cycles `676,601,984`
- VBlank IRQ ordinal 1,127 on both CPUs

At that checkpoint all shared native/oracle event counters agree: IPCSYNC,
both FIFO directions, timer overflow, SPI writes, accepted IRQs, VBlank, the
SOUNDBIAS ramp, and both retired-instruction counts. Native `dma_done` is an
internal counter not yet exposed by the oracle and is therefore not claimed as
a comparative. Both published RGB framebuffers are also byte-identical there:

- top screen SHA-256:
  `ece9895d46099ae5c99878b305d3d20d54260ab5f80cc92d44740ea3cf9d47ea`
- bottom screen SHA-256:
  `040d08b407542e2fd84d74a2e202b38ddd26ca4fbe920a4bfb4d6de93d5e46c6`

The framebuffer proof includes the animated affine-OBJ transition around
ARM9 `insn9=42,080,872`. The native renderer originally published its back
buffer at line 192, 71 scanlines before melonDS's `FinishFrame` swap. With the
front/back lifecycle corrected, the old and newly published frames both match
byte-for-byte while CPU/device counters remain unchanged.

The first sound-capture proof is ARM7 `insn7=32,322,295`:

- Capture registers and both CPU timestamps agree.
- Main RAM `0x02332300..0x023332FF` is byte-identical.
- SHA-256 of that 4 KiB window on both sides:
  `9376bffc96ee1984e686651587b112c8557d89c4389d1b262543288ff408bdee`.
- The former missing sample at `0x0233264E` is `0xFFFD` on both sides.

The continuous-output proof uses a non-destructive, ordinal-addressed S16LE
stereo trace on both debug servers. The `main_menu_controls` cold run compares
328,236 consecutive stereo frames (152,732 non-zero samples), with SHA-256
`0aab2b2ae5d00154b55a5dd335edc0326e79f1d36bc809c7995551e6b1406d2e`.
The independent `calibration_save` cold run compares 656,472 frames (385,418
non-zero samples), with SHA-256
`cfadd01c2afaf7aff89abd6540e115cedb6b67a61c24ddcee137cd7e6ad8c17d`.
Both are byte-identical to melonDS configured for the original DS/DS Lite
10-bit DAC. The trace is independent of SDL consumption and detects missing,
overwritten, short, or out-of-order sample ranges.

The complete eight-scenario cold-process matrix passed twice from clean commit
`4821fd9`. Across the two runs, every scenario's machine-readable traversal
log, continuous-audio frame count/hash, and six static-coverage counters are
identical. All requested frames matched; every static counter is zero. The
retained identity and per-scenario evidence is
`docs/firmware_release_evidence.json`.

The first deterministic interactive ruler is the `calibration_save` scenario
in `oracle/firmware_traversal.json`.  From a cold process it boots the real
BIOS/firmware, enters Settings, completes all four touchscreen-calibration
stages, confirms the result, performs the firmware-flash save, and returns to
the Settings root.  Native and oracle agree on every shared event/cycle/retired
instruction counter and RTC checkpoint.  Both physical 256x192 RGB screens are
byte-identical at every VBlank IRQ ordinal from 933 through 1,120.  The final
frame hashes at VBlank 1,120 are:

- top: `45726356a3b21c52c707a5ecf467fd4f59343a1fb8202b00e385e1eb01f10763`
- bottom: `1eee5b5138b9a78a7d091f5841bf0be686d60fed6cb25920a724bc134fffef55`

Reproduce that interactive ruler with:

```powershell
py -3 oracle\firmware_traversal.py --scenario calibration_save
```

The `date_alarm_save` scenario independently cold-boots, commits a date
change, enables an alarm, and compares both screens at every VBlank across all
editor/save transitions through VBlank 1,200.  Its final RTC status register 2
is `0x04` on both implementations.  This path exposed and now guards a raster
timing edge: the alarm-save transition changes display state partway through a
frame, so the renderer must sample registers/VRAM per scanline at HBlank rather
than redraw the whole frame from its final state.

The `profile_save` scenario independently cold-boots and commits personal
color, birthday, UTF-16 nickname, and message changes.  All four flash-save
paths, both on-screen keyboard animations, shared counters/RTC checkpoints,
and every scanned frame are exact through VBlank 1,640.

The `system_options_save` scenario independently cold-boots and commits
automatic startup, switches the live UI from English to German, and changes
the GBA display-screen preference.  Every transition frame and final device
checkpoint is exact through VBlank 1,460.  Together the four current scenarios
exercise every editable firmware Settings page; shutdown remains a separate
terminal-path gate.

The `shutdown` scenario cold-boots, enters Settings, confirms Quit, compares
every fade frame, and treats console power-off as an expected terminal state.
Both implementations stop at exactly VBlank 693 with ARM9/ARM7 instruction
counts `109,470,869` / `51,905,033`, cycles `867,816,829` / `433,908,415`,
SPI ordinal `265,541`, and identical IRQ/FIFO/timer/audio-ramp counters.  The
terminal screens are byte-identical black frames.  This path also validates
the distinct master-brightness rounding rules used by shutdown fades.

The `pictochat_room_a` scenario independently cold-boots into PictoChat,
compares every VBlank from entry through the lobby and Room A, types and sends
text, records and sends a held multi-point stylus drawing, leaves the room,
cancels Quit once, and then powers off.  The path exercises the ARM7 WiFi
power-state, pre-beacon/beacon-count IRQs, and beacon-transmit completion
model.  Both machines stop at VBlank 1,100 with ARM9/ARM7 instruction counts
`140,959,620` / `78,923,027`, cycles `1,323,726,999` / `661,863,502`, and
matching IPC/FIFO/IRQ/timer/SPI counters.  All four front/back buffers are
cleared at the power-off boundary; both published screens have the all-black
SHA-256 `8123c216413f82bbaa0339c27a43d9822c2a043e20662b27c97874429b996e9a`.

Reproduce the PictoChat ruler with:

```powershell
py -3 oracle\firmware_traversal.py --scenario pictochat_room_a
```

The `download_play_shutdown` scenario scans every VBlank across a sustained
Download Play search, opens and cancels its B-button shutdown dialog, reopens
it, and confirms power-off.  It terminates with exact counters at ARM9/ARM7
instruction counts `151,803,414` / `109,573,682`; the path also proves that
the deliberately different ARM9/ARM7 VBlank IRQ ordinals remain identical to
the oracle instead of being normalized.  The `main_menu_controls` scenario
independently scans four brightness cycles, both clock-display toggles, and
the disabled empty DS-card and Game Pak targets.  Together with the Settings,
PictoChat, and shutdown scenarios, this covers every firmware branch reachable
with the configured empty cartridge slots.

Reproduce the CPU ruler with:

```powershell
python oracle/fp_diverge.py --cpu 9 --known-same 105000000 --max 120000000
python oracle/fp_diverge.py --cpu 7 --known-same 49074405 --max 53068580
```

## Validation infrastructure

- [x] Per-CPU retired-instruction first-divergence bisector.
- [x] Configurable long-run scheduler ceiling; exhaustion is reported
  separately from a true divergence or stall.
- [x] Always-on instruction, IRQ, FIFO, SPI, and bus/watch rings.
- [x] Symmetric register, RAM, I/O, scheduler, CP15, and RTC checkpoints.
- [x] Exact cycle/event counters on both servers.
- [x] Symmetric ordinal-addressed continuous stereo sample diff.
- [ ] Symmetric internal SPU channel/capture-state checkpoint.
- [x] VRAM/palette/OAM state surfaces and dual-framebuffer RGB diff.
- [x] Data-driven touch/key replay with strict stop checks, paired checkpoints,
  and a machine-readable action/result log (`firmware_traversal.py`).
- [x] Extend the manifest across complete Settings, PictoChat Room A, Download
  Play, main-menu brightness/clock controls, empty slots, and terminal/return
  actions reachable with the configured no-cartridge hardware state.
- [x] Automated cold-reset soak proving run-to-run determinism across two
  complete eight-scenario matrices.
- [x] Release/static-coverage manifest tooling with build and image identity.

## Axis 1 - ARM instruction semantics and static BIOS banks

Status: strong on the executed BIOS path, not globally closed.

- [x] ARM7 ARMv4T and ARM9 ARMv5TE BIOS banks execute with matching register
  streams through the current ruler.
- [x] CP15, exception entry, Thumb/ARM interworking, signed loads, multiply,
  and the executed SWI paths are covered by the ruler.
- [x] ARMv5TE decode regression tests exist.
- [ ] Exhaustive instruction/edge-case test ROMs and unexecuted decoder paths.
- [ ] Remove the invalid ARM7 Thumb entry at address zero after proving and
  repairing the producing T-state/control-flow observation.

## Axis 2 - CPU cycles and the dual-CPU scheduler

Status: exact through the current ruler.

- [x] melonDS ARM9 AddCycles combine/refill behavior.
- [x] ARM7 region timing on the exercised memory map.
- [x] One scheduler for both CPUs with event-based rendezvous.
- [x] HALT/WFI and DMA pending-cycle debt, including IRQ wake.
- [x] Exact-instruction HALT observers preserve melonDS's uncommitted debt.
- [x] Continue the ruler beyond 120M ARM9 instructions and across scripted
  input transitions through every no-cartridge firmware branch.
- [ ] Exercise all wait-state regions, cache/TCM transitions, and DMA modes.

## Axis 3 - Memory map and bus ownership

Status: exact on the converged path; remaining bus modes still require
execution-driven coverage.

- [x] Main RAM, BIOS, ITCM/DTCM, shared WRAM, ARM7 WRAM, and current empty GBA
  slot behavior.
- [x] ARM7 device-master bus access for SPU fetch/capture, observable in the
  bus ring independently of the active CPU.
- [x] Accurate WRAMCNT ownership/splitting on both CPU views.
- [x] Physical VRAM banks A-I and CPU/2D-engine mappings.
- [x] Palette RAM and OAM with ARM9 power and byte-write behavior.
- [ ] Display FIFO, remaining mapping modes, open-bus, and remaining MMIO
  semantics.
- [x] Dirty/executable page provenance and generation tracking, including
  alias-aware write invalidation and generation-validated static dispatch.

## Axis 4 - IRQ, IPC, DMA, timers, and system events

Status: exact on the current firmware path, incomplete by mode coverage.

- [x] IPCSYNC and bidirectional IPC FIFO handshake.
- [x] IME/IE/IF behavior and instruction-precise IRQ entry.
- [x] VBlank/HBlank timeline required by the current path.
- [x] Four DMA channels per CPU for the exercised transfer/start modes.
- [x] Four timers per CPU for the exercised prescaler/cascade/IRQ modes.
- [ ] Remaining DMA start modes, repeat/address modes, display triggers, and
  edge cases.
- [ ] Complete display-event and timer edge-case validation.

## Axis 5 - Firmware SPI, RTC, keys, touch, and power

Status: interactive and save-capable across every firmware branch reachable
with the configured no-cartridge hardware state.

- [x] LLE firmware-flash SPI reads, WREN/WRDI status, streaming writes, JEDEC
  ID, mutable contents, and the exercised settings-save transaction stream.
- [x] RTC serial protocol and real-time tick progression on the current path.
- [x] RTC date/time writes; a committed month change and subsequent seconds
  progression match the oracle exactly.
- [x] Host/debug KEYINPUT/EXTKEYIN and touchscreen coordinate injection.
- [x] Basic power-management and touchscreen SPI transactions.
- [x] Deterministic alarm edit/enable/save traversal, including exact RTC alarm
  configuration and every transition frame through VBlank 1,200.
- [x] PictoChat lobby/Room A traversal through text, drawing, room exit, Quit
  cancel, and exact console power-off, including the exercised WiFi timers and
  transmit IRQs.
- [x] Download Play search, cancel/reopen shutdown behavior, all main-menu
  brightness/clock controls, and inert empty DS/GBA slot targets.
- [ ] Long-duration validation of every RTC alarm/periodic IRQ mode and the
  disable/cancel branches.
- [ ] Lid/backlight/power state needed by settings and menu transitions.

## Axis 6 - SPU and audio

Status: internal mixer/capture implemented and capture-validated; SDL output
and bounded pacing are implemented, while stream comparison and soak evidence
remain open.

- [x] Sixteen channels with PCM8, PCM16, ADPCM, PSG, and noise generation.
- [x] Channel timers, looping, volume, panning, and master routing.
- [x] Both sound-capture units with exact 1024-system-cycle scheduling and RAM
  FIFO writes.
- [x] Full 4 KiB capture buffer equality at the first exercised recording.
- [x] Symmetric continuous stereo sample-stream comparator.
- [ ] Symmetric internal channel/capture-state checkpoint.
- [x] SDL stereo device and bounded queue pacing at the native production rate
  (`33,513,982 / 1,024` samples/second), without dropping queued blocks.
- [ ] Objective underrun/overflow soak and audible interactive soak.
- [ ] Validate capture source/addition modes and hardware overflow quirks.

## Axis 7 - 2D graphics and display

Status: pixel-exact and full speed on the exercised firmware paths; not yet
complete by unexercised feature coverage.

- [x] VRAM bank controller and display ownership on the exercised mappings.
- [x] Engines A and B text backgrounds, normal/affine/bitmap OBJ, priority,
  palette, blending, brightness, and front/back publication on the exercised
  path.
- [x] Per-scanline active-display rasterization at HBlank, including a
  mid-frame display-state transition on the alarm-save path.
- [x] Two 256x192 RGB framebuffers exposed symmetrically by both debug servers.
- [ ] Affine/extended/large backgrounds, windows/OBJ-window, exact mosaic,
  display FIFO/capture, 3D layer interaction, and unexercised display modes.
- [ ] Complete HBlank/VBlank edge validation plus unexercised per-scanline
  affine-reference and window behavior.
- [x] Byte/pixel-exact comparison for every frame in the calibration-save
  manifest (VBlank IRQ ordinals 933 through 1,120).
- [x] Extend every-frame comparison across all firmware branches reachable in
  the configured no-cartridge state, including sustained wireless searches
  and terminal fades.
- [x] Profile-driven software-renderer fast paths: flattened direct VRAM,
  palette, and OAM views; one-pass OBJ evaluation; prepared text scanlines;
  mode/blank specialization; and a top-two-layer compositor. Exact mapping
  fallbacks retain the pixel-exact software reference semantics.

## Axis 8 - Static coverage and Tier 3

Status: static and provenance-safe across two deterministic, isolated
all-eight no-cartridge release matrices.

- [x] Native dispatch first, with an opt-in interpreter-assisted discovery
  mode that logs nested immutable targets.
- [x] Guest-byte Tier 3 can hand off back to native banks.
- [x] Tier 3 is restricted to pages proven written/loaded by the guest.
- [x] Structured miss classifications, counts, caller/mode/bytes, run epoch,
  image hashes, and build identity.
- [x] Firmware extraction/capture to ARM9/ARM7 native banks with live-byte
  verification.
- [x] Candidate folding that handles interior aliases without truncating a
  containing function.
- [x] Each of the eight deterministic firmware scenarios has a checkpoint
  proof with zero immutable/static misses, zero invalid targets, and zero
  Tier-3 instructions on both CPUs.

## Axis 9 - SDL host, interaction, and performance

Status: SDL video/input/audio is integrated and the exercised firmware paths
run at full speed; host/audio soak and the combined release matrix remain.

- [x] SDL2 stacked 256x384 window.
- [x] Mouse-to-touch, keyboard-to-buttons, and deterministic input scripting.
- [x] Health-and-Safety, main menu, Settings, PictoChat, Download Play, empty
  slots, and terminal/return paths navigable end to end under deterministic
  debug input.
- [ ] No visible frame mismatch, audible sample mismatch, input fault, or host
  audio/video slowdown against melonDS.
- [x] Sustained native cadence with profiling proving no interpreter hot path.

The authoritative pre-optimization 1,127-frame cold run took 47.403 seconds
(23.77 FPS). After profiling and specializing the renderer, adding direct
flattened views with exact overlap fallbacks, and caching provenance-validated
static dispatch, the same cold run takes 18.185 seconds (61.97 FPS) at normal
process priority. It reaches ARM9/ARM7 instruction counts
`1,353,204,097 / 676,602,048` in 11,417,369 scheduler rounds. The complete
`main_menu_controls` every-frame oracle still passes after these changes, with
all six Tier-3/clean-RAM rejection counters at zero.

## Release gate: ready for a game project

All of the following must be proven together on a cold-reset scripted run:

- Full LLE BIOS and firmware menu boot; no HLE or direct boot.
- Both screens pixel-exact for every captured frame.
- Stereo sample stream exact and continuous with no underruns.
- Complete touch/button navigation through the firmware menu and settings.
- ARM9/ARM7 register, cycle, event, and required memory checkpoints match.
- BIOS and firmware/menu execute from static native banks.
- Zero immutable dispatch misses, invalid targets, and unjustified Tier-3 work.
- Stable high frame rate and deterministic repeated runs.

Only after this gate is green is the runtime considered mature enough to begin
a separate game project such as SM64DS.
