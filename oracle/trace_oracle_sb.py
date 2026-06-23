"""Trace the oracle's SOUNDBIAS + ARM7 PC across early frames to see if melonDS
does the slow sound-bias ramp (like native) or shortcuts it."""
from _client import DebugClient


def main():
    orc = DebugClient(port=19843, timeout=600.0)
    print("oracle:", orc.ping())
    for f in [1, 2, 3, 4, 5, 8, 12, 20, 30, 45, 60, 90, 120]:
        orc.cmd("run_to_event", event="vblank9", count=f)
        sb = orc.cmd("read_io", cpu=7, addr=0x04000504, width=16)["value"]
        r7 = orc.cmd("regs", cpu=7)
        r9 = orc.cmd("regs", cpu=9)
        print("vblank9=%3d SOUNDBIAS=0x%03X  ARM7 pc=0x%08X mode=0x%X  ARM9 pc=0x%08X" % (
            f, sb, r7["r"][15], r7["mode"], r9["r"][15]))
    orc.close()


if __name__ == "__main__":
    main()
