#!/usr/bin/env python3
"""arm7_drift_bisect.py -- first-divergence locator for ARM7 counter drift.

Both ends record their per-CPU retired-instruction ring, IRQ ring and SPI ring
continuously from power-on (io.cpp kInsnTraceSize=262144 entries per CPU on the
native side, the Oracle_OnInsnRetire hook on the melonDS side). This probe never
arms anything: it free-runs both machines forward to a shared hardware-event
ordinal and then QUERIES those always-on rings backwards for the first entry
whose (pc, cycles) disagree.

Stage 1  coarse: advance both to vblank9 = start, start+step, ... comparing
         event_counts at each stop. The first stop whose ARM7 counters differ
         brackets the divergence.
Stage 2  ring: at that bracket, binary-search the ARM7 instruction ring over
         ordinals (last_agreeing_insn7, first_disagreeing_insn7] for the first
         ordinal where the two rings disagree, and print both entries plus the
         IRQ-ring entries that surround it.

    py -3 oracle/arm7_drift_bisect.py --native-port 20000 --oracle-port 20001
"""

import argparse

from _client import DebugClient

ARM7_KEYS = ["insn7", "cyc7", "irq7", "timer_ovf", "spi_w"]
ALL_KEYS = ["insn9", "cyc9", "irq9", "vblank9", "vblank7", "ipcsync_w",
            "fifo9to7", "fifo7to9", "dma_done"] + ARM7_KEYS


def diff_counts(a, b, keys):
    return {k: (a[k], b[k]) for k in keys if k in a and k in b and a[k] != b[k]}


def insn(client, cpu, ordinal):
    r = client.cmd("insn_sample", cpu=cpu, count=ordinal)
    return r if r.get("found") else None


def irq(client, cpu, ordinal):
    r = client.cmd("irq_sample", cpu=cpu, count=ordinal)
    return r if r.get("found") else None


def entry_key(e):
    """The architectural + timing identity of one retired instruction."""
    return (e["pc"], e["cpsr"], e["cycles"], tuple(e["r"]))


def arch_key(e):
    """Architectural identity only (ignores the cycle timestamp)."""
    return (e["pc"], e["cpsr"], tuple(e["r"]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--native-port", type=int, default=20000)
    ap.add_argument("--oracle-port", type=int, default=20001)
    ap.add_argument("--event", default="vblank9")
    ap.add_argument("--start", type=int, default=1)
    ap.add_argument("--step", type=int, default=1)
    ap.add_argument("--max", type=int, default=400)
    ap.add_argument("--timeout", type=float, default=600.0)
    ap.add_argument("--no-reset", action="store_true")
    args = ap.parse_args()

    n = DebugClient(port=args.native_port, timeout=args.timeout)
    o = DebugClient(port=args.oracle_port, timeout=args.timeout)
    if not args.no_reset:
        n.cmd("reset")
        o.cmd("reset")

    prev = None
    hit = None
    ordinal = args.start
    while ordinal <= args.max:
        rn = n.cmd("run_to_event", event=args.event, count=ordinal)
        ro = o.cmd("run_to_event", event=args.event, count=ordinal)
        if not rn.get("reached") or not ro.get("reached"):
            print(f"{args.event}={ordinal}: UNREACHED native={rn} oracle={ro}")
            return 1
        cn = n.cmd("event_counts")
        co = o.cmd("event_counts")
        d7 = diff_counts(cn, co, ARM7_KEYS)
        dall = diff_counts(cn, co, ALL_KEYS)
        tag = "  <<< ARM7 DRIFT" if d7 else ("  <<< other" if dall else "")
        print(f"{args.event}={ordinal:5d}: insn7 {cn['insn7']:>12} / "
              f"{co['insn7']:>12}  cyc7 {cn['cyc7']:>12} / {co['cyc7']:>12} "
              f"irq7 {cn['irq7']:>6} / {co['irq7']:>6} "
              f"tov {cn['timer_ovf']:>6} / {co['timer_ovf']:>6}{tag}",
              flush=True)
        if dall:
            hit = (ordinal, cn, co, dall)
            break
        prev = (ordinal, cn, co)
        ordinal += args.step

    if hit is None:
        print(f"no counter divergence through {args.event}={args.max}")
        return 0

    ordinal, cn, co, dall = hit
    print()
    print(f"first divergent {args.event} ordinal: {ordinal}")
    for k, (a, b) in sorted(dall.items()):
        print(f"  {k}: native={a} oracle={b} (delta {a - b:+d})")
    base = prev[1]["insn7"] if prev else 0
    print(f"  last agreeing insn7 ordinal: {base}")

    # Stage 2: walk the always-on ARM7 instruction ring for the first ordinal
    # whose entries disagree. Binary search over [lo, hi] where lo is known
    # equal and hi is known different (or the end of the shared range).
    hi = min(cn["insn7"], co["insn7"])
    lo = base
    if lo >= hi:
        print("  no shared ARM7 ring window to bisect")
        return 0

    def same(k):
        a = insn(n, 7, k)
        b = insn(o, 7, k)
        if a is None or b is None:
            return None, a, b
        return arch_key(a) == arch_key(b), a, b

    ok, a, b = same(hi)
    if ok is None:
        print(f"  ring entry {hi} not retained on one side "
              f"(native={a} oracle={b}); lower --step")
        return 0
    if ok:
        print(f"  ARM7 ring agrees architecturally through ordinal {hi}; "
              f"the divergence is timing-only in this window")
        # fall through to the timing bisect below
        def same(k):  # noqa: F811 -- timing-sensitive comparison
            a = insn(n, 7, k)
            b = insn(o, 7, k)
            if a is None or b is None:
                return None, a, b
            return entry_key(a) == entry_key(b), a, b
        ok, a, b = same(hi)
        if ok:
            print(f"  ARM7 ring fully agrees through ordinal {hi} too")
            return 0

    while lo + 1 < hi:
        mid = (lo + hi) // 2
        ok, a, b = same(mid)
        if ok is None:
            print(f"  ring entry {mid} evicted; narrowing from {lo}")
            lo = mid
            continue
        if ok:
            lo = mid
        else:
            hi = mid

    print()
    print(f"FIRST divergent ARM7 retired-instruction ordinal: {hi}")
    for label, client in (("native", n), ("oracle", o)):
        e = insn(client, 7, hi)
        print(f"  {label}: pc=0x{e['pc']:08x} cpsr=0x{e['cpsr']:08x} "
              f"cycles={e['cycles']} sys={e['sys']} pending=0x{e['pending']:x}")
    print("  previous (agreeing) entry:")
    for label, client in (("native", n), ("oracle", o)):
        e = insn(client, 7, hi - 1)
        print(f"  {label}: pc=0x{e['pc']:08x} cpsr=0x{e['cpsr']:08x} "
              f"cycles={e['cycles']} sys={e['sys']} pending=0x{e['pending']:x}")

    print("\n  ARM7 IRQ ring around the split:")
    for label, client, counts in (("native", n, cn), ("oracle", o, co)):
        total = counts["irq7"]
        for k in range(max(1, total - 3), total + 1):
            e = irq(client, 7, k)
            if e:
                print(f"  {label} irq7#{k}: sys={e['sys']} cyc7={e['cyc7']} "
                      f"insn={e['insn']} ret=0x{e['return_address']:08x} "
                      f"pending=0x{e['pending']:x}")
    n.close()
    o.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
