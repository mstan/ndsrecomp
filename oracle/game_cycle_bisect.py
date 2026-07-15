#!/usr/bin/env python3
"""Find the first ARM9 cycle-accounting mismatch before SM64DS IPCSYNC 220."""

from concurrent.futures import ThreadPoolExecutor

from _client import DebugClient
from game_fp_bisect import launch_to, normalized


BASE_NATIVE = 48_494_575
BASE_ORACLE = 48_504_902
MAX_K = 3_587


def main():
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
        cn = hn["counts"]["cyc9"]
        co = ho["counts"]["cyc9"]
        same_arch = sn == so
        print(f"K={k:4d} cycles={cn}/{co} delta={cn-co:+d} "
              f"pc={sn[0][15]:08x}/{so[0][15]:08x} "
              f"arch={'OK' if same_arch else 'DIFF'}", flush=True)
        return cn == co and same_arch

    try:
        if not sample(0):
            raise SystemExit("IPCSYNC 219 base is not cycle/state aligned")
        if sample(MAX_K):
            raise SystemExit("IPCSYNC 220 endpoint unexpectedly agrees")
        lo, hi = 0, MAX_K
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if sample(mid):
                lo = mid
            else:
                hi = mid
        print(f"BOUNDARY {lo} {hi}")
        sample(lo)
        sample(hi)
    finally:
        native.close()
        oracle.close()


if __name__ == "__main__":
    main()
