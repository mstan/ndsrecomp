"""Cross-check the SOUNDBIAS write-counter against the actual register VALUE on
both sides — validates whether the oracle's count=0 is real or a hook artifact."""
from _client import DebugClient


def main():
    nat = DebugClient(port=19842, timeout=600.0)
    orc = DebugClient(port=19843, timeout=600.0)
    vn = nat.cmd("read_io", cpu=7, addr=0x04000504, width=16)["value"]
    vo = orc.cmd("read_io", cpu=7, addr=0x04000504, width=16)["value"]
    cn = nat.cmd("event_counts")["soundbias_w"]
    co = orc.cmd("event_counts")["soundbias_w"]
    print(f"native SOUNDBIAS value = 0x{vn:03X}  write-count={cn}")
    print(f"oracle SOUNDBIAS value = 0x{vo:03X}  write-count={co}")
    print()
    if vo >= 0x100 and co == 0:
        print("=> HOOK ARTIFACT: oracle reached high SOUNDBIAS but 16-bit hook saw 0 writes")
        print("   (melonDS writes it via a different width / internal SPU path).")
    elif vo == 0 and co == 0:
        print("=> CONFIRMED: oracle never wrote SOUNDBIAS -> melonDS does not run this ramp here.")
    else:
        print(f"=> inconclusive: oracle value 0x{vo:03X}, count {co}.")
    nat.close()
    orc.close()


if __name__ == "__main__":
    main()
