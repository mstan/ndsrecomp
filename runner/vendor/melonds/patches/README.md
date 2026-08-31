# runner/vendor/melonds/patches/ — required vendored-source modifications

`*.patch` files here are **already applied** to the committed sources in
`runner/vendor/melonds/`; they are retained for provenance and GPLv3 5(a)
("you must cause the modified files to carry prominent notices stating that
you changed the files") rather than applied at build/vendor time. This
mirrors `oracle/patches/`'s convention for the separate oracle build
(`oracle/patches/README.md`), applied to the runner's vendor tree instead.
To re-derive a byte-identical upstream file, reverse-apply the matching
patch here against the committed file, or re-copy from
`ndsref/third_party/melonDS` (pinned tag `1.0rc`,
`e3fa6f4224e0d706df3ee262ae41cfb0deadc593`) and diff.

## 0001-wifi-ap-identity.patch

`src/WifiAP.cpp`: changes the hardcoded AP identity from melonDS's default
(SSID `melonAP`, BSSID `00:F0:77:77:77:77`, channel 6) to this project's
identity (SSID `ndsrecomp`, BSSID `02:4E:44:53:52:01`, channel 1). The new
BSSID has the locally-administered bit set (bit 1 of the first octet) so it
can never collide with a real vendor-assigned MAC address.

## 0002-net-slirp-nonblocking-poll.patch

`src/net/Net_Slirp.cpp`: `Net_Slirp::RecvCheck()` let `slirp_pollfds_fill`
write a nonzero `timeout` into the subsequent `poll()` call, based on
libslirp's own internal TCP/DHCP timer deadlines — a host-wall-clock block
reachable from the guest-cycle-scheduled path `Wifi::USTimer -> CheckRX ->
WifiAP::RecvPacket -> Net::RecvPacket -> RecvCheck`. This violates the
project's "no host-wall-clock-driven networking on the emulation thread"
rule (see the ADR at `docs/adr-melonds-wifi-vendoring.md` §8 Finding 1).
The patch forces `timeout = 0` unconditionally, so `poll()` never blocks.

This is the mandatory half of the fix. The fuller fix — moving host packet
reception to a separate thread that only pushes immutable frames into a
queue, with the emulation thread as sole consumer — is **not yet
implemented**; the seam for it is left explicit in `runner/src/wifi_net.cpp`
(the `Platform::Net_RecvPacket` bridge, which currently calls
`Net::RecvPacket` — and therefore `Net_Slirp::RecvCheck` — synchronously
from the emulation thread).

## 0003-wifi-cpp-net-ring-hooks.patch

`src/Wifi.cpp`: adds the always-on network observability ring's (M0 floor,
`runner/src/net/net_ring.{h,cpp}`, `docs/networking-observability-plan.md`)
egress and ingress hooks that only an edit inside this file can reach,
because they live on the vendored device model's own internal state
machine (not behind any bridge-owned call site):

- `TXSendFrame`, right before the per-slot TX dispatch switch: one
  `net_ring_push(NDS_NET_EVENT_WIFI_TX_FRAME, ...)` call, capturing the
  assembled 802.11 frame's Addr1/Addr2 and length regardless of which
  slot/case consumes it.
- `FinishRX`, at the very top (before the destination-MAC/WEP/RXFilter
  checks that can `return` early): one
  `net_ring_push(NDS_NET_EVENT_WIFI_RX_FRAME, ...)` call, capturing every
  frame the hardware physically received, accepted or not. `StartRX` is
  deliberately NOT a second hook site — at that point only the frame
  length/rate are known, not the full 802.11 header, so a single hook at
  `FinishRX`'s top is both more complete and the "prefer few surgical
  hooks" choice.

Register-level access (`Wifi::Read`/`Write`) and IRQ assertions
(`NDS::SetIRQ`) are deliberately NOT patched here: `nds_wifi_read`/`write`
in `runner/src/wifi_net.cpp` are already the single funnel every bus-level
Wi-Fi access passes through, and `NDS::SetIRQ` is a project-written shim
method (`gpu3d.cpp`), so both are hooked there instead, with zero vendored
source changes needed for that part of the M0 floor.

## 0004-wifiap-cpp-net-ring-hooks.patch

`src/WifiAP.cpp`: adds the observability ring's association/state-change
and 802.11↔Ethernet-boundary hooks:

- `HandleManagementFrame`'s four `ClientStatus` transition points (assoc
  request, deassoc, auth, deauth): one
  `net_ring_push(NDS_NET_EVENT_WIFI_ASSOCIATION, ...)` call each,
  immediately after the state assignment, carrying the station's MAC
  (`data[10..15]`, 802.11 Addr2 of the received frame) and the new
  `ClientStatus` as `aux`.
- `SendPacket`, right before the `Platform::Net_SendPacket` call: one
  `net_ring_push(NDS_NET_EVENT_ETHERNET_TX, ...)` call over the unwrapped
  Ethernet frame in `LANBuffer`.
- `RecvPacket`, right after the destination/source-MAC filter checks pass
  ("packet is good"): one `net_ring_push(NDS_NET_EVENT_ETHERNET_RX, ...)`
  call, symmetric to the TX hook.

These two hooks intentionally also stand in for the observability plan's
separately named "`Net::SendPacket`/`RecvPacket` backend boundary" hook
point: in this single-instance build, `Net::SendPacket`/`RecvPacket`
(`net/Net.cpp`) are pure one-line forwards to the active `NetDriver` with
no logic of their own, reached ONLY via `Platform::Net_SendPacket`/
`Net_RecvPacket` (the bridge in `runner/src/wifi_net.cpp`), which is itself
reached ONLY from these two `WifiAP` call sites — so a second hook inside
`net/Net.cpp` would observe exactly the same event with no new
information, at the cost of a third vendored-file patch. Documented here
rather than duplicated, per "prefer few surgical hooks over scattering
calls."

## 0005-net-slirp-worker-thread-poll.patch

`src/net/Net_Slirp.h`/`.cpp`: completes the fix 0002 only started. 0002
made `RecvCheck()`'s `poll()` non-blocking so it stopped stalling on host
wall-clock, but it still ran host-socket syscalls (`slirp_pollfds_fill`,
`poll`, `slirp_pollfds_poll`) directly on the emulation thread, and
`SendPacket()`'s DNS-synthesis path (`HandleDNSFrame`) still ran a
genuinely blocking `getaddrinfo()` there too -- both are host-scheduling-
jitter/host-wall-clock hazards inside the guest-cycle-scheduled Wi-Fi tick
path (plan §18/§19), independent of whether any individual `poll()` call
blocks.

This patch splits `RecvCheck()`'s old body out into a new method,
`PollHostSockets()`, and makes the `RecvCheck()` override (the one
`Net::RecvPacket` — vendored, not owned by this pass — calls
unconditionally on whatever thread calls `Net::RecvPacket`, always the
emulation thread here) a true no-op. `PollHostSockets()` is not part of
the `NetDriver` interface; it is called exclusively by a dedicated host
networking worker thread owned by `runner/src/wifi_net.cpp`, which is
also the sole caller of `SendPacket()` (drained from a queue on that same
thread), so `Ctx`/`PollList` are never touched from two threads at once
and need no additional locking inside this class. See the design comment
above `WifiBridgeState` in `runner/src/wifi_net.cpp` for the full
architecture: two bounded, mutex-guarded packet queues are the only
channel between that worker thread and the emulation thread; the
emulation thread never calls into libslirp or a host socket API directly
after this patch.

0002's mandatory `timeout = 0` clamp is retained verbatim inside
`PollHostSockets()` — see that function's comments for why (bounded,
predictable worker-thread shutdown latency, not a guest-timing concern
anymore since this runs off the emulation thread).

## 0006-net-slirp-configurable-nameserver.patch

`src/net/Net_Slirp.h`/`.cpp`: adds an optional `nameserver_ipv4_host_order`
constructor parameter to `Net_Slirp`, defaulting to 0 (preserving upstream's
own behavior — the internal fake `kDNSIP` address, whose DNS traffic
`SendPacket()` intercepts and answers locally via `HandleDNSFrame()`'s
`getaddrinfo()` synthesis, never touching a real socket). This is the seam
the project's configurable WFC DNS provider (`runner/src/wifi_net.cpp`'s
`NdsNetworkOptions`, `docs/wiimmfi-runbook.md`) uses: when a provider is
configured, the runner passes that provider's real DNS address here instead
of 0. Once the guest is told (via DHCP option 6) to use that REAL external
address rather than the fake `kDNSIP`, `SendPacket()`'s
`dst == kDNSIP` special case no longer matches such packets, so they fall
through to the ordinary `slirp_input()` NAT path and are genuinely
forwarded over the host's network to the configured address — exactly how
a physical DS reaches a manually- or DHCP-configured WFC DNS server. No
other vendored logic changes: `HandleDNSFrame`'s local-synthesis path is
untouched and still the code path used to prove "ordinary" (non-WFC) DNS
resolution end-to-end.

This is the only call site (`runner/src/wifi_net.cpp`'s
`nds_wifi3d_attach()`) that constructs a `Net_Slirp`, and it always passes
an explicit value (0 when the WFC provider is disabled), so the default
argument only matters for signature compatibility, not for any implicit
runtime behavior change at that call site.

Also bumps `cfg.version` 1 -> 3 and sets `cfg.disable_dns = true`
unconditionally. This was NOT part of the original design and was added
after direct measurement caught a second interception layer: libslirp
itself has a built-in DNS proxy (`SlirpConfig::disable_dns`, gated behind
`SlirpConfig.version >= 3` per `libslirp.h`) that silently answers ANY
UDP:53-destined packet using the HOST's own resolver, regardless of
destination address, when left at its default (`false`). With
`cfg.version` at its original `1`, `disable_dns` was invisible to slirp
even though the struct field existed and was zero-initialized -- the
version number itself gates which fields slirp reads. Symptom: a guest
query sent to a configured WFC provider address (e.g. Kaeru,
`178.62.43.212`) came back answered by a completely different server
(confirmed by directly querying `178.62.43.212:53` from the host outside
the emulator and comparing the response against what the guest actually
received through slirp -- different response size, different source IP).
Setting `disable_dns=true` is safe unconditionally: the unmodified default
path (`nameserver_ipv4_host_order == 0`) never reaches this proxy layer at
all, because `SendPacket()`'s own `dst == kDNSIP` check intercepts and
answers via `HandleDNSFrame()` before `slirp_input()` is ever called.

## 0007-net-slirp-poll-error-observability.patch

`src/net/Net_Slirp.h`/`.cpp`: `PollHostSockets()` (the worker-thread body
split out by `0005`) called `poll()`, then passed only `res<0` as a bare
bool into `slirp_pollfds_poll` — the actual OS error code (`EBADF`/`EINTR`
on POSIX, `WSAENOTSOCK`/etc. on Windows) was read nowhere and discarded.
A genuine host-socket `poll()` failure was therefore completely invisible,
both to a human reading logs and to this project's own always-on network
observability ring (`net_ring.h`'s `NDS_NET_EVENT_BACKEND_ERROR`, defined
since the M0 floor but never actually pushed from anywhere until this
patch).

Adds two atomics to `Net_Slirp` (`PollErrorCount`, `LastPollErrorCode`) and
a `TakePollErrorCount()`/`LastPollError()` accessor pair. `PollHostSockets()`
increments/records on every `poll() < 0`. This can't call `net_ring_push`
directly — `PollHostSockets()` runs on the dedicated worker thread
(`runner/src/wifi_net.cpp`), and `net_ring_push` is not thread-safe by
design (every ring in this runner assumes a single emulation-thread
writer) — so it uses the same atomic-counter-handoff idiom
`runner/src/wifi_net.cpp` already uses for `rx_queue` drops
(`rx_dropped_since_last_report`): the emulation thread's next
`Net_RecvPacket` call drains both fields and pushes the actual
`NDS_NET_EVENT_BACKEND_ERROR` ring event. Purely additive (new members,
new methods, one new `if` block); no existing control flow changed.

## 0008-net-slirp-skip-empty-poll.patch

`src/net/Net_Slirp.cpp`: `PollHostSockets()` called `poll()` unconditionally.
Upstream's own `if (PollListSize > 0)` guard is commented out in the pinned
source, and `PollListSize` is reset to `0` immediately before
`slirp_pollfds_fill()`, so whenever libslirp had no active socket to offer,
this reached `poll()` with `nfds == 0` — which Windows' `WSAPoll()` (what
this platform's `poll()` resolves to) is documented to fail with
`WSAEINVAL`.

That made `0007`'s brand-new `NDS_NET_EVENT_BACKEND_ERROR` channel useless
the moment it started working. Measured on one real MKDS WFC login:
**8170 poll "failures", every one of them `WSAEINVAL` (10022) and every one
benign**, 8094 of which coalesced into a single ring event. The burst's own
timeline proves the cause — the last error lands at guest cycle
`4113489534` and the guest's first DNS query at `4113966067`, i.e. the
errors stop the instant the guest opens its first socket and
`PollListSize` becomes nonzero. A genuine `poll()` failure would have been
indistinguishable from that flood.

Fix: skip only the syscall when `PollListSize == 0`, leaving `res` at `0`
("no error"). `slirp_pollfds_poll()` is still called unconditionally, so
libslirp's internal timers keep advancing exactly as before — the
no-descriptor case simply reports no error instead of a fabricated one.
No behavior change in the has-descriptors path.

## Reverse-applying / verifying a patch

Every patch here has been verified to reverse-apply cleanly and
byte-identically — but **this is a stack, not a set of independent patches.**
Five patches touch `net/Net_Slirp.{h,cpp}`: `0002` clamps the `poll()`
timeout inside `RecvCheck()`, `0005` later moves that whole body out to
`PollHostSockets()`, `0006` adds the configurable-nameserver constructor
parameter on top of both, `0007` adds the poll-error observability counters
on top of all three, and `0008` (newest) guards the `poll()` call itself on
top of all four. So `0002` no longer matches the committed file on its own;
it only reverse-applies once `0008`, `0007`, `0006`, and `0005` have all
been reversed first.

**Reverse in descending order (LIFO): 0008, then 0007, 0006, 0005, 0004,
0003, 0002, 0001.** `0008`'s own reverse-apply was verified 2026-08-11 by a
full round trip with `patch -R -p2` followed by a forward re-apply,
returning `net/Net_Slirp.cpp` to sha256
`f6ed3ae542f7db4f8dcb34723989b81ae6e3e254e1c23bfd23beafcb58e419db`
byte-identically, with the reversed intermediate confirmed equal to the
independently reconstructed pre-`0008` state
(`ae756b611a34ff8775d554bbc81af584e5b3d7abec0e2501219e1cab5d1b48eb`).
Note that `git apply` cannot be used for this in the `claude/wiimmfi`
worktree layout — the worktree's gitdir pointer does not resolve under Git
Bash's `git`, which fails with `fatal: not a git repository` and silently
does nothing, making a naive sha comparison falsely report success. Use
`patch(1)` instead, as above. `0007`'s own reverse-apply was verified
2026-08-10 with
`git apply --check -R -p2 patches/0007-*.patch` (clean) plus a full
round-trip (`-R` then forward-apply again) producing byte-identical
`Net_Slirp.h`/`.cpp`, confirmed against the files captured immediately
before `0007` was written (96 and 531 lines respectively, matching the
pre-`0007` state exactly). The `0001`-`0006` stack was verified
2026-08-10 by unwinding the full stack in a scratch copy:
`net/Net_Slirp.{h,cpp}` matched the pinned upstream tree
(`ndsref-wiimmfi/third_party/melonDS/src/net/Net_Slirp.{h,cpp}`) exactly
after unwinding `0006`, `0005`, and `0002` (`0001`/`0003`/`0004` don't touch
these two files). `Wifi.cpp`/`WifiAP.cpp` were also unwound
(`0004`/`0003`/`0001`) but do NOT match that same reference tree byte-for-
byte — inspection showed the reference copy carries its own unrelated
`MELONDS_ORACLE_HOOKS` instrumentation (the `ndsref` oracle's own
melonDS vendoring, not this runner's), so that particular comparison
target is not a valid pristine baseline for those two files; it does not
indicate a problem with patches 0001/0003/0004 themselves, and re-deriving
against the true pinned upstream commit (`e3fa6f4224e0d706df3ee262ae41cfb0deadc593`)
remains the authoritative check per the header note above. Applying forward
uses ascending order for the same reason. This matters on a future melonDS
pin bump: reversing `0002` before `0006`/`0005` will fail, and that failure
is expected, not a corrupt patch.

From `runner/vendor/melonds/` (this directory's parent):

```
git apply --check -R -p2 patches/000N-*.patch   # dry run
git apply -R -p2 patches/000N-*.patch           # actually reverts

# full unwind to pristine upstream (descending):
for p in 0008 0007 0006 0005 0004 0003 0002 0001; do git apply -R -p2 patches/$p-*.patch; done

# ...but see the git-apply caveat above: in the claude/wiimmfi worktree
# layout git apply resolves no repository and does nothing. patch(1) works:
for p in 0008 0007 0006 0005 0004 0003 0002 0001; do patch -R -p2 -i patches/$p-*.patch; done
```

(`-p2` strips the patch's `a/src/`, `b/src/` prefixes down to the bare
filename, which then resolves against this flattened vendor tree — melonDS
itself has a `src/` layer this repo does not reproduce for the top-level
files, though it does for `net/`, which is why `net/`-rooted patches use
the same `-p2` convention against `a/src/net/...`.)

## 0009-gpu3d-host-adaptive-render-width.patch

`src/GPU3D.{h,cpp}`, `src/GPU3D_Soft.{h,cpp}`: the host-only adaptive
output width. Upstream fixes the 3D raster at 256 pixels; these files gain
`SetRenderWidth`/`GetRenderWidth`, widen `ScrolledLine` to 448, add the
`GetAttrLine` attribute surface the adaptive skybox repair consumes, and
replace the soft rasterizer's hardcoded 256 spans/stencil strides with the
configured width. `SoftRenderer::SetupRenderThread` additionally resets
`RenderedScanlines` and the scanline-count semaphore before posting the
start semaphore, without which a restarted frame can observe a stale count.

The hardware-visible viewport and every guest-facing register are
unchanged; 256 remains the exact native path.

**These changes predate this patch file.** They were applied when the
adaptive widescreen work landed but were never recorded here, and
`THIRD_PARTY_ATTRIBUTION.md` described these files as byte-identical to
upstream. Both are corrected as of 2026-08-16.

## 0010-gpu3d-compute-adaptive-width-attributes-and-internal-resolution.patch

`src/GPU3D_Compute.{h,cpp}`, `src/GPU3D_Compute_shaders.h`: three related
changes to the optional compute renderer.

1. **Adaptive width.** `ScreenWidth` becomes `RenderWidth * ScaleFactor`
   rather than `256 * ScaleFactor`, and the low-resolution framebuffer plus
   its pixel-pack buffer are allocated from `RenderWidth` at settings time
   instead of at a fixed 256 during construction.
2. **Polygon-ID attributes.** The final pass smuggles the six-bit polygon ID
   into the two spare high bits of each six-bit colour channel of the
   low-resolution surface, so the runner's adaptive skybox repair can
   identify sky draws without a second readback.
3. **Internal resolution accessors.** `GetHiResTexture()` and
   `GetScaleFactor()` expose the already-allocated high-resolution render
   target and the active scale, which the HD presenter composites from. No
   upstream behaviour changes: the same final pass already wrote both
   surfaces every frame.

As with 0009, items 1 and 2 predate this file and were previously
undocumented; item 3 is new in the internal-resolution work.

## 0011-texcache-optional-texture-upscaling.patch

`src/GPU3D_Texcache.h`, `src/GPU3D_TexcacheOpenGL.cpp`: routes decoded
textures through the optional upscaler in
`runner/src/melonds_compute/TextureUpscale.{h,cpp}` (project-owned, MIT
xBR-lv2 derived; deliberately not placed in this vendored tree).

- `TexcacheOpenGLLoader::GenerateTexture` allocates array storage at the
  upscale factor. The renderer samples with normalized coordinates
  (`uvf = ivec2(u,v)/16 * InvTextureSize`, with `InvTextureSize` derived
  from the DS texture's own dimensions), so a larger backing store changes
  which texel a coordinate lands on but not the coordinate itself; texel
  addressing and the DS wrap/flip sampler modes are unaffected.
- `TexcacheOpenGLLoader::UploadTexture` dispatches the filter into the
  target layer, falling back to the plain native upload when upscaling is
  inactive or the dispatch fails.
- `Texcache::GetTexture`'s layer budget divides by the factor squared. The
  8 MiB-per-array budget is computed from the native texture size, and
  without this a 1024x1024 array at 4x would request 64 MiB per layer
  across up to 64 layers.

Factor 1 restores the exact upstream allocation and upload path.

## 0012-openglsupport-silence-shader-cache-error.patch

`src/OpenGLSupport.cpp`: removes an unconditional
`Log(LogLevel::Error, "Shader %s from cache was rejected")` on the normal
source-compilation path. Upstream's program-binary cache lookup is
commented out in this import, so every shader compiled -- all 33 of the
compute renderer's -- logged an error while behaving correctly. Also adds
the missing trailing newline at end of file.

No behaviour change beyond the log line.

## 0013-wifiap-cpp-association-hook.patch

`src/WifiAP.cpp`: adds one runner-owned hook call,
`::nds_wifi_on_client_associated()` (runner/src/wifi_net.{h,cpp}), at the
point the virtual AP accepts an association request (ClientStatus -> 2).
The runner uses it to zero a title-configured guest CRT `errno` word
(game.toml `[network.wfc] clear_crt_errno_addr`), fixing in-game error
52200 on every Nintendo WFC reconnect after the first within one boot:
Nintendo's DWC connection-test thread checks `errno == ERANGE` after
`atoi()` without clearing errno first, and modern WFC replacement services
send numeric login fields that overflow the guest's 32-bit strtol, leaving
errno = ERANGE for the rest of the boot (beads-lqa.8; same bug is visible
in stock melonDS 0.9.5 -- kuribo64 board thread 1399). No behavior change
when the knob is unset.

## 0014-gpu3d-compute-memory-barriers.patch

`src/GPU3D_Compute.cpp`: corrects the four compute-renderer
`glMemoryBarrier()` calls to use
`GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT`. The old
`GL_SHADER_STORAGE_BUFFER` argument is a buffer binding target, not a
memory-barrier bitfield. The command barrier makes shader-written indirect
dispatch parameters visible to `glDispatchComputeIndirect`; the correction
matches upstream melonDS commit `77774538e56b118dcb5d64f08d784542ba77c72b`.

## 0015-gpu3d-compute-texture-uniform-guard.patch

`src/GPU3D_Compute.cpp`: updates `InvTextureSize` only for raster variants
that actually bind a texture. On affected Intel OpenGL drivers, updating this
uniform while the no-texture raster program is current can produce
`GL_INVALID_OPERATION`, closing the runner after the first rendered frames.
The shader only reads `InvTextureSize` inside `#ifdef UseTexture`, so this
does not change textured variant sampling or no-texture rendering semantics.

## 0016-gpu3d-guest-wide-projection.patch

`src/GPU3D.{h,cpp}`: adds `GuestWideProjection`, a host mode for titles whose
guest code has itself been patched to build band-wide frusta
(`runner/src/title_patches.cpp` asserts it per frame, only while its guest-side
words are verified resident). While set:

- `SubmitVertex` skips `0009`'s clip-X rescale. That rescale and a widened
  guest frustum are two spellings of the same widening; applying both compounds
  the horizontal FOV.
- `SubmitPolygon` maps EVERY viewport linearly across `RenderWidth` rather than
  only the full-screen one. Guest screen X `0..256` now means the whole band, so
  a partial viewport — the per-room sub-viewports adjacent-room portal rendering
  emits — has to be scaled the same way the full one is, or its geometry hard
  clips at the native 256-pixel edge. The end is computed from
  `Viewport[0] + Viewport[4]` rather than from a separately scaled width so that
  back-to-back viewports still tile without a seam.

Cleared (the default), both sites behave byte-for-byte as `0009` left them, so
this is inert for every title that does not opt in.

## 0017-savestate-index-validation.patch

`src/FIFO.h` and `src/GPU3D.cpp`: reject out-of-range FIFO metadata,
geometry pointer indexes, polygon vertex counts, RAM-bank selectors, and render
list counts while loading a vendored save-state stream. Upstream's serializer
represents pointers as indexes but trusted those indexes before the runner
could semantically validate the detached device. These checks do not change
the stream format or any valid state; they make a checksum-recomputed corrupt
`GP3D` section fail before it can construct or dereference an invalid pointer.
