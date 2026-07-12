#!/usr/bin/env python3
"""Determinism check: reset+run_to_event(N) repeatedly, compare stop state.

If fresh-from-reset is deterministic, every repetition must land at the same
ipcsync_w count and the same ARM7/ARM9 PC. Variation => the break/anchor is
racy and the bisection's exact count is unreliable (the divergence neighborhood
still holds, but precise anchoring needs a fix).
"""
from _client import DebugClient

n = DebugClient(port=19842, timeout=600.0)
o = DebugClient(port=19843, timeout=600.0)


def stop_state(c, N, stall):
    c.cmd("reset")
    r = c.cmd("run_to_event", event="ipcsync_w", count=N, stall=stall)
    cnt = r.get("counts", {}).get("ipcsync_w")
    pc7 = c.cmd("regs", cpu=7)["r"][15]
    pc9 = c.cmd("regs", cpu=9)["r"][15]
    return cnt, pc7, pc9, r.get("reached")


for N in (78, 79, 80):
    print(f"--- target ipcsync_w={N} ---")
    for side, c, stall in (("native", n, 300000), ("oracle", o, 2000)):
        rows = [stop_state(c, N, stall) for _ in range(3)]
        same = len(set(rows)) == 1
        for cnt, pc7, pc9, reached in rows:
            print(f"  {side:7s} stop_ipcsync={cnt} arm7pc={pc7:08x} "
                  f"arm9pc={pc9:08x} reached={reached}")
        print(f"  {side:7s} => {'DETERMINISTIC' if same else 'NONDETERMINISTIC'}")
n.close(); o.close()
