# ADR: Vendor melonDS's Wi-Fi + net backend instead of clean-room reimplementation

Status: proposed. Analysis only — no code changes in this pass.

Every citation below was read directly in this session, either in
`F:\Projects\ndsrecomp\ndsrecomp-wiimmfi` (branch `claude/wiimmfi`, editable) or the
read-only oracle checkout `F:\Projects\ndsrecomp\ndsref\third_party\melonDS` (pinned
to tag `1.0rc`, commit `e3fa6f4224e0d706df3ee262ae41cfb0deadc593`). Claims that rest
on another agent's document (`docs/nds-wifi-status.md`) are marked and, where
practical, independently spot-checked against the source in this session.

## 1. Context

The original plan (`E:\Downloads\PLAN_ndsrecomp_wiimmfi.md`) told the first Wi-Fi
agent to reimplement DS Wi-Fi from specifications/oracle traces because melonDS is
GPL-3.0-or-later and `ndsrecomp` was (believed to be) permissively licensed
(§3.2: "Do not copy GPL code into a permissively licensed `ndsrecomp`"; §27: "treat
melonDS as an oracle unless project licensing is intentionally made compatible
with its GPL obligations").

That premise is now stale. Since 2026-07-16, `nds_runner` already vendors melonDS
GPL-3.0-or-later source wholesale as its 3D device model:

- `runner/CMakeLists.txt:250-252` unconditionally builds `vendor/melonds/GPU3D.cpp`,
  `GPU3D_Soft.cpp`, `Savestate.cpp` into `nds_runner`; line 305 labels the include
  dir `# vendored melonDS GPU3D + shims (GPL-3.0-or-later)`.
- `THIRD_PARTY_ATTRIBUTION.md:49-84` records the decision explicitly: "accept GPL
  for the runner executable rather than reimplement the 3D pipeline," and states
  the consequence in terms that generalize directly to Wi-Fi: "`nds_runner` is a
  combined work with melonDS. Any distribution of the runner binary must comply
  with GPL-3.0-or-later. The recompiler, the generated banks, and all
  `ndsref`-independent tooling remain outside this boundary."
- The pattern already has a working, in-tree template: project-written shim
  headers (`runner/vendor/melonds/NDS.h`, `GPU.h`, `Platform.h`) supply "the
  minimal interface slice the vendored units consume" (comment at
  `NDS.h:2-14`), and a single bridge translation unit
  (`runner/src/gpu3d.cpp:1-5`) implements those shims against the runner's own
  device models. The vendored `.cpp` files themselves are untouched upstream
  source.

**What this overturns, precisely:** Plan §3.2's "do not copy GPL code" and §27's
conditional "unless project licensing is intentionally made compatible" — that
condition has already been met, for the runner binary specifically, by the 2026-07-16
GPU3D decision. Vendoring Wi-Fi is not a new licensing posture; it is applying an
already-adopted one to a second subsystem.

**What this does NOT overturn:**
- Plan §2.1 (no DWC/GameSpy/SSL HLE), §2.3 (no game-specific runtime behavior),
  §2.4 (generated C is never edited) — these constrain the *game code* boundary,
  which vendoring the *hardware/AP/backend* layer doesn't touch.
- Plan §17 (local wireless / Download Play / NiFi out of scope) — addressed in
  §5 below; vendoring makes this *easier* to keep out of scope, not harder,
  because the MP-only call sites are already factored behind `Platform::MP_*`
  in the vendored code, not entangled with infrastructure-mode logic.
- Plan §18 (no host-wall-clock-driven networking) and §19 (single-threaded
  guest-visible state) — these are the sharpest test of whether vendoring is
  *technically* sound, not just legally permitted. §8 below finds one concrete,
  citable violation in the vendored net backend and gives the manageable fix.
- Plan §20 (non-resumable network sessions across savestate) — actually already
  satisfied for free by melonDS's own `Wifi::DoSavestate` (§4, §8).
- `docs/references.md:18-22` rule 3 ("committed implementation must trace to a
  spec or a permissive source, never to copyleft code") — this is the
  recompiler/runtime-independent-authorship rule for the *rest* of the codebase.
  It was already superseded for the runner binary specifically by the GPU3D
  precedent; this ADR proposes the same carve-out apply to `runner/vendor/melonds/`
  Wi-Fi files by the same reasoning, and recommends `references.md` be updated to
  say so explicitly (see §11).

### 1.1 A sibling agent's libslirp recommendation rests on the stale premise

A parallel agent (R4), tasked with libslirp's licensing and its NAT/GameSpy
limitations, recommends vendoring pristine upstream libslirp while explicitly
**not** taking melonDS's `net/Net_Slirp.cpp`/`.h` glue, on the grounds that it
is "GPL glue" to avoid. That reasoning is sound *only* under the plan's
original premise — a permissively-licensed runner that must not absorb GPL
code. §1 above establishes that premise is already false for `nds_runner`
specifically (`runner/CMakeLists.txt:250-252`, `THIRD_PARTY_ATTRIBUTION.md:49-84`).
Once the runner is already a GPL-3.0-or-later combined work, taking melonDS's
`Net_Slirp.cpp` costs **nothing additional** on the licensing axis — the
GPL boundary was crossed the day GPU3D was vendored, not the day Wi-Fi is.

Evaluated on technical merit alone (the only axis left once licensing is a
wash): **take melonDS's `Net_Slirp.cpp`, do not write an equivalent from
scratch.** It is working, oracle-proven glue between the exact vendored
`Wifi.cpp`/`WifiAP.cpp`/`Net.cpp` classes this ADR recommends vendoring and
the exact vendored libslirp both this ADR and R4 recommend vendoring — reusing
it means the three layers (device model, `NetDriver` interface, backend
adapter) arrive already wired together and already validated against real DS
Wi-Fi drivers by melonDS's own userbase, rather than requiring us to
independently re-derive libslirp's `SlirpConfig`/`SlirpCb` calling convention,
its virtual-subnet addressing scheme (`Net_Slirp.cpp:43-48`: fixed
`10.64.0.0/24`-shaped subnet, server/DNS/client IPs, a synthetic server MAC),
and its packet-format assumptions (12-byte melonDS TX header + raw 802.11
frame vs. Ethernet-II framing at the `WifiAP` boundary) by reading libslirp's
own protocol code cold. The one real cost of reusing it — the blocking-`poll()`
scheduling mismatch — is identified and priced into a required local patch in
§8 Finding 1, not glossed over. Rewriting the adapter from scratch would still
have to solve that exact same scheduling problem (the plan's own §19
prescribes the fix), so reuse dominates rewrite here: same problem to solve
either way, but reuse starts from a smaller diff.

## 2. File set to vendor

Enumerated from `ndsref/third_party/melonDS/src` at the pinned commit. Sizes are
line counts read in this session.

### Already vendored in `runner/vendor/melonds/` (reusable as-is, zero new work)

| File | Why Wi-Fi needs it |
|---|---|
| `types.h` | `u8`/`u16`/`u32`/`u64`/`s32` etc. used throughout `Wifi.h`/`Wifi.cpp`. |
| `Savestate.h` / `.cpp` | `Wifi::DoSavestate(Savestate*)` (`Wifi.h:164`, body at `Wifi.cpp:243-316`) serializes through this class unmodified. |
| `FIFO.h` | `Net_Slirp.h:23` uses `FIFO<u32, N>`; `net/PacketDispatcher.h:26,28` uses `melonDS::RingBuffer<0x8000>` from the same header. |
| `NonStupidBitfield.h` | Not used by Wi-Fi/net (confirmed: no match reading `Wifi.h`, `Wifi.cpp`, `WifiAP.h/.cpp`, `Net.h/.cpp`, `Net_Slirp.h/.cpp`, `NetDriver.h`). Listed only to record it is *not* pulled in a second time. |

### New: melonDS-authored device model + net glue (GPL-3.0-or-later, vendor byte-identical except where §7 requires a documented modification)

| File | Lines | Role |
|---|---|---|
| `src/Wifi.cpp` | 2476 | The MAC/register/RF/BB device model + TX/RX state machine. Read in full this session. |
| `src/Wifi.h` | 297 | Register offset enum (matches GBATEK 1:1), `Wifi` class declaration. |
| `src/WifiAP.cpp` | 419 | The virtual access point: beacon/probe/auth/assoc/deauth frame construction, 802.11↔LAN bridging (`SendPacket`/`RecvPacket`). Read in full. |
| `src/WifiAP.h` | 69 | Declares `APName`/`APMac`/`APChannel` — the fields plan §7/§6 requires changing (see §7). |
| `src/net/Net.cpp` | 70 | Thin forwarder: `Net::SendPacket`/`RecvPacket` → `NetDriver`; `Net::RXEnqueue` → `PacketDispatcher`. Read in full. |
| `src/net/Net.h` | 61 | Declares `Net`, owns a `PacketDispatcher` and a `unique_ptr<NetDriver>`. |
| `src/net/NetDriver.h` | 35 | The 2-method backend interface (`SendPacket`, `RecvCheck`) the plan's own `INetworkBackend` (plan §3.1) was going to reinvent. Read in full. |
| `src/net/PacketDispatcher.h` | 50 | Multi-instance packet fan-out `Net.h` depends on. Depends on `melonDS::Platform::Mutex` (new shim, §4) and `FIFO.h`'s `RingBuffer` (already vendored). Confirmed **no** ENet dependency (grepped both files; zero matches for `enet`/`ENet`). |
| `src/net/PacketDispatcher.cpp` | 160 | Implementation. Not read in full this session (low-risk plumbing; single-instance use needs only `registerInstance(0)`/`sendPacket`/`recvPacket`). |
| `src/net/Net_Slirp.cpp` | 459 | libslirp backend adapter. Read in full — **requires a local modification**, see §8 finding 1. |
| `src/net/Net_Slirp.h` | 67 | Declares `Net_Slirp : NetDriver`. Read in full. |
| `src/net/libslirp/` (24 `.c` files under `src/`, `glib/glib.c`+`.h` shim, `CMakeLists.txt`) | ~15k (whole tree) | Vendored libslirp 4.8.0, BSD-3-Clause — confirmed from three independent places read this session: the `COPYRIGHT` file's Gasparovski 3-clause text ("Slirp was written by Danny Gasparovski... Redistribution and use in source and binary forms... are permitted"), `meson.build:3` (`license : 'BSD-3-Clause'`, upstream's own build metadata — not used by us, see §9, but authoritative on licensing), and `src/libslirp.h:1` (`/* SPDX-License-Identifier: BSD-3-Clause */`). Permissive either inside or outside the runner's GPL boundary — its BSD status is not why we're vendoring it (that's §9's build-simplicity argument), but it removes any licensing-stacking concern. Self-contained: ships its own glib *shim* (`glib/glib.c`/`.h`, confirmed by reading `glib.h:1-60`), not a real GLib2 dependency. |

### Vendor now, but gate compilation behind an off-by-default build option

| File | Why "now" rather than "later" |
|---|---|
| `src/net/Net_PCap.cpp` (461 lines) / `.h` (136) / `src/net/pcap/ipnet.h` | Revised from an earlier draft of this ADR, which deferred this file entirely. On reading `Net_PCap.h` in full: it does **not** link against the Npcap/WinPcap SDK's import library at build time. It only needs the `<pcap/pcap.h>` **header** for type declarations (`pcap_t`, `pcap_if_t`, `pcap_pkthdr`, `Net_PCap.h:27`); the actual `wpcap.dll`/`Packet.dll` is loaded **dynamically at runtime** through `Platform::DynamicLibrary_Load`/`LoadFunction`/`Unload` (`Net_PCap.h:75-83,103`, and the shim declarations at `ndsref/.../Platform.h:334-355`, read this session), with `LibPCap::New()` returning `std::optional` — empty, gracefully, if Npcap isn't installed on the host (`Net_PCap.h:63`). That means the *build-integration* cost (this ADR's scope) is small — one more new `Platform::DynamicLibrary_*` shim (~20-30 LOC, trivially `LoadLibraryW`/`GetProcAddress`/`FreeLibrary` on Windows) plus the freely-distributable `pcap/pcap.h` header at compile time — and it is **decoupled from whether Npcap is installed on any given machine**, so there is no reason to defer *vendoring the source*. Gate it exactly like the existing optional-feature precedent already in this CMakeLists — `NDS_ENABLE_COMPUTE_RENDERER` (`CMakeLists.txt:287-301,318-339`) is the template for an off-by-default `NDS_ENABLE_PCAP_BACKEND` option that adds `Net_PCap.cpp` to `target_sources` only when set. This directly answers the orchestrator's question in §11: vendor both backends' source now; PCap's *licensing and NAT/hole-punching limitations* remain R4's territory and are not re-derived here, but milestone 7 (plan §14, "If Slirp is insufficient... add another backend: PcapBackend") does not have to wait on a second vendoring pass if this ADR's recommendation is adopted. |
| `src/net/LocalMP.cpp`, `LAN.cpp`, `Netplay.cpp`, `MPInterface.cpp` | These are the **local-wireless / multi-instance-netplay** implementations behind melonDS's `net-utils` CMake target (`ndsref/.../src/net/CMakeLists.txt:3-12`) and are what pulls in the **ENet** dependency (`net/CMakeLists.txt:28-35`, `pkg_check_modules(ENet REQUIRED IMPORTED_TARGET libenet)`). None of `Wifi.cpp`/`WifiAP.cpp`/`Net.cpp`/`Net_Slirp.cpp`/`Net_PCap.cpp` `#include` any of these four files (confirmed by reading all of them). **Not vendoring these four files means we never need ENet at all** — a real, avoidable dependency the plan never asked for, and local wireless is explicitly out of scope (plan §17). This one stays deferred/never, not "now-but-gated" — there is no milestone that needs it. |

## 3. Symbol → runner facility → shim mapping

Read in full: `Wifi.cpp`, `Wifi.h`, `WifiAP.cpp` (all this session); `Net.cpp`,
`Net.h`, `NetDriver.h`, `Net_Slirp.h`, `Net_Slirp.cpp` (all this session).

| melonDS symbol | What it does | Runner facility | Shim needed? |
|---|---|---|---|
| `NDS.RegisterEventFuncs(Event_Wifi, this, {MakeEventThunk(Wifi, USTimer)})` (`Wifi.cpp:94`) | Registers exactly **one** timer callback (`USTimer`) under `Event_Wifi`. | The runner has no generic event table; each subsystem (`nds_wifi_next_event_time`/`nds_wifi_run_events`) is hardcoded into `scheduler.cpp`'s min-chain (`scheduler.cpp:81-83`, `next_scheduled_event_time()`; and the `nds_wifi_run_events(rendezvous)` call cited by `docs/nds-wifi-status.md:217-220`, itself citing `scheduler.cpp:367,378` — not independently re-verified this session, but consistent with the pattern I did verify at `scheduler.cpp:81-83`). | **Yes, but tiny.** Because `Wifi` only ever registers this one func, `NDS::RegisterEventFuncs`/`UnregisterEventFuncs`/`ScheduleEvent`/`CancelEvent` can be a single-slot shim (one deadline variable + one function pointer), not a generic multi-event dispatcher. Est. 30-40 LOC. |
| `NDS.SetIRQ(1, IRQ_Wifi)` — actually called as `Wifi::CheckIRQ` → `NDS.SetIRQ(1, IRQ_Wifi)` (`Wifi.cpp:381`) | Raises ARM7 IRQ bit for Wi-Fi. | `nds_raise_irq(1, 0x01000000u)` already exists and is exercised by the runner's own from-scratch `wifi.cpp:196` (`// ARM7 IF bit 24 = Wi-Fi`). `IRQ_Wifi` is positionally **24** in melonDS's IRQ enum (`NDS.h:95-119`: counting from `IRQ_HBlank` at the visible top of the excerpt through `IRQ_SPI`, `IRQ_Wifi` lands at 24, matching GBATEK's documented IRQ bit 24 = Wifi and confirming no discrepancy with the runner's existing `0x01000000` constant). | **Yes, but already proven correct.** `NDS::SetIRQ(u32 cpu, u32 irq)` forwards 1-for-1 to the existing `nds_raise_irq`. This is the same shape as the existing `NDS::SetIRQ`/`ClearIRQ` GPU3D shim already declared in `runner/vendor/melonds/NDS.h:44-45` — extend the same class, don't invent a second one. |
| `NDS.SPI.GetFirmware()` (`Wifi.cpp:147`) → `Firmware::GetHeader()` → `fwheader.RFChipType`, `.ConsoleType`, `.Type2Config.InitialRF56Values[84]`, `.Type3Config.{RFIndex1,RFIndex2,RFData1[14],RFData2[14]}` (`Wifi.cpp:150-182`) | Reads RF chip type + per-channel RF calibration table straight out of the firmware image at `Wifi::Reset()` time. | The runner already holds the complete raw firmware image in `g_fw` (`runner/src/io.cpp:1014`, populated at `io.cpp:1982`) and already partially decodes the header via `tools/fw_inspect.py` (per `docs/nds-wifi-status.md:243-267`, not independently re-verified this session but consistent with `io.cpp:1014-1068`'s SPI-firmware-read path, which I did read). The exact byte layout melonDS expects is a real, GBATEK-documented 512-byte header (`ndsref/.../SPI_Firmware.h:242-334`, read in full this session — confirmed field order/offsets for `ConsoleType`, `MacAddr`, `RFChipType`, `Type2Config`/`Type3Config`). | **Yes — new, but small and low-risk.** A `Firmware`/`FirmwareHeader` shim class that is a thin typed view over the runner's existing `g_fw` bytes (no new parsing logic; the offsets already exist in the raw image). Est. 40-70 LOC. This is genuinely new work (the GPU3D precedent didn't need a Firmware shim), but it is "reinterpret the bytes we already have," not "invent calibration data." |
| `NDS.ConsoleType` (`Wifi.cpp:191`) | `0` = DS, `1` = DSi. Runner does not model DSi. | Constant `0`. | Trivial field on the `NDS` shim (already has `ARM9ClockShift`-style plain fields per `NDS.h:41-42`). |
| `NDS.UserData` (threaded through every `Platform::MP_*`/`Platform::Net_*` call, e.g. `Wifi.cpp:96,357,365,653` etc.) | Opaque context pointer melonDS's Qt/libui frontends use to find "which instance." | The runner's bridge pattern (`gpu3d.cpp`) uses file-scope statics instead of threading a context pointer (e.g. `namespace { melonDS::NDS g_nds; ... }`, `gpu3d.cpp:33`). | Trivial: set to `nullptr`, mirror the GPU3D bridge's globals-not-userdata convention in the new `wifi3d`-equivalent bridge file. |
| `NDS.ARM7`, ARM7/ARM9 cycle counters | **Not used.** Grepped the full text of `Wifi.cpp` for `ARM7`/`ARM9`; the only hit is a comment (`Wifi.cpp:2019`, `// TODO: rotate the sequence based on the ARM7 cycle counter (if this is important)` — dead TODO, no code reads it). `Wifi` tracks its own internal `USTimestamp`/`USCounter`, not `NDS.ARM7Timestamp`. | N/A | **None.** This is narrower than the task brief assumed going in — worth stating plainly since it materially shrinks the shim surface versus the GPU3D precedent, which *did* need `ARM9Timestamp`/`ARM9ClockShift` (`NDS.h:41-42`). |
| `Platform::Log(LogLevel, fmt, ...)` | Diagnostic logging throughout. | Already shimmed (`Platform.h:36`, implemented in `gpu3d.cpp` per its own doc comment). | **None — reuse existing shim as-is.** |
| `Platform::Thread_*`/`Semaphore_*` | Not used by Wi-Fi/net at all (no threading inside `Wifi.cpp`/`WifiAP.cpp`/`Net.cpp`; the only threading-adjacent primitive net code needs is a `Mutex`, next row). | Already shimmed for GPU3D's SoftRenderer. | **None new**, but see next row for the one *new* Platform primitive net code needs. |
| `melonDS::Platform::Mutex*` (`PacketDispatcher.h:45`) | Guards the multi-instance packet queues. | Not currently shimmed (GPU3D never needed a mutex type, only `Thread`/`Semaphore`). | **Yes, new, tiny.** `Mutex_Create/Lock/Unlock/Free`, same shape as the existing `Semaphore_*` shim (`Platform.h:43-48`). Est. 15-20 LOC (a `std::mutex` wrapper, matching the doc comment's stated policy of "real primitives... but the runner never enables [multi-threaded] rendering" — same posture applies here: real mutex, single-instance use, no actual host thread contention expected on the MVP path). |
| `Platform::MP_Begin/MP_End/MP_SendPacket/MP_SendCmd/MP_SendReply/MP_SendAck/MP_RecvPacket/MP_RecvHostPacket/MP_RecvReplies` (9 functions, declared `Platform.h:296-304`) | Local-wireless (multiplayer/NiFi) transport — out of scope per plan §17. | N/A | **Stub shim, no-ops.** See §5 — this is the one interface slice we deliberately give a *trivial* implementation to (not a bridge to a real device model), because local wireless is out of scope. Est. 20-30 LOC for 9 one-line stubs. |
| `Platform::Net_SendPacket(u8*, int, void*)` / `Net_RecvPacket(u8*, void*)` (`Platform.h:309-310`) | The frontend-level hook `WifiAP::SendPacket`/`RecvPacket` call to reach the actual `Net`/`NetDriver` singleton (`WifiAP.cpp:304,371`). | New: bridge file owns a `melonDS::Net` instance and forwards these two calls to `Net::SendPacket`/`RecvPacket`. | **New, small.** ~15-20 LOC — this is the seam where the runner's own scheduler-driven tick loop calls into the vendored `Net`/`Net_Slirp` machinery; see §8 finding 1 for why this exact seam is also where the adversarial risk lives. |
| `Savestate::Section/VarArray/Bool32/Var8/Var16/Var32/Var64` (`Wifi.cpp:243-316`) | Wi-Fi's full savestate. | Already vendored (`Savestate.cpp`, `.h`). | **None.** Reuse as-is; this is a real win — the runner's own hand-written `wifi.cpp` has **no savestate support at all** today (confirmed: no `Savestate`/serialize symbol anywhere in the 844-line file I read in full), so vendoring is a net *improvement* to savestate coverage, not a new risk. |

## 4. Runner-side facility mapping (scheduler, IRQ, firmware, bus)

- **Scheduler.** `runner/src/scheduler.cpp:81-83` (`next_scheduled_event_time()`)
  already folds `nds_wifi_next_event_time()` into the global "next event" `min`
  chain alongside RTC/SPU/LCD deadlines, and the existing hand-written
  `runner/src/wifi.cpp:174-183` (`schedule_timer`) computes its deadline with the
  **identical formula** melonDS's `Wifi::ScheduleTimer` uses (`Wifi.cpp:319-329`:
  `cycles = 33513982 * kTimerInterval; cycles -= TimerError; delay = (cycles +
  999999) / 1000000; TimerError = delay*1000000 - cycles`) — I compared both
  functions token-for-token and they match. This means the runner's scheduler
  granularity is *already* melonDS-faithful for Wi-Fi timing; the vendored
  `Wifi::ScheduleTimer`/`USTimer` won't be fighting a foreign timing model, it
  will be dropping into one that was reverse-derived from the exact same
  reference and never diverged.
- **ARM7 IRQ.** `nds_raise_irq(1, 0x01000000u)` (bit 24) is the existing,
  already-correct target for the shimmed `NDS::SetIRQ(1, IRQ_Wifi)` (§3).
- **Firmware image.** `runner/src/io.cpp:1014` (`std::vector<uint8_t> g_fw;`),
  populated at `io.cpp:1982` (`g_fw.assign(p, p + n);`), served to the guest via
  the SPI firmware-read command path (`io.cpp:1051-1053` read, `io.cpp:1068`
  write, per my read of that region). This is the byte store the new `Firmware`
  shim (§3) reads from directly — no new firmware-loading logic, just a typed
  view.
- **Bus routing.** `runner/src/bus.cpp` already funnels every ARM7 access in
  `0x04800000-0x0480FFFF` through `nds_wifi_address`/`nds_wifi_read`/
  `nds_wifi_write` at six call sites (`bus.cpp:713-714,737-738,750-751,763-764,
  779-780,793-794,807-808`, read directly this session) plus the POWCNT2 gate
  (`(nds_powercontrol7() & 2u) != 0u`) threaded through every call. **This
  routing does not change.** Vendoring only swaps what's on the other side of
  `nds_wifi_read`/`nds_wifi_write` (today: `runner/src/wifi.cpp`'s hand-written
  `read16`/`write16`; after: a bridge that forwards into the vendored `Wifi::Read`/
  `Write`). `bus.cpp`'s width-splitting/byte-select/POWCNT2 semantics are
  reproduced independently on both sides already (compare `bus.cpp`'s call-site
  shape to `nds_wifi_read`'s own width handling at `wifi.cpp:821-832`), so this is
  a like-for-like swap behind a stable interface, matching exactly how
  `gpu3d.cpp` sits behind the existing GX-register bus routing today.

## 5. Out-of-scope MP surface — verified safe to stub

Plan §17 puts local wireless / Download Play / NiFi out of scope. Read every
call site in `Wifi.cpp` and `WifiAP.cpp` that touches `Platform::MP_*` or the
`IsMP`/`IsMPClient` state, rather than assuming:

- **`Platform::MP_Begin`/`MP_End`** (`Wifi.cpp:357,365`) fire unconditionally
  from `Wifi::UpdatePowerOn()` on every power on/off transition — **in both
  infra and MP modes**. A no-op stub is correct in both.
- **TX path** (`Wifi::TXSendFrame`, `Wifi.cpp:601-671`): cases `2`/`3` (regular
  LOC1-3 data slots — the ones infrastructure-mode association/data traffic
  actually uses) call `Platform::MP_SendPacket(...)` **and then** `if (!IsMP)
  WifiAP->SendPacket(...)` (`Wifi.cpp:653-654`). `IsMP` is only ever set true by
  `Wifi::Write`'s `W_TXSlotBeacon` handler (`Wifi.cpp:2416`, `IsMP = (val &
  0x8000) != 0`) or by `Wifi::CheckRX` receiving an association-response/host
  frame while acting as an MP client (`Wifi.cpp:1672`). Neither path is reached
  by a station merely associating with an infrastructure AP. So for
  infrastructure mode, `IsMP` stays `false`, and the real transmit path — the
  `WifiAP->SendPacket` call — **always runs regardless of what `MP_SendPacket`
  does**, as long as `MP_SendPacket` is a harmless no-op. Cases `1` (CMD, MP
  host-only) and `5` (Reply, MP client-only) are only reachable via `FireTX`'s
  `W_TXSlotCmd` bit (`Wifi.cpp:782-787`, sets `MPClientFail` unconditionally
  before calling `StartTX_Cmd` — a path infra-mode drivers never trigger) and
  `SendMPReply` (only called from `Wifi::FinishRX`'s CMD-frame-reply branch,
  itself gated on receiving a real CMD frame from an MP host, `Wifi.cpp:1481-1499`)
  respectively.
- **RX path** (`Wifi::CheckRX(int type)`, `Wifi.cpp:1558-1724`): `type==0`
  (invoked from the main tick loop, `Wifi.cpp:1852`, is the only type value the
  periodic path ever passes) does `rxlen = Platform::MP_RecvPacket(...); if
  ((rxlen <= 0) && (!IsMP)) rxlen = WifiAP->RecvPacket(RXBuffer);`
  (`Wifi.cpp:1581-1583`). A stub `MP_RecvPacket` that always returns `≤0` (no
  local peer traffic) makes this fall straight through to `WifiAP->RecvPacket`
  every single tick — which is exactly the AP-mode receive path infra mode
  needs. The function's own inline comment confirms the intent: `"hack: ignore
  MP frames if not engaged in a MP comm"` guards a block that explicitly skips
  MP-addressed frames entirely when `!IsMP` (`Wifi.cpp:1615-1623`).
- **`WifiAP::SendPacket`/`RecvPacket`** themselves (`WifiAP.cpp:262-418`) never
  call any `Platform::MP_*` function — they only call `Platform::Net_SendPacket`/
  `Net_RecvPacket` (`WifiAP.cpp:304,371`), confirming the AP/infra path and the MP
  path are cleanly separated at the `Platform::` boundary, not entangled inside
  a shared function.

**Conclusion, evidence-based rather than assumed:** stubbing all nine
`Platform::MP_*` functions as immediate no-ops/empty-returns (`MP_Begin`/`MP_End`
do nothing; every `MP_Send*` returns `0`; every `MP_Recv*` returns `≤0` /
`false`) leaves every infrastructure-mode code path in `Wifi.cpp`/`WifiAP.cpp`
fully reachable and semantically unchanged, because the source itself branches
on `IsMP`/return-value-fallthrough at exactly the seams needed. This matches
melonDS's own default frontend behavior when no local multiplayer peer is
configured — the stub isn't a shortcut we're inventing, it's the state melonDS
itself is normally in.

## 6. Firmware-menu validation gate

**R1's inventory** (`docs/nds-wifi-status.md`, an existing artifact in this
worktree — treated as an unverified claim and spot-checked, not taken on faith)
states in §(e): "Firmware-menu Wi-Fi touch today: none observed" across all 23
`bios/firmware_banks/*.toml` scenario banks, with a full-file grep across
`bios/` for `wifi`/`POWCNT2`/`0x0480` returning no relevant hits.

**Independent spot-check performed this session:** I read the runner's entire
hand-written `runner/src/wifi.cpp` (844 lines) and compared its `reset_bb()`
(`wifi.cpp:151-167`), `update_power_status()` (`wifi.cpp:217-273`), and
`nds_wifi_load_firmware()` (`wifi.cpp:725-732`) against melonDS's
`Wifi::Reset()` BB-default table (`Wifi.cpp:119-145`) and `UpdatePowerStatus()`
(`Wifi.cpp:456-561`) token-for-token. They match almost exactly (the runner's
version is a faithful, deliberately-trimmed port), which corroborates R1's
characterization that the runner's device model was built *from* melonDS's
Wi-Fi model as a behavioral reference — consistent with, though not identical
proof of, the claim that no firmware-menu capture exercises this address range
today (a `reset_bb` match doesn't independently prove zero *access*, only that
whatever access happens produces the same reset defaults either way).

**Gate for this specific migration, given that finding:** because the
firmware-menu boot (the project's hard accuracy gate, `PRINCIPLES.md:20-38`)
does not appear to touch `0x04800000-0x0480FFFF` at all, swapping the device
model behind that address range carries near-zero regression risk *to the
existing gate*, PROVIDED:
1. The swap is validated by re-running the full firmware-menu boot-to-menu
   oracle comparison (the same visual/in-memory/interactive triple check in
   `PRINCIPLES.md:24-33`) **after** the swap, not assumed safe from the "no
   observed access" finding alone — absence of observed access in today's
   captured scenarios is not proof of absence of access in scenarios not yet
   captured (R1's own hedge, `docs/nds-wifi-status.md:362-365`, "no per-game
   banks exist... only firmware/BIOS banks were inventoried").
2. `POWCNT2`'s Wi-Fi-power gate (the aperture-zeroing behavior at
   `nds_wifi_address`/`nds_wifi_read`'s `powered` parameter, `wifi.cpp:26-27,
   821-822`) must be reproduced identically on the vendored side — this is a
   bus-level contract `bus.cpp` depends on (§4), not a `Wifi` class internal, so
   it is unaffected by the swap itself, but must be re-confirmed once the bridge
   exists.
3. Any *new* firmware-menu capture work (e.g. a scenario that does touch
   Wi-Fi/network settings) discovered after this ADR must re-run this same gate
   before being trusted, per `PRINCIPLES.md`'s general "Validation" section
   (`PRINCIPLES.md:161-167`: "a fix is done only when... a deterministic smoke /
   oracle comparison exercises it").

## 7. Required local modifications and how to record them

Plan §6/§7 (renumbered in this ADR's terms; original request items) requires
SSID `ndsrecomp` and BSSID `02:4E:44:53:52:01` (locally-administered) instead of
melonDS's `APName = "melonAP"` / `APMac = {0x00,0xF0,0x77,0x77,0x77,0x77}`
(confirmed at `WifiAP.cpp:36-37`, read this session).

**Three mechanisms were weighed:**

1. *Inline edit + attribution note* — modify `WifiAP.cpp:36-37` directly, add a
   comment block. Simple, but breaks "vendor byte-identical" for the file GPL
   requires marking as modified anyway (17 U.S.C./GPLv3 §5(a): "must carry
   prominent notices stating that you changed the files").
2. *Tracked patch file*, mirroring `oracle/patches/`'s existing convention for
   the separate oracle build. Keeps the upstream file byte-identical in the
   repository and the diff auditable, but the *build* still produces a modified
   file, and the patch must be re-applied/re-verified on every re-vendor.
3. *Parameterization through the shim*, i.e. make `APName`/`APMac` non-`const`
   or wrap them behind a small setter the bridge calls once at startup, keeping
   `WifiAP.cpp` truly byte-identical to upstream.

**Recommendation: mechanism 2 (tracked patch file), matching this repo's own
established convention, not mechanism 1 or 3.** Reasoning:
- The repo already has exactly this precedent for a different melonDS build
  (`oracle/patches/`, referenced in `THIRD_PARTY_ATTRIBUTION.md:43-46`:
  "tracked `oracle/patches/` files contain patch context against melonDS...
  intended solely for that GPL-covered build"). Reusing a pattern the project
  has already validated is more consistent than introducing a third pattern
  (parameterization) for what is fundamentally the same kind of change (a
  small, permanent, GPL-notice-worthy edit to an otherwise-unmodified upstream
  file).
- Mechanism 3 (parameterization) looks appealing because it preserves
  byte-identity, but `APName`/`APMac`/`APChannel` are `static const` **class
  members with out-of-line definitions** (`WifiAP.h:35-37`), not
  runtime-injected config — turning them into runtime-settable fields is itself
  a semantic change to the vendored class's shape (new constructor parameter or
  setter, touching the header), which is *more* invasive than a two-line literal
  patch, not less. It trades a small, auditable literal change for a structural
  one, in exchange for a byte-identity property that mechanism 2 already
  preserves in the tracked source tree (the patch lives beside the source, not
  inside it).
- Put the new patch at `runner/vendor/melonds/patches/0001-wifi-ap-identity.patch`
  (a new subdirectory, since `oracle/patches/` is scoped to the separate oracle
  build per its own doc comment, and `runner/vendor/melonds/` has no patches
  directory yet) with a one-paragraph header explaining the GPLv3 §5(a)
  "you changed the files" notice requirement, and update
  `THIRD_PARTY_ATTRIBUTION.md`'s Wi-Fi section (mirroring its existing GPU3D
  section, `THIRD_PARTY_ATTRIBUTION.md:49-84`) to name the patch and the exact
  fields it changes.
- Apply the patch as a build-time or vendoring-time step, exactly like
  `oracle/setup-melonds.sh` does for the oracle checkout (per
  `THIRD_PARTY_ATTRIBUTION.md:43-44`) — i.e., the *committed* `WifiAP.cpp` in
  `runner/vendor/melonds/net/` (or wherever it lands) is already patched, and
  the patch file records *why*, the same relationship `GPU3D_Compute.cpp` etc.
  have to their own "byte-identical from tag 1.0rc" provenance note
  (`THIRD_PARTY_ATTRIBUTION.md:61-63`) — i.e. patched-then-committed, with the
  patch retained for provenance/re-vendor, not applied-at-build.

## 8. Adversarial case against vendoring

This is the section most likely to change the recommendation, so it gets full
weight rather than a token paragraph.

### Finding 1 — a real, citable host-wall-clock violation, but localized and fixable (not fatal)

`Net_Slirp::RecvCheck()` (`Net_Slirp.cpp:445-457`, read in full) does:

```cpp
void Net_Slirp::RecvCheck() noexcept {
    if (!Ctx) return;
    u32 timeout = 0;
    PollListSize = 0;
    slirp_pollfds_fill(Ctx, &timeout, SlirpCbAddPoll, this);
    int res = poll(PollList, PollListSize, timeout);          // <-- blocking host syscall
    slirp_pollfds_poll(Ctx, res<0, SlirpCbGetREvents, this);
}
```

`timeout` is an **out-parameter from libslirp itself** (`slirp_pollfds_fill`),
meaning libslirp — not our code — decides how long `poll()` may block, based on
its own internal TCP/DHCP timer state. This function is reached from the
**guest-cycle-scheduled** tick path: `Wifi::USTimer` (the function registered
under `Event_Wifi`, run every `kTimerInterval`=8 Wi-Fi-µs by the scheduler,
§3/§4) calls `Wifi::CheckRX(0)` roughly every 512 Wi-Fi-µs while idle
(`Wifi.cpp:1850-1852`: `if ((!(RXCounter & 0x1FF & kTimeCheckMask)) && ...)
CheckRX(0);`), which calls `WifiAP->RecvPacket` (`Wifi.cpp:1583`), which calls
`Platform::Net_RecvPacket` (`WifiAP.cpp:371`), which — through the frontend's
implementation of that function, i.e. **our new bridge** — reaches
`Net::RecvPacket` (`Net.cpp:56-68`), which calls `Driver->RecvCheck()`
(`Net.cpp:61`) before touching the dispatcher.

This is a direct violation of plan §18 ("Do not make networking
host-wall-clock-driven... Do not mutate guest Wi-Fi state directly from a
network worker thread") and §19 ("Only the emulation/recomp execution thread
may... consume packet[s]") in spirit if not in the literal wording — it's not a
*separate thread* mutating state (§19's literal target), but it *is* the
single emulation thread blocking on real wall-clock/socket readiness *inside*
a call chain the deterministic scheduler (`scheduler_run`, `run_to_event`,
`run_cycles` — the debug-server primitives `docs/networking-observability-plan.md`
and `TCP.md` build determinism around) expects to advance purely on guest
cycles. A `run_to_event`/`run_cycles` debug-server call that happens to land on
a Wi-Fi idle-RX-poll tick would now have unbounded wall-clock latency
determined by libslirp's internal timer state, not by guest cycles requested —
exactly the "arm-then-capture" / non-reproducible-window failure mode the
user's global CLAUDE.md calls out for observability tooling, now showing up
inside the *emulated device* itself rather than in a probe.

**Is it fatal? No — it is narrow and the fix is well-precedented.** The
violation lives entirely inside one 13-line function in one file
(`Net_Slirp.cpp`), which is already the file this ADR's own vendoring plan
earmarks as melonDS-authored *glue*, not the untouchable "hardware device
model" core (`Wifi.cpp`/`WifiAP.cpp` — the pieces most worth keeping
byte-identical for oracle-parity reasons — never call `RecvCheck` themselves;
they call `Net::RecvPacket`, which is one indirection away from `RecvCheck`).
The fix, matching plan §19's own prescribed shape almost exactly: force
`slirp_pollfds_fill`'s timeout to `0` (non-blocking poll always) inside
`RecvCheck`, and if actual blocking-wait behavior is wanted for host-CPU
efficiency, move it to a **separate host polling thread** that only pushes
readiness/received-packet data into a queue the guest-tick thread drains
non-blockingly — i.e., adopt the exact `INetworkBackend`/worker-thread split
the plan already specified in §19, but implement it as a *modification to the
vendored `Net_Slirp.cpp`* rather than a clean-room network stack. This is
strictly less work than clean-room reimplementing DHCP/TCP/UDP over libslirp's
wire protocol, while still requiring genuine engineering (not a stub).
**Recorded as a required local modification, same GPLv3 §5(a) marking
obligation and same tracked-patch mechanism as §7.**

### Finding 2 — real host wall-clock inside libslirp's own timers (inherent, not fixable, already anticipated by the plan)

`Net_Slirp::SlirpCbClockGetNS` (`Net_Slirp.cpp:96-101`) reads
`CLOCK_MONOTONIC` directly and feeds it to libslirp as its internal timer
clock (used for TCP retransmission backoff, DHCP lease timers, etc. inside
libslirp's own state machine, not inspected further this session per scope —
R4's territory). If the runner ever runs faster/slower than real-time (fast
oracle-diff replay, deterministic frame-stepped debugging, or genuinely fast
hardware), libslirp's internal protocol timers advance on **wall-clock time**
while the guest device model advances on **guest cycles**, so a given guest
cycle window will see different libslirp-internal timer firings on different
runs (different host scheduling jitter) even with byte-identical guest input.

**Is it fatal? No — the plan already prices this in.** Plan §20 ("declare live
network sessions non-resumable... do not serialize host TCP sockets") and §21
("Live Internet services cannot be the only regression oracle... packet
capture/replay at the Ethernet backend boundary") already assume the live
network path is inherently non-deterministic and build the *regression*
strategy around a separate `ReplayBackend` instead of trying to make live
networking reproducible. Vendoring doesn't make this worse than a clean-room
libslirp-equivalent would be — any real IP stack talking to the real Internet
has this property; it's intrinsic to the problem, not an artifact of vendoring
melonDS's adapter specifically.

### Finding 3 — savestate/threading assumptions: checked, and they hold up better than expected

Explicitly checked for the three things the task asked about:
- **Does `Wifi` assume it owns the ARM7 IRQ exclusively?** No — it only ever
  *raises* `IRQ_Wifi` through `NDS.SetIRQ`, never asserts exclusive control of
  the IRQ line or dispatch table (confirmed: no other IRQ number appears
  anywhere in `Wifi.cpp`/`WifiAP.cpp`).
- **Does it assume it owns the SPI device?** No — `NDS.SPI.GetFirmware()` is a
  single read-only call at `Wifi::Reset()` time (`Wifi.cpp:147`), not a
  持续/retained handle, and nothing else in the file touches SPI. No exclusivity
  conflict with the runner's existing SPI/firmware-chip emulation
  (`io.cpp`'s SPI firmware-read path).
- **Savestate format:** `Wifi::DoSavestate` (`Wifi.cpp:243-316`) serializes the
  complete device state (RAM, IO, BB/RF regs, TX slots, RX buffer, MP
  bookkeeping) through the already-vendored `Savestate` class, and — this is
  the pleasant surprise — **does not serialize any `Net`/`Net_Slirp`/libslirp
  state at all**. That is exactly plan §20's prescribed behavior (hardware
  state saved, live network session not resumable) *for free*, with zero
  additional engineering, because melonDS's own author already drew that same
  line for the same reason (their own savestate/netplay code comment at
  `Wifi.cpp:246-250` even jokes about it: "not sure we're saving enough shit at
  all there... savestate and wifi can't fucking work together!!"). This is a
  case where vendoring inherited a *correct* architectural decision rather than
  a foreign constraint to fight.
- **Threading:** `Wifi.cpp`/`WifiAP.cpp` contain zero threading primitives
  themselves; the only thread-adjacent surface Wi-Fi vendoring adds is the
  `Mutex` used by `PacketDispatcher` (§3), which is inert for a single-instance
  (non-netplay) configuration. The one genuine threading/timing risk is
  Finding 1, already addressed above — it is not a *savestate* or *ownership*
  problem, it is a *scheduling* problem confined to one function.

### Net assessment of the case against

Two of three lines of attack (Findings 2 and 3) either don't apply or the
vendored code is already better-behaved than a naive reimplementation would
be by default. Finding 1 is real, correctly flagged by the plan's own §18/§19,
and requires a genuine (not cosmetic) local modification to exactly one file —
which is already budgeted into this ADR's "required local modifications"
category (§7) rather than treated as a surprise. Nothing found here rises to
"discovered at milestone 5" severity; it is a milestone-2-scoped, one-file fix
with a clear implementation path (non-blocking poll + optional worker thread,
matching the plan's own prescribed architecture).

## 9. libslirp build integration (mingw-w64, `C:\msys64\mingw64\bin\cmake.exe`)

Aside, not load-bearing for this section: Kaeru WFC's DS entry point is
DNS-only, at `178.62.43.212` ("No hacks, patches, nor flashcards are
required" — kaeru.world/projects/wfc, per the orchestrator's verification).
That confirms the plan's no-ROM-patch architecture (§2.2) holds all the way to
a real, currently-running WFC-compatible service, and confirms the
`NetDriver`/backend boundary this ADR analyzes — not the DNS-redirection
layer — is the real integration risk. Nothing in this ADR needed to change
because of that fact; it is corroborating context, not a new requirement.

melonDS's own build (`ndsref/.../src/net/CMakeLists.txt`, 1019 bytes, read in
full) does **not** use meson (upstream libslirp's own build system,
`net/libslirp/meson.build`) and does **not** use `pkg_check_modules`/system
libslirp by default —
`USE_SYSTEM_LIBSLIRP` defaults `OFF`, and the default path is:

```cmake
add_subdirectory(libslirp EXCLUDE_FROM_ALL)
target_link_libraries(net-utils PUBLIC slirp)
```

`libslirp`'s own `CMakeLists.txt` (`ndsref/.../net/libslirp/CMakeLists.txt`,
1666 bytes, read in full — melonDS's hand-written CMake shim around upstream
libslirp's source list, not upstream's own `meson.build`) is fully
self-contained:

- 24 `.c` sources under `src/` + one shim file (`glib/glib.c`), no external
  `find_package`/`pkg_check_modules` calls at all.
- Ships its **own glib replacement** (`glib/glib.h`/`.c`, confirmed by reading
  `glib.h:1-60`: byte-order macros, `g_assert`, `g_warn_if_fail`, etc. built
  from `<stdint.h>`/`<stdbool.h>`/`<string.h>` only) — **there is no real GLib
  dependency to satisfy on Windows.** This directly answers the "glib?" check
  the task asked for: no, not the real GLib/GObject stack, just a header-only
  substitute.
- Windows-specific linkage is already declared in the same file:
  `if (WIN32) target_link_libraries(slirp PRIVATE ws2_32 iphlpapi) ...` — the
  two Winsock/IP-helper import libraries libslirp needs are wired in
  automatically by `add_subdirectory`, not something the runner's own
  `CMakeLists.txt` has to add by hand.
- `configure_file(...libslirp-version.h.in... libslirp-version.h)` generates a
  version header into the build directory — another reason to use
  `add_subdirectory` rather than hand-listing the `.c` files the way
  `vendor/melonds/GPU3D.cpp` etc. are listed directly in `nds_runner`'s sources
  (`CMakeLists.txt:250-252`): libslirp's build has real generated-file and
  per-platform-link machinery that duplicating by hand risks getting subtly
  wrong, whereas melonDS's own `GPU3D.cpp` et al. need none of that (just
  `-w` to silence warnings, `CMakeLists.txt:313-317`).

**Concrete recipe for `runner/CMakeLists.txt`:**
1. Vendor `net/libslirp/` wholesale under `runner/vendor/melonds/net/libslirp/`
   (its own `CMakeLists.txt` travels with it, unmodified).
2. Add `add_subdirectory(vendor/melonds/net/libslirp EXCLUDE_FROM_ALL)` near
   the other vendoring setup in `runner/CMakeLists.txt` (before
   `add_executable(nds_runner ...)`, mirroring where `nds_armv4t` is declared
   at `CMakeLists.txt:221-227`, i.e. as its own small static-lib target rather
   than folded into `nds_runner`'s source list).
3. `target_link_libraries(nds_runner PRIVATE ... slirp)` alongside the existing
   `nds_banks nds_support nds_armv4t` line (`CMakeLists.txt:340`).
4. Add `Wifi.cpp`, `WifiAP.cpp`, `net/Net.cpp`, `net/PacketDispatcher.cpp`,
   `net/Net_Slirp.cpp` directly to `nds_runner`'s own `target_sources` (or a new
   small static lib, matching the existing convention that melonDS's own C++
   files are listed directly rather than via a nested CMakeLists —
   `CMakeLists.txt:250-252` for the GPU3D precedent), and set
   `COMPILE_OPTIONS "-w"` on them via `set_source_files_properties`, exactly
   mirroring lines `313-317`.
5. `libslirp`'s CMake requires `cmake_minimum_required(VERSION 3.16)` — check
   against whatever CMake version `C:\msys64\mingw64\bin\cmake.exe` reports;
   not verified this session (a two-second check, but out of scope for a
   read-only analysis pass — flagged as **unverified**, not assumed passing).

**mingw-w64 compatibility risk, explicitly flagged as not empirically
verified in this pass (design-only per task constraints):**
`Net_Slirp.h`/`Net_Slirp.cpp` guard their Windows-vs-POSIX socket includes on
the `__WIN32__` macro (`Net_Slirp.h:29-33`, `Net_Slirp.cpp:28-35,50`). MinGW-w64
does define `__WIN32__` for compatibility (via its own `<_mingw.h>`), and the
existing runner already builds and links successfully under
`C:\msys64\mingw64` per the memory note "Build from PowerShell not git-bash," so
the toolchain itself is proven for this kind of C/C++ vendoring — but whether
`__WIN32__` specifically (as opposed to the more standard `_WIN32`) is defined
by this exact mingw-w64 installation was **not verified this session**; a
one-line preprocessor probe or a first build attempt is the actual test, and
should be the first thing an implementation agent checks, not assumed.

## 10. Estimated integration size

| Category | Files | Est. new/modified LOC |
|---|---|---|
| Vendored byte-identical (melonDS `.cpp`/`.h`) | `Wifi.cpp/.h`, `Net.cpp/.h`, `NetDriver.h`, `PacketDispatcher.cpp/.h` | 0 new (copy as-is) |
| Vendored + one tracked patch (§7) | `WifiAP.cpp/.h` (AP identity), `Net_Slirp.cpp` (non-blocking `RecvCheck`, §8 Finding 1) | ~10-20 lines of patch diff total |
| Vendored (foreign build, `add_subdirectory`) | `net/libslirp/*` (24 `.c` + shim) | 0 new (copy as-is + one `CMakeLists.txt` edit) |
| New shim: `NDS.h` extension | `SPI` member, `ScheduleEvent`/`CancelEvent`/`RegisterEventFuncs`/`UnregisterEventFuncs` (single-slot), `ConsoleType`, `UserData` | ~40-60 LOC |
| New shim: `Firmware`/`FirmwareHeader` view | typed view over `g_fw` | ~40-70 LOC |
| New shim: `Platform::Mutex_*` | 4 functions | ~15-20 LOC |
| New shim: `Platform::DynamicLibrary_*` (only if `NDS_ENABLE_PCAP_BACKEND` is turned on) | `Load`/`Unload`/`LoadFunction` + opaque struct, backing `LibPCap`'s runtime `wpcap.dll` load (§2) | ~20-30 LOC, `LoadLibraryW`/`GetProcAddress`/`FreeLibrary` |
| New shim: `Platform::MP_*` stubs | 9 functions, all one-liners | ~20-30 LOC |
| New bridge: `Platform::Net_SendPacket`/`Net_RecvPacket` + owning `melonDS::Net` instance | new file, e.g. `runner/src/wifi_net.cpp` | ~40-60 LOC, plus the `Net_Slirp` non-blocking rework from Finding 1 (~30-60 LOC of real logic: a poll thread or non-blocking timeout clamp + a lock-free/mutexed handoff queue) |
| `CMakeLists.txt` edits | `add_subdirectory`, `target_sources`, `target_link_libraries`, `set_source_files_properties` | ~15-25 LOC |
| **Total new/modified project-written code** | | **roughly 200-330 LOC**, on top of ~3,900 lines of vendored-as-is melonDS device-model/net-glue code and ~15k lines of vendored-as-is libslirp. |

This compares favorably to the alternative: a clean-room reimplementation of
the 802.11 AP state machine, the full Wi-Fi register/RF/BB device model
(2,476+419 lines of intricate, timing-sensitive, GBATEK-plus-hardware-quirk
behavior that took melonDS's authors years of hardware testing to get right —
their own code comments cite specific "hardware tests" and "CHECKME" notes
throughout, e.g. `Wifi.cpp:1373-1378`), and a DHCP/TCP/UDP/NAT stack
equivalent to libslirp, all independently authored and independently
correctness-tested against the same oracle. The vendoring path's ~250 LOC of
new project-written code is concentrated almost entirely at *interface
seams* (shims, a bridge, a build recipe) rather than at *hardware/protocol
behavior* — which is exactly the risk profile the GPU3D precedent already
proved out for a different, similarly intricate subsystem.

## 11. Recommendation

**Vendor — both the Wi-Fi/AP/net-glue layer and both backends' source
(Slirp active now, PCap vendored-but-gated for milestone 7).** High
confidence on the device-model/glue layer; high confidence on taking
`Net_Slirp.cpp` as-is rather than rewriting it (§1.1); medium-high confidence
on vendoring `Net_PCap.cpp`/`.h` now rather than deferring the source
(the technical case in §2 is solid — dynamic-load, no link-time Npcap
dependency — but this ADR did not re-derive R4's NAT/hole-punching findings,
so "is PCap the *right* fallback for milestone 7" stays R4's call; this ADR
only argues that *if* PCap is wanted eventually, vendoring its source now
alongside everything else is strictly cheaper than a second pass later, per
the project's "always pick the most complete option" standard).

- Legally: the licensing objection is already spent for the runner binary
  (§1) — this is applying an adopted posture to a second subsystem, not
  adopting a new one.
- Technically: the device-model core (`Wifi.cpp`/`WifiAP.cpp`) is
  scheduler-compatible with the runner's existing (independently-derived, but
  formula-identical, §4) timing model, doesn't fight IRQ/SPI ownership (§8
  Finding 3), and comes with working savestate support the runner's own
  hand-written model currently lacks entirely (§3, §8 Finding 3). The MP-only
  surface stubs out cleanly with evidence, not assumption (§5). The one real
  architectural friction point — `Net_Slirp::RecvCheck`'s blocking `poll()` on
  the guest-tick call path (§8 Finding 1) — is narrow, precisely
  located, and fixable with a change of the same size and kind the plan
  already anticipated needing to write from scratch (§19's worker-thread
  split), just applied as a patch to one vendored file instead of built from
  zero.
- Cost: an estimated ~200-330 lines of new project-written shim/bridge/patch
  code, versus clean-room reimplementing ~2,900 lines of hardware-quirk-laden
  device model plus a DHCP/TCP/UDP stack from specifications alone — the plan's
  own §3.2 rationale for *not* vendoring ("do not copy GPL code into a
  permissively licensed ndsrecomp") no longer holds, and no other rationale in
  the plan argues against vendoring on technical grounds.

**What I could not determine / explicit gaps:**
- Whether `C:\msys64\mingw64\bin\cmake.exe`'s version satisfies libslirp's
  `cmake_minimum_required(VERSION 3.16)`, and whether `__WIN32__` is defined by
  this exact mingw-w64 installation (§9) — both are quick empirical checks an
  implementation pass should do first, not assumed from this analysis.
- `PacketDispatcher.cpp`'s 160-line implementation was not read in full (only
  its 50-line header) — low risk given it's pure single-process ring-buffer
  plumbing with no external dependency, but not independently verified line by
  line the way `Wifi.cpp`/`WifiAP.cpp`/`Net.cpp`/`Net_Slirp.cpp` were.
- The exact real-hardware register-bit semantics questions R1's own document
  flags as open (`docs/nds-wifi-status.md`'s "Open questions" section) are
  unaffected by this vendor-vs-clean-room decision either way and are out of
  this ADR's scope.
- Whether any *other* melonDS file transitively required by `Net_PCap.cpp`
  (deferred, §2) pulls in Npcap-specific build machinery beyond what's already
  documented was not investigated, since PCap is explicitly deferred and its
  licensing/limitations are R4's assignment.
