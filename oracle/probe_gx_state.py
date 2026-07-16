"""Native-vs-ndsref 3D geometry-engine state comparison at matched ordinals.

Advances both machines to the same hardware-event ordinals (default: insn9
milestones) and compares the guest-visible geometry-engine registers at each
stop: DISP3DCNT, GXSTAT, polygon/vertex RAM counts, position/vector test
results, and the clip/vector matrix readback ports. Both sides answer through
their real IO dispatch, so the values are exactly what the guest would read.

Servers must already be running (fresh processes; see stale-oracle rule):
  nds_runner --serve [--rom ...]     on --native-port (default 19842)
  ndsref --boot firmware [--rom ...] on --oracle-port (default 19843)

Usage:
  py -3 probe_gx_state.py --start 50000000 --step 50000000 --count 10
  py -3 probe_gx_state.py --event vblank9 --stops 200,400,600
  py -3 probe_gx_state.py --nav sm64ds-title --start 100000000 --count 4

--nav sm64ds-title resets both machines and performs the SM64DS boot
navigation (firmware-menu game-panel touch at vblank 100, title touch at
900) before probing; without it the machines are probed as-is.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor

from _client import DebugClient

# (name, addr, width_bits). Matrix ports are deterministic lazily-computed
# state on both implementations; reading them is side-effect-free at the
# guest-visible level.
REGS = (
    [("DISP3DCNT", 0x04000060, 16),
     ("GXSTAT", 0x04000600, 32),
     ("RAM_COUNT", 0x04000604, 32)]
    + [(f"POS_RESULT[{i}]", 0x04000620 + 4 * i, 32) for i in range(4)]
    + [(f"VEC_RESULT[{i}]", 0x04000630 + 2 * i, 16) for i in range(3)]
    + [(f"CLIPMTX[{i}]", 0x04000640 + 4 * i, 32) for i in range(16)]
    + [(f"VECMTX[{i}]", 0x04000680 + 4 * i, 32) for i in range(9)]
)

COUNTS = ("insn9", "insn7", "vblank9", "dma_done", "irq9", "irq7")


def read_regs(client: DebugClient) -> dict[str, int]:
    out = {}
    for name, addr, width in REGS:
        out[name] = client.cmd("read_io", cpu=9, addr=addr, width=width)["value"]
    return out


def nav_sm64ds_title(both) -> None:
    """Reset and drive both machines through the SM64DS boot touches
    (mirrors oracle/game_boot_smoke.py)."""
    both("reset")
    both("run_to_event", event="vblank9", count=100, stall=300_000)
    both("touch", x=128, y=48, down=True)
    both("run_to_event", event="vblank9", count=102, stall=300_000)
    both("touch", x=128, y=48, down=False)
    both("run_to_event", event="vblank9", count=900, stall=300_000)
    both("touch", x=128, y=120, down=True)
    both("run_to_event", event="vblank9", count=930, stall=300_000)
    both("touch", x=128, y=120, down=False)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native-port", type=int, default=19842)
    parser.add_argument("--oracle-port", type=int, default=19843)
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--event", default="insn9")
    parser.add_argument("--stops", default="",
                        help="comma-separated ordinals (overrides start/step/count)")
    parser.add_argument("--start", type=int, default=50_000_000)
    parser.add_argument("--step", type=int, default=50_000_000)
    parser.add_argument("--count", type=int, default=10)
    parser.add_argument("--keep-going", action="store_true",
                        help="report every diverging stop instead of the first")
    parser.add_argument("--nav", choices=["sm64ds-title"], default=None,
                        help="reset + boot navigation to run before probing")
    args = parser.parse_args()

    if args.stops:
        stops = [int(s) for s in args.stops.split(",") if s]
    else:
        stops = [args.start + args.step * i for i in range(args.count)]

    native = DebugClient(port=args.native_port, timeout=args.timeout)
    oracle = DebugClient(port=args.oracle_port, timeout=args.timeout)
    pool = ThreadPoolExecutor(max_workers=2)

    def both(name, **cmd_args):
        futs = [pool.submit(c.cmd, name, **cmd_args)
                for c in (native, oracle)]
        return [f.result() for f in futs]

    if args.nav == "sm64ds-title":
        nav_sm64ds_title(both)

    status = 0
    for stop in stops:
        futs = [pool.submit(c.run_to_event, args.event, stop)
                for c in (native, oracle)]
        for f in futs:
            f.result()

        nat_regs, orc_regs = pool.submit(read_regs, native), \
            pool.submit(read_regs, oracle)
        nat_regs, orc_regs = nat_regs.result(), orc_regs.result()
        nat_counts = native.event_counts()
        orc_counts = oracle.event_counts()

        reg_diffs = [
            (name, nat_regs[name], orc_regs[name])
            for name, _, _ in REGS if nat_regs[name] != orc_regs[name]
        ]
        count_diffs = [
            (k, nat_counts.get(k), orc_counts.get(k))
            for k in COUNTS if nat_counts.get(k) != orc_counts.get(k)
        ]

        if not reg_diffs and not count_diffs:
            print(f"[{args.event}={stop}] ok  GXSTAT=0x{nat_regs['GXSTAT']:08X} "
                  f"RAM_COUNT=0x{nat_regs['RAM_COUNT']:08X}")
            continue

        status = 1
        print(f"[{args.event}={stop}] DIVERGED")
        for name, n, o in reg_diffs:
            print(f"  reg {name}: native=0x{n:08X} oracle=0x{o:08X}")
        for name, n, o in count_diffs:
            print(f"  count {name}: native={n} oracle={o}")
        if not args.keep_going:
            break

    native.close()
    oracle.close()
    pool.shutdown()
    print("status:", "pass" if status == 0 else "FAIL")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
