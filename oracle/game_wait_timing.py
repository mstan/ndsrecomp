#!/usr/bin/env python3
"""Measure the latest ARM7 BIOS WaitByLoop span from each trace ring."""

from _client import DebugClient


for name, port in (("native", 19842), ("oracle", 19843)):
    c = DebugClient(port=port, timeout=600.0)
    try:
        end = c.cmd("event_counts")["insn7"]
        rows = []
        for count in range(end - 4095, end + 1):
            s = c.cmd("insn_sample", cpu=7, count=count)
            if s.get("found") and s["pc"] in (0x2F08, 0x2F0A):
                rows.append(s)
        starts = [x for x in rows if x["pc"] == 0x2F08 and x["r"][0] == 1000]
        finishes = [x for x in rows if x["pc"] == 0x2F0A and x["r"][0] == 0]
        print(name, "end", end, "wait_rows", len(rows))
        print(name, "starts", [(x["count"], x["sys"], x["cycles"]) for x in starts])
        print(name, "finishes", [(x["count"], x["sys"], x["cycles"]) for x in finishes])
        if starts and finishes:
            a = starts[-1]
            later = [x for x in finishes if x["count"] > a["count"]]
            if later:
                b = later[0]
                print(name, "latest complete delta",
                      "insn", b["count"] - a["count"],
                      "sys", b["sys"] - a["sys"],
                      "cycles", b["cycles"] - a["cycles"])
    finally:
        c.close()
