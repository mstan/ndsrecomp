#!/usr/bin/env python3
"""Bisect ARM9 state from the matching SM64DS handoff boundary."""

import argparse

from _client import DebugClient


BASE_NATIVE = 48_498_170
BASE_ORACLE = 48_508_497


def launch_to(c, target):
    c.cmd("reset")
    c.cmd("run_to_event", event="vblank9", count=100)
    c.cmd("touch", x=128, y=48, down=True)
    c.cmd("run_to_event", event="vblank9", count=102)
    c.cmd("touch", x=128, y=48, down=False)
    hit = c.cmd("run_to_event", event="insn9", count=target,
                stall=300_000)
    rg = c.cmd("regs", cpu=9)
    return hit, rg


def normalized(rg, oracle=False):
    r = list(rg["r"])
    if oracle:
        r[15] = (r[15] - (2 if rg["cpsr"] & 0x20 else 4)) & 0xFFFFFFFF
    return tuple(r), rg["cpsr"]


def report(k, sn, so):
    diffs = []
    for i, (a, b) in enumerate(zip(sn[0], so[0])):
        if a != b:
            diffs.append(f"r{i}={a:08x}/{b:08x}")
    if sn[1] != so[1]:
        diffs.append(f"cpsr={sn[1]:08x}/{so[1]:08x}")
    print(f"K={k:6d} pc={sn[0][15]:08x}/{so[0][15]:08x} "
          + ("OK" if not diffs else "DIFF " + " ".join(diffs)), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max", type=int, default=131072)
    ap.add_argument("--known-same", type=int, default=0)
    args = ap.parse_args()
    n = DebugClient(port=19842, timeout=600.0)
    o = DebugClient(port=19843, timeout=600.0)

    def sample(k):
        hn, rn = launch_to(n, BASE_NATIVE + k)
        ho, ro = launch_to(o, BASE_ORACLE + k)
        sn, so = normalized(rn), normalized(ro, oracle=True)
        if not hn.get("reached") or not ho.get("reached"):
            print("UNREACHED", k, hn, ho, flush=True)
            return False, sn, so
        report(k, sn, so)
        return sn == so, sn, so

    try:
        lo, hi = args.known_same, None
        if not args.known_same:
            ok, _, _ = sample(0)
            if not ok:
                raise SystemExit("handoff boundary does not match")
        if args.known_same:
            ok, _, _ = sample(args.max)
            if ok:
                lo = args.max
            else:
                hi = args.max
        else:
            k = 1
            while k <= args.max:
                ok, _, _ = sample(k)
                if not ok:
                    hi = k
                    break
                lo = k
                k *= 2
        if hi is None:
            print(f"No divergence through K={lo}")
            return
        while hi - lo > 1:
            mid = (lo + hi) // 2
            ok, _, _ = sample(mid)
            if ok:
                lo = mid
            else:
                hi = mid
        print("BOUNDARY", lo, hi)
        sample(lo)
        sample(hi)
    finally:
        n.close()
        o.close()


if __name__ == "__main__":
    main()
