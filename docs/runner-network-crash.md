# `nds_runner` mid-session network crash — diagnosis pass (I15, 2026-08-10/11)

Scope of this pass: **diagnosis only**, no runner source edits. Tracked at
`beads-yjp.1.12` (top blocker). Related: `beads-yjp.4` (the already-filed
`g_log_budget` race), `beads-yjp.1.4` (worker-thread shutdown never verified
under `--serve`/`--interactive`), `beads-yjp.1.7` (M5 login evidence, incl.
a prior "local-rig-specific" false alarm that is directly relevant here).

## Bottom line

**I could not reproduce the crash.** 14 controlled, isolated runs (9 local
with real network traffic, 4 local with `--network off`, 1 full run against
real Kaeru with a live debugger attached, including two complete NAS
TLS/SSLv3 login handshakes) — **zero crashes**. Every anomaly I *did*
observe during this pass turned out, on inspection, to have a mundane
explanation unrelated to the wifi worker thread (my own test harness, or an
environment/PATH problem) — none of it is offered as the root cause. I am
reporting this as a rigorous negative result plus two confirmed, real,
independent latent bugs found by code reading, not as a solved crash.

## What reproduction looked like

### Environment friction (read this before trying gdb again)

- `C:\msys64\mingw64\bin\gdb.exe` **does not run** when invoked from a Git
  Bash-derived shell (the Bash tool) or from any process chain that
  inherited Git Bash's `PATH`: it fails immediately with
  `STATUS_ENTRYPOINT_NOT_FOUND` (0xC0000139), because
  `C:\Program Files\Git\mingw64\bin\*.dll` (Git for Windows' own bundled,
  differently-versioned MinGW runtime) shadows msys64's own
  `libgcc_s_seh-1.dll`/`libstdc++-6.dll`/`libwinpthread-1.dll` earlier in
  `PATH`. Confirmed by direct `-Version`/`--version` probes returning the
  same exit code both for `gdb.exe` and, once, for `nds_runner.exe` itself
  when launched through a nested Bash→PowerShell→`Start-Process` chain (see
  "false leads" below) — that one produced a real
  `(pid.tid): Unknown exception - code c0000139 (first chance)` line in a
  debugger transcript that briefly looked like a genuine crash and was not.
  **Fix for next time:** launch everything through the PowerShell tool
  directly, never through a chain that passes through Git Bash, or prepend
  `C:\msys64\mingw64\bin` ahead of any Git directory in the child's own
  `PATH` explicitly.
- Given gdb was unusable, I used **`cdb.exe`** (Windows Debugging Tools,
  `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe`, already
  installed) instead — the task's own suggested fallback ("another
  approach"), not a source change. `cdb -g -G -cf <script>` launches the
  target, `sxe` arms stop-on-exception for access violations and C++
  exceptions, and `bp kernel32!ExitProcess` / `bp ntdll!RtlExitUserProcess`
  catches a clean/intentional exit too, each followed by `~*kP 200` (all
  threads, all frames). This worked well once I stopped putting `!analyze
  -v` in the action list (it tries to reach Microsoft's public symbol
  server and can stall for minutes under the network load this box already
  had going — starved my own capture on one attempt, see below) and once I
  redirected cdb's own stdout straight to a file (`RedirectStandardOutput`)
  instead of buffering it in .NET's `ReadToEndAsync().Result`, which only
  flushes once the process exits and therefore hides everything if cdb
  itself hangs.

### Method

Built a fresh `RelWithDebInfo` (`-O2 -g`) copy of `nds_runner` in
`runner/build-wiimmfi-i15/` (kept separate from I13's `build-wiimmfi-i13/`,
touched no shared build dir). Drove `oracle/mkds_wfc_scenario.py
--native-only` against it, port 19848, using:

- **Local traffic**, real (not synthetic): the already-running
  `dwc_network_server_emulator` Docker deployment
  (`docs/local-wfc-server.md`) plus its `wfc_dns.py` responder, which I
  restarted for this session (`--wfc-provider 127.0.0.1`). This produces
  genuine DHCP→DNS→TCP `conntest`-HTTP traffic through the worker thread —
  unlike the "ten local runs" cited in the task brief, which per the doc's
  own record never got real traffic flowing at all.
- **`--network off`** as a control (same boot path, same debug server,
  *zero* wifi backend/worker thread — `nds_wifi3d_attach()`'s
  `g_network_config.enabled == false` branch never sets `slirp_driver` or
  starts the thread).
- **One real-Kaeru run** (`--wfc-provider kaeru`, i.e. 178.62.43.212),
  sparingly, isolated (no other `nds_runner`/heavy process running), under
  cdb, `--stall 300000` to give the real login handshake room.

Every attempt drove the scenario through cold boot → AP scan → tap →
connection test (the three points named in the bug report), and the
Kaeru run continued into the real NAS handshake.

### Results

| Condition | Runs | Crashes | What happened |
|---|---|---|---|
| Local, `--network on`, real traffic, isolated (1 instance at a time) | 9 | **0** | Every run reached the connection test; local server answered but the game reported an on-screen WFC error code (expected — separate, already-known local-server limitation, not this bug) |
| Local, `--network off` (control, no wifi backend/worker thread at all) | 4 | **0** | Every run reached the connection test and timed out waiting for a backend that was never there (expected) |
| Real Kaeru, isolated, live debugger attached | 1 | **0** | Full scripted run completed; net ring shows **two** complete real SSLv3 handshakes to `178.62.43.212:443` (ClientHello→ServerHello→ClientKeyExchange→ChangeCipherSpec×2→ApplicationData both directions) — a real, successful NAS login round trip, twice, with the debugger live the whole time |

All 24 cdb transcripts captured this pass (`sxe` on access violations and
C++ exceptions armed, plus `bp` on both process-exit entry points) were
grepped for any exception/AV/exit-breakpoint signature after the fact:
**zero matches, in every single one.** No thread ever hit a fault, an
unhandled C++ exception, or even the "clean exit" breakpoints, in any of
the 24 debugger-attached launches this pass.

### False leads I chased and disproved (recorded so the next session doesn't re-walk them)

1. **"nds_runner itself won't launch" (`c0000139`).** Traced to the same
   Git-Bash-`PATH`-shadowing issue as gdb above, not a runner bug — see
   "Environment friction."
2. **An apparent crash under heavy concurrent load** (I was, at the time,
   running several `nds_runner`+`cdb` pairs back-to-back plus two Docker
   containers plus a Python DNS responder on one machine while iterating on
   my own test harness): a batch of 6 same-machine relaunches showed the
   *client's* socket read returning empty (`ConnectionError: debug server
   closed the connection`) consistently at the exact same point — `run_to_event
   vblank9 count=900`, i.e. **plain firmware→title-screen boot, before any
   Wi-Fi register access at all**. Disproved as network/wifi-related two
   ways: (a) it reproduced 0/4 times in the `--network off` control, which
   passes through the identical boot phase; (b) it reproduced 0/9 times
   once I stopped running concurrent instances and went back to one
   `nds_runner`+`cdb` pair at a time. This matches
   `beads-yjp.1.7`'s own prior note about a "LOCAL-RIG-SPECIFIC" `52100`
   anomaly that also didn't reproduce against real Kaeru — same category of
   noise, most likely resource contention from my own parallel testing
   (possibly interacting with the single-connection-backlog `listen(...,
   1)` accept loop in `debug_server.cpp`, but I did not pin that down
   further since it never recurred once isolated).
3. **One apparent "process just disappeared" case** (attempt 34 in my own
   numbering) that looked like a real crash at first glance
   (`runner_alive=False` recorded by my harness *before* it had called
   `Stop-Process`) — but the cdb transcript for that exact run shows nothing
   but ordinary Wi-Fi AP-scan channel-hopping log lines
   (`wifi: switching to channel N`, `wifi: TRX power ON/OFF`, `received
   frame but bad channel...`) right up to the point the file was truncated
   mid-line — which is what happens when *I* force-kill a still-running
   process whose stdout was being buffered via `ReadToEndAsync().Result`
   rather than flushed to disk (see "Environment friction"). I cannot rule
   out a real event here, but I also have no positive evidence for one —
   the log shows completely ordinary operation, not a fault.
4. **Considered and ruled out (not observed):** `NDS_ENABLE_COMPUTE_RENDERER`
   is `ON` in this build, and `gpu3d.cpp`'s compute-renderer failure path
   (`compute_readback_failed()`, fired by any `glGetError()` during
   frame-readback submit/map) sets a flag that `debug_server.cpp`'s request
   loop (`debug_serve()`, ~line 1282) checks after *every* request and, if
   set, treats as fatal: closes the connection and lets `main()` return a
   nonzero exit code — a **clean, intentional shutdown**, not a segfault,
   that would look exactly like "the runner closed its socket." This is a
   real, live code path (confirmed by reading `gpu3d.cpp`'s
   `nds_gpu2d_requires_3d_readback()`-gated call from
   `nds_gpu3d_vcount215()`, which runs every frame in headless `--serve`
   mode too, not just when a display is attached) and GPU-driver contention
   from many concurrent OpenGL/compute-shader contexts (exactly the
   situation this multi-agent session runs in generally) is a plausible
   trigger. I flag it because it's real and worth knowing about, **not**
   because I caught it: `compute_gl_stage_failed()` always logs the specific
   GL error code (`"[gpu3d] compute %s GL error: 0x%04X\n"`), and that
   string does not appear in any of the 24 transcripts from this pass. If a
   future session sees `nds_runner` exit unexpectedly, grep stderr for
   `"[gpu3d] compute"` and `"GL error"` before assuming it's the wifi
   thread — that would be the tell.

## What I actually confirmed by code reading (static analysis)

Read in full: `runner/src/wifi_net.{h,cpp}`, `runner/vendor/melonds/net/
Net_Slirp.{h,cpp}`, `Net.{h,cpp}`, `PacketDispatcher.cpp`, `NetDriver.h`,
`runner/src/net/net_ring.{h,cpp}`, `runner/src/net/net_classify.cpp` (call
sites), `runner/src/gpu3d.cpp`, `runner/src/debug_server.cpp` (both the
headless `debug_serve()` accept loop and the `--interactive`
play-mode-pump), `runner/src/main.cpp` (full boot/CLI-parsing/serve-mode
path), `runner/vendor/melonds/{Wifi,WifiAP}.cpp` (call sites into
`Platform::Net_*`/`Platform::Log`), and both threading patches
(`0002-net-slirp-nonblocking-poll.patch`, `0005-net-slirp-worker-thread-poll.patch`).

**The queue/worker design in `wifi_net.cpp` holds up.** Specifically verified:

- `WifiBridgeState::~WifiBridgeState()` calls `StopWorker()` **first** (sets
  the stop flag, wakes the condvar, `Thread_Wait` joins), and only *then*
  do the ordinary C++ member destructors run in reverse declaration order —
  `net` (which owns `Net_Slirp` via `unique_ptr<NetDriver>`, and whose
  destructor calls `slirp_cleanup(Ctx)`) is destroyed well after the join
  completes. No path lets the worker thread touch a half-destroyed
  `Net_Slirp`/libslirp `Ctx`.
- `Net_Slirp::SendPacket()`/`PollHostSockets()` (the only two methods that
  touch `Ctx`/`PollList`) are, by construction, called from exactly one
  thread each in this build: `SendPacket()` only from
  `WifiWorkerThreadMain`'s drain loop (`wifi_net.cpp:434-438`), never from
  `Platform::Net_SendPacket` directly (that function only ever *enqueues*
  into `tx_queue`). `PollHostSockets()` is likewise only called from that
  same worker loop. `RecvCheck()` is a deliberate no-op
  (patch 0005) — confirmed still true in the vendored source, not just in
  the comment.
- The two `BoundedPacketQueue`s (`tx_queue`/`rx_queue`) are real
  single-producer/single-consumer, `std::mutex`-guarded, move-only —
  verified no aliasing, no shared pointer, no place either side reads after
  the other side has taken ownership.
- `net_ring_push` (the always-on ring) is confirmed single-writer: grepped
  every call site (`wifi_net.cpp`, `gpu3d.cpp`'s `NDS::SetIRQ`,
  `net_classify.cpp`) and every one of them is reachable only from the
  emulation thread (`Net_SendPacket`/`Net_RecvPacket`'s TX/RX classify
  hooks, `wifi_reg_read16`/`write16`, and `Wifi::SetIRQ` — all bus-access- or
  guest-cycle-driven, never from `WifiWorkerThreadMain`). The worker thread
  only ever increments the plain `std::atomic<uint32_t>
  rx_dropped_since_last_report` counter and leaves the actual ring write to
  the emulation thread's next `Net_RecvPacket` call, exactly as its own
  comment claims.
- `debug_serve()` (headless `--serve`, the mode used by the bug report and
  by every reproduction attempt here) is **single-threaded**: the same
  thread that `accept()`s/`recv()`s/`send()`s the debug-protocol socket is
  the emulation thread (`handle(req)` calls straight into
  `scheduler_run_round()` etc.). `--interactive` uses a genuinely different
  model (a dedicated I/O thread plus a frontend thread, `debug_pump()`), but
  **I did not test `--interactive` this pass** — ran out of scope/time
  after the real-Kaeru run. This is the one item from the task's checklist
  I left unverified by execution.

## Two real, confirmed latent bugs (neither observed to crash anything this pass)

### 1. `g_log_budget` data race — re-examined, still just a counter (re-confirms `beads-yjp.4`, does not overturn the "benign" judgment)

`runner/src/gpu3d.cpp:36`, `int g_log_budget = 64;`, decremented at
`gpu3d.cpp:287-288` inside `Platform::Log()` with no synchronization. I went
looking for *siblings* of this exact mistake (the task's explicit ask) and
confirmed the race is real and is actually hit from both threads in
practice, not just theoretically: `Wifi.cpp`/`WifiAP.cpp` fire `Debug`-level
logs from the emulation thread constantly during association/scan/power
transitions (17 call sites, e.g. `Wifi.cpp:359,367,425,549,560` —
`"WIFI: ON"`, `"wifi: TRX power ON"`, etc.), while `Net_Slirp.cpp` fires
`Debug`-level logs from the **worker** thread on every poll-fd
register/unregister and every DNS/response packet (`Net_Slirp.cpp:80,118,
123,128,260`). Both hit the same global `g_log_budget` concurrently,
confirmed by a live transcript (`cdb_run34.log`) showing exactly this
interleaving pattern during an AP scan.

Despite that, I could not find a **second, different** shared-mutable-state
mistake anywhere reachable from the worker thread — checked
`net_capture.cpp` (no globals, only touched from `Net_SendPacket`/
`Net_RecvPacket` on the emulation thread), `Net_Slirp`'s own members
(`PollList`/`PollListSize`/`IPv4ID`, all touched only from the
worker-thread-exclusive methods), and `PacketDispatcher` (internally
mutex-guarded, and only ever driven from the emulation thread in this
build anyway since local wireless/`MP_*` are permanently stubbed to zero).
`g_log_budget` looks like the *only* instance of this mistake, not the
first of several. A plain aligned `int` race like this cannot itself
corrupt memory or crash on x86 — worst case is a mis-counted rate limiter
(prints slightly more or fewer debug lines than intended). Recommended fix
is still exactly what `beads-yjp.4` already proposes (`std::atomic<int>`);
I did not make this change (out of file-ownership scope this pass and
non-blocking for the crash itself).

### 2. `WSAStartup()`/worker-thread-start ordering hazard — new finding, not previously filed

`main.cpp:1048` calls `boot()`, which runs `nds_io_reset()` →
`nds_wifi_reset()` → `nds_wifi3d_attach()` (`wifi_net.cpp:584-717`), which
**starts the host-networking worker thread** (`wifi_net.cpp:712-715) and
lets it begin calling `PollHostSockets()` — which calls `poll()`, `#define`d
to `WSAPoll` on Windows (`Net_Slirp.cpp:52`) — immediately, in a tight
~2 ms loop.

`WSAStartup()` is **only** called inside `debug_serve()`
(`debug_server.cpp:1237`) or `debug_pump_start()` (`debug_server.cpp:1386`)
— both of which run strictly *after* `boot()` returns (`main.cpp:1088` and
`main.cpp:1069` respectively), and in the non-interactive `--serve` path
used by every reproduction attempt in this bug report, also after
`nds_compute_host_start()` (`main.cpp:1054`) — which, in this build
(`NDS_ENABLE_COMPUTE_RENDERER=ON`), creates a hidden OpenGL context and
compiles 33 compute shaders (confirmed via a live transcript:
`"[gpu3d] compute shader 1/33"` … `"33/33"`) before returning. That is real,
measurable wall-clock work, not a few instructions — so the window in which
the worker thread calls `WSAPoll()` before Winsock has been initialized
*anywhere in the process* is not negligible.

Calling Winsock APIs before `WSAStartup()` succeeds is undefined per
Microsoft's own documentation (typically surfaces as `WSANOTINITIALISED`,
but is not a *guaranteed* graceful failure across every Winsock provider/LSP
configuration). I did not observe this crash anything in 14 clean runs on
this machine — `PollHostSockets()`'s return value from `poll()` is never
checked for error either way, so a `WSANOTINITIALISED` failure here would
be silently swallowed rather than surfaced, meaning **absence of an
observed crash from this specific hazard is weak evidence**, not proof it's
harmless on every machine/every Winsock LSP stack. This is exactly the kind
of thing that could plausibly explain "different site each time, traffic
correlated, low but nonzero rate" if a particular host's LSP chain reacts
worse than this one did.

**Recommended fix:** call `WSAStartup()` once, unconditionally, at the very
top of `main()` — before `boot()` — rather than inside `debug_serve()`/
`debug_pump_start()`. `WSACleanup()` already only runs at the end of those
same two functions today; either move it to match (call once at true
process shutdown) or leave the extra `WSAStartup()`/`WSACleanup()` pairs in
place (Winsock reference-counts them, so it's harmless to call more than
once) — the only change that matters is moving the *first* call earlier
than the worker thread's first socket-API use.

## What I did not verify by execution

- `--interactive` mode: not tested this pass (ran out of time after the
  real-Kaeru run). The task specifically asked whether the crash also
  reproduces there, to rule the debug server's I/O thread in or out — still
  open.
- The WSAStartup-ordering hazard and the `g_log_budget` race: both
  confirmed real by code reading; neither confirmed (nor ruled out) as an
  actual crash cause by execution, because no crash occurred in any run I
  could construct.
- Whether the *original* 3-in-7 real-Kaeru crashes correlate with
  concurrent GPU load (another `nds_runner` instance, or another
  GPU-heavy process) on whichever machine produced that report — I have no
  visibility into that machine's state at the time, and my own strongest
  (though disproved-for-my-own-anomalies) lead pointed straight at
  concurrent-load effects. Worth asking directly.
- Real backtrace of the faulting thread: never obtained, because the fault
  never occurred under a live debugger in this pass, despite 24
  debugger-attached launches (18 with exception/exit breakpoints armed) and
  zero hits.

## Recommended next steps, in order

1. Land the two confirmed fixes regardless (both are correct on their own
   merits, neither is destructive, neither conflicts with I13's
   `net/**`/`wifi_net.*`/`CMakeLists.txt` ownership since the WSAStartup fix
   is in `main.cpp` and the log-budget fix is in `gpu3d.cpp`):
   - `gpu3d.cpp:36`: `int g_log_budget` → `std::atomic<int> g_log_budget`,
     adjust the read/decrement at `gpu3d.cpp:287-288` to
     `fetch_sub`/relaxed-load-and-compare.
   - `main.cpp`: hoist a single `WSAStartup(MAKEWORD(2,2), &wsa)` call to
     the very top of `main()`, before `boot()`. Leave the existing calls in
     `debug_server.cpp` as harmless extra ref-counted calls, or remove them
     if preferred — either is safe.
2. Next reproduction attempt should run with exactly **one** `nds_runner`
   process alive on the machine (confirm via `Get-CimInstance
   Win32_Process -Filter "Name='nds_runner.exe'"` before starting) and
   should grep stderr for `"[gpu3d] compute"`/`"GL error"` on any
   unexpected exit before attributing it to the network thread.
3. Test `--interactive` under cdb the same way this pass tested `--serve` —
   still the one item from the original ask left undone.
4. If it still won't reproduce under a live debugger, the next-best
   evidence is the always-on `net_ring`/`event_counts`/`tier3_coverage`
   query surface *from whichever run actually crashed against real Kaeru* —
   query it retroactively for the window right before death, per the
   project's own ring-buffer-first policy, rather than trying to catch it
   live again.
