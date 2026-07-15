#!/usr/bin/env python3
"""Compare native/oracle timelines at fresh SM64DS launch IPCSYNC writes."""

import argparse
import json
from concurrent.futures import ThreadPoolExecutor

from _client import DebugClient


def sample(client, count):
    client.cmd("reset")
    client.cmd("run_to_event", event="vblank9", count=100)
    client.cmd("touch", x=128, y=48, down=True)
    client.cmd("run_to_event", event="vblank9", count=102)
    client.cmd("touch", x=128, y=48, down=False)
    run = client.cmd("run_to_event", event="ipcsync_w", count=count,
                     stall=300_000)
    return {
        "run": run,
        "sched": client.cmd("sched_state"),
        "counts": client.cmd("event_counts"),
        "regs9": client.cmd("regs", cpu=9),
        "regs7": client.cmd("regs", cpu=7),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("counts", nargs="*", type=int,
                        default=[212, 216, 220])
    args = parser.parse_args()
    native = DebugClient(port=19842, timeout=600.0)
    oracle = DebugClient(port=19843, timeout=600.0)
    try:
        for count in args.counts:
            with ThreadPoolExecutor(max_workers=2) as pool:
                fn = pool.submit(sample, native, count)
                fo = pool.submit(sample, oracle, count)
                n = fn.result()
                o = fo.result()
            print(f"ipcsync_w={count}")
            print("  native", json.dumps(n, sort_keys=True))
            print("  oracle", json.dumps(o, sort_keys=True))
    finally:
        native.close()
        oracle.close()


if __name__ == "__main__":
    main()
