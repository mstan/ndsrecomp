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

### 5.1 Palette / OAM versioned snapshot

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

### 5.4 Serialization classes (rendered inline on the emu thread)

- any frame with display capture live (F1/F2/F3)
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

## 9. What is explicitly not done

- ARM9/ARM7 threading. Out of scope by directive: shared-memory
  ordering cannot be reconciled with the byte-exact oracle gates.
- SPU off-thread (§2) — three enumerated feedback paths, ~2.4 %.
- Wifi servicing off-thread (§2) — deeply cycle-coupled, ~1.3 %, and
  the blocking half is already off-thread.
- Moving the adaptive/HD compositor or `prepare_direct_frame` off the
  emu thread. Both run at frame boundaries where the renderer is
  quiesced; they are a separate, later seam.
