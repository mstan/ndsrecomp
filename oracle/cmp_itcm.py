"""Content-anchored compare of static code regions: native (post-boot copy)
vs oracle (menu). ITCM holds the IRQ handler + context-switch routine that
the crash runs. If the CODE matches, the copy/recompile is correct and the
bug is runtime state/timing; if it differs, it's a copy/decode bug.
"""
from _client import DebugClient


def region(c, name):
    return bytes.fromhex(c.cmd("read_region", region=name)["hex"])


def cmp_region(nat, orc, name, base):
    a = region(nat, name)
    b = region(orc, name)
    n = min(len(a), len(b))
    diffs = [i for i in range(n) if a[i] != b[i]]
    print(f"\n== {name} ({len(a)}/{len(b)} bytes) : {len(diffs)} differing bytes ==")
    if not diffs:
        print("   IDENTICAL")
        return
    # cluster diffs into ranges
    runs = []
    s = diffs[0]
    p = diffs[0]
    for i in diffs[1:]:
        if i == p + 1:
            p = i
        else:
            runs.append((s, p))
            s = p = i
    runs.append((s, p))
    print(f"   {len(runs)} diff-runs; first 24:")
    for (lo, hi) in runs[:24]:
        aw = a[lo:min(lo+8, len(a))].hex()
        bw = b[lo:min(lo+8, len(b))].hex()
        print(f"   off 0x{lo:05X}-0x{hi:05X} (~0x{base+lo:08X}) nat={aw} orc={bw}")


def main():
    nat = DebugClient(port=19842, timeout=600.0)
    orc = DebugClient(port=19843, timeout=600.0)
    print("native:", nat.ping(), " oracle:", orc.ping())
    print("native run to 50M cyc...", nat.cmd("run_cycles", arm9=50_000_000)["cycles"])
    print("oracle run to vblank9=120...", orc.cmd("run_to_event", event="vblank9", count=120)["reached"])

    cmp_region(nat, orc, "itcm", 0x01000000)
    cmp_region(nat, orc, "wramshared", 0x03000000)
    nat.close(); orc.close()


if __name__ == "__main__":
    main()
