"""Decisive SoundBias invariant: compare the SOUNDBIAS (0x04000504) write
stream on native vs oracle. A side-effect loop — count + first + last value
must match if both run the same BIOS ramp. Anchored on a generous run, not a
time point: we just let each side run until the ramp is clearly complete.

Outcomes:
  count_nat == count_orc, first/last match  -> SAME ramp; the slowness was a
      pure timing artifact of the cycle/vblank anchor. Ignore it.
  count_orc << count_nat (e.g. 1)           -> melonDS shortcuts/HLEs the ramp
      (oracle-model mismatch), our LLE ramp is correct.
  counts differ otherwise                   -> real divergence to chase.
"""
from _client import DebugClient


def sb(counts):
    return (counts["soundbias_w"], counts["soundbias_first"], counts["soundbias_last"])


def main():
    nat = DebugClient(port=19842, timeout=600.0)
    orc = DebugClient(port=19843, timeout=600.0)
    print("native:", nat.ping(), " oracle:", orc.ping())

    # Native: run well past the ramp (~26M cyc) with margin.
    rn = nat.cmd("run_cycles", arm9=60_000_000)
    print("native ran to cyc", rn["cycles"])
    # Oracle: run to a generous vblank count (it reaches the menu by ~120).
    ro = orc.cmd("run_to_event", event="vblank9", count=130)
    print("oracle ran to vblank9=130 reached", ro["reached"])

    cn = nat.cmd("event_counts")
    co = orc.cmd("event_counts")
    print("\nnative counts:", cn)
    print("oracle counts:", co)

    wn, fn, ln = sb(cn)
    wo, fo, lo = sb(co)
    print("\n== SOUNDBIAS write invariant ==")
    print(f"  native: writes={wn:5d} first=0x{fn:03X} last=0x{ln:03X}")
    print(f"  oracle: writes={wo:5d} first=0x{fo:03X} last=0x{lo:03X}")
    if wn == wo and fn == fo and ln == lo:
        print("  VERDICT: IDENTICAL ramp -> slowness was a pure timing artifact. Ignore.")
    elif wo <= 2 and wn > 50:
        print("  VERDICT: oracle shortcuts the ramp (HLE/model) -> our LLE ramp is correct.")
    else:
        print("  VERDICT: counts differ -> investigate (real divergence or width/hook mismatch).")
    nat.close(); orc.close()


if __name__ == "__main__":
    main()
