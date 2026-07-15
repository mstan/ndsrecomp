#!/usr/bin/env python3
"""Boot SM64DS through its title touch and compare core events with ndsref."""

from concurrent.futures import ThreadPoolExecutor

from _client import DebugClient


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


def boot(port):
    client = DebugClient(port=port, timeout=600.0)
    try:
        client.cmd("reset")
        client.cmd("run_to_event", event="vblank9", count=100,
                   stall=300_000)
        client.cmd("touch", x=128, y=48, down=True)
        client.cmd("run_to_event", event="vblank9", count=102,
                   stall=300_000)
        client.cmd("touch", x=128, y=48, down=False)

        client.cmd("run_to_event", event="vblank9", count=900,
                   stall=300_000)
        client.cmd("touch", x=128, y=120, down=True)
        client.cmd("run_to_event", event="vblank9", count=930,
                   stall=300_000)
        client.cmd("touch", x=128, y=120, down=False)

        hit = client.cmd("run_to_event", event="vblank9", count=1100,
                         stall=300_000)
        return hit, client.cmd("event_counts")
    finally:
        client.close()


def main():
    with ThreadPoolExecutor(max_workers=2) as pool:
        native_future = pool.submit(boot, 19842)
        oracle_future = pool.submit(boot, 19843)
        native = native_future.result()
        oracle = oracle_future.result()

    if not native[0].get("reached") or native[0].get("terminal"):
        raise SystemExit(f"native boot failed: {native[0]}")
    if not oracle[0].get("reached"):
        raise SystemExit(f"oracle boot failed: {oracle[0]}")

    differences = [
        f"{name}={native[1][name]}/{oracle[1][name]}"
        for name in MATCHED_COUNTERS
        if native[1][name] != oracle[1][name]
    ]
    if differences:
        raise SystemExit("core event counters differ: " + ", ".join(differences))

    print("SM64DS title touch: reached VBlank 1100")
    for name in MATCHED_COUNTERS:
        print(f"{name}: {native[1][name]}")


if __name__ == "__main__":
    main()
