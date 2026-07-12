#!/usr/bin/env python3
"""find_first_diverge.py — fresh-per-N native-vs-oracle bisector (reset-based).

For each candidate count N of a guest-driven hardware-event counter (default
ipcsync_w) this RESETS both ends to power-on, runs BOTH fresh to exactly the
Nth event, then compares state. Resetting per N removes the cross-CPU
interleaving perturbation that repeated *incremental* event-breaks introduced
once the cores couple (~ipcsync 36); those produced false positives (e.g.
dtcm@0x2f60 at N=36, mainram@0x330988 at N=48) that vanish fresh-from-reset.

Strategy (fast + robust, no blind monotonicity assumption):
  1. Coarse scan at --step to find the first SAMPLE that is semantic. A genuine
     path split is monotonic — once two deterministic states diverge they stay
     diverged — so the first semantic sample reliably brackets it.
  2. Linearly refine inside (prev_clean, first_semantic_sample] to the first N
     that is semantic.
  3. Persistence-confirm: a real divergence stays diverged at N, N+1 ... N+K.
     A semantic-looking blip that CLEARS at N+1 is a benign write-ordering
     artifact of the shared counter (see below), not a root cause — report it
     and keep scanning for the first PERSISTENT one.

    python find_first_diverge.py --event ipcsync_w --start 55 --max 211
    python find_first_diverge.py --event ipcsync_w --linear        # full scan

Benign divergence classes (reported, never treated as first-divergence):
  - Lagging-core stack scratch. The Nth ipcsync write is a specific guest store,
    so both ends break at the same architectural point of the *writing* core;
    but the OTHER core sits wherever the (different native vs oracle) timing left
    it, so its live/popped stack frame differs. Appears as a dtcm diff at/above
    SP9 (ARM9 stack) or a wram7 diff at/above SP7 (ARM7 stack). Classified by SP.
  - Write-ordering transients. If the lagging core was mid-update of a shared
    region (mainram/wramshared) at the break, that region differs at N and
    agrees at N+1. Caught by the persistence filter, not the SP classifier.
  - IF7 bit22 (lid/hinge): melonDS SetLidClosed(false) pulses it at boot; real
    HW does not at power-on, so native correctly omits it. Masked.
  - IPCSYNC value is not compared: at a shared-count anchor it differs by which
    core wrote the Nth (the count anchor already pins ordering).

NEVER anchor on vblank9: frames-elapsed natively vs IRQs-delivered on the oracle.
"""

import argparse
from _client import DebugClient

# Deterministic regions other than the two that hold a CPU stack: ANY diff here
# (after the persistence filter) is a real divergence.
ALL_REGIONS = ["mainram", "wram7", "wramshared", "itcm", "dtcm"]
# region -> (base addr, owning CPU). A diff at/above that CPU's SP is stack
# scratch of the lagging core, hence benign.
STACK_REGION = {"dtcm": (0x03000000, 9), "wram7": (0x03800000, 7)}
STACK_GUARD = 0x40  # tolerate a few bytes below SP (in-flight push)

# (label, cpu, addr, width, ignore_mask). IPCFIFOCNT (0x184) carries FIFO
# enable/error/empty/full bits; the low fill-level bits can wobble by ±1 at a
# shared anchor (caught by the persistence filter), but a structural mismatch
# (FIFO never enabled / error flag) is a real signal — native does ~0 FIFO
# traffic where the oracle does thousands.
IO_REGS = [
    ("IF9", 9, 0x04000214, 32, 0),
    ("IF7", 7, 0x04000214, 32, (1 << 22)),
    ("IE9", 9, 0x04000210, 32, 0), ("IE7", 7, 0x04000210, 32, 0),
    ("IME9", 9, 0x04000208, 32, 0), ("IME7", 7, 0x04000208, 32, 0),
    ("POSTFLG9", 9, 0x04000300, 8, 0), ("POSTFLG7", 7, 0x04000300, 8, 0),
    ("IPCFIFOCNT9", 9, 0x04000184, 16, 0), ("IPCFIFOCNT7", 7, 0x04000184, 16, 0),
]


def in_vector_page(pc):
    """True if PC sits in an ARM exception-vector page — where a core lands and
    wedges after a fault/SWI whose handler can't be dispatched. Low vectors
    0x00..0x20 (ARM7, and ARM9 if VE off) or high vectors 0xFFFF0000.. (ARM9).
    Normal ARM9 BIOS execution at 0xFFFF03xx is OUTSIDE the 0x20-byte page."""
    return pc < 0x20 or 0xFFFF0000 <= pc < 0xFFFF0020


def first_diff_offset(a, b):
    """Lowest index where a,b differ, or None. Fast path: C-speed `a==b`."""
    if a == b:
        return None
    m = min(len(a), len(b))
    for i in range(m):
        if a[i] != b[i]:
            return i
    return m  # one is a prefix of the other


def io(c, cpu, addr, w):
    try:
        return c.cmd("read_io", cpu=cpu, addr=addr, width=w)["value"]
    except RuntimeError:
        return None


def regs(c, cpu):
    r = c.cmd("regs", cpu=cpu)
    return r["r"][15], r["r"][13]  # pc, sp


def compare_fresh(n, o, event, N, stall_n=60000, stall_o=800):
    """Reset both, run fresh to the Nth `event`, classify divergences.

    stall_n/stall_o cap the no-progress early-out (native rounds / oracle
    frames) so a persistence-lookahead probe past native's ceiling bails in
    seconds instead of grinding to the absolute cap.

    Returns a dict: reached_n/reached_o, semantic[(region,off,addr)],
    benign[(region,off,addr,cpu)], io[(label,vn,vo)], plus pc/sp per CPU.
    """
    n.cmd("reset"); o.cmd("reset")
    rn = n.cmd("run_to_event", event=event, count=N, stall=stall_n)
    ro = o.cmd("run_to_event", event=event, count=N, stall=stall_o)
    res = {"N": N, "reached_n": rn.get("reached", True),
           "reached_o": ro.get("reached", True),
           "semantic": [], "benign": [], "io": [], "pc_div": []}
    if not (res["reached_n"] and res["reached_o"]):
        res["counts_n"] = rn.get("counts"); res["counts_o"] = ro.get("counts")
        return res

    pc9n, sp9n = regs(n, 9); pc9o, sp9o = regs(o, 9)
    pc7n, sp7n = regs(n, 7); pc7o, sp7o = regs(o, 7)
    res["pc"] = {9: (pc9n, pc9o), 7: (pc7n, pc7o)}
    sp = {9: (sp9n, sp9o), 7: (sp7n, sp7o)}
    res["sp"] = sp

    # Control-flow wedge: native core parked in an exception-vector page while
    # the oracle's same core runs on. This catches a stuck SWI/abort BEFORE it
    # writes divergent RAM (a region compare alone misses it by many anchors).
    for cpu, (pn, po) in res["pc"].items():
        if in_vector_page(pn) and not in_vector_page(po):
            res["pc_div"].append((cpu, pn, po))

    for r in ALL_REGIONS:
        try:
            a, b = n.read_region(r), o.read_region(r)
        except RuntimeError:
            continue
        off = first_diff_offset(a, b)
        if off is None:
            continue
        if r in STACK_REGION:
            base, cpu = STACK_REGION[r]
            addr = base + off
            spn, spo = sp[cpu]
            # Stack grows down: scratch is the suffix [SP-guard, top]. first-diff
            # is the LOWEST diff addr, so if it is in that suffix, ALL diffs are.
            if addr >= min(spn, spo) - STACK_GUARD:
                res["benign"].append((r, off, addr, cpu))
                continue
            res["semantic"].append((r, off, addr))
        else:
            res["semantic"].append((r, off, None))

    for label, cpu, addr, w, ign in IO_REGS:
        vn, vo = io(n, cpu, addr, w), io(o, cpu, addr, w)
        if vn is None or vo is None:
            continue
        if (vn & ~ign) != (vo & ~ign):
            res["io"].append((label, vn, vo))
    return res


def is_semantic(res):
    return (bool(res.get("semantic")) or bool(res.get("io"))
            or bool(res.get("pc_div")))


def fmt(res):
    if not (res["reached_n"] and res["reached_o"]):
        cn = (res.get("counts_n") or {}).get("ipcsync_w")
        co = (res.get("counts_o") or {}).get("ipcsync_w")
        who = "native" if not res["reached_n"] else "oracle"
        return f"UNREACHED ({who} stalled; native={cn} oracle={co})"
    sem = ",".join(f"{r}@{off:#x}" + (f"[{a:#x}]" if a else "")
                   for r, off, a in res["semantic"])
    ben = ",".join(f"{r}@{off:#x}~SP{cpu}" for r, off, a, cpu in res["benign"])
    iod = ",".join(f"{l}:{vn:#x}/{vo:#x}" for l, vn, vo in res["io"])
    pcd = ",".join(f"ARM{cpu}@vec={pn:#x}(ora {po:#x})"
                   for cpu, pn, po in res["pc_div"])
    parts = []
    if pcd:
        parts.append("WEDGE[" + pcd + "]")
    parts.append("SEMANTIC[" + sem + "]" if sem else "regions=OK")
    if iod:
        parts.append("IO[" + iod + "]")
    if ben:
        parts.append("benign(" + ben + ")")
    return " ".join(parts)


def persistence(n, o, event, N, k, log):
    """Is the divergence at N persistent over N+1..N+k? Returns (bool, [res])."""
    confirms = []
    persistent = True
    for j in range(1, k + 1):
        r = compare_fresh(n, o, event, N + j)
        confirms.append(r)
        mark = "SEM" if is_semantic(r) else "clear"
        log(f"      persistence {event}={N+j:3d}: {mark:5s} {fmt(r)}")
        if not is_semantic(r):
            persistent = False
    return persistent, confirms


def report(res, event):
    print("\n" + "=" * 64)
    if is_semantic(res):
        print(f"FIRST PERSISTENT divergence at {event}={res['N']}")
    else:
        print(f"divergence at {event}={res['N']}")
    if res.get("pc"):
        (p9n, p9o), (p7n, p7o) = res["pc"][9], res["pc"][7]
        (s9n, s9o), (s7n, s7o) = res["sp"][9], res["sp"][7]
        print(f"  ARM9 pc native={p9n:08x} oracle={p9o:08x}  "
              f"sp native={s9n:08x} oracle={s9o:08x}")
        print(f"  ARM7 pc native={p7n:08x} oracle={p7o:08x}  "
              f"sp native={s7n:08x} oracle={s7o:08x}")
    for cpu, pn, po in res["pc_div"]:
        print(f"  ARM{cpu} WEDGED in exception-vector page: "
              f"native pc=0x{pn:08x} oracle pc=0x{po:08x}")
    for r, off, a in res["semantic"]:
        print(f"  region {r} first-diff @0x{off:x}"
              + (f" (addr 0x{a:08x})" if a else ""))
    for l, vn, vo in res["io"]:
        print(f"  IO {l}: native={vn:#x} oracle={vo:#x}")
    print(f"  -> python snapshot.py --event {event}:{res['N']}")
    print("=" * 64)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--event", default="ipcsync_w")
    ap.add_argument("--start", type=int, default=1)
    ap.add_argument("--max", type=int, default=211)
    ap.add_argument("--step", type=int, default=8,
                    help="coarse-scan stride (1 = full linear scan)")
    ap.add_argument("--persist", type=int, default=4,
                    help="lookahead count for the persistence filter")
    ap.add_argument("--linear", action="store_true",
                    help="full linear scan (step=1), report every N")
    ap.add_argument("--native-port", type=int, default=19842)
    ap.add_argument("--oracle-port", type=int, default=19843)
    args = ap.parse_args()

    step = 1 if args.linear else max(1, args.step)
    n = DebugClient(port=args.native_port, timeout=600.0)
    o = DebugClient(port=args.oracle_port, timeout=600.0)

    def scan(N):
        r = compare_fresh(n, o, args.event, N)
        tag = "  <<<" if is_semantic(r) else ""
        print(f"  {args.event}={N:3d}: {fmt(r)}{tag}", flush=True)
        return r

    transients = []

    def find_first_persistent(lo, hi):
        """Linear scan [lo,hi]; return first N whose divergence persists."""
        N = lo
        while N <= hi:
            r = compare_fresh(n, o, args.event, N)
            if not (r["reached_n"] and r["reached_o"]):
                print(f"  {args.event}={N:3d}: {fmt(r)}", flush=True)
                return None, r
            if is_semantic(r):
                print(f"  {args.event}={N:3d}: {fmt(r)}  <<< candidate",
                      flush=True)
                persistent, _ = persistence(n, o, args.event, N, args.persist,
                                            print)
                if persistent:
                    return N, r
                print(f"      -> {args.event}={N} is a TRANSIENT "
                      f"(clears within {args.persist}); continuing", flush=True)
                transients.append(N)
            else:
                print(f"  {args.event}={N:3d}: {fmt(r)}", flush=True)
            N += 1
        return None, None

    print(f"# fresh-per-N bisection: {args.event} in [{args.start},{args.max}] "
          f"step={step} persist={args.persist}")

    first_n, first_res = None, None
    if step == 1:
        first_n, first_res = find_first_persistent(args.start, args.max)
    else:
        prev_clean = args.start - 1
        N = args.start
        sample_hit = None
        while N <= args.max:
            r = scan(N)
            if not (r["reached_n"] and r["reached_o"]):
                report(r, args.event)
                n.close(); o.close()
                return 0
            if is_semantic(r):
                sample_hit = N
                break
            prev_clean = N
            N = min(N + step, args.max) if N < args.max else args.max + 1
        if sample_hit is None:
            print(f"\nno semantic divergence through {args.event}={args.max} "
                  f"(step {step})")
            n.close(); o.close()
            return 0
        print(f"# coarse hit at {sample_hit}; refining ({prev_clean+1}.."
              f"{sample_hit})", flush=True)
        first_n, first_res = find_first_persistent(prev_clean + 1, sample_hit)

    if first_n is not None:
        report(first_res, args.event)
    else:
        print(f"\nno PERSISTENT divergence found "
              f"(transients seen: {transients or 'none'})")
    n.close(); o.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
