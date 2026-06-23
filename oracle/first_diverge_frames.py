"""Step native + oracle frame-by-frame (vblank9), find the FIRST frame where
ARM9-visible memory diverges. Small regions (DTCM/ITCM/WRAM) every frame;
mainram too. Reports first diverging region + offset + regs.
"""
import sys
from _client import DebugClient

REGIONS = ["dtcm", "itcm", "wramshared", "wram7", "mainram"]
MAXF = int(sys.argv[1]) if len(sys.argv) > 1 else 88


def region(c, name):
    r = c.cmd("read_region", region=name)
    return bytes.fromhex(r["hex"])


def first_diff(a, b):
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    if len(a) != len(b):
        return n
    return -1


def regs(c, cpu):
    r = c.cmd("regs", cpu=cpu)
    return r


def main():
    nat = DebugClient(port=19842, timeout=600.0)
    orc = DebugClient(port=19843, timeout=600.0)
    print("native:", nat.ping(), " oracle:", orc.ping())

    for f in range(1, MAXF + 1):
        rn = nat.cmd("run_to_event", event="vblank9", count=f)
        ro = orc.cmd("run_to_event", event="vblank9", count=f)
        diffs = []
        for name in REGIONS:
            try:
                a = region(nat, name)
                b = region(orc, name)
            except Exception as e:
                diffs.append((name, f"err {e}"))
                continue
            d = first_diff(a, b)
            if d >= 0:
                aw = int.from_bytes(a[d & ~3:(d & ~3)+4], "little")
                bw = int.from_bytes(b[d & ~3:(d & ~3)+4], "little")
                diffs.append((name, d, aw, bw, len(a), len(b)))
        tag = "OK" if not diffs else "DIVERGE"
        print(f"vblank9={f:3d} nat={rn.get('reached')} orc={ro.get('reached')} {tag}")
        if diffs:
            for d in diffs:
                if len(d) == 2:
                    print("   ", d)
                else:
                    name, off, aw, bw, la, lb = d
                    base = {"dtcm": 0x03000000, "itcm": 0x01000000,
                            "wramshared": 0x03000000, "wram7": 0x03800000,
                            "mainram": 0x02000000}[name]
                    print(f"    {name}: first diff @off 0x{off:X} (~0x{base+off:08X}) "
                          f"nat=0x{aw:08X} orc=0x{bw:08X} len {la}/{lb}")
            r9n, r9o = regs(nat, 9), regs(orc, 9)
            r7n, r7o = regs(nat, 7), regs(orc, 7)
            print(f"    ARM9 nat pc=0x{r9n['r'][15]:08X} cpsr=0x{r9n['cpsr']:08X} "
                  f"| orc pc=0x{r9o['r'][15]:08X} cpsr=0x{r9o['cpsr']:08X}")
            print(f"    ARM7 nat pc=0x{r7n['r'][15]:08X} | orc pc=0x{r7o['r'][15]:08X}")
            print("STOP at first diverging frame.")
            break

    nat.close()
    orc.close()


if __name__ == "__main__":
    main()
