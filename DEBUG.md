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

All on in Release. Eviction keeps memory bounded; targeted dumps pull
the requested slice.

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
