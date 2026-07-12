#!/usr/bin/env python3
"""recon_state.py — quick native-vs-oracle state reconnaissance.

For each (event, count) anchor, advance BOTH ends to that hardware-event
count, then compare every region both sides expose, printing the first
divergence per region AND the actual counter value each side reached
(exposing run_to_event overshoot, which differs by side granularity).

    python recon_state.py --anchors vblank9:1 vblank9:2 vblank9:5 ipcsync_w:10

This is reconnaissance, not the final bisector — it tolerates the coarse
oracle frame-granularity and just shows the lay of the land.
"""

import argparse
from _client import DebugClient, parse_event, first_divergence

# Regions the NATIVE side can currently produce (bus_get_region). VRAM/pal/oam
# come once VRAM is mapped in the bus.
COMMON = ["mainram", "wram7", "wramshared", "itcm", "dtcm"]


def counts(c):
    return c.event_counts()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--anchors", nargs="+",
                    default=["vblank9:1", "vblank9:2", "vblank9:5",
                             "vblank9:10", "vblank9:20"])
    ap.add_argument("--regions", nargs="*", default=COMMON)
    ap.add_argument("--native-port", type=int, default=19842)
    ap.add_argument("--oracle-port", type=int, default=19843)
    ap.add_argument("--context", type=int, default=24)
    args = ap.parse_args()

    native = DebugClient(port=args.native_port, timeout=600.0)
    oracle = DebugClient(port=args.oracle_port, timeout=600.0)
    print("native ping:", native.ping(), " oracle ping:", oracle.ping())

    for spec in args.anchors:
        event, count = parse_event(spec)
        rn = native.run_to_event(event, count)
        ro = oracle.run_to_event(event, count)
        cn, co = rn.get("counts", {}), ro.get("counts", {})
        print(f"\n=== anchor {event}={count} ===")
        print(f"  native reached: {event}={cn.get(event)}  "
              f"vblank9={cn.get('vblank9')} ipcsync_w={cn.get('ipcsync_w')} "
              f"fifo9to7={cn.get('fifo9to7')} fifo7to9={cn.get('fifo7to9')} "
              f"sb_w={cn.get('soundbias_w')} sb_last={cn.get('soundbias_last')}")
        print(f"  oracle reached: {event}={co.get(event)}  "
              f"vblank9={co.get('vblank9')} ipcsync_w={co.get('ipcsync_w')} "
              f"fifo9to7={co.get('fifo9to7')} fifo7to9={co.get('fifo7to9')} "
              f"sb_w={co.get('soundbias_w')} sb_last={co.get('soundbias_last')}")
        for r in args.regions:
            try:
                a = native.read_region(r)
            except RuntimeError as e:
                print(f"  {r:11s}: native absent ({e})")
                continue
            try:
                b = oracle.read_region(r)
            except RuntimeError as e:
                print(f"  {r:11s}: oracle absent ({e})")
                continue
            div = first_divergence(a, b)
            if div is None:
                print(f"  {r:11s}: IDENTICAL ({len(a)} bytes)")
            else:
                i, av, bv = div
                nz_a = sum(1 for x in a if x)
                nz_b = sum(1 for x in b if x)
                lo, hi = max(0, i - args.context), i + args.context
                print(f"  {r:11s}: DIVERGE @0x{i:06x} "
                      f"native={av:#04x} oracle={bv:#04x} "
                      f"(len {len(a)}/{len(b)}, nonzero {nz_a}/{nz_b})")
                print(f"               native: {a[lo:hi].hex()}")
                print(f"               oracle: {b[lo:hi].hex()}")

    native.close()
    oracle.close()


if __name__ == "__main__":
    raise SystemExit(main())
