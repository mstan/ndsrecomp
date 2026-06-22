# TCP debug protocol

Both the native runtime's debug server and the melonDS **oracle** speak
the same line-delimited JSON request/response protocol, so one diff
harness can talk to either. One JSON object per line; one response line
per request.

- Native runtime debug server: `127.0.0.1:19842`
- Oracle (melonDS): `127.0.0.1:19843` (one above native)

Configurable via `debug.ini` per build.

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
| `read_region` | `region` | raw region bytes (see below) |
| `framebuffer` | `engine`:A\|B | `{w:256,h:192,rgb:<hex>}` |
| `touch` | `x`,`y`,`down` | injects a TSC touch (oracle + native both accept) |
| `keys` | `mask` | sets the DS button state |

`region` ∈ `mainram` (0x02000000, 4 MB) · `wram7` (0x03800000) ·
`wramshared` · `vramA..vramI` · `palA` · `palB` · `oam` · `itcm` · `dtcm`.

## Diff loop

A harness runs both ends to the **same hardware-event count**
(`run_to_event vblank9 N`), pulls a region from each, and prints the
**first** byte divergence. Earliest divergence is the only one with a
root cause. See `oracle/` for the probes and `DEBUG.md` for the method.
