#!/usr/bin/env python3
"""find_first_diverge.py — find the FIRST hardware-event checkpoint at
which any tracked region diverges between native and oracle.

    python find_first_diverge.py --event-name vblank9 --max 600

Linear scan by default (each step is one execution span and the earliest
divergence is the only one with a root cause, so we want the first, not
a bisected approximation). Use --bisect for a coarse locate when spans
are expensive, then re-scan the located window linearly.
"""

import argparse
from _client import DebugClient, first_divergence

REGIONS = ["mainram", "wram7", "vramA", "vramB", "vramC", "palA", "palB",
           "oam"]


def diverges_at(native, oracle, event, count, regions):
    native.run_to_event(event, count)
    oracle.run_to_event(event, count)
    for r in regions:
        try:
            if first_divergence(native.read_region(r),
                                oracle.read_region(r)) is not None:
                return r
        except RuntimeError:
            continue  # region not present yet at this checkpoint
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--event-name", default="vblank9")
    ap.add_argument("--start", type=int, default=1)
    ap.add_argument("--max", type=int, default=600)
    ap.add_argument("--step", type=int, default=1)
    ap.add_argument("--regions", nargs="*", default=REGIONS)
    ap.add_argument("--native-port", type=int, default=19842)
    ap.add_argument("--oracle-port", type=int, default=19843)
    args = ap.parse_args()

    native = DebugClient(port=args.native_port)
    oracle = DebugClient(port=args.oracle_port)

    for n in range(args.start, args.max + 1, args.step):
        region = diverges_at(native, oracle, args.event_name, n, args.regions)
        if region:
            print(f"FIRST DIVERGENCE at {args.event_name}={n} in {region!r}")
            print(f"  -> python diff_mem.py --region {region} "
                  f"--event {args.event_name}:{n}")
            return 1
        print(f"  {args.event_name}={n}: identical")
    print(f"no divergence through {args.event_name}={args.max}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
