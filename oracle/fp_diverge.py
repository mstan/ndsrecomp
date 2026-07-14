#!/usr/bin/env python3
"""fp_diverge.py — retired-instruction fp-stream microscope.

Anchors BOTH ends on a per-CPU RETIRED-INSTRUCTION count (insn7 / insn9) and
binary-searches for the FIRST instruction index K at which the recompiler's CPU
state diverges from the melonDS oracle. Unlike the ipcsync_w anchor (which pins
the ARM9 and leaves the ARM7 wherever timing left it), an insn7 anchor puts BOTH
ARM7s at EXACTLY their Kth retired instruction, so the compare is apples-to-
apples: from reset the two must be byte-identical in every register until a real
divergence (an I/O read that returns a different value, or a codegen bug).

    python fp_diverge.py            # ARM7 (insn7)
    python fp_diverge.py --cpu 9    # ARM9 (insn9)

Reports the boundary: the last-identical K-1 (the branch/load site) and the
first-divergent K, with the full register diff, so the culprit instruction and
the register it wrote differently are pinned.

Requires both servers to expose the insn7/insn9 events (run_to_event) — native
runner :19842 and melonDS oracle :19843.
"""
import argparse
from _client import DebugClient

# Registers that legitimately track between the two harnesses. All of r0..r15 +
# cpsr must match at an exact instruction index unless the guest genuinely
# diverged; nothing here is whitelisted up front (classify after, per DEBUG.md).
def state(c, cpu, K):
    """reset -> run to insn{cpu}==K -> full ARM state. Returns (tuple16, cpsr,
    reached, insn)."""
    ev = f"insn{cpu}"
    c.cmd("reset")
    r = c.cmd("run_to_event", event=ev, count=K)
    reached = r.get("reached", False)
    insn = r["counts"][ev]
    rg = c.cmd("regs", cpu=cpu)
    return tuple(rg["r"]), rg["cpsr"], reached, insn


def same(a, b):
    """Compare full architectural state at an exact instruction index.

    melonDS stores R15 pipeline-ahead, so normalize its raw value by 4 in ARM
    state or 2 in Thumb state before comparing it with the runner's next-PC.
    A failed/short run is a coverage ceiling, never evidence of agreement.
    """
    if not (a[2] and b[2]) or a[3] != b[3]:
        return False
    oracle_pc = (b[0][15] - (2 if (b[1] & 0x20) else 4)) & 0xFFFFFFFF
    return (a[0][:15] == b[0][:15] and a[1] == b[1]
            and a[0][15] == oracle_pc)


def diff_report(a, b):
    out = []
    for i in range(15):                       # r0..r14 (r15 shown separately)
        if a[0][i] != b[0][i]:
            nm = {13: "sp", 14: "lr"}.get(i, f"r{i}")
            out.append(f"    {nm:>3}: native={a[0][i]:08x} oracle={b[0][i]:08x}")
    if a[1] != b[1]:
        out.append(f"   cpsr: native={a[1]:08x} oracle={b[1]:08x}")
    oracle_pc = (b[0][15] - (2 if (b[1] & 0x20) else 4)) & 0xFFFFFFFF
    out.append(f"     pc: native={a[0][15]:08x} oracle={oracle_pc:08x}"
               f"   (oracle raw R15={b[0][15]:08x})")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpu", type=int, default=7, choices=(7, 9))
    ap.add_argument("--native-port", type=int, default=19842)
    ap.add_argument("--oracle-port", type=int, default=19843)
    ap.add_argument("--max", type=int, default=8_000_000,
                    help="upper cap on instruction index to search")
    ap.add_argument("--known-same", type=int, default=0,
                    help="previously verified identical lower bound; validate it, then bisect directly to --max")
    args = ap.parse_args()
    cpu = args.cpu
    n = DebugClient(port=args.native_port, timeout=600.0)
    o = DebugClient(port=args.oracle_port, timeout=600.0)

    def cmp_at(K):
        sn = state(n, cpu, K)
        so = state(o, cpu, K)
        return sn, so

    # 1) Sanity: from reset the streams must AGREE. Print the first 12 so a
    #    counting-semantics mismatch (e.g. Thumb-BL counted differently) shows
    #    up immediately as an early PC skew rather than a false "divergence".
    print(f"# fp-stream sanity (ARM{cpu}, insn{cpu}=1..12) — must match:")
    first_bad = None
    for K in range(1, 13):
        sn, so = cmp_at(K)
        ok = same(sn, so)
        print(f"  K={K:2d}: native pc={sn[0][15]:08x} oracle pc={so[0][15]:08x} "
              f"{'OK' if ok else 'DIFF'}")
        if not ok and first_bad is None:
            first_bad = K
    if first_bad == 1:
        print("\n!! diverge at K=1 — the two sides do not even agree at reset;\n"
              "   likely an insn-counting-semantics mismatch, not a guest bug.\n"
              "   Reconcile the counters before trusting the bisection.")

    # 2) Geometric probe upward for the first K that differs.
    lo_same = args.known_same
    hi_diff = None
    if lo_same:
        if lo_same >= args.max:
            ap.error("--known-same must be lower than --max")
        sn, so = cmp_at(lo_same)
        if not same(sn, so):
            raise SystemExit(f"--known-same={lo_same} is not identical on this build")
        sn, so = cmp_at(args.max)
        if same(sn, so):
            lo_same = args.max
        else:
            hi_diff = args.max
    else:
        K = 1
        while True:
            sn, so = cmp_at(K)
            if same(sn, so):
                lo_same = K
                if K == args.max:
                    break
                K = min(K * 4, args.max)
            else:
                hi_diff = K
                break
    if hi_diff is None:
        print(f"\nNo ARM{cpu} state divergence up to insn{cpu}={args.max}. "
              f"The first divergence is elsewhere (other CPU / IPC / later).")
        return

    # 3) Binary-search the boundary in (lo_same, hi_diff].
    while hi_diff - lo_same > 1:
        mid = (lo_same + hi_diff) // 2
        sn, so = cmp_at(mid)
        if same(sn, so):
            lo_same = mid
        else:
            hi_diff = mid

    Kdiv = hi_diff
    print(f"\n================================================================")
    print(f"FIRST ARM{cpu} divergence at insn{cpu}={Kdiv} "
          f"(last identical at {lo_same})")
    # last-identical state (the site that branches/loads divergently)
    sn0, so0 = cmp_at(lo_same)
    print(f"\n-- at insn{cpu}={lo_same} (IDENTICAL): pc={sn0[0][15]:08x} "
          f"lr={sn0[0][14]:08x} sp={sn0[0][13]:08x}")
    print(f"   this instruction, when executed, produced the divergence below.")
    sn1, so1 = cmp_at(Kdiv)
    print(f"\n-- at insn{cpu}={Kdiv} (DIVERGES):")
    print(f"   native: pc={sn1[0][15]:08x} lr={sn1[0][14]:08x} sp={sn1[0][13]:08x} "
          f"reached={sn1[2]} insn={sn1[3]}")
    print(f"   oracle: pc={so1[0][15]:08x} lr={so1[0][14]:08x} sp={so1[0][13]:08x} "
          f"reached={so1[2]} insn={so1[3]}")
    print("   register diff:")
    print(diff_report(sn1, so1))
    print("================================================================")


if __name__ == "__main__":
    main()
