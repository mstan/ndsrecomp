"""After the SPI/RTC fix: does the native ARM7 still do the slow SoundBias ramp?
Step the native, watch SOUNDBIAS, ARM7 PC, and ITCM[0] (firmware-copy marker)."""
from _client import DebugClient


def main():
    c = DebugClient(port=19842, timeout=600.0)
    print("ping:", c.ping())
    for cyc in [2_000_000, 4_000_000, 6_000_000, 8_000_000, 12_000_000,
                16_000_000, 20_000_000, 30_000_000, 45_000_000]:
        c.cmd("run_cycles", arm9=cyc)
        sb = c.cmd("read_io", cpu=7, addr=0x04000504, width=16)["value"]
        r7 = c.cmd("regs", cpu=7); r9 = c.cmd("regs", cpu=9)
        itcm0 = c.cmd("read_mem", cpu=9, addr=0x01000000, len=4)["hex"]
        in_fw = "FW" if r7["r"][15] >= 0x03800000 else ("RAM" if r7["r"][15] >= 0x02000000 else "BIOS")
        print("arm9cyc=%9d SOUNDBIAS=0x%03X ARM7 pc=0x%08X[%s] ARM9 pc=0x%08X ITCM[0]=%s" % (
            cyc, sb, r7["r"][15], in_fw, r9["r"][15], itcm0))
    c.close()


if __name__ == "__main__":
    main()
