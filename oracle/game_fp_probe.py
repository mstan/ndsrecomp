#!/usr/bin/env python3
"""Fresh-reset ARM9 game-entry fingerprint probe for SM64DS diagnostics."""

from _client import DebugClient


ENTRY = 0x02004804


def launch(c):
    c.cmd("reset")
    c.cmd("run_to_event", event="vblank9", count=100)
    c.cmd("touch", x=128, y=48, down=True)
    c.cmd("run_to_event", event="vblank9", count=102)
    c.cmd("touch", x=128, y=48, down=False)


def main():
    n = DebugClient(port=19842, timeout=600.0)
    o = DebugClient(port=19843, timeout=600.0)
    try:
        bases = {"native": 48_498_170, "oracle": 48_508_498}
        for name, c in (("native", n), ("oracle", o)):
            launch(c)
            kwargs = {"pc": ENTRY}
            if name == "oracle":
                kwargs.update(cpu=9, timeout=1000)
            else:
                kwargs.update(max_rounds=100_000_000)
            hit = c.cmd("run_to_pc", **kwargs)
            print(name, "hit", hit)
            print(name, "regs", c.cmd("regs", cpu=9))
            print(name, "counts", c.cmd("event_counts"))
            for delta in (-1, 0, 1, 2):
                launch(c)
                target = bases[name] + delta
                hit = c.cmd("run_to_event", event="insn9", count=target,
                            stall=300_000)
                rg = c.cmd("regs", cpu=9)
                pc = rg["r"][15]
                if name == "oracle":
                    pc = (pc - (2 if rg["cpsr"] & 0x20 else 4)) & 0xFFFFFFFF
                print(name, "insn", target, "delta", delta,
                      "reached", hit.get("reached"), "pc", hex(pc),
                      "lr", hex(rg["r"][14]), "cpsr", hex(rg["cpsr"]))
    finally:
        n.close()
        o.close()


if __name__ == "__main__":
    main()
