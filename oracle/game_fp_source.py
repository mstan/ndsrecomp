#!/usr/bin/env python3
"""Inspect both sides immediately before the first divergent SM64DS load."""

from _client import DebugClient
from game_fp_bisect import BASE_NATIVE, BASE_ORACLE, launch_to, normalized


K = 4_971_153


def main():
    for name, port, base, is_oracle in (
        ("native", 19842, BASE_NATIVE, False),
        ("oracle", 19843, BASE_ORACLE, True),
    ):
        c = DebugClient(port=port, timeout=600.0)
        try:
            hit, rg = launch_to(c, base + K)
            state = normalized(rg, oracle=is_oracle)
            addr = state[0][3]
            print(name, "hit", hit)
            print(name, "regs", [f"{v:08x}" for v in state[0]],
                  f"cpsr={state[1]:08x}")
            print(name, "sched", c.cmd("sched_state"))
            counts = c.cmd("event_counts")
            print(name, "counts", counts)
            rg7 = c.cmd("regs", cpu=7)
            r7 = normalized(rg7, oracle=is_oracle)
            print(name, "regs7", [f"{v:08x}" for v in r7[0]],
                  f"cpsr={r7[1]:08x}")
            print(name, "io_state", c.cmd("io_state"))
            end7 = counts["insn7"]
            print(name, "insn7_tail")
            for i in range(max(1, end7 - 24), end7 + 1):
                s = c.cmd("insn_sample", cpu=7, count=i)
                if s.get("found"):
                    print(" ", i, f"pc={s['pc']:08x}",
                          f"r0={s['r'][0]:08x}", f"r1={s['r'][1]:08x}",
                          f"r2={s['r'][2]:08x}", f"r3={s['r'][3]:08x}")
            print(name, "r3", f"0x{addr:08x}", "mem",
                  c.cmd("read_mem", cpu=9, addr=addr & ~3, len=8))
            if 0x04000000 <= addr < 0x05000000:
                print(name, "io16", c.cmd("read_io", cpu=9,
                                            addr=addr & ~1, width=16))
        finally:
            c.close()


if __name__ == "__main__":
    main()
