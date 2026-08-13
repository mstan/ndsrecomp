# M8: packet capture/replay at the Ethernet backend boundary

Owner: I13 (Wiimmfi meta epic). Status: implemented, built, and proven
end-to-end against a real DHCP session on 2026-08-10/11. This document is
the authoritative design record; see the code's own doc comments
(`runner/src/net/net_capture.h`, `net_sanitize.h`, `net_replay.h`) for the
line-level rationale this doc summarizes.

## Why this exists

M0-M5 boot Mario Kart DS to real NAS auth against Kaeru, but every one of
those results currently depends on live Internet services to reproduce.
That is not a viable regression story (Kaeru is a free volunteer-run
service; endpoints change; network timing varies run to run). M8's answer:
record a real session once at the Ethernet backend boundary
(`Platform::Net_SendPacket`/`Net_RecvPacket` in `runner/src/wifi_net.cpp`),
then replay it deterministically with host networking disabled entirely.

## 1. Capture format: `NDSNETREPLAY1`

**Files:** `runner/src/net/net_capture.{h,cpp}`, `net_pcap.{h,cpp}`.

A fixed binary file header (`NdsNetCaptureFileHeader`, 141 bytes: magic,
format_version, header_size, sanitized flag, created_unix_time, rom_sha1,
scenario label) followed by a flat stream of records
(`NdsNetCaptureRecordHeader`: `guest_cycle` (u64), `len` (u32), `direction`
(u8) + reserved) each immediately followed by `len` raw Ethernet frame
bytes.

**Why binary-with-a-readable-magic instead of the task's own text-log
sketch** (`NDSNETREPLAY1` with `cycle=... dir=RX len=...` lines): the
magic/version string is kept exactly as sketched (readable in a hex dump
or `strings` output), but the per-record payload is raw Ethernet bytes,
which are not safely representable as one text line without a base64-style
encoding tax this project doesn't otherwise pay anywhere. Three fixed
binary fields carry exactly the same information as three `key=value`
tokens, with zero encoding overhead.

**No record count in the header.** A writer killed mid-session (this
project's documented shutdown convention is `taskkill`, not a clean quit)
never gets to seek back and patch a count. Instead `Write()` calls
`fflush()` after every record, so the file on disk is always "N complete
records" for whatever N was reached, and the reader (`ReadAll`) simply
reads until a clean EOF. This makes "file ends between records" (a normal
kill) and "file ends mid-record" (real corruption/truncation)
unambiguously distinguishable — see the mismatch-reporting proof below.

**Corruption/truncation are reported, not silently absorbed.**
`NdsNetCaptureReader::ReadAll` checks, per record: a short header read, an
implausible `len` (> `kNdsNetCaptureMaxFrameBytes` = 2048, mirroring
`wifi_net.cpp`'s own backend send/recv limit), an invalid direction byte,
and a short payload read. Each failure names the exact record index and
byte counts. `main.cpp`'s CLI validation calls this *before* the emulator
ever starts (see §4), so a bad `--net-capture-in` file is a startup-time
`exit(2)`, never a mid-run surprise.

**pcap alongside, not instead of.** `NdsNetCaptureWriter` also writes a
classic-pcap sibling (`<path>.pcap`, LINKTYPE_ETHERNET) by default —
Wireshark-openable, a large diagnostic win for later NATNEG/TCP work. It is
diagnostic-only: pcap's native per-packet timestamp is a (seconds,
microseconds) pair, which cannot exactly carry an arbitrary 64-bit guest
cycle without lossy scaling, so replay reads *only* the native file, never
the `.pcap`. `guest_cycle` is converted via the ARM7 Wi-Fi timer's own base
rate (33513982 Hz) purely for human-readable relative ordering.

Classic pcap (not pcapng) was chosen for simplicity: a flat Ethernet
capture with no per-packet comments or multiple interfaces doesn't need
pcapng's block/TLV framing, and classic pcap is materially simpler to
hand-write correctly.

## 2. ReplayBackend (`melonDS::NetReplay`)

**Files:** `runner/src/net/net_replay.{h,cpp}`.

A NetDriver **sibling** implementation (`runner/vendor/melonds/net/
NetDriver.h`, included read-only, never modified) alongside the existing
`Net_Slirp` and `Net_PCap`. No vendor patch was needed — everything lives
in new project-owned files under `runner/src/net/`.

- **Construction** takes the already-loaded, already-validated
  `std::vector<NdsNetCaptureRecord>` (see §4 — main.cpp loads and
  validates the file, `NetReplay`'s constructor never touches a file) plus
  a `Platform::SendPacketCallback` (the same type `Net_Slirp` takes, so
  `wifi_net.cpp` wires either backend through an identical lambda shape).
- **No worker thread.** Unlike `Net_Slirp`, replay never touches a host
  socket or libslirp, so `nds_wifi3d_attach()` skips starting the
  background thread entirely for this backend — `SendPacket`/`RecvCheck`
  run synchronously, directly on the emulation thread.
- **`SendPacket` (guest TX)** compares the live frame against the next
  expected recorded TX record, in order. On any divergence it latches the
  **first** mismatch only (matching this project's "always debug the
  first divergence" doctrine) and reports: TX frame ordinal, guest cycle,
  ARM9/ARM7 PC (from `scheduler_cpu_state(0|1).R[15]`, populated by
  `wifi_net.cpp` before every call), expected bytes, actual bytes, and a
  human reason (`"length mismatch: ..."` or `"byte mismatch at offset N:
  expected 0x.., actual 0x.."`). The guest is never stalled or denied — a
  mismatch is *reported*, never enforced as a hang.
- **`RecvCheck` (host RX)** delivers every recorded RX record whose
  `guest_cycle` has been reached, via the callback — quantized exactly
  like a live host packet: `RecvCheck` is reached only from
  `Wifi::CheckRX`'s guest-cycle-scheduled poll, so a recorded frame
  becomes guest-visible only once a real guest tick asks for it, never on
  its own schedule.
- **Causal gate (`rx_required_tx_`).** Recorded `guest_cycle` alone is
  *not* a sufficient delivery gate — see the "bugs found and fixed" below.
  An RX record is held back until the guest has sent at least as many TX
  frames as causally preceded it in the original recording, regardless of
  whether its cycle has technically passed.
- **Sanitize-aware comparison.** When the loaded capture's header says
  `sanitized=1`, `SendPacket` sanitizes a **scratch copy** of the live
  frame with its own `NdsNetSanitizeState` before comparing — never the
  frame it actually accepts. This is sound (not just convenient) because
  the sanitizer's identity mapping is deterministic-by-hash of the
  console's real MAC, so a fresh state re-derives byte-identically the
  same synthetic value the capture was written with, given the same real
  MAC (which the guest, booting deterministically, reproduces exactly).
  `RecvCheck` uses the **same** state's learned identity to reverse the
  mapping on RX frames before delivery (see §3).

### Query surface

`bool nds_wifi_replay_status(NdsNetReplayStatus*)` (wifi_net.h) and the new
debug-server command `net_replay_status` (mid-session, live — see below)
expose: active/mismatch flags, TX matched/total, RX delivered/total, and
the full mismatch detail when present. Added specifically so a replay's
outcome can be queried **while the session is still running**, matching
this project's always-on-ring philosophy (`DEBUG.md`: query live state,
never arm-then-capture) rather than waiting for process exit.
`main.cpp`'s end-of-run path also prints a PASS/FAIL summary in both
`--serve` and one-shot batch modes.

### `--network-backend` wiring

`slirp` (default) and `replay` are both fully constructed by
`nds_wifi3d_attach()`. `pcap` is accepted by `nds_parse_network_backend`
(pre-existing) but deliberately **left unwired**, exactly as before this
milestone — see `docs/adr-melonds-wifi-vendoring.md` §2. New flags:
`--net-capture-out FILE`, `--net-capture-in FILE`, `--net-capture-raw`,
`--net-capture-no-pcap`, `--net-capture-scenario NAME`; TOML mirrors under
`[network.capture]`.

## 3. Sanitizer

**Files:** `runner/src/net/net_sanitize.{h,cpp}`, ported line-for-line to
`tools/net_capture_tool.py`.

### Design: rewrite exactly ONE learned identity

The mapping strategy is **identity-based**, not "rewrite every MAC-shaped
field": the console's own real MAC is **learned** from the Ethernet source
of the first TX-direction (guest→host) frame — by construction, the only
address guaranteed to be the console's own identity. Once learned, every
occurrence of that *exact* 6-byte value anywhere in any frame (Ethernet
header, ARP payload, BOOTP `chaddr`, DHCP option 61 when MAC-shaped) — in
either direction — is rewritten to one stable synthetic substitute
(locally-administered bit set, so it can never collide with a real
vendor-assigned address). DHCP option 12 (hostname) is separately replaced
with a same-length synthetic ASCII string. The synthetic value is derived
by **hashing** the real bytes (FNV-1a 64-bit), not by insertion order —
this makes the same real MAC map to the same synthetic value even across
separate capture files or between the C++ and Python implementations,
with no shared state file, which is exactly what lets the replay-side
comparison (§2) and RX desanitize (below) independently re-derive the
identical mapping from a fresh state.

IPv4 addresses, ports, and timestamps are never touched — they're assigned
by this project's own virtual AP/DHCP server, not the owner's identity,
and rewriting them would break the very "same lease" proof this milestone
exists to make.

### Two real bugs found and fixed by direct execution (not theorized)

**Bug 1 — RX desanitize timing (causal ordering).** A DHCP client
validates that a `DHCPOFFER`/`DHCPACK`'s echoed `chaddr` matches its own
real hardware address. The very first replay attempt delivered the
recorded ACK *before* the guest had sent its own REQUEST (because
`RecvCheck`'s original "deliver everything whose recorded cycle has
passed" logic doesn't wait for the correlated TX to have actually
happened — replay's own execution pacing can drift slightly from the
original capture's, e.g. because RX delivery is now synchronous instead
of cross-thread-queued). The guest's DHCP client rejected the
out-of-order exchange and retried DISCOVER indefinitely. **Fix:** the
causal gate (`rx_required_tx_`, §2) — an RX record is never delivered
until the guest has sent every TX frame that causally preceded it in the
original recording.

**Bug 2 — rewriting non-identity MACs breaks protocol filtering.** An
earlier version of the sanitizer rewrote *any* 6-byte value found at a
recognized MAC-shaped field position, including the Ethernet **broadcast**
destination (`FF:FF:FF:FF:FF:FF`, used by a `DHCPOFFER` before the client
has an IP) and this project's own virtual DHCP-server MAC. Sanitizing
broadcast into a synthetic-looking-but-arbitrary address made melonDS's
own `WifiAP::RecvPacket` destination-address filter silently drop the
frame *before* the guest's IP stack ever saw it — invisible at the
passive observability ring (which hooks one layer earlier, at the
`Net_SendPacket`/`RecvPacket` boundary, so the ring still showed "OFFER
received") but fatal to the guest's actual protocol state. **Fix:** the
identity-based redesign above — since only the one learned identity MAC
is ever rewritten, broadcast/multicast and this project's own
infrastructure MACs are never touched in the first place; no special-case
guard was even needed once the design was corrected.

Both bugs were caught by first proving the **raw** (unsanitized) capture
replays perfectly byte-for-byte (isolating "is the core replay design
correct" from "is the sanitizer correct"), then adding sanitization back
and diagnosing the *specific* divergence each time from the live
`net_replay_status`/`net_ring_dump` evidence — never patched speculatively.

### Making the safe path the default (defense in depth)

1. **Sanitize-by-default at the point of capture.** Every
   `NdsNetCaptureWriter` construction site defaults `sanitize=true`;
   `--net-capture-raw` is the one explicit opt-out, and using it prints a
   loud stderr warning naming the file and telling the operator to run the
   sanitizer tool before publishing.
2. **A hard refusal at the point of publishing**, independent of #1:
   `tools/net_capture_tool.py publish IN DEST` reads `IN`'s header and
   refuses (nonzero exit, `DEST` untouched) unless `sanitized=1`. This is
   the "tooling refuses to write a fixture into a tracked path unless it
   is sanitized" mechanism, and it does not trust the writer's own
   sanitized flag having been set correctly by some other path — it
   re-checks independently at the point of publication.

Both layers exist deliberately (not just one) — a capture-time bug could
in principle produce a mis-flagged file, and a publish-time gate is the
last line of defense before anything reaches a path a commit could pick
up.

### Deliberately out of scope

TLS-layer identifiers (any console ID exchanged during real NAS auth)
cannot be rewritten by a frame-level sanitizer — by the time such a value
is on the wire it is inside an encrypted TLS record. This is not a gap in
execution, it's a structural limit: avoiding capture of that material
requires not capturing/committing that exchange at all (this project's
pre-existing session-random / never-record policy,
`docs/networking-observability-plan.md` §(g)), which is exactly why this
milestone's own capture/proof deliberately stops before the WFC-match/NAS-
login steps (see §5) rather than sanitizing them after the fact.

Non-MAC-shaped DHCP option 61 values are also left untouched (deliberate
narrowing from an earlier, buggier version that guessed at rewriting
arbitrary opaque bytes): this project's own DHCP client only ever produces
the standard `htype=1 + 6-byte-MAC` shape (confirmed by direct capture),
and guessing at "which bytes of an arbitrary opaque identifier are the
sensitive part" without that structure reintroduces the same class of
protocol-breaking mis-rewrite bug 2 above already demonstrated.

## 4. CLI validation flow (`main.cpp`)

`--network-backend replay` requires `--net-capture-in`. The file is opened
and **fully parsed** (`NdsNetCaptureReader::Open` + `ReadAll`) during CLI
argument validation, before `nds_wifi_configure_network()` is even called
— a corrupt/truncated capture is therefore an `exit(2)` CLI error with a
message naming the exact record, identical in shape to every other
malformed-input case in this file, never a runtime surprise discovered
mid-boot. The header's `sanitized` flag is threaded through
(`NdsWifiNetworkConfig::replay_sanitized`) so `NetReplay` knows whether to
run its comparison in sanitized space (§2/§3).

## 5. End-to-end proof

**Driver:** `tools/m8_dhcp_capture_drive.py` — reuses
`oracle/mkds_wfc_scenario.py`'s own navigation constants and helper
functions (`tap`, `press_a`, `verify_screen`, `wait_for_connection_test_
result`, coordinate constants) via import, replicating that file's own
steps **verbatim** up to and including the `connection_test_settled`
checkpoint, then stopping — deliberately never reaching the WFC-match/NAS-
login steps that follow in that file, which contact the real Kaeru
service. (`mkds_wfc_scenario.py` itself was not edited — I12 owns it and
is concurrently making its navigation more deterministic.) DHCP
(association + DORA) completes well before that point and is fully local:
melonDS's virtual AP + libslirp's built-in DHCP server, no Internet
required.

**Capture → sanitize (default) → replay, same lease:**

```
Live capture (--network-backend slirp --net-capture-out ...):
  DHCP DISCOVER -> OFFER -> REQUEST -> ACK, yiaddr=10.64.0.16, server=10.64.0.1
  connection_test_settled reached at vblank9=4300

Replay (--network-backend replay --net-capture-in <same sanitized file>):
  net_replay_status: {"mismatch": false, "tx_matched": 25/25, "rx_delivered": 22/22}
  DHCP DISCOVER -> OFFER -> REQUEST -> ACK, yiaddr=10.64.0.16 (IDENTICAL)
  connection_test_settled reached at vblank9=4300 (IDENTICAL)
  host networking fully disabled (no slirp, no libslirp, no sockets constructed at all)
```

Independently confirmed the sanitized capture never contains the real
MAC (`tools/net_capture_tool.py dump` + a direct byte comparison against a
parallel raw capture of the same session).

**Corruption/truncation reporting fires and names the first divergence
(live, on a real file — not only the unit test):**

- Flipped one byte inside the recorded DISCOVER frame's DHCP payload of
  the real sanitized capture, then replayed it:
  `net_replay_status` → `{"mismatch": true, "mismatch_tx_frame_index": 0,
  "mismatch_reason": "byte mismatch at offset 100: expected 0xFF, actual
  0x00 (compared post-sanitize)"}` — the guest visibly never completed the
  WFC connection test (times out in "running"), exactly as expected for a
  genuinely broken exchange.
- Truncated the same real capture (cut the last 50 bytes, mid-record):
  `nds_runner --network-backend replay --net-capture-in <truncated>`
  refused to start at all — `exit(2)`, `"truncated record 46: expected
  356 payload bytes, got 306"`.

**Regression gate**, confirmed unchanged on the build carrying every M8
change (isolated build directory, see "environment note" below):
`vblank9=120 vblank7=120 ipcsync_w=211 spi_w=152359`; network ring
`438` Wi-Fi register events, `0x815E`(`W_BB_BUSY`)=214, `0x8158`
(`W_BB_CNT`)=107, `0x815A`(`W_BB_WRITE`)=106, first event `wifi_reg=
0x8036`(`W_POWER_US`). All exact matches.

**Unit tests** (`runner/tests/net_capture_test.cpp`, wired into
`runner/CMakeLists.txt`): format round-trip (including the pcap sibling),
sanitizer determinism across fresh states, UDP checksum recompute
verified against an independent inline implementation, broadcast-MAC
non-rewriting, identity consistency across TX/RX in one writer session,
and both the truncation and corrupt-length detection paths. All pass.

## 6. Environment note: isolated build directory

Mid-session, several `nds_runner` launches collided with **sibling
agents'** own concurrently-running servers in the same shared
`runner/build-wiimmfi/` binary and the default port 19842 — including one
episode where a stale/sibling process on that port fabricated phantom
DHCP evidence before this was diagnosed. Once identified, all further
work in this session moved to a dedicated build directory
(`runner/build-wiimmfi-i13/`, same CMake configuration) and a dedicated
port (19847), with every process kill scoped to a specific PID this
session itself launched and verified via `Get-CimInstance ... | Select
CommandLine` — never a kill by process name. The `build-wiimmfi-i13`
directory and its contents are build output (gitignored, like every other
`build-*` directory in this tree) and are not part of the deliverable;
`runner/CMakeLists.txt` is the actual source change, buildable into either
directory identically.

## 7. Unresolved / not verified by execution

1. **Periodic self-initiated outbound traffic (heartbeats) is not
   handled.** A sibling agent's live session against real Kaeru observed
   a UDP heartbeat roughly every 5-6 seconds once past NAS login. The
   current `SendPacket` comparison expects TX frames in **strict recorded
   order** with no tolerance for a heartbeat landing at a different
   relative position — a longer capture spanning that phase would need
   either (a) classifying heartbeats as a separate, order-insensitive
   comparison class, or (b) tolerating cycle-based reordering within a
   bounded window. DHCP itself is short and fully local and is
   unaffected, but this is a real gap for anything past the acceptance
   bar. Not designed, let alone implemented, in this pass.
2. **TLS-layer console identifiers are out of scope for the sanitizer**
   (see §3) — mitigated only by not capturing that phase at all in this
   milestone's own proof, not solved generally.
3. **DNS replay was not additionally proven.** The task named DHCP as the
   acceptance bar and DNS as a nice-to-have; time went to finding and
   fixing the two sanitizer bugs above instead. The mechanism (capture
   writer/reader/replay driver) is protocol-agnostic — nothing here is
   DHCP-specific — but a DNS-specific capture-then-replay run was not
   executed.
4. **`rom_sha1` is never populated** in captures taken via the CLI
   (`NdsWifiNetworkConfig::rom_sha1` stays empty) — the ROM's SHA-1 is
   computed later in `main.cpp`'s boot sequence than the network config is
   resolved and handed off. Low-value field (provenance only, not
   correctness-affecting); left blank rather than restructuring boot
   order under time pressure.
5. **`pcap` is separate M7 live-backend work.** Capture/replay does not
   depend on the live pcap backend.
6. **A capture that legitimately starts with an RX frame before any TX**
   (not possible for this project's own DHCP-first flows, but not
   structurally forbidden by the format) would have no MAC-shaped field
   rewritten in that leading RX frame, since identity is learned only
   from a TX source. Documented in `net_sanitize.h`; not exercised.
7. **Multi-hour or multi-session capture files** (e.g. a full NAS login
   + gameplay session) were not exercised — file sizes, ring capacity
   interaction, and sanitizer cache growth for a much longer capture are
   unverified, though nothing in the design is expected to scale poorly
   (the sanitizer's caches are keyed by a handful of distinct values per
   session, not per-record).
