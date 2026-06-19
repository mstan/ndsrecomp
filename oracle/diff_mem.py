#!/usr/bin/env python3
"""diff_mem.py — diff a memory region between native and oracle at a
hardware-event checkpoint, printing the FIRST divergence.

    python diff_mem.py --region mainram --event vblank9:30

Both ends are advanced to the same event count (never a raw frame index)
before the region is pulled and compared. See ../TCP.md and ../DEBUG.md.
"""

import argparse
from _client import DebugClient, parse_event, first_divergence


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--region", required=True,
                    help="mainram|wram7|vramA..I|palA|palB|oam|itcm|dtcm")
    ap.add_argument("--event", default="vblank9:30",
                    help="hardware-event checkpoint, e.g. vblank9:30")
    ap.add_argument("--native-port", type=int, default=19842)
    ap.add_argument("--oracle-port", type=int, default=19843)
    ap.add_argument("--context", type=int, default=16)
    args = ap.parse_args()

    event, count = parse_event(args.event)
    native = DebugClient(port=args.native_port)
    oracle = DebugClient(port=args.oracle_port)

    native.run_to_event(event, count)
    oracle.run_to_event(event, count)

    a = native.read_region(args.region)
    b = oracle.read_region(args.region)
    div = first_divergence(a, b)

    if div is None:
        print(f"IDENTICAL  region={args.region}  at {event}={count}  "
              f"({len(a)} bytes)")
        return 0

    i, av, bv = div
    print(f"DIVERGE    region={args.region}  at {event}={count}")
    print(f"  first byte differs at offset 0x{i:06x}: "
          f"native={av:#04x} oracle={bv:#04x}")
    lo = max(0, i - args.context)
    hi = i + args.context
    print(f"  native [0x{lo:06x}..]: {a[lo:hi].hex()}")
    print(f"  oracle [0x{lo:06x}..]: {b[lo:hi].hex()}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
