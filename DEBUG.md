# DEBUG.md — Observability & the Debug Loop

## Always-on rings, never arm-then-capture

Every diagnostic here is a **ring buffer that records continuously**
from the moment the runtime starts. Probes **query** the ring for the
window of interest; they never arm recording at probe time. LLM
round-trips and process startup take real wall-clock — by the time you
"armed a trace and ran," the interesting event already happened.

If you catch yourself reasoning "the event must have happened before I
attached" — STOP. The ring isn't covering enough. **Extend the ring**
(add the event class to the always-on path), then query it.

This applies doubly to the two CPUs: **never pause both, step them in
lockstep, and read state to compare.** That is arm-then-capture in
disguise and synthesizes a state instead of reading history. Free-run,
query the rings for the window, diff. Pause/step is only a
human-at-a-debugger control-plane primitive, never a way to
synchronize the ARM9 and ARM7 observers.

## Ring inventory (build out as phases land)

- `bus_ring9` / `bus_ring7` — every ARM9 / ARM7 bus access
  (pc, addr, value, width, r/w, cycle).
- `ipc_ring` — every IPCSYNC write and IPC FIFO send/recv, both
  directions, with the cycle and the CPU.
- `dispatch_ring9` / `dispatch_ring7` — recompiled-function entries
  per CPU (for control-flow reconstruction).
- `dirty_ring` — every promotion of a RAM region to dirty-RAM
  interpretation, and every block interpreted there.
- `frame_record` — per-frame snapshot: both CPUs' regs, POWCNT,
  DISPCNT A/B, VRAMCNT, IF/IE/IME per CPU.
- `irq_ring9` / `irq_ring7` — IRQ raise / acknowledge / BIOS return.
- `dispatch_misses.log` — append-only; a miss is a P0 silent bug.
- `hostprof` — the only ring that records HOST addresses rather than
  guest ones (`runner/src/host_profile.h`). A sampler thread started at
  runner init suspends each registered thread (emu at 250 Hz; 2D/3D
  workers, audio and the live compiler at a quarter of that, because the
  cost is entirely per-suspend), copies its registers and 64 KB of stack, and
  unwinds the copy against the PE `.pdata`/`.xdata` tables — so it needs
  no PDB and works on stripped Release builds and on shard DLLs. 1<<18
  samples x 16 frames = 36 MB, ~17 minutes of history. Query with
  The 250 Hz is a measurement, not a preference: one
  suspend/context/resume cycle costs ~33 us median regardless of how much
  stack is copied, so the sampled thread loses `hz * 33 us` per second and
  only 250 Hz fits inside 1 percent. `NDS_HOSTPROF_HZ=1000` is an opt-in
  for a narrow high-resolution session, at 3.3 percent. Query with
  `hostprof_top` (top-K by self time, module+RVA), `hostprof_dump`
  (samples + module map to a file) and `hostprof_status`; the whole ring
  also lands in the diagnostics bundle at shutdown. Names come from
  `tools/hostprof_symbolize.py` offline, which also prints the
  self/inclusive/category tables. `NDS_HOSTPROF=off` opts out;
  `NDS_HOSTPROF_HZ`, `NDS_HOSTPROF_RING` tune it.

  This is the answer to "emu_ms is 17 ms/frame and every diagnostic we
  have points at a guest address". It does NOT replace the guest-side
  surfaces: read `pc_hot`/`emu_attrib` for what the GUEST is doing and
  `hostprof` for what the HOST is spending, and cross them.

All on in Release. Eviction keeps memory bounded; targeted dumps pull
the requested slice.

One documented exception, the **deep-trace policy** (runtime_arm.h): the
interactive frontend exposes no query surface, so the per-access payloads
(`bus_ring` entries for RAM data accesses via the inline bus fast path,
mem_r/mem_w trace events, per-insn register images) are dropped there for
real-time headroom. Every mode with a query surface (`--serve`, batch)
keeps them all; `NDS_DEEP_TRACE=0` on a serve server opts into the
interactive behavior for fast-path equivalence proofs and honest perf A/B.
Event counters (insn9/insn7, event_counts) advance in every mode.

## The loop

1. Find the **first** divergence vs the melonDS oracle, not the final
   visible bug. Everything after the first divergence is consequence.
2. Classify it: discovery/codegen, scheduling/timing, memory/bus, I/O,
   IPC/sync, IRQ/DMA/timer, CP15/TCM, VRAM-bank mapping, 2D-engine
   raster, SPI/touch, or config.
3. Sync via **hardware events**, not frame numbers (IPCSYNC writes,
   FIFO counts, VBlank IRQ count, BIOS-IRQ-return count, a specific PC
   at a specific function entry on a named CPU).
4. Fix the **producing** logic (recompiler / runtime / config), regen,
   rebuild, re-measure. Never hand-patch generated C.

## Tool skepticism

Treat every tool result as untrusted until cross-checked against the
oracle or a known-good case. Validate first outputs (disassembly,
decoder results, oracle responses, frame dumps) by hand. If
observability is missing, **extend the structured debug surface** (a
ring, a trace, a snapshot) — never build conclusions on ad-hoc
`printf` spam.
