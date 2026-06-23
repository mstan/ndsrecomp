"""Is the SoundBias ramp (0->0x200) the divergence? Read SOUNDBIAS(0x04000504,
ARM7) on native (parked mid-boot) and oracle (parked at menu), then restart the
oracle and watch SOUNDBIAS ramp over early frames to see how fast melonDS does it.
"""
import sys
from _client import DebugClient


def sb(c):
    return c.cmd("read_io", cpu=7, addr=0x04000504, width=16)["value"]


def main():
    nat = DebugClient(port=19842, timeout=600.0)
    orc = DebugClient(port=19843, timeout=600.0)
    print("native parked: cyc=%s SOUNDBIAS=0x%03X" % (
        nat.cmd("run_cycles", arm9=1)["cycles"], sb(nat)))
    r9 = nat.cmd("regs", cpu=7)
    print("   native ARM7 pc=0x%08X r1(delay)=0x%08X" % (r9["r"][15], r9["r"][1]))
    print("oracle parked (vblank120): SOUNDBIAS=0x%03X counts=%s" % (
        sb(orc), orc.cmd("event_counts")))
    nat.close(); orc.close()


if __name__ == "__main__":
    main()
