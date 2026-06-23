"""The native server is parked mid-boot. Dump what each CPU is doing:
PCs, event counts, IO state, and the runtime trace tail (reveals spin loops).
"""
from _client import DebugClient

KIND = {0: "dispatch", 1: "exchange", 2: "swi", 3: "mem_w", 4: "branch",
        5: "irq", 6: "call", 7: "mem_r"}


def main():
    c = DebugClient(port=19842, timeout=600.0)
    print("ping:", c.ping())
    print("cycles now:", c.cmd("run_cycles", arm9=1)["cycles"])  # no-op, just report
    r9 = c.cmd("regs", cpu=9); r7 = c.cmd("regs", cpu=7)
    print("ARM9 pc=0x%08X cpsr=0x%08X mode=0x%X lr=0x%08X sp=0x%08X" % (
        r9["r"][15], r9["cpsr"], r9["mode"], r9["r"][14], r9["r"][13]))
    print("     R:", " ".join("r%d=%08X" % (i, r9["r"][i]) for i in range(8)))
    print("ARM7 pc=0x%08X cpsr=0x%08X mode=0x%X lr=0x%08X sp=0x%08X" % (
        r7["r"][15], r7["cpsr"], r7["mode"], r7["r"][14], r7["r"][13]))
    print("     R:", " ".join("r%d=%08X" % (i, r7["r"][i]) for i in range(8)))
    print("event_counts:", c.cmd("event_counts"))
    print("io_state:", c.cmd("io_state"))

    ev = c.cmd("runtime_trace", max=40)["events"]
    print(f"\nruntime_trace tail ({len(ev)}):")
    for e in ev:
        print("  seq=%d cyc=%d %-8s pc=0x%08X cpsr=0x%08X addr=0x%08X val=0x%08X aux=0x%X sp=0x%08X lr=0x%08X" % (
            e["seq"], e["cycles"], KIND.get(e["kind"], "?"), e["pc"], e["cpsr"],
            e["addr"], e["value"], e["aux"], e["sp"], e["lr"]))
    c.close()


if __name__ == "__main__":
    main()
