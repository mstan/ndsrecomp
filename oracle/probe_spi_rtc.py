"""Confirm what the native ARM7 firmware polls during early boot:
dump the bus watch ring filtered for SPI (0x040001C0-C3), RTC (0x04000138),
and read current SPICNT device-select + RTC latch. Run before SoundBias starts.
"""
from _client import DebugClient


def main():
    c = DebugClient(port=19842, timeout=600.0)
    print("ping:", c.ping())
    # run to ~8M ARM9 cyc (early boot, before the SoundBias ramp ~26M)
    print("run:", c.cmd("run_cycles", arm9=8_000_000)["cycles"])
    spicnt = c.cmd("read_io", cpu=7, addr=0x040001C0, width=16)["value"]
    print("SPICNT=0x%04X  device-select=%d  (0=powerman,1=fw,2=touch)" % (
        spicnt, (spicnt >> 8) & 3))
    print("RTC(0x138) latch:", c.cmd("read_mem", cpu=7, addr=0x04000138, len=4)["hex"])

    ev = c.cmd("watch", max=512)["events"]
    # filter for SPI + RTC region
    interesting = [e for e in ev if 0x04000134 <= e["addr"] <= 0x040001C4]
    print(f"\n{len(interesting)} SPI/RTC bus accesses in last-512 ring:")
    seen = {}
    for e in interesting:
        a = e["addr"]
        seen[a] = seen.get(a, 0) + 1
    for a in sorted(seen):
        print("  addr=0x%08X count=%d" % (a, seen[a]))
    print("\nlast 30 SPI/RTC events (pc, addr, val):")
    for e in interesting[-30:]:
        print("  pc=0x%08X addr=0x%08X val=0x%08X w=%d" % (
            e["pc"], e["addr"], e["value"], e["width"]))
    c.close()


if __name__ == "__main__":
    main()
