#!/usr/bin/env python3
"""Pin the wedged ARM7 SWI: the call site in WRAM, the BIOS vector table, and
the full ARM7 register/IPC state at the wedge. Reset to a clean anchor first."""
from _client import DebugClient


def rd(c, cpu, addr, ln):
    return bytes.fromhex(c.cmd("read_mem", cpu=cpu, addr=addr, len=ln)["hex"])


def regs(c, cpu):
    return c.cmd("regs", cpu=cpu)


for name, port, stall in (("native", 19842, 300000), ("oracle", 19843, 2000)):
    c = DebugClient(port=port, timeout=600.0)
    c.cmd("reset")
    c.cmd("run_to_event", event="ipcsync_w", count=30, stall=stall)
    r7 = regs(c, 7)
    pc, lr = r7["r"][15], r7["r"][14]
    print(f"=== {name} @ ipcsync_w=30 ===")
    print(f"  ARM7 pc={pc:08x} lr={lr:08x} cpsr={r7['cpsr']:08x} "
          f"mode={r7['mode']:02x} sp={r7['r'][13]:08x} r0={r7['r'][0]:08x} "
          f"r1={r7['r'][1]:08x} r2={r7['r'][2]:08x} r12={r7['r'][12]:08x}")
    # SWI call site: lr-4 .. lr+4 (Thumb SWI is at lr-2; show context)
    site = (lr - 6) & ~1
    b = rd(c, 7, site, 16)
    print(f"  WRAM call-site @0x{site:08x}: {b.hex()}")
    # decode candidate Thumb halfwords around lr-2
    hw = [int.from_bytes(b[i:i+2], "little") for i in range(0, len(b), 2)]
    print(f"    halfwords: {' '.join(f'{h:04x}' for h in hw)}")
    # ARM7 BIOS exception vector table 0x00..0x20
    v = rd(c, 7, 0x00000000, 0x20)
    print(f"  BIOS vectors 0x00..0x20: {v.hex()}")
    vw = [int.from_bytes(v[i:i+4], "little") for i in range(0, len(v), 4)]
    print(f"    words: {' '.join(f'{w:08x}' for w in vw)}")
    c.close()
