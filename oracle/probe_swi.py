"""Probe the ARM9 SWI-from-IRQ wild jump at ~99.4M cycles.

Drives the native debug server: runs to the crash, then reads the frozen
state retroactively (ring/bus query model, no arm-then-capture).
"""
import sys
from _client import DebugClient


def rd(c, addr, n, cpu=9):
    r = c.cmd("read_mem", cpu=cpu, addr=addr, len=n)
    return bytes.fromhex(r["hex"])


def hexwords(b, base):
    out = []
    for i in range(0, len(b) - 3, 4):
        w = int.from_bytes(b[i:i+4], "little")
        out.append(f"  {base+i:08X}: {w:08X}")
    return "\n".join(out)


def main():
    c = DebugClient(port=19842, timeout=600.0)
    print("ping:", c.ping())
    target = 99_413_000
    print(f"run_cycles -> {target} (this runs to the ARM9 halt)...")
    r = c.cmd("run_cycles", arm9=target)
    print("run_cycles:", r)

    regs9 = c.cmd("regs", cpu=9)
    print("\nARM9 regs:", regs9)
    print("  mode=0x%X cpsr=0x%08X pc=0x%08X sp=0x%08X lr=0x%08X" % (
        regs9["mode"], regs9["cpsr"], regs9["r"][15], regs9["r"][13], regs9["r"][14]))

    # SWI instruction (Thumb) at LR_svc-2.  LR_svc was 0x2C in the trace.
    print("\n-- ITCM 0x00000000..0x40 (low; SWI lives at 0x2A) --")
    print(hexwords(rd(c, 0x00000000, 0x40), 0x00000000))
    swi = rd(c, 0x00000028, 4)
    print("  raw @0x28:", swi.hex(), "-> halfword@0x2A =",
          hex(int.from_bytes(swi[2:4], "little")),
          "swi_num =", hex(swi[2]))

    print("\n-- ITCM IRQ handler region 0x01FF8180..0x01FF8200 --")
    print(hexwords(rd(c, 0x01FF8180, 0x80), 0x01FF8180))

    print("\n-- ITCM 0x00000280..0x000002B0 (the 0x294/0x2A0 code) --")
    print(hexwords(rd(c, 0x00000280, 0x30), 0x00000280))

    # DTCM stack area + the IRQ vector pointer at DTCM_top-4 (0x03003FFC).
    print("\n-- DTCM 0x03003F00..0x03004000 (stacks + IRQ vec ptr @0x3FFC) --")
    print(hexwords(rd(c, 0x03003F00, 0x100), 0x03003F00))

    c.close()


if __name__ == "__main__":
    main()
