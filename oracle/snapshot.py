#!/usr/bin/env python3
"""snapshot.py — anchor BOTH ends on a hardware-event count and dump a
side-by-side state snapshot: event counts, both CPUs' regs, the key boot
I/O registers, and per-region first-divergence.

    python snapshot.py --event ipcsync_w:5

Anchors on a guest-driven write event (ipcsync_w / fifo9to7 / fifo7to9),
never vblank (vblank9 is frames-elapsed natively but IRQs-delivered on the
oracle — see DEBUG.md). Reconnaissance/diff primitive for the bisector.
"""

import argparse
from _client import DebugClient, parse_event, first_divergence

COMMON = ["mainram", "wram7", "wramshared", "itcm", "dtcm"]

IO_REGS = [  # (label, cpu, addr, width)
    ("IPCSYNC9",  9, 0x04000180, 16),
    ("IPCSYNC7",  7, 0x04000180, 16),
    ("IPCFIFOCNT9", 9, 0x04000184, 16),
    ("IPCFIFOCNT7", 7, 0x04000184, 16),
    ("IME9", 9, 0x04000208, 32),
    ("IME7", 7, 0x04000208, 32),
    ("IE9",  9, 0x04000210, 32),
    ("IE7",  7, 0x04000210, 32),
    ("IF9",  9, 0x04000214, 32),
    ("IF7",  7, 0x04000214, 32),
    ("POSTFLG9", 9, 0x04000300, 8),
    ("POSTFLG7", 7, 0x04000300, 8),
]


def regs(c, cpu):
    r = c.cmd("regs", cpu=cpu)
    return r["r"], r["cpsr"], r["mode"]


def fmt_regs(label, rr):
    r, cpsr, mode = rr
    return (f"  {label}: pc={r[15]:08x} lr={r[14]:08x} sp={r[13]:08x} "
            f"cpsr={cpsr:08x} mode={mode:02x}  "
            f"r0={r[0]:08x} r1={r[1]:08x} r2={r[2]:08x} r12={r[12]:08x}")


def read_io(c, cpu, addr, width):
    try:
        return c.cmd("read_io", cpu=cpu, addr=addr, width=width)["value"]
    except RuntimeError:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--event", default="ipcsync_w:5")
    ap.add_argument("--regions", nargs="*", default=COMMON)
    ap.add_argument("--native-port", type=int, default=19842)
    ap.add_argument("--oracle-port", type=int, default=19843)
    ap.add_argument("--context", type=int, default=32)
    args = ap.parse_args()

    event, count = parse_event(args.event)
    n = DebugClient(port=args.native_port, timeout=600.0)
    o = DebugClient(port=args.oracle_port, timeout=600.0)

    # Reset first: run_to_event is a no-op if the server is already PAST the
    # target, so without a reset this would silently report whatever stale
    # state the server was left in by a prior command (e.g. a stalled ceiling).
    n.cmd("reset"); o.cmd("reset")
    rn = n.run_to_event(event, count)
    ro = o.run_to_event(event, count)
    cn, co = rn.get("counts", {}), ro.get("counts", {})

    print(f"=== anchor {event}={count} ===")
    keys = ["vblank9", "ipcsync_w", "fifo9to7", "fifo7to9", "dma_done",
            "timer_ovf", "soundbias_w", "soundbias_last"]
    print("  native counts:", {k: cn.get(k) for k in keys})
    print("  oracle counts:", {k: co.get(k) for k in keys})

    print("\n-- ARM9 --")
    print(fmt_regs("native9", regs(n, 9)))
    print(fmt_regs("oracle9", regs(o, 9)))
    print("-- ARM7 --")
    print(fmt_regs("native7", regs(n, 7)))
    print(fmt_regs("oracle7", regs(o, 7)))

    print("\n-- I/O regs (native / oracle) --")
    for label, cpu, addr, width in IO_REGS:
        vn = read_io(n, cpu, addr, width)
        vo = read_io(o, cpu, addr, width)
        flag = "" if vn == vo else "  <<< DIFF"
        sn = "----" if vn is None else f"{vn:#x}"
        so = "----" if vo is None else f"{vo:#x}"
        print(f"  {label:12s} {sn:>10s} / {so:>10s}{flag}")

    print("\n-- regions --")
    for r in args.regions:
        try:
            a = n.read_region(r)
        except RuntimeError as e:
            print(f"  {r:11s}: native absent ({e})"); continue
        try:
            b = o.read_region(r)
        except RuntimeError as e:
            print(f"  {r:11s}: oracle absent ({e})"); continue
        div = first_divergence(a, b)
        nz_a = sum(1 for x in a if x); nz_b = sum(1 for x in b if x)
        if div is None:
            print(f"  {r:11s}: IDENTICAL ({len(a)} bytes, nonzero {nz_a})")
        else:
            i, av, bv = div
            lo, hi = max(0, i - args.context), i + args.context
            print(f"  {r:11s}: DIVERGE @0x{i:06x} native={av:#04x} "
                  f"oracle={bv:#04x} (nonzero {nz_a}/{nz_b})")
            print(f"               native: {a[lo:hi].hex()}")
            print(f"               oracle: {b[lo:hi].hex()}")
    n.close(); o.close()


if __name__ == "__main__":
    raise SystemExit(main())
