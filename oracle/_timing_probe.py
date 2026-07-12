#!/usr/bin/env python3
"""One-off: measure fresh reset+run_to_event cost for reachable N (flushed)."""
import sys, time
from _client import DebugClient

n = DebugClient(port=19842, timeout=600.0)
o = DebugClient(port=19843, timeout=600.0)

for N in (60, 150, 211):
    for name, c in (("native", n), ("oracle", o)):
        t0 = time.time()
        c.cmd("reset")
        t1 = time.time()
        r = c.run_to_event("ipcsync_w", N)
        t2 = time.time()
        cnt = r.get("counts", {}).get("ipcsync_w")
        print(f"{name:7s} N={N:3d}  reset={t1-t0:5.2f}s  run={t2-t1:6.2f}s  "
              f"reached={r.get('reached')}  ipcsync_w={cnt}", flush=True)
n.close(); o.close()
