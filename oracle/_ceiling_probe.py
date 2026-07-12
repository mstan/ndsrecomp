#!/usr/bin/env python3
"""Find each side's max reachable ipcsync_w via the new no-progress early-out.

Reset, then run_to_event toward an unreachable target; the stall early-out
bails at the plateau and reports where. Confirms native-vs-oracle ceiling gap.
"""
import time
from _client import DebugClient

n = DebugClient(port=19842, timeout=600.0)
o = DebugClient(port=19843, timeout=600.0)

for name, c in (("native", n), ("oracle", o)):
    c.cmd("reset")
    t0 = time.time()
    r = c.cmd("run_to_event", event="ipcsync_w", count=100000)
    dt = time.time() - t0
    cnt = r.get("counts", {})
    print(f"{name:7s} ceiling: ipcsync_w={cnt.get('ipcsync_w')} "
          f"reached={r.get('reached')} stalled={r.get('stalled')} "
          f"iters={r.get('rounds', r.get('frames'))} in {dt:.2f}s", flush=True)
    print(f"         other counts: vblank9={cnt.get('vblank9')} "
          f"vblank7={cnt.get('vblank7')} fifo9to7={cnt.get('fifo9to7')} "
          f"fifo7to9={cnt.get('fifo7to9')} timer_ovf={cnt.get('timer_ovf')} "
          f"soundbias_w={cnt.get('soundbias_w')} dma_done={cnt.get('dma_done')}",
          flush=True)
n.close(); o.close()
