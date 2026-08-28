# Device-work parallelization: threaded GPU2D scanline render

Bead: `beads-yjp.46`. Branch: `wt/device-parallel`. Base: `cddba78`.

## 1. Why

Field evidence (2026-08-27, `beads-6vqh` notes): on a struggling player's
machine devices are ~37 % of emu time (~8.9 ms/frame), of which
`gpu2d_render` is 2.23 ms measured exactly and ~4.75 ms is unattributed
device time. The scheduler already breaks the device block into
`display_ns / spu_ns / wifi_ns / rtc_ns / sysev_ns`
(`runner/src/scheduler.cpp:428-450`), and on the local pinned run
`perf-results/20260826-185628-forced-tier3-phase0-pinned/report.json`
gives `spu_ns` ≈ 2.4 % and `wifi_ns` ≈ 1.3 % of sampled round time, with
display dominating the rest.

So the target is display. `gpu2d` line rendering produces **host output
only**, with exactly one write-back into guest state (DISPCAPCNT
capture). That makes it the one device with a real off-thread seam.

## 2. Scope decision: GPU2D yes, SPU and wifi no

### GPU2D — implement

Per-scanline rendering onto worker thread(s) driven from a per-line
latch. Details below.

### SPU — deferred, with reasons

Three live feedback paths inside `mix()`:

1. `cnt &= ~0x80000000u` on one-shot end
   (`runner/src/spu.cpp:124`, `:135`, `:161`) is the exact bit the guest
   polls back through `nds_spu_read*` (`spu.cpp:372/384/396`). It is a
   live bit, not a snapshot.
2. SNDCAP writes guest RAM through `bus_device_write32`
   (`spu.cpp:252`), which runs `note_ram_write` → per-4 KiB
   code-provenance generation bumps and `runtime_note_code_write()`.
   That is the tier-3 dirty-code machinery, not just guest data.
3. `bus_device_read32/write32` (`runner/src/bus.cpp:856-869`)
   temporarily overwrite the global `g_nds_active`, which the executing
   CPU's fast path reads on every access
   (`recompiler/armv4t/runtime_arm.h:237-259`). Concurrent execution
   would mis-resolve WRAM windows.

Sample-window pre-copies plus an atomic done-bit committed at
rendezvous would make this liftable, but it is a redesign of the mixer,
not a move, and it buys ~2.4 %. Deferred.

### Wifi — deferred, with reasons

The valuable half is **already** off-thread: sockets / libslirp /
`getaddrinfo` live on `WifiWorkerThreadMain`
(`runner/src/wifi_net.cpp:1448-1476`) behind two bounded queues. What
remains on the emu thread is the guest-cycle-quantized device model,
which:

- writes `Wifi::RAM[0x2000]` and the `W_*` IOPORT file while the ARM7
  reads/writes the same arrays through `nds_wifi_read/write`;
- raises a guest IRQ synchronously (`Wifi.cpp:387` →
  `runner/src/gpu3d.cpp:216` → `nds_raise_irq`), which can wake a
  halted CPU;
- writes back into the scheduler via `nds_reschedule_slice`
  (`wifi_net.cpp:136`), shortening the live dispatch slice;
- reads `g_runtime_cycles` mid-slice on every register access
  (`wifi_net.cpp:2069`) to establish "now".

`scheduler.cpp:99-109` records a measured 46→19 FPS regression from
delaying eventful wifi ticks by under 2 µs. With networking off (the
default) servicing costs ~28 ns/round, essentially all measurement
overhead. Deferred; the honest wifi optimization is coalescing the 8 µs
`USTimer` when powered on, which is emu-thread work, not threading.

## 3. Feedback-path inventory (GPU2D)

Everything the 2D line pipeline can push into, or that a CPU can read
back out of, the render path. This is the list the design must handle
explicitly.

| # | Path | Site | Handling |
|---|------|------|----------|
| F1 | **DISPCAPCNT capture writes guest VRAM.** `do_capture` writes `width` 16-bit pixels per line into `nds_vram_bank_data(dstbank)`, bypassing `bank_write` (so it sets no `g_vram_written[]` and bumps no `g_vram_generation[]`). | `gpu2d.cpp:1048-1146`, called `:1240` | **Serialize.** A frame with `capture_latch` (or DISPCAPCNT bit 31) live renders entirely on the emu thread. Decided at `nds_gpu2d_start_frame`, where `g_frame_capture_active` is already computed (`:1670-1672`). |
| F2 | **`nds_vram_note_capture_write()` bumps `g_texture_generation`**, read by the 3D engine's flat-VRAM coherence at VCount215 (`gpu3d.cpp:232-272`). | `gpu2d.cpp:1145` | Covered by F1 (capture frames are serial). |
| F3 | **DISPCAPCNT bit 31 auto-clear** at line 192, gated on `capture_latch` which the renderer sets at engine 0, y==0. The guest reads DISPCAPCNT back at `0x04000064`. | set `gpu2d.cpp:1162-1163`; cleared `:1698-1707`; read `:1370,1379` | **Move the latch-set onto the emu thread**: it becomes part of the per-line latch step, not the worker. |
| F4 | **`refx_internal`/`refy_internal` accumulate across lines**, advancing only on lines where that affine/extended layer is actually decoded, and reloading on any guest write to BG2/3X/Y. Not guest-readable, but order-dependent. | advance `gpu2d.cpp:554-558`; reload `:1410-1426`, `:1690-1695` | **Emu thread owns the accumulator.** The latch step decides the active BG set (the same code that used to run inside the renderer, factored out), records the pre-advance coordinates into the job, and advances the master. Workers never touch `g_unit`. |
| F5 | **`nds_gpu3d_line(y)` is not a pure read**: `SoftRenderer::GetLine` blocks on and consumes tokens from `Sema_ScanlineCount`, and `GPU3D::GetLine` writes the single shared `ScrolledLine` scratch when `RenderXPos != 0`. Compute mode additionally touches a thread-affine GL context. | `GPU3D_Soft.cpp:1820-1834`, `GPU3D.cpp:2593-2660`, `gpu3d.cpp:763-800` | **Emu thread calls it, in order, once per line**, and copies the line into the job. Never called from a worker. |
| F6 | **`nds_gpu2d_requires_3d_readback()`** (returns `!g_direct_frame_active`) gates the compute renderer's PBO readback and must be resolved before VCount215. | `gpu2d.cpp:1733`, `gpu3d.cpp:742,757,905` | Unchanged: `g_direct_frame_active` is set at `start_frame` on the emu thread. |
| F7 | **VRAM display mode 2** reads live guest VRAM through the LCDC mapping (`nds_video_read(9, 0x06800000+…)`). Pure read, no pointer advance. | `gpu2d.cpp:1227-1234` | Covered by the VRAM write fence (§5). Mode 2 stays threaded. |
| F8 | **Main-memory display mode 3 / DISP_MMEM_FIFO** — not implemented; writes to `0x04000068` fall through every case and are not even shadowed. No FIFO pointer state exists. | `gpu2d.cpp:1235-1239`, `:1382`, `:1390-1498` | Nothing to synchronize. |
| F9 | **VCOUNT / DISPSTAT / VBlank / VCount-match IRQs** are not touched by gpu2d at all; they live in `nds_tick_display`. DISPSTAT bit 1 is never set and there is no HBlank IRQ. | `io.cpp:2646-2676` | Unaffected. Latching is inserted before this loop, exactly where the render call is today. |
| F10 | **DMA**: `nds_dma_trigger` is reached only from the gamecard IRQ path and `NDS::CheckDMAs` from the 3D engine. No VBlank DMA, no HBlank DMA, no display-capture DMA, no display-FIFO DMA. | `io.cpp:2527-2533`, callers `io.cpp:2258`, `gpu3d.cpp:221` | Nothing in the scanline path starts a DMA. Unaffected. |
| F11 | **3D FIFO / GXSTAT**: the line path never writes them. The only gpu2d→gpu3d writes are `nds_gpu3d_set_render_xpos` from the *register-write* path. | `gpu2d.cpp:1399,1443,1446` | Register writes stay on the emu thread. Unaffected. |
| F12 | **POWCNT1** read three ways in the line path: engine enable, bit-15 screen routing, direct-class. Palette/OAM/view accessors are POWCNT-gated and return `nullptr` when off, which the renderer turns into a force-blank line. | `gpu2d.cpp:1150-1151,1157,1367`; `vram.cpp:483-497` | **Latched into the job** (enable bit + routed screen index), so a POWCNT write mid-frame cannot retroactively reroute an already-latched line. This matches the existing intent documented at `gpu2d.cpp:60-65`. |
| F13 | **`g_front` buffer flip** in `nds_gpu2d_finish_frame` is an unsynchronized plain `int`, and the present path reads `g_fb[g_front]`. | `gpu2d.cpp:1697`, `:1708` | Jobs carry their destination buffer index explicitly, and `finish_frame` **drains** before flipping. Frame-boundary drain also covers the adaptive/HD compositor and `prepare_direct_frame`. |
| F14 | **VRAMCNT remap** rebuilds `g_renderer_view[]` and the `g_abg/g_aobj/...` mask arrays that workers dereference. | `vram.cpp` `nds_vram_map` | **Fence** (§5). |
| F15 | **Palette / OAM writes** land in `g_palette` / `g_oam`, which workers read live today. | `vram.cpp:441-451` | **Versioned snapshot** (§5). |
| F16 | **Function-local mutable statics** in the render path: `static std::array<BgLine,4> bg_lines` twice, `static std::array<uint32_t,256> comp6`. Not guest state, but a data race that would corrupt output. | `gpu2d.cpp:997`, `:1257`, `:1209` | Moved into a per-worker scratch struct passed down. |
| F17 | **Profiling counters** incremented inside the line path (`g_obj_ns`, `g_render_ns`, `g_engine_ns[]`, `g_text_lines[][]`, `g_no_effect_lines[]`, `g_render_scanlines`, `g_direct_class_engine_a_ns[]`, `g_direct_extra_bg_mask_engine_a_ns[]`). | `gpu2d.cpp:991,1250,1295,1310,1335,1572-1584` | Per-worker accumulators merged at each drain. Not guest-visible, but must not tear. |
| F18 | **Wide-3D snapshot** `g_wide_3d_frame[g_front^1]` / `_attr_` / `_width` written per line. | `gpu2d.cpp:1189-1207` | Per-line rows are disjoint; `g_wide_3d_width` is set once on the emu thread at latch. |

Not a feedback path, but worth recording as checked and clear: the
renderer-facing VRAM read helpers (`nds_vram_read_bg/obj/bg_extpal/
obj_extpal`, `mapped_read`, `nds_vram_renderer_palette/oam/view`,
`vram.cpp:468-497`) are pure reads over mask arrays with no mutable
cache, so they are safe to call concurrently as long as no writer runs.

## 4. Latch model

A `LineJob` must carry everything a line render reads that can change
before the worker runs.

```
struct LineJob {
    Unit     unit[2];        // full register mirror, both engines
    int32_t  refx[2][2];     // pre-advance affine coords, per engine
    int32_t  refy[2][2];
    uint8_t  active_bg[2][4];// ordered, front-first, from the latch step
    uint8_t  active_bg_count[2];
    uint16_t powcnt;         // engine enable + bit-15 routing
    uint8_t  screen[2];      // routed physical screen per engine
    uint8_t  buffer;         // destination g_fb index (was g_front ^ 1)
    uint8_t  y;
    bool     engine_a;       // false when direct-present skips engine A
    bool     bg0_3d;
    uint32_t capw;           // 0 when this line does not capture
    const PalOamSnapshot* pal_oam[2];   // versioned, immutable
    const uint32_t* line3d;  // owned copy, 256 or wide_width entries
    uint16_t wide_width;
};
```

Classification of every `Unit` member (`gpu2d.cpp:19-41`):

- **guest-writable register mirrors**, all read live per line today and
  therefore all latched: `dispcnt`, `bgcnt[4]`, `bgx[4]`, `bgy[4]`,
  `pa/pb/pc/pd[2]`, `refx[2]`, `refy[2]`, `win[12]`, `bg_mosaic_x/y`,
  `obj_mosaic_x/y`, `bldcnt`, `bldalpha`, `eva`, `evb`, `evy`,
  `master_bright`, `capture`.
  Note DISPCNT is **not** latched at line 0 today — every layer decode
  reads it live. Per-line latching reproduces that exactly.
- **accumulated across lines**: `refx_internal[2]`, `refy_internal[2]`
  — see F4. Owned by the emu thread.
- **latched at frame start / line 0**: `capture_latch` — see F3.
- Reloaded at frame start from `refx`/`refy` in
  `nds_gpu2d_start_frame` (`:1690-1695`), unchanged.

Memory the line render reads, and how each is captured:

| Region | Size | Strategy |
|--------|------|----------|
| Palette (per engine) | 1 KiB | Versioned snapshot (§5.1) |
| OAM (per engine) | 1 KiB | Versioned snapshot (§5.1) |
| BG / OBJ / ext-pal VRAM | up to 656 KiB | Write fence (§5.2) |
| `g_renderer_view` chunk tables + bank masks | small, but rebuilt on remap | Write fence (§5.2) |
| 3D line (and wide/attr lines) | 1 KiB / 1.8 KiB | Copied into the job by the emu thread (F5) |
| LCDC banks for mode 2 and capture source B | — | Write fence (§5.2); capture frames serial (F1) |

## 5. Sync points

> **As built.** The palette/OAM versioned snapshot below is **deferred**;
> palette and OAM writes currently take the same fence as VRAM (§5.2). The
> fence is exact either way, and the snapshot is purely an optimization to
> reduce fence frequency. The census that decides whether it is worth
> building now exists: `fence_drains` / `fenced_lines` are reported per
> cause, and on the firmware route the split is 556 VRAM drains against 1
> OAM drain and 0 palette drains over 866 frames — palette/OAM writes during
> active display are not the problem there. Revisit against an MPH route
> before building the ring.
>
> **As built.** Capture serialization is per **line**, not per frame (§5.4).

### 5.1 Palette / OAM versioned snapshot (deferred)

`g_palette` and `g_oam` are 2 KiB total each. A monotonically increasing
`g_pal_oam_generation` is bumped by every palette/OAM write in
`nds_video_write` (`vram.cpp:441-451`). The latch step compares the
current generation against the generation of the newest snapshot; if
unchanged, the job reuses that snapshot, otherwise a new slot in a ring
of snapshot buffers is filled with a 4 KiB memcpy.

Cost: 4 KiB per *change*, not per line. Frames that never touch palette
or OAM during active display pay one memcpy per frame. A frame that
rewrites the palette on every scanline pays 192 × 4 KiB = 768 KiB,
which is still far cheaper than a fence.

The snapshot ring is sized so a slot cannot be recycled while a job
still references it; recycling is checked against the completed-job
counter at latch time, and if the ring is full the latch step drains.

### 5.2 VRAM write fence

Any guest write that changes what a pending worker would read from VRAM
must not be applied while jobs are outstanding. The fence is a drain:
block until all queued jobs have completed, then apply the write.

Fence points:

- `nds_video_write` for the `0x06000000` VRAM aperture (`vram.cpp:445`)
- `nds_vram_map` (VRAMCNT remap → rebuilds views, F14)
- any DMA or CPU path that reaches VRAM (all of them funnel through
  the same accessors)

The check on the hot path is one relaxed atomic load of the outstanding
count; the drain is taken only when it is non-zero. Because the render
of line *N* happens at HBlank of line *N*, writes that arrive during
line *N+1* were always meant to affect line *N+1* and not line *N* —
so the fence is what preserves exactness, and its frequency is a
performance question, not a correctness one.

On drain, the emu thread also **executes pending jobs itself** rather
than idling, so a pathological write pattern degrades to the
single-threaded cost plus queue overhead instead of stalling.

Fence frequency is instrumented (`fence_drains`, `fenced_lines`,
`fence_ns`) and reported; if MPH turns out to write VRAM every active
scanline, the honest outcome is "no win on this route", not a silently
wrong framebuffer.

### 5.3 Frame boundary drain

`nds_gpu2d_finish_frame` (263→0 wrap) drains before flipping `g_front`
(F13). That single point also guarantees:

- `nds_gpu2d_start_frame` / `prepare_direct_frame` see a quiesced
  renderer;
- the frontend's `nds_gpu2d_framebuffer` / `adaptive_framebuffer` /
  `hd_frame` / `direct_frame` all read a complete frame;
- the 3D renderer's next frame (started at VCount215, one buffer, not
  double-buffered) cannot be overwritten while a worker still holds a
  pointer into it — jobs own copies anyway (F5), but the drain makes
  the invariant unconditional.

An additional drain is taken before any debug-server read of the
framebuffer or the gpu2d profile, so instruction-precise queries stay
exact.

### 5.4 Display capture: staged, not serialized

Capture lines are **not** serialized. Serializing them capped MPH — which
captures on ~79 % of frames at full height — at 11–21 % eligible lines, so
the capture unit is split in two instead:

- `stage_capture` runs on whichever thread renders the line. It reads
  source A (already in hand) and source B (an LCDC bank) and writes only
  into the job's `capture_pixels`, recording the destination bank, the
  start address and the pixel count. It touches no guest state.
- `apply_staged_capture` runs on the **scheduler thread only**, in ring
  order, at the next drain. It performs the guest-visible half: the VRAM
  write (reproducing the exact `& 0xFFFF` address sequence from the
  recorded start) and the `nds_vram_note_capture_write()` texture-generation
  bump (F2).

F3 is unaffected: the DISPCAPCNT enable latch was already set in
`latch_line` and cleared in `nds_gpu2d_vblank`, both on the scheduler
thread, and `nds_gpu2d_vblank` drains first — so every capture write for a
frame lands before VCount 192, well before the 3D engine's VCount 215.

**Why deferring the write is exact.** `DoCapture` writes only when the
destination bank is LCDC-mapped, and a bank has exactly one VRAMCNT
setting, so an LCDC bank is by construction absent from the BG/OBJ renderer
views and from the texture slots. **BG/OBJ decode therefore can never read
a staged bank**, and a VRAMCNT remap that would change that already fences
(F14). That leaves exactly three readers:

| Reader | Handling |
|--------|----------|
| Display mode 2 (reads the DISPCNT-selected LCDC bank) | `line_reads_staged_bank` drains before the line is published — after which the line still goes to a worker, not inline |
| Capture source B (same bank field; only when the capture actually uses B, i.e. not source-A-only and not the all-zero FIFO) | same drain |
| The guest / DMA / debug server | `nds_video_read` and `nds_video_get_region` take a read-side fence on `nds_gpu2d_staged_captures`. Guest *writes* already fenced on `nds_gpu2d_jobs_outstanding`. |

The read fence costs one relaxed atomic load, and the counter is only ever
non-zero on capture frames with threading on, so the single-threaded path
pays nothing.

Pinned by `test_capture_serializes_and_matches` in
`runner/tests/gpu2d_window_test.cpp`: the captured VRAM bank and both
framebuffers must be byte-identical between the two modes, the captured
bytes must be non-zero, every capture write must have been applied (192 and
128 for the two DISPCAPCNT sizes), and all 192 lines must be threaded.

On MPH this takes the census from 110,761 threaded / 413,760 inline to
**524,521 threaded / 0 inline**, with **zero** capture-hazard drains.
- the frame after a reset / power transition (`nds_gpu2d_reset`,
  `nds_gpu2d_stop`)
- when `NDS_GPU2D_THREADED=0` (the default until proven)

## 6. Thread topology

- A fixed pool of `NDS_GPU2D_WORKERS` threads (default 1; the whole
  2.23 ms fits in the emu frame's slack with one worker, and one
  worker minimizes both fence latency and memory traffic).
- A ring of `LineJob` slots (512, > 2 × 192 so a whole frame plus
  margin fits). The emu thread publishes with a release store to an
  atomic head; workers claim slots with `fetch_add` on a claim index
  and publish completion with `fetch_add` on a completed counter.
- Workers wait on a counting semaphore (the same
  `std::mutex`/`std::condition_variable` shim shape used by
  `gpu3d.cpp:303-342` and `live_overlay.cpp:150-162`).
- The pool is created lazily on first threaded frame and joined at
  shutdown.

This mirrors the in-tree GPU3D contract (atomic progress counter +
counting semaphore + lazy consumer block, `GPU3D_Soft.cpp:1786-1832`)
rather than inventing a job system.

## 7. Runtime toggle

`NDS_GPU2D_THREADED=0|1`, default **0**, validated and logged at
startup next to `NDS_3D_THREADED` (`main.cpp:1335-1366`), read once via
the function-local `static const` lambda pattern
(`gpu3d.cpp:92-103`). `NDS_GPU2D_WORKERS=<n>` sizes the pool. With the
toggle off, the latch step still runs but each job is executed inline
immediately, i.e. the identical sequence of renders in the identical
order — so the LLE-faithful path stays not just forceable but
structurally the same code.

## 8. Staging and gates

| Stage | Change | Gate |
|-------|--------|------|
| S1 | Thread-safety refactor only: hoist the two `bg_lines` statics and `comp6` into a per-render scratch struct; split `render_engine_line` into a latch step (emu thread, owns `refx_internal` and `capture_latch`) and a pure render step; route profiling through a merge-able accumulator. Still fully single-threaded and inline. | `ctest`; `probe_machinery_bytelock.py` framebuffer + register + event-counter byte-lock vs the base build |
| S2 | Job ring, worker pool, palette/OAM snapshot ring, VRAM write fence, frame-boundary drain, capture serialization, `NDS_GPU2D_THREADED` toggle. | S1 gates with the toggle off; byte-lock with the toggle on vs off; firmware scenario set; MPH attract + adventure |
| S3 | Measurement: interleaved A/B min-of-3 on the dev box and on an affinity-limited configuration. | `display_ns` / `gpu2d_render_ns` / `phase_ms_per_frame.emu` deltas reported with fence counters |

Gate commands (verified paths):

```
# ctest
C:\msys64\mingw64\bin\ctest.exe --test-dir <build> --output-on-failure

# byte-lock: framebuffer SHAs + both register files + full event_counts
py -3 tools\probe_machinery_bytelock.py --exe <build>\nds_runner.exe ^
   --bios F:\Projects\ndsrecomp\ndsrecomp\bios --boot direct ^
   --start 100000000 --step 100000000 --count 7 --output <dir>

# firmware scenario set (8 scenarios, tier3=0)
F:\Projects\ndsrecomp\run_all_g1.ps1

# MPH routes
py -3 tools\measure_mph_scenario.py --route attract|adventure ^
   --exe <build>\nds_runner.exe --boot direct --repetitions 3
```

Capture coverage: there is **no** existing DISPCAPCNT scenario in either
repo (`ENHANCEMENTS.md:105` lists it as remaining work). The runner
already counts capture frames as direct class 5, published as
`profile.direct_class_frames.capture` under `NDS_PROFILE_GPU=1`
(`gpu2d.cpp:139-144`, `debug_server.cpp:1269-1281`), so the capture case
is located empirically by censusing the routes with that counter, and the
serialization path (§5.4) is what the census must show being taken.

## 9. Measured results

All numbers are same-binary A/B on `NDS_GPU2D_THREADED`, anchored on guest
`insn9` so both legs execute identical guest work, with one worker.

### Correctness

| Gate | Result |
|------|--------|
| `ctest` | 18/18, including the new `test_capture_serializes_and_matches` |
| Byte-lock, firmware LLE, threaded vs single, 7 stops 50M..350M | **PASS** — `fb_A`, `fb_B`, `regs9`, `regs7`, full `event_counts` identical |
| Byte-lock, baseline `cddba78` build vs this build, both legs | **PASS** — identical at all 7 stops |
| Byte-lock, MPH direct boot, threaded vs single, 7 stops 100M..700M | **PASS** — identical at all 7 stops |
| Witness that threading ran | firmware `threaded_lines`=181,969, `inline_lines`=0 |

### Firmware route (LLE menu), insn9=100M, 866 frames, 181,969 scanlines

100 % of lines are eligible for a worker on this route
(`threaded_lines`=181,969, `inline_lines`=0), so it measures the mechanism
itself. Interleaved min-of-3, one worker, all cores:

| leg | wall (uninstrumented) | `display_ns` | `devices_ns` | `sampled_round_ns` |
|-----|----------------------:|-------------:|-------------:|-------------------:|
| threaded off | 5.928 s | 1603.4 ms | 2433.5 ms | 6745.3 ms |
| threaded on  | **5.007 s** | **769.5 ms** | 1584.9 ms | 5713.7 ms |
| delta | **−15.5 %** | **−52.0 %** | −34.9 % | −15.3 % |

The wall-time column is the honest end-to-end number: profiling is off, so
nothing is instrumented, and the two legs are the same binary reaching the
same guest anchor.

#### Core scaling

Same route, same binary, same min-of-3, with both legs pinned to the same
core mask (`-Affinity`) so the comparison is fair at each width:

| cores | wall off | wall on | Δ wall | `display_ns` off | on | Δ display |
|-------|--------:|--------:|-------:|-----------------:|---:|----------:|
| all (dev box) | 5.928 s | 5.007 s | **−15.5 %** | 1603.4 ms | 769.5 ms | −52.0 % |
| 4 (`0xF`) | 6.010 s | 5.111 s | **−15.0 %** | 1771.5 ms | 784.0 ms | −55.7 % |
| 2 (`0x3`) | 5.990 s | 5.867 s | **−2.1 %** | 1710.5 ms | 921.2 ms | −46.1 % |

The emu thread's display cost roughly halves at every width — that part is
the mechanism working. End-to-end wall time only follows when there is a
spare core to absorb the work: at two cores the worker competes with the
emu thread and the GPU3D render thread for the same two CPUs, so the win
collapses to ~2 %. **This is a latency-hiding change, not a work-reduction
change**, and it needs a core to hide the work on. A genuinely dual-core
field machine gains almost nothing; a four-core one gains ~15 %.

Cost of the latch when threading is off, single runs against the baseline
`cddba78` build on the same route: `display_ns` 1346.6 ms baseline against
1398.3 ms with the latch, i.e. roughly **+3.8 %** of display time, or
+0.06 ms per emulated frame, for the snapshot and the 3D-line copy.

Fences on this route: 556 VRAM drains, 1 OAM, 0 palette, 795 frame-boundary.
8,316 of 181,969 lines (4.6 %) were finished by the helping scheduler thread
rather than a worker.

### MPH (direct boot), insn9=400M, 2,710 frames, 524,521 scanlines

**MPH runs display capture on roughly 79 % of frames at full 192-line
height.** With threading on, `inline_lines`=413,760 against
`threaded_lines`=110,761 — only **21 %** of scanlines are eligible for a
worker, because capture lines serialize by construction (§5.4).

With capture staging (§5.4) MPH is **100 % eligible**: 463,104 threaded
lines against 0 inline at insn9=200M, and zero capture-hazard drains.

Interleaved min-of-5 at insn9=200M, 2,391 frames, one worker, both legs
pinned to the same core mask, measured on an **idle** box (no other runner,
compiler or ninja process; verified before and after each configuration):

| cores | wall off | wall on | Δ wall | `display_ns` off | on | Δ display |
|-------|--------:|--------:|-------:|-----------------:|---:|----------:|
| all (16 logical / 8 physical, 9800X3D) | 15.692 s | 15.033 s | **−4.2 %** | 3041.7 ms | 3184.8 ms | +4.7 % |
| 4 (`0xF`) | 16.562 s | 15.620 s | **−5.7 %** | 3669.9 ms | 2755.4 ms | **−24.9 %** |
| 2 (`0x3`) | 15.073 s | 17.283 s | **+14.7 %** | 3732.9 ms | 3766.3 ms | +0.9 % |

**Read this plainly.**

- **4 cores and all cores are a real win**: 4–6 % off end-to-end wall time,
  and at 4 cores a quarter off the emu thread's display cost. These are the
  decision numbers, because the field hardware in question (5700X, 7730U)
  is 8-core.
- **2 cores is a clear regression, −14.7 % throughput.** With two CPUs the
  emu thread, the GPU2D worker and the GPU3D render thread contend, and the
  emu thread ends up blocking in the vblank drain for work it could have
  done itself. Do not enable the toggle on a dual-core host.
- The all-cores `display_ns` rising while wall time falls is consistent with
  that same effect at a smaller scale: with 16 logical CPUs the OS spreads
  the three threads across CCX boundaries, so the drain wait lands inside
  `display_ns` even though total throughput improves. The uninstrumented
  wall column is the metric to trust.

For contrast, **before** capture staging the same MPH route measured
**+4.3 %** wall at all cores — a regression — because only 11–21 % of lines
were eligible. Staging is what turned MPH from a regression into a win.

Earlier single unrepeated runs at insn9=400M appeared to show display
−18.8 %, and an earlier min-of-3 sweep produced wall times that *fell* as
cores were removed (22.3 s at all cores against 17.9 s at two), which is
physically impossible and proves that run was dominated by another
session's concurrent PGO A/B. Neither set is quoted; both were discarded
and re-measured on the idle box.

**Honest conclusion.** The mechanism works, is exact, and every route is
now 100 % eligible. It is worth ~15 % of end-to-end run time on the
firmware route and ~4–6 % on MPH at four or more cores, and it is a
**regression on two cores**. It is latency hiding, not work reduction: it
needs a spare core to hide the work on.

That is why the toggle ships **default-off**. Enabling it is a per-host
decision — sensible on the 4+ core parts that dominate the field hardware,
wrong on a dual-core machine. A future auto-enable should key on
`std::thread::hardware_concurrency()` rather than being global.

### Remaining follow-ups

- **Auto-enable policy.** Turn the toggle on by default above a core-count
  threshold, measured rather than guessed.
- **Worker count.** Everything above uses one worker. `NDS_GPU2D_WORKERS`
  goes to 16 and has not been swept; MPH's `display_ns` behaviour at all
  cores suggests the drain wait, not the render, is the remaining cost, so
  more workers may help there and hurt at low core counts.
- **Palette/OAM snapshot ring** (§5.1), if a route ever shows palette or
  OAM writes dominating the fence census. Neither the firmware route nor
  MPH does today.

## 10. What is explicitly not done

- ARM9/ARM7 threading. Out of scope by directive: shared-memory
  ordering cannot be reconciled with the byte-exact oracle gates.
- SPU off-thread (§2) — three enumerated feedback paths, ~2.4 %.
- Wifi servicing off-thread (§2) — deeply cycle-coupled, ~1.3 %, and
  the blocking half is already off-thread.
- Moving the adaptive/HD compositor or `prepare_direct_frame` off the
  emu thread. Both run at frame boundaries where the renderer is
  quiesced; they are a separate, later seam.
