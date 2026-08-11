#!/usr/bin/env python3
"""Boot Mario Kart DS by tapping the DS-card slot icon in the firmware menu,
and compare core hardware-event counters with the melonDS oracle.

Root-cause context (beads-yjp.3): an earlier investigation concluded
scripted touch could not drive the cart-launch icon, after holding/releasing
a tap and inspecting the framebuffer at or near the release event. It could
all along: the firmware's card-slot fade-out and the subsequent game-boot
IPC handshake unfold over roughly 15-200+ VBlanks *after* release, so a
snapshot taken too soon after releasing the touch shows no visible change
and reads as "no reaction." This smoke test runs the simulation far enough
forward to observe the actual reaction, and asserts the game-boot IPC
handshake fired (`ipcsync_w` reaching the same 12520 signature already
established for the `--startup-mode automatic` path in
docs/wiimmfi-runbook.md #7b) instead of judging reaction from a premature
snapshot. (Running further -- confirmed manually to VBlank 2000+ during
this investigation, not by this automated test -- reaches the real MKDS
title screen and then attract-mode gameplay; this test deliberately stops
shortly after the handshake to stay decoupled from MKDS's own separate,
ongoing game-code boot-accuracy effort.)

Harness fix (beads-yjp.7): this test used to run *both* machines to a raw
frame number (`run_to_event vblank9 count=300`) and only then compare
counters -- exactly the raw-frame-number comparison CLAUDE.md's DUAL-CPU
RULE forbids. `vblank9=300` is not a rendezvous: nothing guarantees native
and oracle are at architecturally the same point in MKDS's own boot code
just because both have counted the same number of VBlank IRQs, and in
practice they were not -- an orchestrator run at the old boundary saw
`fifo9to7` 296/612, `irq9` 2053/1393, `spi_w` 157618/157771, an apparent
"divergence" that was actually just the two sides sitting at different
phases of the post-handshake boot, with no way to tell a real divergence
from a phase artifact using that comparison point. Fixed by rendezvousing
on the hardware event this whole test is *about* -- `ipcsync_w` reaching
the game-boot handshake value 12520 -- and comparing MATCHED_COUNTERS only
at that shared boundary. `ipcsync_w` is a monotonically-incrementing
counter that `run_to_event` stops exactly *at* (see the sub-event break
arming in `runner/src/debug_server.cpp`'s `run_to_event` handler), so both
machines are guaranteed to stop at the identical architectural handshake
instant, not merely the same VBlank tally. Verified after the fix:
native/oracle agree exactly on every MATCHED_COUNTERS field at that
boundary -- the old apparent divergence was entirely a rendezvous
artifact of comparing at different phases of the post-handshake boot, not
a real bug. See beads-yjp.7 for the actual run's numbers.

Launch both servers with the SAME cartridge inserted and `manual` startup
(so the run stops at the interactive firmware menu instead of auto-booting
the cart), then run this script:

  nds_runner <bios-dir> --serve --port 19842 --config game.toml \
      --rom "Mario Kart DS.nds" --no-save --startup-mode manual
  ndsref --bios9 <bios9.rom> --bios7 <bios7.rom> --firmware <firmware.bin> \
      --rom "Mario Kart DS.nds" --boot firmware --port 19843

  python oracle/mkds_cart_icon_launch_smoke.py
"""

from concurrent.futures import ThreadPoolExecutor

from _client import DebugClient


# Hardware-event counters with a shared architectural meaning on both
# implementations (VBlank IRQ, IPC, SPI transaction count, sound-DMA bias
# writes). Deliberately excludes insn9/insn7/cyc9/cyc7: two independently
# implemented ARM cores retiring the same guest program do not necessarily
# retire byte-for-byte identical *host* instruction/cycle counts even when
# architecturally faithful -- see game_boot_smoke.py for the established
# precedent of the same exclusion for SM64DS's title-touch boot.
MATCHED_COUNTERS = (
    "vblank9",
    "vblank7",
    "ipcsync_w",
    "fifo9to7",
    "fifo7to9",
    "spi_w",
    "irq9",
    "irq7",
    "soundbias_w",
)

# Bottom-screen pixel coordinates of the DS-card slot banner box (grid-
# verified against the rendered framebuffer; the box spans roughly
# x:8-248, y:8-68 -- see generated/captures/wiimmfi-runbook/
# firmware_menu_bottom_screen_grid_debug.png in the game worktree).
CART_ICON_X = 128
CART_ICON_Y = 38

# The game-boot IPC handshake signature already established for the
# automatic-startup path (docs/wiimmfi-runbook.md #7b): ipcsync_w reaches
# 12520 once the ARM7/ARM9 game code has taken over from the firmware menu.
# This is also the harness's rendezvous point (see module docstring):
# both machines run_to_event on this exact counter value, not on a raw
# VBlank tally, so the counters compared below are guaranteed to be sampled
# at the identical architectural instant on both sides.
GAME_BOOT_IPCSYNC_W = 12520


def launch_from_menu(port):
    client = DebugClient(port=port, timeout=600.0)
    try:
        client.cmd("reset")
        client.cmd("run_to_event", event="vblank9", count=120, stall=300_000)
        client.cmd("touch", x=CART_ICON_X, y=CART_ICON_Y, down=True)
        # A short hold is sufficient: 10 VBlanks matches the shortest hold
        # already tried (and mistakenly judged inert) in the original
        # investigation.
        client.cmd("run_to_event", event="vblank9", count=130, stall=300_000)
        client.cmd("touch", x=CART_ICON_X, y=CART_ICON_Y, down=False)

        # Rendezvous on the hardware event itself, not a raw VBlank count:
        # run each machine until its OWN ipcsync_w counter reaches the
        # game-boot handshake value. The fade-out plus handshake unfolds
        # over several dozen VBlanks after release; the generous stall
        # budget tolerates that without pretending a specific frame number
        # is where the two sides are comparable.
        hit = client.cmd("run_to_event", event="ipcsync_w",
                          count=GAME_BOOT_IPCSYNC_W, stall=300_000)
        return hit, client.cmd("event_counts")
    finally:
        client.close()


def main():
    with ThreadPoolExecutor(max_workers=2) as pool:
        native_future = pool.submit(launch_from_menu, 19842)
        oracle_future = pool.submit(launch_from_menu, 19843)
        native = native_future.result()
        oracle = oracle_future.result()

    # Hard gate (preserved): if either machine's own ipcsync_w counter never
    # reaches the game-boot handshake value, the cart-icon tap genuinely did
    # not launch the game on that machine -- fail loudly rather than compare
    # counters that were never at a real rendezvous.
    if not native[0].get("reached") or native[0].get("terminal"):
        raise SystemExit(f"native cart launch failed to reach the game-boot "
                          f"IPC handshake (ipcsync_w={GAME_BOOT_IPCSYNC_W}): "
                          f"{native[0]}")
    if not oracle[0].get("reached") or oracle[0].get("terminal"):
        raise SystemExit(f"oracle cart launch failed to reach the game-boot "
                          f"IPC handshake (ipcsync_w={GAME_BOOT_IPCSYNC_W}): "
                          f"{oracle[0]}")
    assert native[1]["ipcsync_w"] == GAME_BOOT_IPCSYNC_W
    assert oracle[1]["ipcsync_w"] == GAME_BOOT_IPCSYNC_W

    # Now that both sides are rendezvoused on the SAME hardware event (not a
    # raw frame number), any surviving counter difference is a genuine
    # divergence, not a phase artifact -- no tolerance band needed or
    # wanted. Fail loudly and report every differing counter with evidence.
    differences = {
        name: (native[1][name], oracle[1][name])
        for name in MATCHED_COUNTERS
        if native[1][name] != oracle[1][name]
    }
    if differences:
        raise SystemExit(
            "core event counters differ at the ipcsync_w=12520 rendezvous "
            "-- this is a genuine divergence now that both sides are "
            "compared at the same hardware event, not a raw-frame-number "
            "phase artifact: "
            + ", ".join(f"{k}={v[0]}/{v[1]}" for k, v in differences.items())
        )

    print("MKDS cart-icon tap: reached the game-boot IPC handshake "
          f"(ipcsync_w={GAME_BOOT_IPCSYNC_W}) on both machines, rendezvoused "
          "on the hardware event (not a raw VBlank count)")
    print(f"native rounds={native[0].get('rounds')} "
          f"oracle rounds={oracle[0].get('rounds')} "
          "(expected to differ -- host round-trip cost, not compared)")
    for name in MATCHED_COUNTERS:
        print(f"{name}: native={native[1][name]} oracle={oracle[1][name]}")


if __name__ == "__main__":
    main()
