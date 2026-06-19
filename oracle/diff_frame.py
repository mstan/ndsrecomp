#!/usr/bin/env python3
"""diff_frame.py — diff the engine-A and engine-B framebuffers between
native and oracle at a hardware-event checkpoint. On mismatch, dump both
sides as PNG (or raw RGB if Pillow is unavailable).

    python diff_frame.py --event vblank9:60

Engine A is the top screen, engine B the bottom. Each is 256x192 RGB.
"""

import argparse
from _client import DebugClient, parse_event, first_divergence


def dump(path, w, h, rgb):
    try:
        from PIL import Image
        Image.frombytes("RGB", (w, h), rgb).save(path + ".png")
        return path + ".png"
    except Exception:
        with open(path + ".rgb", "wb") as f:
            f.write(rgb)
        return path + ".rgb"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--event", default="vblank9:60")
    ap.add_argument("--native-port", type=int, default=19842)
    ap.add_argument("--oracle-port", type=int, default=19843)
    args = ap.parse_args()

    event, count = parse_event(args.event)
    native = DebugClient(port=args.native_port)
    oracle = DebugClient(port=args.oracle_port)
    native.run_to_event(event, count)
    oracle.run_to_event(event, count)

    rc = 0
    for engine in ("A", "B"):
        w, h, na = native.framebuffer(engine)
        _, _, ob = oracle.framebuffer(engine)
        div = first_divergence(na, ob)
        if div is None:
            print(f"engine {engine}: IDENTICAL ({w}x{h})")
            continue
        i, _, _ = div
        px = i // 3
        print(f"engine {engine}: DIVERGE at byte 0x{i:x} "
              f"(pixel {px % w},{px // w})")
        print("  wrote", dump(f"native_{engine}_{event}{count}", w, h, na))
        print("  wrote", dump(f"oracle_{engine}_{event}{count}", w, h, ob))
        rc = 1
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
