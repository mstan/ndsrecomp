# TCP debug protocol

Both the native runtime's debug server and the melonDS **oracle** speak
the same line-delimited JSON request/response protocol, so one diff
harness can talk to either. One JSON object per line; one response line
per request.

- Native runtime debug server: `127.0.0.1:19842`
- Oracle (melonDS): `127.0.0.1:19843` (one above native)

Configurable via `debug.ini` per build.

**Two native modes, one protocol.** `--serve` is the headless oracle
surface: commands drive execution (`run_to_*`). `--interactive` (play
mode) serves the SAME protocol on the same port from a dedicated I/O
thread; each command executes on the frontend thread between frames
(psxrecomp handoff model), so the window keeps running. In play mode the
`run_to_*` commands return an error — the frontend owns execution; query
the always-on rings / `event_counts` / `frontend_stats` and sample twice.
`ping` is answered on the I/O thread even while a frame is in flight.
Play-mode extras: `frontend_stats` (cumulative presented-frame/phase/
underrun counters + host perf clock — diff two samples for fps),
`profile` (raw NDS_PROFILE_GPU/SCHED accumulators), `deep_trace`
(`{"on":0|1}` — live toggle for the per-access payload policy; the
inline bus fast path engages while off, the default in play mode).

## Sync on hardware events, never frame indices

The DS has two CPUs at different clocks. Comparing native vs oracle by
"frame N" is meaningless across engines. **Sync on counted hardware
events** and only then read state:

- ARM9 VBlank-IRQ count, ARM7 VBlank-IRQ count
- IPCSYNC write count, IPC FIFO send/recv count (per direction)
- DMA-completion count (per CPU/channel), timer-overflow count
- a named PC reached on a named CPU

## Commands

| cmd | args | returns |
|---|---|---|
| `ping` | — | `{"pong":true}` |
| `regs` | `cpu`:9\|7 | `{r:[16],cpsr,spsr,mode}` |
| `event_counts` | — | `{vblank9,vblank7,ipcsync_w,fifo9to7,fifo7to9,dma_done,timer_ovf}` |
| `io_state` | — | ARM9/ARM7 `{ime,ie,if,postflg,ipcsync}`, `cpu_stop`, `num_frames`, and event counts |
| `run_to_event` | `event`,`count` | advances until the named counter reaches `count`; `{reached,counts}` |
| `read_mem` | `cpu`:9\|7,`addr`,`len` | `{hex}` — the **CPU's** memory view (ARM9 and ARM7 differ) |
| `read_io` | `cpu`:9\|7,`addr`,`width`:8\|16\|32 | `{value}` — exact-width I/O register read |
| `cartridge` | `max` (default 128, maximum 8192) | Cartridge presence/current controller state plus a passive chronological ring of ROMCTRL, command, data-ready, and completion events |
| `read_region` | `region` | raw region bytes (see below) |
| `framebuffer` | `engine`:A\|B | `{w:256,h:192,rgb:<hex>}` |
| `audio_samples` | `start`,`count` (max 4096) | Non-destructive ordinal trace: `{start,count,oldest,produced,pcm_s16le}` |
| `touch` | `x`,`y`,`down` | injects a TSC touch (oracle + native both accept) |
| `keys` | `mask` | sets the DS button state |
| `gx_state` | — | geometry-engine internals: gate flags, GXSTAT raw, FIFO/PIPE levels, vertex/polygon counts, packed-GXFIFO protocol state (native + oracle) |
| `gx_run_sample` | optional `count` | no `count`: `{latest}`; else ring entry `{arm9,stat_before,stat_after,cc_before,cc_after}` for the count-th GPU3D::Run() invocation. The engine's drain state is Run()-call-time dependent, so both sides expose this cadence (native + oracle) |
| `gx_write_sample` | optional `count` | ring of ARM9 writes into the 3D register window with engine state before/after each (native + oracle) |
| `dma_sample` | optional `count` | ring of ALL DMA channel completions (`dma_done` in event_counts mirrors the oracle's SetIRQ-hook and counts only IRQ-raising ones) (native only) |

`region` ∈ `mainram` (0x02000000, 4 MB) · `wram7` (0x03800000) ·
`wramshared` · `vramA..vramI` · `palA` · `palB` · `oam` · `itcm` · `dtcm`.

## Diff loop

A harness runs both ends to the **same hardware-event count**
(`run_to_event vblank9 N`), pulls a region from each, and prints the
**first** byte divergence. Earliest divergence is the only one with a
root cause. See `oracle/` for the probes and `DEBUG.md` for the method.
