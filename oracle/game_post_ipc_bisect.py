#!/usr/bin/env python3
"""Bisect ARM9 state after the last matching SM64DS IPCSYNC write."""

import argparse
from concurrent.futures import ThreadPoolExecutor

from _client import DebugClient
from game_fp_bisect import normalized, report


BASE_NATIVE = 53_473_294
BASE_ORACLE = 53_483_621


def launch_to(client, target):
    client.cmd("reset")
    client.cmd("run_to_event", event="vblank9", count=100)
    client.cmd("touch", x=128, y=48, down=True)
    client.cmd("run_to_event", event="vblank9", count=102)
    client.cmd("touch", x=128, y=48, down=False)
    anchor = client.cmd("run_to_event", event="ipcsync_w", count=4541,
                        stall=300_000)
    if not anchor.get("reached"):
        return anchor, client.cmd("regs", cpu=9)
    hit = client.cmd("run_to_event", event="insn9", count=target,
                     stall=300_000)
    return hit, client.cmd("regs", cpu=9)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max", type=int, default=2175)
    args = ap.parse_args()
    native = DebugClient(port=19842, timeout=600.0)
    oracle = DebugClient(port=19843, timeout=600.0)

    def sample(k):
        with ThreadPoolExecutor(max_workers=2) as pool:
            fn = pool.submit(launch_to, native, BASE_NATIVE + k)
            fo = pool.submit(launch_to, oracle, BASE_ORACLE + k)
            hn, rn = fn.result()
            ho, ro = fo.result()
        sn = normalized(rn)
        so = normalized(ro, oracle=True)
        report(k, sn, so)
        if not hn.get("reached") or not ho.get("reached"):
            print("UNREACHED", k, hn, ho, flush=True)
            return False
        return sn == so

    try:
        if not sample(0):
            raise SystemExit("IPCSYNC 4541 anchor does not match")
        if sample(args.max):
            print(f"No divergence through K={args.max}")
            return
        lo, hi = 0, args.max
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if sample(mid):
                lo = mid
            else:
                hi = mid
        print("BOUNDARY", lo, hi)
        sample(lo)
        sample(hi)
    finally:
        native.close()
        oracle.close()


if __name__ == "__main__":
    main()
