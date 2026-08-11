# Networking observability plan

Design-only. No implementation code in this change. Every claim below is
cited `file:line` against the `ndsrecomp-wiimmfi` worktree (branch
`claude/wiimmfi`) or the read-only `ndsref` oracle checkout. Anything I
could not pin down from the code is called out explicitly under
"Open questions / unverified" at the end of each section.

## (a) Existing always-on rings

All of these are plain fixed-size C arrays at namespace/global scope,
written unconditionally on the producing call path (no arm/disarm step),
and read out either by absolute ordinal (`count`/`seq`, 1-based, 0 means
"none yet") or as a "most recent N" copy-out. None of them allocate or
branch on a debug-server connection being present.

| Ring | Entry struct | Capacity | Writer | Reader | Release? | Cite |
|---|---|---|---|---|---|---|
| `g_ring` (bus access ring) | `BusEvent` (anon, bus.cpp) | 8192 | `ring_push` in every `bus_read/write_u{8,16,32}_slow` | `bus_dump_access_ring` (stderr dump only; no debug-server command) | Yes, but payload-dropping under the deep-trace policy in play mode (see below) | `runner/src/bus.cpp:231-247` (struct+size), `runner/src/bus.cpp:329-339` (`ring_push`), `runner/src/bus.cpp:575-588` (dump) |
| `g_watch` (address-range watch ring) | `BusWatchEvent` (`state.h:115-125`) | 512 | `watch_push`, gated by `watch_addr()` allow-list (firmware job-control blocks, IRQ stack, Wi-Fi MMIO tail 0x04800000-0x04810000, etc.) | `bus_debug_watch_copy` → debug-server `watch` | Yes | `runner/src/bus.cpp:239-270` (size+allow-list), `runner/src/bus.cpp:313-327` (push), `runner/src/bus.cpp:720-728` (copy), `runner/src/debug_server.cpp:973-995` (`watch` cmd) |
| `g_spi_trace` (ARM7 SPIDATA writes) | `NdsSpiTraceEntry` (`io.h:73-82`) | 2048 | inline at the SPIDATA write site (io.cpp) | `nds_spi_trace_get` → debug-server `spi_sample` | Yes | `runner/src/io.cpp:513-514` (size), `runner/src/debug_server.cpp:386-398` |
| `g_irq_trace[2]` (per-CPU IRQ accept/return) | `NdsIrqTraceEntry` (`io.h:84-94`) | 256 per CPU | `nds_note_irq_accept` | `nds_irq_trace_get` → debug-server `irq_sample` | Yes | `runner/src/io.cpp:515-516`, `runner/src/debug_server.cpp:399-414` |
| `g_insn_trace[2]` (per-CPU retired-instruction register images) | `NdsInsnTraceEntry` (`io.h:116-124`) | 262144 per CPU ("a full firmware frame") | `runtime_insn_slow`, gated by `g_runtime_deep_trace` | `nds_insn_trace_get` → debug-server `insn_sample` | Yes, but **dropped in play mode** while deep-trace is off (see exception below) | `runner/src/io.cpp:517-521`, `runner/src/io.cpp:628-645`, `runner/src/debug_server.cpp:544-561` |
| `g_fifo_trace[2]` (IPC FIFO sends, both directions) | `NdsFifoTraceEntry` (`io.h:127-135`) | 64 per direction | `fifo_send` | `nds_fifo_trace_get` → debug-server `fifo_sample` | Yes | `runner/src/io.cpp:522-523`, `runner/src/io.cpp:659-681`, `runner/src/debug_server.cpp:562-575` |
| `g_dma_trace` (all DMA completions, IRQ-raising or not) | `NdsDmaTraceEntry` (`io.h:102-112`) | 8192 | DMA completion path (io.cpp) | `nds_dma_trace_get` → debug-server `dma_sample` | Yes | `runner/src/io.cpp:526-528`, `runner/src/debug_server.cpp:524-543` |
| `g_card_trace` (gamecard ROMCTRL/command/data-ready/complete) | `NdsCardTraceEntry` (`io.h:147-169`) | 8192 | `card_trace_push` | `nds_card_debug_trace_copy` → debug-server `cartridge` | Yes | `runner/src/io.cpp:530-536`, `runner/src/io.cpp:549-579`, `runner/src/debug_server.cpp:191-255,297-300` |
| `g_gx_run_trace` (GPU3D::Run() cadence) | `NdsGxRunTraceEntry` (`gpu3d.h:131-139`) | 65536 | `Oracle`-mirrored native GX Run hook (gpu3d.cpp) | `nds_gpu3d_run_trace_get` → debug-server `gx_run_sample` | Yes | `runner/src/gpu3d.cpp:44` (size), `runner/src/gpu3d.cpp:536-542` (get), `runner/src/debug_server.cpp:504-523` |
| `g_gx_write_trace` (ARM9 writes into the 3D register window) | `NdsGxWriteTraceEntry` (`gpu3d.h:144-156`) | 8192 | 3D-window write path (gpu3d.cpp) | `nds_gpu3d_write_trace_get` → debug-server `gx_write_sample` | Yes | `runner/src/gpu3d.cpp:48` (size), `runner/src/gpu3d.cpp:525-532` (get), `runner/src/debug_server.cpp:484-503` |
| Tier-3 step trace | `Tier3TraceEvent` (`tier3.h:13-31`) | 4096 | `tier3_run` interpreter loop | `tier3_debug_trace_copy` → debug-server `tier3_trace` | Yes | `runner/src/tier3.cpp:36` (size), `runner/src/debug_server.cpp:997-1020` |
| Tier-3 static-discovery coverage | `Tier3CoverageEntry` (`tier3.h:54-61`) | caller-bounded (max 262144 at the server) | `tier3` discovery path | `tier3_coverage_copy` → debug-server `tier3_coverage` | Yes | `runner/src/debug_server.cpp:355-373` |
| `runtime_trace` (generic recent-execution ring; pre-inline-counter banks) | `RuntimeTraceEntry` (runtime_arm.h) | — (see `runtime_trace_copy_recent`) | `runtime_arm.cpp` | `runtime_trace_copy_recent` → debug-server `runtime_trace`; also dumped to stderr at end of a plain batch run | Yes | `runner/src/runtime_arm.cpp:742` (copy fn), `runner/src/debug_server.cpp:1022-1044`, `runner/src/main.cpp:849-850` (batch dump) |
| Audio output ring | (raw `int16_t` stereo samples, no named struct) | up to 4096 samples/request, backing ring larger (produced/oldest ordinals) | SPU mixer (spu.cpp) | `nds_spu_debug_copy_output` → debug-server `audio_samples` | Yes | `runner/src/debug_server.cpp:301-327` |
| `dispatch_misses.log` | append-only text, not a ring | unbounded (file) | `runtime_arm.cpp:1141` | manual `cat`/CI check, not TCP-queryable | Yes | `runner/src/runtime_arm.cpp:1141` |

Also present but not "rings" in the trace sense — architectural
snapshots read live, not historized (`cp15_state`, `rtc_state`,
`io_state`, `sched_state`, `gx_state`, `gx_polygon(s)`, `exec_provenance`,
`static_coverage`, `hle_heat`, `mem_timing_profile`, `dispatch_stats`,
`frontend_stats`, `profile`) — all listed in the debug-server command
table below for completeness since a network feature will want to add
one of these too (`net_state`-style live snapshot) alongside the ring.

**Ring-inventory items named in `DEBUG.md` that do NOT exist as separate
storage today:** `DEBUG.md:24-35` lists `dispatch_ring9/7`, `dirty_ring`,
and `frame_record` as inventory items; I could not find distinct storage
for any of the three under those names. What exists instead:
- "control-flow reconstruction" is covered by `runtime_trace` +
  `tier3_trace` + `tier3_coverage` (dispatch-entry-shaped data lives
  there, not in a separate `dispatch_ring`).
- "dirty-RAM promotion" is covered by `tier3_coverage` (`TIER3_COVERAGE_*`
  kinds) plus the provenance/generation bookkeeping in `bus.cpp`
  (`bus_exec_page_generation`, `bus_debug_exec_provenance`,
  `runner/src/bus.cpp:663-704`), not a ring named `dirty_ring`.
- I found no per-frame register/POWCNT/DISPCNT/VRAMCNT/IF/IE/IME
  snapshot ring; `frame_record` as described in `DEBUG.md` does not
  exist. `frontend_stats` (`runner/src/debug_server.cpp:577-594`) is a
  *cumulative counter* struct, not a per-frame historized ring.
This is a real gap between the doctrine document and the code, not
something the network ring needs to resolve — noted so the net-ring
work isn't modeled on phantom precedent.

**The one documented exception — deep-trace policy** (`DEBUG.md:40-47`,
mirrored at `TCP.md`): in the interactive frontend, `g_runtime_deep_trace`
defaults off, which drops the bus ring's per-access *payload* fields, the
`mem_r`/`mem_w` trace events, and the per-instruction register images
(`g_insn_trace`) for real-time headroom; `NDS_DEEP_TRACE=0` opts a
`--serve` server into the same behavior for fast-path equivalence
testing. Event *counters* (`g_insn_count`, `NdsEventCounts`) keep
advancing in every mode. Confirmed at `runner/src/debug_server.cpp:761-769`
(`deep_trace` command) and `runner/src/main.cpp:790-818` (play-mode /
serve-mode wiring).

## (b) Debug-server command surface and how to add a command

**There is no dispatch table.** `debug_server.cpp` has exactly one
function, `handle(const std::string& line)`
(`runner/src/debug_server.cpp:268-1081`), that extracts `cmd` via
`json_str(line, "cmd")` (`debug_server.cpp:269`) and then runs a long
`if (cmd == "...") { ...; return json; }` chain, falling through to
`return "{\"error\":\"unknown cmd\"}";` at `debug_server.cpp:1080`.
**Both** transports call this same function:
- `debug_serve()` (headless, blocking accept loop) calls
  `handle(req)` per newline-terminated request at
  `debug_server.cpp:1144`.
- `debug_pump()` (play-mode I/O-thread handoff to the frontend thread)
  calls `handle(req)` at `debug_server.cpp:1288`, at the one safe point
  between frames.

**Mechanical recipe for a new command** (e.g. `net_sample`,
`net_ring_dump`, `net_state`):
1. Add a new `if (cmd == "your_cmd") { ...; return json_string; }` block
   inside `handle()` in `runner/src/debug_server.cpp`, anywhere before
   the final `return "{\"error\":\"unknown cmd\"}";` at line 1080. Order
   doesn't matter functionally; the file groups by subsystem (SPI/IRQ/GX
   near each other, `run_to_*` together, etc.) — put net commands as a
   new group, e.g. right after `fifo_sample` (`debug_server.cpp:562-575`)
   since that's the last of the "ring-by-absolute-ordinal" group.
2. If the command should be rejected while the frontend owns execution
   (i.e. it *advances* the machine, like `run_to_event`), add its name to
   the play-mode guard at `debug_server.cpp:792-796`. A ring *query* never
   needs this — it's read-only and safe in both modes, same as
   `gx_run_sample`/`dma_sample`/etc.
3. Add `#include "net_ring.h"` to the `debug_server.cpp` include block
   (`debug_server.cpp:16-27` is the existing per-subsystem include list —
   `io.h`, `gpu2d.h`, `gpu3d.h`, `tier3.h` etc. each own their trace
   struct + accessor declarations).
4. JSON is hand-built with `std::snprintf` into a stack buffer (fixed-size
   fields, e.g. `gx_run_sample` at `debug_server.cpp:504-523`) or
   `std::string` concatenation for variable-length lists (e.g.
   `tier3_coverage` at `debug_server.cpp:355-373`, `cartridge` at
   `debug_server.cpp:191-255`). No JSON library; `append_hex` at
   `debug_server.cpp:57-63` is the shared hex-encode helper for binary
   payloads (framebuffers, PCM, region dumps).
5. Nothing else needs to change — no registration list, no protocol
   version bump. `debug_server.h` only declares the two entry points
   (`debug_serve`, `debug_pump_start/pump/stop`) and `debug_set_reset_fn`;
   it does not need touching for a new command.

**How a client discovers valid index ranges** (index windows): every
by-ordinal ring uses the *same* two-step idiom, and net_ring should match
it exactly:
- `cmd` with `count`/`index` omitted or `0` returns just
  `{"latest": N}` (the newest produced ordinal) — see `gx_write_sample`
  and `gx_run_sample` at `debug_server.cpp:484-511`.
- `cmd` with an explicit `count` returns `{"found": false}` if that
  ordinal was never produced *or* has since been evicted; the accessor
  functions implement this by storing the entry's own absolute `count` in
  the struct and checking `stored.count == requested` after the modulo
  index (`bool nds_gpu3d_write_trace_get`, `gpu3d.cpp:525-532`, and
  identically `nds_gpu3d_run_trace_get`, `gpu3d.cpp:536-542`). A client
  therefore always queries `{"count":0}` first to learn `latest`, then
  requests `latest-N+1 .. latest` and treats any `found:false` as
  "evicted, ring too small for the window you wanted" rather than an
  error.
- The "most recent N" flavor (`watch`, `tier3_trace`, `runtime_trace`,
  `cartridge`) instead takes a client-supplied `max` (clamped server-side,
  e.g. `max > 512 → 512` at `debug_server.cpp:975`) and returns however
  many are actually retained, oldest-first, computed from a running
  `count`/`w` write cursor — see `bus_debug_watch_copy`
  (`bus.cpp:720-728`) for the canonical "copy the last N in order" loop.

Full command list as implemented today (supersedes the older subset in
`TCP.md`, which predates several of these): `ping`, `reset`, `regs`,
`event_counts`, `cartridge`, `audio_samples`, `static_coverage`,
`exec_provenance`, `tier3_coverage`, `rtc_state`, `spi_sample`,
`irq_sample`, `gx_state`, `gx_polygon`, `gx_polygons`, `gx_write_sample`,
`gx_run_sample`, `dma_sample`, `insn_sample`, `fifo_sample`, `io_state`,
`frontend_stats`, `frontend_exit`, `black_band_scan`,
`black_band_capture`, `framebuffer_sync`, `hle_heat`,
`mem_timing_profile`, `dispatch_stats`, `cart_save_info`, `cart_save`,
`cart_save_flush`, `profile`, `deep_trace`, `sched_state`, `run_to_pc`,
`cp15_state`, `run_to_event`, `run_cycles`, `run_rounds`, `read_region`,
`read_mem`, `read_io`, `watch`, `tier3_trace`, `runtime_trace`,
`framebuffer`, `touch`, `keys` — enumerated by reading
`runner/src/debug_server.cpp:268-1080` in full.

## (c) CLI flag convention and where to extend it

Two independent conventions coexist; pick the one that matches what the
flag *is*.

**Convention 1 — persistent frontend/render options**
(`NdsFrontendOptions`, `runner/src/frontend.h:32-70`). Each option has:
a TOML key under `[display]`/`[system]`/`[cartridge]` parsed in
`nds_load_frontend_config` (`runner/src/frontend_config.cpp:189-395`), an
`NDS_<NAME>` environment-variable override
(`runner/src/main.cpp:327-370`), a `--flag value` CLI override
(`runner/src/main.cpp:264-281` for parsing into a `cli_*` string, then
validated/applied at `main.cpp:371-448`), and a dedicated
`bool nds_parse_<thing>(const std::string&, T*)` validator declared in
`frontend.h:77-88` and defined in `frontend_config.cpp:22-166`. Priority
order is config file → env var → CLI (CLI applied last, so it wins) —
read the sequence at `main.cpp:318-448` top to bottom. This is the right
home for something that changes *behavior* (e.g. a persistent "network
enabled" toggle), not for a one-shot diagnostic dump.

**Convention 2 — plain boolean/action flags**, parsed directly in the
`argv` loop with no TOML/env-var mirror: `--serve`, `--interactive`,
`--discover-static-misses`, `--rtc-host`
(`runner/src/main.cpp:242-252`), each just setting a local `bool` (or, for
`--rtc-host`, the global `g_nds_rtc_host`) inline in the same
`else if (a == "--flag") { ... }` chain that runs from
`main.cpp:240` to `main.cpp:309`. `--port`, `--rom`, `--save-path`
(`main.cpp:253-259`) show the `--flag value` variant of this same
convention (no separate validator, just a direct assign/parse).

**This is the right home for `--net-ring-dump` /
`--net-ring-last N` / `--net-ring-filter <class>`** — they are one-shot
diagnostic actions for a batch run, exactly like the existing end-of-run
dump block. The dump idiom to mirror is already in `main.cpp`: a plain
(non-serve, non-interactive) run ends by calling
`nds_dump_irq(); nds_profile_report(stderr); runtime_trace_dump_recent(24);`
at `runner/src/main.cpp:847-850`. Concretely:
1. In the flag-parsing loop (`main.cpp:240-309`), add
   `bool net_ring_dump = false; uint64_t net_ring_last = 256;`
   declared alongside the other locals (`main.cpp:230-236`), and three
   new `else if` arms: `--net-ring-dump` (sets the bool, no value),
   `--net-ring-last <n>` (parses via `std::strtoull`, same pattern as
   `budget` at `main.cpp:304`), `--net-ring-filter <class>` (captures the
   raw string into `cli_net_ring_filter`, validated the same way
   `cli_screen_layout` is).
2. Declare `bool nds_parse_net_ring_filter(const std::string&, NdsNetEventClass*)`
   in `frontend.h` next to the other `nds_parse_*` declarations
   (`frontend.h:77-88`) and define it in `frontend_config.cpp` next to
   `nds_parse_adaptive_screens` (`frontend_config.cpp:39-59`) — same
   `lower_ascii` + string-compare shape.
3. Validate the parsed filter after the loop, in the same block that
   validates `cli_screen_layout` etc. (`main.cpp:371-378`).
4. At the batch-mode tail (`main.cpp:847-850`), add
   `if (net_ring_dump) net_ring_dump_recent(net_ring_last, filter);`
   calling into the new `net_ring.h` API (design in (d)).
5. Update the `--help` usage string (`main.cpp:283-298`) with the three
   new flags, matching the existing one-line-per-flag format.

The TCP debug-server route ((b) above) is the *primary*, always-available
query surface per `DEBUG.md`'s "query retroactively" doctrine; the CLI
flags are a convenience for a plain batch run (no server) and for CI /
scripted smoke runs that want a stderr dump without standing up a
listener — matching how `runtime_trace_dump_recent(24)` already works
today with zero server involvement.

## (d) `runner/src/net/net_ring.{h,cpp}` design

Modeled directly on the gamecard trace
(`NdsCardTraceEntry`/`card_trace_push`, `io.h:138-169`, `io.cpp:530-579`)
because it's the closest existing precedent for "one ring, several event
*kinds*, a small fixed-size command/header payload inline, no unbounded
data in the entry itself" — exactly the network ring's shape (Wi-Fi
register events, association/state events, and packet-header events all
share one timeline). New subdirectory `runner/src/net/` because Wi-Fi
networking is a new subsystem with several source files to come (device,
AP, bridge, backend per the plan's Phase 2-5); it should not be bolted
onto `wifi.cpp`, which today is only the MMIO register core
(`runner/src/wifi.h:1-6`, `runner/src/wifi.cpp` — read as the ARM7 device
register model, no network-plane code yet).

### Event enum

```cpp
// net_ring.h
enum class NdsNetEventKind : uint8_t {
    WifiRegRead, WifiRegWrite, WifiIrq,
    WifiTxBegin, WifiTxFrame, WifiRxFrame,
    WifiAssociation, WifiStateChange,
    EthernetTx, EthernetRx,
    Arp, Dhcp, DnsQuery, DnsResponse,
    TcpOpen, TcpClose, TcpReset, TcpPacket,
    UdpPacket,
    BackendDrop, BackendError,
};
```

This is the plan's own enum (`PLAN_ndsrecomp_wiimmfi.md` section 4,
lines 238-265) adopted unchanged — it already matches the project's
`enum class ... : uint8_t` style used for `Tier3CoverageKind`
(`tier3.h:49-53`, an unscoped `enum` there, but `NdsCardTraceKind` at
`io.h:138-143` is the closer match: unscoped `enum ... : uint8_t` so it
converts to `uint8_t` without a cast at the JSON-serialization call site
in `debug_server.cpp:172-180`). Recommend following the `NdsCardTraceKind`
shape (unscoped, `NDS_NET_EVENT_*` names) for the same reason: the
JSON-builder switch statement in `debug_server.cpp` never needs a
`static_cast` to print it.

### Entry struct

```cpp
// net_ring.h
struct NdsNetTraceEntry {
    uint64_t count;          // absolute ordinal from reset, self-describing
    uint64_t sys;             // scheduler_system_timestamp() at push
    uint64_t cyc9, cyc7;      // scheduler_cpu_cycles(0/1) at push
    uint64_t insn9, insn7;    // g_insn_count[0/1] at push
    uint32_t arm7_pc;         // ARM7 owns the Wi-Fi device; always recorded
    uint32_t arm9_pc;         // 0 unless the event is ARM9-driven (IPC-relayed)
    uint8_t  kind;            // NdsNetEventKind
    uint8_t  direction;       // 0 = guest->host (TX/egress), 1 = host->guest (RX/ingress)
    uint16_t wifi_reg;        // MMIO offset for WifiReg{Read,Write}; 0 otherwise
    uint32_t wifi_value;      // register value, or protocol-specific aux
    uint8_t  src_mac[6], dst_mac[6];
    uint32_t src_ipv4, dst_ipv4;
    uint16_t src_port, dst_port;
    uint16_t payload_len;     // on-wire length; NOT the captured length
    uint32_t hostname_ref;    // index into g_net_hostname_pool, or 0 = none
    uint32_t aux;             // event-kind-specific (e.g. TCP flags, DHCP opcode)
};
```

This is the plan's suggested struct (section 4, lines 271-296) with the
sync-timestamp fields from `NdsCardTraceEntry`/`NdsFifoTraceEntry` added
(`sys`/`cyc9`/`cyc7`/`insn9`/`insn7` — every existing cross-subsystem ring
carries these four so a probe can anchor a network event against the IPC
ring, the IRQ ring, or a VBlank count without a second round-trip; see
`NdsCardTraceEntry` at `io.h:147-169` and `NdsFifoTraceEntry` at
`io.h:127-135`) and `hostname_ref` replacing an inline hostname buffer
(rationale below). `arm9_pc`/`arm7_pc` split rather than the plan's single
generic PC pair, matching the dual-CPU rule (`PRINCIPLES.md:104-126`):
Wi-Fi is ARM7-owned hardware, but association/DHCP/DNS state is often
driven by ARM9-side library code relayed over IPC, so both PCs are
useful and neither should be guessed from "whichever CPU is active" the
way `BusEvent::cpu` does (`bus.cpp:231-238`) — record both explicitly.

### Capacity

`constexpr uint32_t kNetTraceSize = 16384;` — one step above the
gamecard/DMA/GX-write rings (8192, chosen there to "span two maximum-size
blocks" / "a full frame of them", `io.cpp:524-532`) because a Wi-Fi
association + DHCP + DNS + a burst of NATNEG/gameplay UDP packets in one
frame window can plausibly exceed a gamecard transfer's event count, and
because — unlike the gamecard ring, which only needs to span one
transfer — a network regression probe (Phase 21 of the plan) wants to
retain a whole multi-second connection sequence (scan → associate → DHCP
→ DNS → auth) without evicting the start before the end is even reached.
16384 entries × the struct's ~64 bytes ≈ 1 MiB, in line with the existing
budget (the 262144-entry `g_insn_trace[2]` alone is already tens of MB;
this ring is small by comparison).

### Auxiliary hostname string pool

DNS hostnames are variable-length (up to 253 bytes) and don't belong
inline in a fixed-size entry (every other ring keeps entries fixed-size —
`NdsCardTraceEntry::command` is a *fixed* 8-byte field, not a pointer,
`io.h:156`). Model as a parallel fixed-capacity array indexed by the same
modulo as the main ring, so it evicts in lockstep automatically with no
separate bookkeeping:

```cpp
constexpr uint32_t kNetHostnameMaxLen = 254;  // 253 + NUL
char g_net_hostname_pool[kNetTraceSize][kNetHostnameMaxLen];
```

`hostname_ref` in the entry is simply `count` again (not a separate
allocator) — a DNS event writer does
`push(kind=DnsQuery, ...); std::snprintf(g_net_hostname_pool[(count-1) % kNetTraceSize], ..., hostname);`
right after `net_ring_push` returns the assigned `count`, and a reader
that already has the entry (because it queried by that same `count`)
indexes the pool the identical way — no extra "ref" field is actually
needed since the ring's own ordinal *is* the ref; I list `hostname_ref`
above only so the field is self-documenting in the wire JSON, but it will
always equal `count` for entries that carry one. Simplify to just
"present" as a bool if that redundancy bothers a reviewer.

### Write API (call sites: Wi-Fi device / AP / bridge / backend)

```cpp
// net_ring.h
uint64_t net_ring_push(NdsNetEventKind kind, uint8_t direction,
                        uint16_t wifi_reg, uint32_t wifi_value,
                        const uint8_t* src_mac, const uint8_t* dst_mac,
                        uint32_t src_ipv4, uint32_t dst_ipv4,
                        uint16_t src_port, uint16_t dst_port,
                        uint16_t payload_len, uint32_t aux);
// Convenience overload for the two PC-carrying call sites (Wi-Fi device
// register access is always ARM7; IPC-relayed protocol events want both).
void net_ring_set_hostname(uint64_t count, const char* hostname);
```

Matches the shape of `card_trace_push` (`io.cpp:549-579`) exactly: one
function, all fields as parameters, self-timestamps internally
(`scheduler_system_timestamp()`/`scheduler_cpu_cycles()`/`g_insn_count`,
the same calls `card_trace_push` makes at `io.cpp:557-561`), returns
nothing there — I add a `uint64_t` return here only because
`net_ring_set_hostname` needs the assigned ordinal as a second call
(DNS hostnames aren't known atomically with the rest of the header in
every backend). Call sites, once the Wi-Fi device/AP/bridge/backend
exist (none of `wifi.cpp`'s current MMIO-only code has a network plane
yet — confirmed by reading `runner/src/wifi.cpp` and
`runner/src/wifi.h:1-27` in full, which expose only
`nds_wifi_read`/`write`/`reset`/`load_firmware`/power-control, no framing
or socket code):
- `runner/src/wifi.cpp` (device register model, once it grows association
  state): `WifiRegRead`/`WifiRegWrite`/`WifiIrq`/`WifiStateChange` on the
  existing `nds_wifi_read`/`nds_wifi_write` entry points
  (`wifi.h:26-27`) — mirroring where `Oracle_OnIOWrite` hooks the general
  IO-write path today (`oracle_hooks pattern`, see (e)).
- A future `runner/src/net/wifi_ap.cpp` (virtual AP, plan Phase 3):
  `WifiAssociation`, `WifiTxBegin`/`WifiTxFrame`/`WifiRxFrame`.
- A future `runner/src/net/eth_bridge.cpp` (802.11↔Ethernet bridge, plan
  Phase 4): `EthernetTx`/`EthernetRx`, `Arp`.
- A future `runner/src/net/backend_slirp.cpp` (plan Phase 5):
  `Dhcp`, `DnsQuery`/`DnsResponse`, `TcpOpen`/`Close`/`Reset`/`Packet`,
  `UdpPacket`, `BackendDrop`/`BackendError`.

### Debug-server + CLI query surface

Following (b)'s recipe exactly, add to `debug_server.cpp`'s `handle()`:
- `net_sample` — same two-step idiom as `gx_run_sample`/`dma_sample`:
  `{"cmd":"net_sample"}` or `{"cmd":"net_sample","count":0}` →
  `{"latest": N}`; `{"cmd":"net_sample","count":K}` →
  the full entry as JSON, or `{"found":false}` if evicted/not-yet-produced
  (same `stored.count == requested` check as `nds_gpu3d_write_trace_get`,
  `gpu3d.cpp:525-532`). Include the hostname string when
  `hostname_ref != 0`.
- `net_ring_dump` — "most recent N" flavor, same as `watch`/`tier3_trace`
  (`debug_server.cpp:973-1020`): takes `max` (clamp to e.g. 4096) and an
  optional `filter` (one of the enum names, or "all"), returns entries
  oldest-first as a JSON array.
- `net_state` — a live (non-historized) snapshot analogous to `gx_state`
  (`debug_server.cpp:415-430`): current Wi-Fi power state, association
  state, DHCP-lease state, ring `produced`/`oldest` ordinals (mirroring
  `NdsCardDebugState::produced`/`oldest`/`capacity`, `io.h:185-187`) — the
  thing a client calls once before deciding what window of `net_sample`
  to pull.

CLI surface per (c): `--net-ring-dump`, `--net-ring-last N`,
`--net-ring-filter <class>` calling `net_ring_dump_recent(uint32_t max,
NdsNetEventClass filter)` (stderr text dump, same destination as
`runtime_trace_dump_recent`) from the batch-mode tail in `main.cpp`. The
plan's `--net-ring-dump FILE` variant (section 23, line 1135) is a
convenience wrapper that redirects the same text — not a different
implementation.

### Open questions / unverified
- Exact byte size of "the average" event's captured metadata vs. the
  16384 capacity target is a judgment call, not something the existing
  code dictates — flagged as such above rather than presented as
  measured fact.
- Whether `hostname_ref` should collapse to a bool is a naming
  preference, not a technical requirement; noted, not resolved.

## (e) Oracle-side plan (ndsref / melonDS)

**The `gx_run_sample`/`gx_write_sample` precedent, read in full**
(`F:\Projects\ndsrecomp\ndsref\src\ndsref.cpp` and
`F:\Projects\ndsrecomp\ndsref\patches\0001-ndsref-hooks.patch`):
melonDS is vendored unmodified except for a small patch
(`ndsref/patches/0001-ndsref-hooks.patch`) that inserts
`#ifdef MELONDS_ORACLE_HOOKS` blocks at specific call sites in
`NDS.cpp`/`ARM.cpp`/`GPU3D.cpp`/`NDSCart.cpp`/`NDSCart.h`/`NDS.h`
(confirmed by the patch's own diff headers,
`0001-ndsref-hooks.patch:1-560`), each block containing a **one-line
`extern` declaration** of an `Oracle_On*` function plus a call to it —
e.g. `extern void Oracle_OnGxRunPre(NDS* nds);` /
`Oracle_OnGxRunPre(this);` wrapped around the `GPU3D.Run()` call inside
`NDS::RunSystem` (`0001-ndsref-hooks.patch:120-190`, corroborated by the
comment "Resolved at link time by the patched NDS::RunSystem GPU3D.Run()
wrapper" at `ndsref/src/ndsref.cpp:293`). The actual ring storage,
push logic, and JSON serialization all live in the **shim**
(`ndsref/src/ndsref.cpp`, not in melonDS), which defines
`Oracle_OnGxRunPre`/`Oracle_OnGxRunPost`
(`ndsref.cpp:294-311`), the ring (`OracleGxRunTraceEntry`/
`g_gx_run_trace[kGxRunTraceSize=65536]`, `ndsref.cpp:115-127`), and the
`gx_run_sample` TCP command (`ndsref.cpp:1073-1091`, same
count-ordinal-with-self-check idiom as the native side). The
register-write ring (`gx_write_sample`) works the same way but hooks
`Oracle_OnIOWrite`, called from the patched `NDS::ARM9IOWriteN`
functions for every width (`0001-ndsref-hooks.patch:331-399`) — this is
the general IO-write hook, and it filters to the 3D window itself
(`addr >= 0x04000320 && addr < 0x040006A4`, `ndsref.cpp:415-427`).

**Critical finding: the general `Oracle_OnIOWrite` hook does NOT see
Wi-Fi register accesses**, because 0x04800000-0x0480FFFF is dispatched
directly from `NDS::ARM7Read8/16/32`/`ARM7Write8/16/32`'s
`case 0x04800000:` branch straight to `Wifi.Read(addr)`/`Wifi.Write(addr,
val)` — confirmed by reading
`ndsref/third_party/melonDS/src/NDS.cpp:2395-2402` (the `ARM7Read8` case)
and the identical shape at the `ARM7Read16`/`ARM7Read32`/write-side cases
(`grep` for `0x04800000` in `NDS.cpp` returns 5 matches, one per
width/direction). This path never goes through `ARM7IOWriteN`/
`ARM7IOReadN`, so the existing `Oracle_OnIOWrite` patch site
(inside `ARM9IOWriteN`/`ARM7IOWriteN`, `0001-ndsref-hooks.patch:331-399`)
is the *wrong* place to add Wi-Fi register sampling. A **new** hook is
required, patched into `NDS::ARM7Read{8,16,32}`/`ARM7Write{8,16,32}`'s
`0x04800000` case, or equivalently directly inside melonDS's
`Wifi::Read`/`Wifi::Write` (`ndsref/third_party/melonDS/src/Wifi.cpp:2000`,
`:2103`). Patching `Wifi::Read`/`Write` is the tighter, single-site
option since every CPU-side caller funnels through them regardless of
width (`Wifi.cpp:2000-2160`+); recommend hooking there rather than at
each of the five `NDS.cpp` call sites.

**Concrete sampling insertion points, read directly**:
- `melonDS::Wifi::Write(u32 addr, u16 val)` —
  `ndsref/third_party/melonDS/src/Wifi.cpp:2103` — one hook call at
  entry, mirrors the native `WifiRegWrite` event. Carries `addr`, `val`,
  and (from the surrounding `NDS`) `ARM7.R[15]` for `arm7_pc`.
- `melonDS::Wifi::Read(u32 addr)` — `Wifi.cpp:2000` — same, for
  `WifiRegRead` (the native ring only needs to sample writes for a first
  pass per the plan's "no game-specific HLE / minimal surface" spirit,
  but the melonDS side can cheaply carry both since it's a single
  function).
- `melonDS::Wifi::TXSendFrame(const TXSlot* slot, int num)` —
  `Wifi.cpp:601-672` — the **egress** point: `TXBuffer` holds the
  complete outgoing 802.11 frame (`12` bytes of internal TX header +
  `len` bytes of real 802.11 frame, `Wifi.cpp:640`), already assembled
  with frame-control, sequence number, and (for non-multiplayer traffic)
  is about to be handed to `WifiAP->SendPacket(TXBuffer, 12+len)`
  (`Wifi.cpp:654`) or `Platform::MP_Send*` for local multiplayer
  (`Wifi.cpp:653,659,664,669` — out of scope per the plan's "local
  wireless is explicitly out of scope", section 17). Hook right before
  the `switch(num)` dispatch (`Wifi.cpp:648`) to capture the frame
  uniformly regardless of which TX path consumes it; `num` and
  `CurChannel` (`Wifi.cpp:646`) map to the native ring's `direction`/`aux`.
- `melonDS::Wifi::StartRX()` (`Wifi.cpp:1211-1237`) and
  `melonDS::Wifi::FinishRX()` (`Wifi.cpp:1239`+) — the **ingress** point:
  `RXBuffer` holds the incoming frame; `FinishRX` is where destination-MAC
  filtering (`Wifi.cpp:1264-1269`) and frame-type dispatch
  (`Wifi.cpp:1289` `switch ((framectl >> 2) & 0x3)`) happen, so a hook at
  the top of `FinishRX` (before the filter can `return` early) captures
  every frame the hardware physically received, and a second hook after
  the filter/dispatch (or a flag on the same event) can record whether it
  was accepted — this maps to the native ring's `WifiRxFrame` plus
  enough `aux` bits to reconstruct "accepted vs. filtered".
- `melonDS::WifiAP::SendPacket(const u8* data, int len)` —
  `ndsref/third_party/melonDS/src/WifiAP.cpp:262-` — where the virtual
  AP either answers locally (association/probe/beacon handling inside
  `WifiAP`, not read in full here) or forwards to
  `Platform::Net_SendPacket(LANBuffer, lan_len, UserData)`
  (`WifiAP.cpp:304`) — this is the 802.11-to-Ethernet bridge boundary and
  the right place for `WifiAssociation`/`EthernetTx` events once the
  frame has been unwrapped to its Ethernet-payload form.
- `melonDS::Net::SendPacket`/`Net::RecvPacket` —
  `ndsref/third_party/melonDS/src/net/Net.cpp:48-68` — thin forwarders to
  `Driver->SendPacket`/`Dispatcher.recvPacket`; a hook here sees raw
  Ethernet frames at the host-backend boundary regardless of which
  backend (`Net_Slirp` or a future replay backend) is active — matches
  the plan's Phase 21 "capture/replay at the Ethernet backend boundary"
  exactly (`PLAN_ndsrecomp_wiimmfi.md:1028`).
- `Net_Slirp.cpp` (`ndsref/third_party/melonDS/src/net/Net_Slirp.cpp`,
  459 lines, not read in full for this pass) is where DHCP/DNS get
  terminated by libslirp itself inside the backend, so it's the natural
  place for `Dhcp`/`DnsQuery`/`DnsResponse`/`TcpOpen`/`UdpPacket` samples
  if the oracle wants protocol-level events rather than only raw-frame
  events — flagged as **unverified in detail**: I confirmed the file
  exists and is 459 lines but did not read its internals in this pass:
  a follow-up read of `Net_Slirp.cpp` is needed before writing the actual
  patch, to find the exact libslirp callback functions to hook (likely
  `slirp_cb_*` — the standard libslirp callback vtable — matching the
  plan's own libslirp policy discussion in `PLAN_ndsrecomp_wiimmfi.md`
  section 3.1, lines 129-168).

**What each oracle sample carries, and 1:1 mapping to native event
classes**: every hook above should push into an
`OracleNetTraceEntry`/`g_net_trace[kNetTraceSize]` ring in
`ndsref/src/ndsref.cpp`, field-for-field identical to
`NdsNetTraceEntry` (d) — same `count`/`sys`/`cyc9`/`cyc7`/`insn9`/`insn7`
timestamp block (using `nds->OracleSysTimestamp()`/`nds->ARM9Timestamp`/
`nds->ARM7Timestamp`/`g_oracle_counts.insn9`/`insn7`, the exact calls
`cartTracePush` already makes at `ndsref.cpp:197-201`), same `kind`/
`direction`/`wifi_reg`/`wifi_value`/MAC/IPv4/port/`payload_len`/`aux`
fields. A `gx_run_sample`-style `net_sample` command
(`ndsref.cpp:1073-1091` is the template) exposes it on port 19843,
letting a single reducer diff native port-19842 `net_sample` against
oracle port-19843 `net_sample` at matched `count` (or, better, at matched
hardware-event ordinals — see (f)) with zero protocol asymmetry.

## (f) Normalized comparison design

**Extends, does not duplicate, the existing diff tooling.** The relevant
precedent is `oracle/probe_gx_state.py`
(`F:\Projects\ndsrecomp\ndsrecomp-wiimmfi\oracle\probe_gx_state.py`, read
in full): it drives *both* ends to the same named hardware-event ordinal
via `run_to_event` (using the shared `DebugClient` in
`oracle/_client.py:12-53`), reads a fixed register/count set from each
side in parallel (`ThreadPoolExecutor`, `probe_gx_state.py:91-96`),
diffs, and reports the first stop where any tracked value differs
(`probe_gx_state.py:113-135`). `oracle/README.md:41-49` and
`oracle/find_first_diverge.py` describe the sibling "bisect over event
counts to find the first divergence" mode. A network probe
(`oracle/probe_net_state.py`, by naming convention) should be a new
script using the exact same `DebugClient`/`both()`/bisection shape, not
a new harness — the only new pieces are (1) which command to call
(`net_sample`/`net_ring_dump` instead of `read_io`) and (2) the
normalization step below, since raw ring entries contain host-only noise
that register reads don't.

**Sync key**: hardware-event ordinal (`run_to_event`), never a ring
`count` directly — the native and oracle rings can assign different
absolute ordinals to "the same" guest-visible event if either side ever
drops/coalesces one differently, so the comparison must first establish
"both sides are at guest state S" via `run_to_event`
(`TCP.md`'s existing doctrine, `TCP.md:30-40` and this worktree's
`TCP.md:30-40` verbatim) and only then pull each side's `net_sample`
window and compare *by position in a filtered, normalized sequence*, not
by raw ordinal.

**Normalize away** (per the plan, section 22, lines 1080-1085, refined
against what the entry struct in (d)/(e) actually carries):
- `sys`/`cyc9`/`cyc7` absolute values — the two engines' schedulers are
  not required to reach a given guest event at bit-identical host or
  even guest cycle counts on every code path (only *ordering* is a
  correctness invariant); compare cycle *deltas between consecutive
  network events*, not absolute values, if timing is compared at all.
- Any TCP/IP checksum field (IP header checksum, TCP/UDP checksum) —
  these are a pure function of the other preserved fields, so comparing
  them is redundant with comparing the fields they're computed from, and
  they're liable to differ if one side's stack computes checksums
  slightly differently on non-payload-affecting padding.
- Session-random fields: DHCP transaction ID, TCP initial sequence
  number, source port chosen by ephemeral allocation, NATNEG session
  IDs, GameSpy session tokens — these are expected to differ between two
  independent stack instances and are exactly the kind of field the plan
  flags in section 28 as never-record.
- Host-only metadata: whichever backend driver produced the packet
  (`Net::Driver`), OS-level socket errno, wall-clock time.

**Preserve** (plan section 22, lines 1087-1096, and this maps directly
onto `NdsNetTraceEntry` fields from (d)):
- guest cycle → `cyc9`/`cyc7` (as deltas, per above)
- guest PC → `arm9_pc`/`arm7_pc`
- Wi-Fi registers → `wifi_reg`/`wifi_value`
- direction → `direction`
- type → `kind`
- lengths → `payload_len`
- guest-visible addresses → `src_mac`/`dst_mac`/`src_ipv4`/`dst_ipv4`/
  `src_port`/`dst_port` (these are guest-observable, unlike a checksum,
  because the guest's own network stack computed/consumed them — e.g. the
  DHCP-assigned IP the guest actually uses for all subsequent traffic)
- protocol state → whatever `aux` carries for that `kind` (TCP flags,
  DHCP opcode, association result code) plus the live `net_state`
  snapshot (d) for state-machine-level fields (associated? DHCP lease
  held? DNS resolved?) that aren't naturally one ring event.

**Output table shape** — adopt the plan's table
(`PLAN_ndsrecomp_wiimmfi.md:1098-1111`) unchanged; it is already the
right shape and matches the spirit of `probe_gx_state.py`'s
"print every diverging stop, report first, `status: pass/FAIL`" output
(`probe_gx_state.py:101-141`):

```
EVENT                         ORACLE        NDSRECOMP
---------------------------------------------------------
Wi-Fi power on               yes           yes
scan begins                  yes           yes
probe request                yes           yes
probe response                yes           yes
association request          yes           yes
association response         yes           yes
DHCP discover                 yes           yes
DHCP offer                    yes           NO   <-- first divergence
```
The reducer produces this by walking both sides' normalized event
sequences (filtered/normalized per above) in lockstep by *matched
milestone kind* (not raw index — a dropped or reordered event on one
side must not cascade into every later row reading FAIL), stopping at
and reporting the first row where the milestone kind, or any preserved
field on it, differs — mirroring "always debug the first divergence"
(`PLAN_ndsrecomp_wiimmfi.md:1113`, and the project-wide doctrine at
`DEBUG.md:49-53`).

## (g) Privacy

Directly adopts the plan's section 28
(`PLAN_ndsrecomp_wiimmfi.md:1442-1457`) as the binding policy, made
concrete against the entry struct in (d):

- **Metadata by default.** `NdsNetTraceEntry` as designed in (d) never
  carries payload bytes — only headers/addresses/lengths/register
  values. This is the default and requires no flag.
- **Full-payload capture is a separate, explicit opt-in.** If a future
  debugging need requires actual packet bytes (e.g. to replay a captured
  DHCP exchange per plan section 21), that is a *second*, larger ring
  (`g_net_payload_pool`, keyed by the same `count` as the header ring,
  same lockstep-eviction trick as the hostname pool in (d)) that only
  allocates/writes when explicitly armed — modeled on the existing
  `deep_trace` on/off toggle (`debug_server.cpp:761-769`,
  `DEBUG.md:40-47`) and `black_band_scan`'s "opt-in partial-black-row
  observer" pattern (`TCP.md`'s description, `debug_server.cpp:600-605`):
  a `net_payload_capture {"on":0|1}` command, off by default, so a
  normal run pays zero cost for it (same "normal runs are unaffected"
  guarantee `io.cpp:585` documents for the sub-event break).
- **Must never be recorded or committed**, per the plan verbatim: auth
  tokens, console-identifying data (the DS's own Wi-Fi MAC, any
  console-unique ID exchanged during NAS authentication), player
  credentials, session/matchmaking identifiers. Concretely: even with
  full-payload capture armed, the *header* ring's `hostname_ref` pool
  and any TCP/UDP payload pool must be excluded from anything checked
  into the repository (regression fixtures per plan section 21) unless
  passed through a sanitizer first (plan section 28, line 1457) — no
  sanitizer exists yet; this plan does not design one, since the payload
  ring itself is future work past the header-ring MVP this document
  scopes.
- **Regression fixtures** (plan section 21) must be synthetic/local where
  possible (a local slirp-backed DHCP/DNS server under test control, not
  live Nintendo/Wiimmfi traffic) specifically so the credentials/session
  problem above doesn't arise in committed fixtures at all — preferring
  "don't capture the sensitive case" over "capture and then scrub."

### Open questions / unverified
- No sanitizer or payload-pool code exists today to point to; the
  guidance above is a policy statement for when that work happens, not a
  description of existing code.
