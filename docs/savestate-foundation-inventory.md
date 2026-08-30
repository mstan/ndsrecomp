# Save-State Foundation Inventory

This document is the review boundary for `beads-q7fj`'s first save-state
foundation. The implemented container deliberately covers the deterministic
core needed by `savestate_test`; it is not a user-facing whole-console state
format yet.

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

These are inventory items only in this foundation. The runner does not yet
embed melonDS `Savestate` payloads into the new top-level container.

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
  device debug rings. Not covered.
- `runner/src/vram.cpp`: VRAM bank contents, VRAMCNT/VRAMSTAT mapping, palette,
  OAM, CPU/video mappings, renderer flattened views, LCDC capture visibility,
  provenance, and texture generations. Not covered.
- `runner/src/gpu2d.cpp`: 2D engine registers, framebuffers, scanline
  renderer state, affine/internal reference accumulators, display capture
  latches, direct-frame/HD state, threaded render jobs, and GPU profile
  counters. Not covered.
- `runner/src/gpu3d.cpp` and `runner/vendor/melonds/GPU3D.cpp`: bridge state,
  renderer selection, power/render width/wide projection state, run/write
  traces, compute/soft renderer resources, texture-cache coherence, and the
  vendored GPU3D device state. Not covered.
- `runner/src/spu.cpp`: ARM7 sound registers, channel/capture state, bias,
  mixer timing/output queues, and debug output rings. Not covered.
- `runner/src/wifi_net.cpp` and `runner/vendor/melonds/Wifi.cpp`: runner Wi-Fi
  bridge state, firmware binding, POWCNT2 power gate, event scheduling, local
  multiplayer/replay/slirp state, packet rings, and vendored Wi-Fi device
  state. Not covered.
- `runner/src/cart_backup.cpp`, `runner/src/battery_save.cpp`, and
  firmware/cartridge paths reached through `io.cpp`: persistent flash/EEPROM
  backing state, dirty flags, SPI transaction state, and atomic save flushing.
  Not covered.
- `runner/src/live_overlay.cpp`, `runner/src/coverage_manifest.cpp`, and
  diagnostics/debug modules: host-only compilation/cache/diagnostic state.
  Not guest-visible by itself, but load must invalidate or refresh any cache
  that depends on guest memory or CPU state. Covered: runtime dispatch/direct
  link invalidation hook. Not covered: preserving debug/profiling histories.

## Blockers for user-facing save states

The following sections must be implemented and tested before frontend F-key
save/load slots are safe:

- IO core: IRQ, HALT, IPC, timers, DMA, DIV/SQRT, display counters, power,
  input, and system-event queue/deadline serialization.
- Cartridge/firmware/RTC: gamecard transfer state, AUXSPI transaction state,
  cart backup chip state, firmware SPI state, dirty persistence flags, and RTC
  serial datetime state.
- VRAM/video memory: VRAM bank contents/mapping, palette, OAM, renderer views,
  LCDC capture state, provenance, and texture generations.
- 2D/3D GPU: 2D engine registers/framebuffers/capture state plus vendored
  GPU3D `DoSavestate` integration and runner-owned bridge/compute state.
- SPU: sound register/channel/capture/mixer queues and sample timing.
- Wi-Fi: vendored Wi-Fi `DoSavestate` integration plus runner bridge, packet
  rings, replay/local-MP/slirp ownership, power and scheduled events.
- Debug/profiling policy: decide which always-on rings/counters are reset,
  preserved, or explicitly marked host-only across load.
