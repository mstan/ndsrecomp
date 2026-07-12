#!/usr/bin/env python3
"""Sweep native vs oracle ARM7 (and ARM9) PC across fresh ipcsync_w anchors to
find when native's ARM7 first wedges at the SWI/exception vector (pc=0x08)."""
from _client import DebugClient

n = DebugClient(port=19842, timeout=600.0)
o = DebugClient(port=19843, timeout=600.0)


def at(c, N, stall):
    c.cmd("reset")
    c.cmd("run_to_event", event="ipcsync_w", count=N, stall=stall)
    r7 = c.cmd("regs", cpu=7); r9 = c.cmd("regs", cpu=9)
    return (r7["r"][15], r7["cpsr"], r7["r"][14],
            r9["r"][15])  # arm7 pc, arm7 cpsr, arm7 lr, arm9 pc


for N in (2, 5, 10, 20, 30, 40, 50, 55, 60, 65, 70, 74, 76, 78):
    p7n, c7n, l7n, p9n = at(n, N, 300000)
    p7o, c7o, l7o, p9o = at(o, N, 2000)
    mark = "  <<< ARM7 pc DIFF" if p7n != p7o else ""
    print(f"N={N:3d}  ARM7 nat pc={p7n:08x} cpsr={c7n:08x} lr={l7n:08x} | "
          f"ora pc={p7o:08x}   ARM9 nat={p9n:08x} ora={p9o:08x}{mark}",
          flush=True)
n.close(); o.close()
