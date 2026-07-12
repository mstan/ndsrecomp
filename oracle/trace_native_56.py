#!/usr/bin/env python3
"""trace_native_56.py — anchor both at ipcsync_w=5 (now both break AT the
write), confirm identical, then step the NATIVE side finely through the
[5,6] interval sampling ARM7 state to expose the spin/divergence that makes
native take ~2 frames where the oracle takes a fraction of one."""

from _client import DebugClient, first_divergence

REGIONS = ["mainram", "wram7", "wramshared", "itcm", "dtcm"]


def r7(c):
    r = c.cmd("regs", cpu=7)
    return r["r"], r["cpsr"]


def io(c, cpu, addr, w):
    try:
        return c.cmd("read_io", cpu=cpu, addr=addr, width=w)["value"]
    except RuntimeError:
        return -1


def main():
    n = DebugClient(port=19842, timeout=600.0)
    o = DebugClient(port=19843, timeout=600.0)

    n.run_to_event("ipcsync_w", 5)
    o.run_to_event("ipcsync_w", 5)

    print("=== ipcsync_w=5 baseline ===")
    for r in REGIONS:
        a, b = n.read_region(r), o.read_region(r)
        d = first_divergence(a, b)
        print(f"  {r:11s}: {'IDENTICAL' if d is None else 'DIVERGE @0x%06x' % d[0]}")
    rn, cn = r7(n); ro, co = r7(o)
    print(f"  native7 pc={rn[15]:08x} lr={rn[14]:08x} sp={rn[13]:08x} cpsr={cn:08x} "
          f"r0={rn[0]:08x} r1={rn[1]:08x} r2={rn[2]:08x} r3={rn[3]:08x}")
    print(f"  oracle7 pc={ro[15]:08x} lr={ro[14]:08x} sp={ro[13]:08x} cpsr={co:08x} "
          f"r0={ro[0]:08x} r1={ro[1]:08x} r2={ro[2]:08x} r3={ro[3]:08x}")

    # current native ARM9 cycle
    st = n.cmd("run_cycles", arm9=0)
    cyc = st["cycles"][0]
    print(f"\n=== stepping native from arm9cyc={cyc} (until ipcsync_w>=6) ===")
    print("  arm9cyc    d_ipc  pc7      lr7      r0       r1       r2       r3      "
          " IF7     IF9   DISPSTAT7")
    for i in range(60):
        cyc += 8000
        st = n.cmd("run_cycles", arm9=cyc)
        ipc = st["counts"]["ipcsync_w"]
        rr, cc = r7(n)
        if7 = io(n, 7, 0x04000214, 32)
        if9 = io(n, 9, 0x04000214, 32)
        disp7 = io(n, 7, 0x04000004, 16)
        print(f"  {st['cycles'][0]:9d}  {ipc:5d}  {rr[15]:08x} {rr[14]:08x} "
              f"{rr[0]:08x} {rr[1]:08x} {rr[2]:08x} {rr[3]:08x} "
              f"{if7:7x} {if9:5x} {disp7:6x}")
        if ipc >= 6:
            print("  -> reached ipcsync_w=6")
            break
    n.close(); o.close()


if __name__ == "__main__":
    raise SystemExit(main())
