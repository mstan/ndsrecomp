# Save-State Foundation Inventory

This document is the review boundary for `beads-q7fj`'s save-state foundation.
The implemented container covers the deterministic CPU, memory, I/O, storage,
and video core; it is not a user-facing whole-console state format yet.

## Existing melonDS integrations

The vendored melonDS save-state class exists, but it only covers vendored
device models that explicitly call it. The custom runner owns most DS state
outside that class.

- `runner/vendor/melonds/Savestate.{h,cpp}` implements melonDS' sectioned
  save-state stream.
- `runner/vendor/melonds/GPU3D.cpp` implements `GPU3D::DoSavestate`.
- `runner/vendor/melonds/GPU3D.cpp` implements `Vertex::DoSavestate`.
- `runner/vendor/melonds/FIFO.h` implements FIFO `DoSavestate` helpers used
  by the vendored GPU/Wi-Fi models.
- `runner/vendor/melonds/Wifi.cpp` implements `Wifi::DoSavestate`.

`GPU3D::DoSavestate` is embedded as the payload of the top-level `GP3D`
section. The Wi-Fi serializer remains an inventory item only.

## Covered foundation sections

The new runner container is exact-build and exact-ROM gated by an identity
section. It rejects wrong magic, unsupported format/section versions, unknown
or duplicate sections, empty sections, non-contiguous/trailing bytes, section
checksum mismatches, build-id mismatches, and ROM SHA-1 mismatches. It writes
through a temporary path and atomically replaces the destination.

- `IDEN`: exact build id and ROM SHA-1.
- `SCHD`: both CPU slots, scheduler cycle timestamps, system timestamp,
  terminal-halt flags, deferred cycles, and the recomp call-return stack.
- `MEMR`: main RAM, ARM9 ITCM/DTCM, shared WRAM, ARM7 WRAM, BIOS mirrors,
  guest-write provenance, and executable page generations.
- `CP15`: ARM9 CP15-visible control bits, TCM sizing/base, timing generation,
  and MPU/cache/access registers.
- `RTIM`: runner-side runtime counters/toggles needed by this core roundtrip
  and host dispatch/cache invalidation after load.
- `IOCR`: explicit little-endian IRQ/IME/IE/IF and HALT state, IPCSYNC and
  IPC FIFOs, DMA channels, timers, DIV/SQRT units and deadlines, display
  counters/phase, power/input latches, and the generic I/O register backing.
  The section contains no raw device structs, pointers, callbacks, mutexes,
  host threads, strings, or trace rings.
- `IOPF`: gamecard transfer/KEY1 phase and deadlines, AUXSPI controller and
  backup-chip protocol, cartridge backup bytes, firmware SPI/controller and
  mutable firmware bytes, and RTC serial/calendar/IRQ phase. Immutable
  cartridge geometry and firmware size are checked against the currently
  inserted exact-ROM hardware before apply.
- `VRAM`: all physical VRAM banks, write provenance, executable generations,
  palette, OAM, VRAMCNT mapping, and the texture-coherence generation.
- `GP2D`: both engines' explicit guest registers and affine accumulators, four
  native framebuffers, frame/presentation capture state, and capture latches.
- `GP3D`: the vendored geometry/FIFO/register/render-list device graph plus the
  runner timestamp used for geometry command timing.

## Guest-visible state owners

The following runner owners must eventually participate in a whole-console
state. Items marked "covered" are included in this foundation's deterministic
roundtrip; the rest are blockers before exposing frontend save/load slots.

- `runner/src/runtime_arm.cpp`: live CPU ABI state, active CPU selector,
  runtime cycle accumulator, fast-limit/yield state, dispatch cache, direct
  link guards/epoch, retired instruction counters, force-Tier3 state,
  call-return stack, deferred cycles, trace/debug rings, and HLE heat data.
  Covered: CPU-adjacent runtime state, retired instruction counters,
  force-Tier3 toggles, call-return stack, deferred cycles, and host-cache
  invalidation. Not covered: trace rings, dispatch statistics/timing totals,
  HLE heat/profile samples, static-miss discovery logs.
- `runner/src/scheduler.cpp`: CPU slots for ARM9/ARM7, started/halted state,
  per-CPU cycles, system timestamp, current slot save/restore, scheduler
  profile counters, and event rendezvous calculations. Covered: CPU slots,
  cycle timing, system timestamp, terminal-halt flags, call-return stack, and
  deferred cycles. Not covered: profile counters.
- `runner/src/bus.cpp`: main RAM, ITCM, DTCM, shared WRAM, ARM7 WRAM, BIOS
  images, bus fast-map windows, code timing caches, guest-write provenance,
  executable page generations, and bus access/debug rings. Covered: memory,
  provenance, generations, BIOS mirrors, timing-cache reset, fast-map refresh.
  Not covered: bus access/debug rings and mem-timing profile counters.
- `runner/src/cp15.cpp`: ARM9 CP15 control/high-vector/TCM state, MPU region
  registers, cache config registers, access permissions, and timing generation.
  Covered.
- `runner/src/io.cpp`: interrupt controllers, IME/IE/IF pending caches, HALT
  and wake state, IPCSYNC, IPC FIFO queues, POSTFLG, EXMEMCNT, WRAMCNT,
  POWCNT, key/touch input latches, timers, DMA engines, VCOUNT/DISPSTAT,
  display scheduling, DIV/SQRT units, SPI firmware/RTC/gamecard protocol,
  AUXSPI/cart backup state, system-event queue/deadlines, event counters, and
  device debug rings. Covered: IRQ/HALT, IPC, DMA, timers, DIV/SQRT deadlines,
  display counters/phase, core power/input latches, and generic I/O backing.
  Covered: cartridge/firmware/RTC serial protocols and their deadlines plus
  persistent backup/firmware contents. Not covered: event counters and device
  debug rings. Each restored deadline is validated with the protocol state
  that gives it meaning and against scheduler time.
- `runner/src/vram.cpp`: VRAM bank contents, VRAMCNT/VRAMSTAT mapping, palette,
  OAM, CPU/video mappings, renderer flattened views, LCDC capture visibility,
  provenance, and texture generations. Covered. Derived CPU/video mappings
  and flattened renderer views are rebuilt from restored VRAMCNT state.
- `runner/src/gpu2d.cpp`: 2D engine registers, framebuffers, scanline
  renderer state, affine/internal reference accumulators, display capture
  latches, direct-frame/HD state, threaded render jobs, and GPU profile
  counters. Covered: guest registers/accumulators, native framebuffers, and
  capture state. Worker jobs are drained at the transaction boundary;
  adaptive/direct/HD presentation caches and counters are host-only and reset.
- `runner/src/gpu3d.cpp` and `runner/vendor/melonds/GPU3D.cpp`: bridge state,
  renderer selection, power/render width/wide projection state, run/write
  traces, compute/soft renderer resources, texture-cache coherence, and the
  vendored GPU3D device state. Covered: the vendored device graph and command
  timing timestamp. Renderer selection/settings remain host configuration.
  Renderer objects, compute buffers, GL handles/fences, texture views, and
  traces are host-only: restore invalidates them and renders the restored
  guest render list into newly coherent resources.
- `runner/src/spu.cpp`: ARM7 sound registers, channel/capture state, bias,
  mixer timing/output queues, and debug output rings. Not covered.
- `runner/src/wifi_net.cpp` and `runner/vendor/melonds/Wifi.cpp`: runner Wi-Fi
  bridge state, firmware binding, POWCNT2 power gate, event scheduling, local
  multiplayer/replay/slirp state, packet rings, and vendored Wi-Fi device
  state. Not covered.
- `runner/src/cart_backup.cpp`, `runner/src/battery_save.cpp`, and
  firmware/cartridge paths reached through `io.cpp`: persistent flash/EEPROM
  backing state, dirty flags, SPI transaction state, and atomic save flushing.
  Covered. A historical state restores the guest-visible mutable flash into a
  session detached from the canonical host files. Automatic transaction and
  shutdown flushes are suppressed for that session, so loading an old state
  cannot silently overwrite a newer cartridge save or firmware profile. A
  fresh process boot reloads and reattaches the canonical files.
- `runner/src/live_overlay.cpp`, `runner/src/coverage_manifest.cpp`, and
  diagnostics/debug modules: host-only compilation/cache/diagnostic state.
  Not guest-visible by itself, but load must invalidate or refresh any cache
  that depends on guest memory or CPU state. Covered: runtime dispatch/direct
  link invalidation hook. Not covered: preserving debug/profiling histories.

## Blockers for user-facing save states

The video sections are implemented but frontend F-key save/load slots remain
blocked until the following owners and policy are implemented and tested:

- SPU: sound register/channel/capture/mixer queues and sample timing.
- Wi-Fi: vendored Wi-Fi `DoSavestate` integration plus runner bridge, packet
  rings, replay/local-MP/slirp ownership, power and scheduled events.
- Debug/profiling policy: decide which always-on rings/counters are reset,
  preserved, or explicitly marked host-only across load.

## Transaction and network policy

Save/load is accepted only synchronously on the scheduler's owning thread and
between rounds. A control/debug thread is rejected, so it cannot pass a
quiescence check and race the next emulation round. This deliberately avoids
putting a mutex or atomic exchange in every 64-cycle scheduler round; frontend
commands must marshal the transaction onto the emulation thread.

Every decoded core section is structurally and semantically validated before
the first live owner is changed. The apply path still snapshots the current
core and restores it if an owner reports an apply-time failure, so a future
owner cannot turn a failed load into a half-loaded machine.

An eligibility callback can reject both save and load before state is read or
applied. It is intentionally not wired to `NdsWifiNetworkState`: that query
reports backend and worker activity, but not guest association or local-MP
connection state. Treating an enabled backend as connected would disable
offline save states and would fake the requested connected-only policy. The
Wi-Fi owner must expose actual WFC association/local-peer state, then register
that callback before frontend slots are enabled.
