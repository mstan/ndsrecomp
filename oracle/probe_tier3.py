"""Dump the tier3 per-instruction ring around the ARM9 SWI wild jump."""
from _client import DebugClient

RESULT = {0: "Normal", 1: "Branch", 2: "SWI", 3: "Undef", 4: "NotImpl"}
PHASE = {0: "entry", 1: "pre", 2: "post", 3: "exit"}


def main():
    c = DebugClient(port=19842, timeout=600.0)
    print("ping:", c.ping())
    r = c.cmd("run_cycles", arm9=99_413_000)
    print("run_cycles:", r["cycles"])

    ev = c.cmd("tier3_trace", max=4096)["events"]
    # keep ARM9 (cpu 0) events; show the tail
    a9 = [e for e in ev if e["cpu"] == 0]
    print(f"\ntotal tier3 events={len(ev)} arm9={len(a9)}; showing last 60 ARM9 pre/post:")
    shown = [e for e in a9 if e["phase"] in (1, 2)][-60:]
    for e in shown:
        print("  seq=%d %-4s pc=%08X raw=%08X thumb=%d cpsr=%08X r0=%08X r1=%08X r2=%08X sp=%08X lr=%08X res=%s cyc=%d" % (
            e["seq"], PHASE[e["phase"]], e["pc"], e["raw"], e["thumb"], e["cpsr"],
            e["r0"], e["r1"], e["r2"], e["sp"], e["lr"], RESULT.get(e["result"], "?"),
            e["cycles"]))

    # find the SWI event explicitly
    swis = [e for e in a9 if e["result"] == 2 and e["phase"] == 2]
    print(f"\nSWI post-step events: {len(swis)}")
    for e in swis[-5:]:
        print("  SWI seq=%d pc=%08X raw=%08X thumb=%d cpsr=%08X lr=%08X" % (
            e["seq"], e["pc"], e["raw"], e["thumb"], e["cpsr"], e["lr"]))
    c.close()


if __name__ == "__main__":
    main()
