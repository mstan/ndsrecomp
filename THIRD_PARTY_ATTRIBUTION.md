# Copyright and Third-Party Attribution

Software is copyrighted even when its source is publicly visible. This file
records the origin and licensing posture of code and tooling in this repository;
it is not a legal guarantee of non-infringement.

## Project-owned code

Except for the items described below, the tracked implementation and
documentation are copyright © 2026 Matthew Stanley and contributors and are
licensed under the MIT License; see [`LICENSE`](LICENSE).

The MIT grant covers this project's own source. It does not and cannot relicense
the third-party code described below, and it does not make every build artifact
redistributable under MIT terms. In particular, the native runner links vendored
melonDS sources, so the `nds_runner` **executable** is a combined work whose
distribution must comply with GPL-3.0-or-later — see
[melonDS vendored GPU3D (runner)](#melonds-vendored-gpu3d-runner) below. The
recompiler, the generated banks, and all `ndsref`-independent tooling stay
outside that boundary and are distributable under MIT alone.

## gbarecomp

The portable ARMv4T core and portions of the recompiler driver and function
finder began as ports from the sibling
[`mstan/gbarecomp`](https://github.com/mstan/gbarecomp) project, whose commits
and this repository's commits have the same copyright owner.

- Upstream: https://github.com/mstan/gbarecomp
- Upstream license: PolyForm Noncommercial License 1.0.0
- Local scope: primarily `recompiler/armv4t/`, with adapted recompiler support
  under `recompiler/finder/` and `recompiler/src/`

The port has since gained Nintendo DS-specific ARMv5TE, CP15, dual-CPU timing,
and dispatch behavior. Upstream `gbarecomp` remains available under the PolyForm
Noncommercial terms; see the
[official license text](https://polyformproject.org/licenses/noncommercial/1.0.0/).
Because both repositories share one copyright owner, the ported portions as they
exist *here* are offered under this repository's MIT grant; the upstream project's
own terms are unaffected, and this statement does not relicense upstream.

## melonDS optional oracle

melonDS is copyright its authors and contributors and is distributed under the
GNU General Public License v3.0 or later.

- Upstream: https://github.com/melonDS-emu/melonDS
- Pinned version used by the retained evidence: tag `1.0rc`
- License: GPL-3.0-or-later
- Local scope: `oracle/`

`oracle/setup-melonds.sh` clones melonDS into the ignored `third_party/`
directory. The tracked `oracle/patches/` files contain patch context against
melonDS and are intended solely for that GPL-covered build. The tracked oracle
shim compiles with melonDS into a separate executable; any distribution of
that combined executable must comply with the GPL.

## melonDS vendored GPU3D (runner)

Since 2026-07-16 the native runner additionally vendors the melonDS 3D
geometry engine and software rasterizer as its 3D device model (decision:
accept GPL for the runner executable rather than reimplement the 3D
pipeline; the guest still produces every register and GXFIFO write, so this
remains a device model, not HLE).

- Local scope: `runner/vendor/melonds/`
- Vendored unmodified from tag `1.0rc` (`src/` paths, byte-identical):
  `FIFO.h`, `types.h`, `Savestate.h`, `Savestate.cpp`,
  `NonStupidBitfield.h`, `GPU3D_Texcache.cpp`, `GPU3D_Texcache.h`,
  `GPU3D_TexcacheOpenGL.cpp`, `GPU3D_TexcacheOpenGL.h`,
  `OpenGLSupport.cpp`, `OpenGLSupport.h`, and `PlatformOGL.h`, plus
  `xxhash/xxhash.c`, `xxhash/xxhash.h`, and
  `frontend/glad/{glad.c,glad.h,khrplatform.h}` stored locally under
  `runner/vendor/melonds/glad/`.
- **Modified** from the same tag, per the tracked patches in
  `runner/vendor/melonds/patches/` (GPLv3 §5(a) "you changed the files"
  notices): `GPU3D.cpp`, `GPU3D.h`, `GPU3D_Soft.cpp`, `GPU3D_Soft.h`
  (patch `0009`, host-only adaptive render width plus the attribute
  surface and a soft-renderer thread-restart fix), and
  `GPU3D_Compute.cpp`, `GPU3D_Compute.h`, `GPU3D_Compute_shaders.h`
  (patch `0010`, adaptive width, polygon-ID attributes in the low
  resolution surface, and the internal-resolution accessors).

  Correction, 2026-08-16: this section previously listed all seven of
  those files as byte-identical to upstream. They were not — the adaptive
  widescreen work modified them without recording a change notice. The
  patches and this list are the correction; no upstream behaviour claim
  was affected, but the GPLv3 §5(a) notice was missing and is now present.
- Project-written shim headers in the same directory (`NDS.h`, `GPU.h`,
  `Platform.h`) replace the melonDS headers of the same names with the
  minimal interface slice the vendored units consume; as derived interfaces
  they are likewise GPL-3.0-or-later. The bridge `runner/src/gpu3d.cpp`
  implements them against the runner's own device models.
- Project-written compute adapters under `runner/src/melonds_compute/`
  provide the host GL-context owner, the `GLCompositor` interface slice, and
  the `Platform` file-service slice consumed by the byte-identical upstream
  compute sources. These interfaces are derived from melonDS and are likewise
  GPL-3.0-or-later; they do not replace or alter the faithful soft renderer.
- Consequence: `nds_runner` is a combined work with melonDS. Any
  distribution of the runner binary must comply with GPL-3.0-or-later.
  The recompiler, the generated banks, and all `ndsref`-independent tooling
  remain outside this boundary and do not compile or link melonDS code.

The native implementation uses melonDS as a behavioral and timing reference.
An audit before the first public release found no exact normalized six-line
code block shared between the tracked native recompiler/runtime sources and
the pinned melonDS source tree. That mechanical check cannot prove independent
authorship; provenance comments and the repository history remain the primary
record.

## melonPrimeDS Metroid Prime Hunters controls reference

The MPH-specific `Prime Controls` input layer mirrors the user-facing control
scheme and gameplay touch-helper behavior from makinori's melonPrimeDS fork.

- Upstream: https://github.com/makinori/melonPrimeDS
- Upstream license: GPL-3.0-or-later, inherited from melonDS
- Local scope: the AMHE0 control binding defaults and touch-helper behavior in
  `runner/src/frontend.cpp`, plus the game launcher's Mods-page control
  surface in the Metroid Prime Hunters title repository
- Source reference: melonPrimeDS `metroid/mph-us-1.0.lua`,
  `metroid/mph-us-1.1.lua`, and `src/frontend/qt_sdl/EmuThread.cpp`

This repository already distributes the runner as a GPL-3.0-or-later combined
work because it vendors melonDS device-model code. The control-layer port stays
inside that same runner/launcher boundary; the recompiler and generated banks
remain outside it.

## melonDS vendored Wifi device model + net glue (runner)

Since 2026-08 the native runner additionally vendors melonDS's DS Wi-Fi
device model (`Wifi`/`WifiAP`) and its network backend glue (`Net`,
`NetDriver`, `PacketDispatcher`, `Net_Slirp`), following the same decision
already made for the vendored GPU3D engine above: the runner is already a
GPL-3.0-or-later combined work, so taking a second melonDS subsystem costs
nothing additional on the licensing axis. See
[`docs/adr-melonds-wifi-vendoring.md`](docs/adr-melonds-wifi-vendoring.md)
for the full analysis. As with GPU3D, the guest's own ARM7 Wi-Fi driver
still writes every register; this is a device model, not HLE, and no
DWC/GameSpy/SSL/matchmaking service logic is emulated.

- Local scope: `runner/vendor/melonds/{Wifi.cpp,Wifi.h,WifiAP.cpp,WifiAP.h,net/}`
- Vendored from tag `1.0rc`, commit
  `e3fa6f4224e0d706df3ee262ae41cfb0deadc593`:
  - Byte-identical (hash-verified against the pinned `ndsref` checkout at
    vendoring time): `Wifi.cpp`, `Wifi.h`, `net/Net.cpp`, `net/Net.h`,
    `net/NetDriver.h`, `net/PacketDispatcher.cpp`, `net/PacketDispatcher.h`,
    `net/Net_PCap.cpp`, `net/Net_PCap.h`, `net/pcap/*.h` (the freely
    distributable libpcap public headers, BSD-licensed, vendored wholesale
    the same way melonDS itself does), and the entire `net/libslirp/` tree
    (libslirp 4.8.0, BSD-3-Clause — see its own `COPYRIGHT`).
  - Modified per the tracked patches in `runner/vendor/melonds/patches/`
    (GPLv3 §5(a) "you changed the files" notices; see that directory's
    README for all five patches and their rationale): `WifiAP.cpp` (AP
    identity: SSID `ndsrecomp`, locally-administered BSSID
    `02:4E:44:53:52:01`, replacing melonDS's default `melonAP`/
    `00:F0:77:77:77:77`; plus the association/state-change and
    802.11<->Ethernet-boundary network-observability-ring hooks),
    `Wifi.cpp` (the egress/ingress network-observability-ring hooks in
    `TXSendFrame`/`FinishRX`), and `net/Net_Slirp.{h,cpp}` (patch 0002
    forces `RecvCheck()`'s `poll()` timeout to 0 so it never blocks;
    patch 0005 goes further and moves all host-socket polling AND all
    guest->host packet sends off the emulation thread entirely, onto a
    dedicated worker thread owned by `runner/src/wifi_net.cpp` --
    `RecvCheck()` becomes a true no-op and its old body moves to a new
    method, `PollHostSockets()`, called only by that worker thread. See
    `runner/vendor/melonds/patches/README.md`'s `0005-*` entry and the
    design comment above `WifiBridgeState` in `runner/src/wifi_net.cpp`).
  - Reused as-is from the already-vendored GPU3D set: `types.h`,
    `Savestate.h`/`.cpp`, `FIFO.h`.
- `net/Net_PCap.cpp`/`.h` compile only when the off-by-default
  `NDS_ENABLE_PCAP_BACKEND` CMake option is set; they only need the
  `<pcap/pcap.h>` header at compile time — the actual Npcap/WinPcap DLL is
  loaded dynamically at runtime, not linked. `net/LocalMP.cpp`, `LAN.cpp`,
  `Netplay.cpp`, `MPInterface.cpp` (melonDS's local-wireless/netplay code,
  which pulls in ENet) are deliberately **not** vendored; local wireless/
  Download Play/NiFi stays out of scope.
- Project-written shim headers in the same directory (`NDS.h` extended,
  `Platform.h` extended, new `SPI.h`, new `SPI_Firmware.h`) supply the
  minimal interface slice the vendored units consume, the same pattern as
  the GPU3D shims; as derived interfaces they are likewise
  GPL-3.0-or-later. The bridge `runner/src/wifi_net.cpp` implements them
  against the runner's own firmware buffer and a single-slot scheduler
  shim keyed to the live scheduler's guest-cycle rendezvous
  (`scheduler.cpp`'s `next_scheduled_event_time()`/`scheduler_run_round()`
  fold `nds_wifi_next_event_time()`/`nds_wifi_run_events()` into the same
  chain every other device deadline uses).
- As of 2026-08-10 this bridge is wired to `bus.cpp`/`io.cpp`/
  `scheduler.cpp` and is the SOLE Wi-Fi device model on the live bus.
  `runner/src/wifi.cpp` (the prior hand-written model) has been deleted;
  `runner/src/wifi.h` is kept unchanged as the bus-facing declaration
  surface (bus.cpp/io.cpp/scheduler.cpp already call those exact names),
  now implemented in `wifi_net.cpp` instead. The guest's own ARM7 Wi-Fi
  driver still writes every register and still derives its own MAC
  address from firmware over SPI (`Wifi::Reset()` never seeds
  `W_MACAddr0-2` with a real value; melonDS zeroes them on the
  `W_ModeReset` bit-14 reset path exactly like the retired hand-written
  model did) — this remains a device-model swap, never HLE, and no MAC is
  injected from the host/config/firmware image directly.
- Consequence: no change to the GPU3D section's licensing conclusion above
  — `nds_runner` was already a GPL-3.0-or-later combined work; this adds a
  second vendored subsystem inside the same boundary.

## mGBA

mGBA is copyright © Jeffrey Pfau and contributors and is distributed under the
Mozilla Public License 2.0.

- Upstream: https://github.com/mgba-emu/mgba
- License: MPL-2.0
- Use here: behavioral and ARM7 timing reference inherited in part through the
  portable `gbarecomp` core; mGBA is not vendored, compiled, or linked here

## Specifications and documentation

The ARM Architecture Reference Manual and Martin Korth's GBATEK documentation
are implementation references. DeSmuME is a GPL-2.0 secondary behavioral
cross-check. PikalaxALT/ndsbios may be cloned locally for symbol research but is
ignored and is not distributed in this repository.

See [`docs/references.md`](docs/references.md) for the development provenance
policy. Names such as Nintendo DS, Nintendo, Discord, melonDS, DeSmuME, mGBA,
and ARM belong to their respective owners; references do not imply endorsement.
