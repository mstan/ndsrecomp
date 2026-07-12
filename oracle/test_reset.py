#!/usr/bin/env python3
"""Smoke-test the `reset` command on both servers and the fresh-per-N compare."""
from _client import DebugClient, first_divergence

n = DebugClient(port=19842, timeout=600.0)
o = DebugClient(port=19843, timeout=600.0)

for name, c in [("native", n), ("oracle", o)]:
    c.run_to_event("ipcsync_w", 10)
    before = c.event_counts()["ipcsync_w"]
    c.cmd("reset")
    after = c.event_counts()["ipcsync_w"]
    print(f"{name}: ipcsync_w {before} -> after reset {after}  "
          f"({'OK' if after == 0 else 'FAIL'})")

# fresh-from-reset compare at ipcsync_w=5 should match the earlier fresh result
n.run_to_event("ipcsync_w", 5)
o.run_to_event("ipcsync_w", 5)
for r in ["mainram", "wramshared", "itcm", "dtcm"]:
    d = first_divergence(n.read_region(r), o.read_region(r))
    print(f"  {r:11s}: {'IDENTICAL' if d is None else 'DIVERGE @0x%06x' % d[0]}")
n.close(); o.close()
