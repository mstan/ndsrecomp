#!/usr/bin/env python3
"""Drive Mario Kart DS from the title screen into actual Single Player
race gameplay (via `scenarios/race_start.json`'s scripted A-presses/waits
in the game worktree -- read, not modified), then measure two things
Wiimmfi M7 needs answered before online-race work is worth doing (I12
Job 2):

1. Sustained throughput during real gameplay (VBlanks of guest time per
   wall-clock second) -- NOT a re-proof that racing works; the project
   owner has already confirmed that from real interactive play. This
   script only measures whether the recompiled runtime can hold real
   time (60 VBlanks/s) during course rendering + kart physics, which a
   menu-only or firmware-only measurement cannot answer: racing exercises
   a completely different code closure (3D course geometry, kart
   physics, AI, HUD) than everything Wiimmfi M0-M5 exercised.
2. `dispatch_misses.log` and Tier-3 interpreter coverage during that same
   gameplay window -- first-time-seen addresses here are expected and
   must be reported with counts/samples, never silently patched (that is
   coverage-seed + regen, i.e. title-side work, not something this script
   does).

Usage (against an already-running `--serve` instance -- this script does
not launch the process itself, matching every other oracle/*.py probe in
this repo):

    python oracle/mkds_gameplay_perf_probe.py --port 19842 \
        --actions ../mariokartdsrecomp-wiimmfi/scenarios/race_start.json \
        --measure-seconds 20 \
        --shots-dir ../mariokartdsrecomp-wiimmfi/generated/captures/wiimmfi-perf

The runner must have been launched with `--startup-mode automatic`
(reaches the MKDS title screen with no cart-icon tap needed, per
docs/wiimmfi-runbook.md section 7b) and, for the dispatch-miss check to
mean anything, from a working directory where `dispatch_misses.log` can
be observed afterward (relative to the runner's own CWD, per
runtime_arm.cpp).
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Any

from _client import DebugClient

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None


KEY_BITS = {
    "a": 0, "b": 1, "select": 2, "start": 3, "right": 4, "left": 5,
    "up": 6, "down": 7, "r": 8, "l": 9, "x": 10, "y": 11,
}
RELEASED_KEYS = 0x0FFF


def shoot(client: DebugClient, path: Path) -> None:
    if Image is None:
        return
    w, h, rgb_a = client.framebuffer("A")
    wb, hb, rgb_b = client.framebuffer("B")
    img = Image.new("RGB", (max(w, wb), h + hb))
    img.paste(Image.frombytes("RGB", (w, h), rgb_a), (0, 0))
    img.paste(Image.frombytes("RGB", (wb, hb), rgb_b), (0, h))
    path.parent.mkdir(parents=True, exist_ok=True)
    img.save(path)


def advance(client: DebugClient, frames: int, stall: int) -> dict[str, Any]:
    cur = client.cmd("event_counts")["vblank9"]
    target = cur + frames
    hit = client.cmd("run_to_event", event="vblank9", count=target, stall=stall)
    if hit.get("terminal") or hit.get("stalled"):
        raise RuntimeError(f"advance({frames}): execution stalled/halted at "
                            f"target vblank9={target}: {hit}")
    return hit


def press_key(client: DebugClient, key: str, hold: int, settle: int,
               stall: int) -> None:
    mask = RELEASED_KEYS & ~(1 << KEY_BITS[key])
    client.cmd("keys", mask=mask)
    advance(client, hold, stall)
    client.cmd("keys", mask=RELEASED_KEYS)
    advance(client, settle, stall)


def run_race_start_actions(client: DebugClient, actions: list[dict[str, Any]],
                            hold: int, settle: int, stall: int) -> None:
    for action in actions:
        kind = action["kind"]
        if kind == "key":
            press_key(client, action["key"], hold, 0, stall)
        elif kind == "wait":
            advance(client, action["frames"], stall)
        else:
            raise ValueError(f"unsupported action kind in race_start.json: {kind!r}")
    # One final settle so whatever the last input triggered has visibly
    # landed before the caller screenshots/measures.
    advance(client, settle, stall)


def measure_throughput(client: DebugClient, seconds: float, stall: int,
                        chunk_vblanks: int = 60) -> dict[str, Any]:
    """Advance the guest in `chunk_vblanks`-sized slices (never one giant
    jump, so a stall/halt is caught promptly) for at least `seconds` of
    WALL-CLOCK time, and report guest VBlanks actually produced per
    wall-clock second -- the number that answers "can this hold real
    time"."""
    start_counts = client.cmd("event_counts")
    start_vb = start_counts["vblank9"]
    t0 = time.perf_counter()
    target = start_vb
    iterations = 0
    while time.perf_counter() - t0 < seconds:
        target += chunk_vblanks
        hit = client.cmd("run_to_event", event="vblank9", count=target, stall=stall)
        if hit.get("terminal") or hit.get("stalled"):
            raise RuntimeError(f"measure_throughput: stalled/halted at "
                                f"vblank9 target={target}: {hit}")
        iterations += 1
    elapsed = time.perf_counter() - t0
    end_counts = client.cmd("event_counts")
    end_vb = end_counts["vblank9"]
    guest_vblanks = end_vb - start_vb
    return {
        "wall_seconds": elapsed,
        "guest_vblanks": guest_vblanks,
        "guest_seconds": guest_vblanks / 59.8261,  # NDS VBlank rate
        "vblanks_per_wall_second": guest_vblanks / elapsed,
        "realtime_ratio": (guest_vblanks / 59.8261) / elapsed,
        "iterations": iterations,
        "start_counts": start_counts,
        "end_counts": end_counts,
    }


def coverage_snapshot(client: DebugClient) -> dict[str, Any]:
    try:
        return client.cmd("static_coverage")
    except Exception as exc:  # pragma: no cover - diagnostic path
        return {"error": str(exc)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=19842)
    parser.add_argument("--actions", type=Path, required=True,
                         help="path to scenarios/race_start.json (or "
                              "equivalent) in the game worktree")
    parser.add_argument("--shots-dir", type=Path, required=True)
    parser.add_argument("--measure-seconds", type=float, default=20.0)
    parser.add_argument("--hold-frames", type=int, default=6)
    parser.add_argument("--settle-frames", type=int, default=6)
    parser.add_argument("--stall", type=int, default=300_000)
    parser.add_argument("--dispatch-misses-log", type=Path, default=None,
                         help="path to dispatch_misses.log to check "
                              "before/after (relative to the runner's own "
                              "CWD, per runtime_arm.cpp)")
    args = parser.parse_args()

    actions = json.loads(args.actions.read_text(encoding="utf-8"))["actions"]

    client = DebugClient(port=args.port, timeout=1800.0)
    try:
        client.cmd("reset")
        # game.toml's own default startup_mode=automatic reaches the title
        # screen with no cart-icon tap needed (docs/wiimmfi-runbook.md
        # section 7b) -- this script assumes the runner was launched with
        # --startup-mode automatic, so the very first VBlank wait already
        # lands past the boot presentation.
        advance(client, 900, args.stall)
        shoot(client, args.shots_dir / "00_title.png")

        coverage_before = coverage_snapshot(client)
        dispatch_log_existed_before = (
            args.dispatch_misses_log.exists()
            if args.dispatch_misses_log else None
        )

        run_race_start_actions(client, actions, args.hold_frames,
                                args.settle_frames, args.stall)
        shoot(client, args.shots_dir / "01_after_race_start_actions.png")
        after_actions_counts = client.cmd("event_counts")

        print("--- after race_start.json actions ---")
        print("event_counts:", json.dumps(after_actions_counts))

        print(f"\n--- measuring throughput for {args.measure_seconds}s "
              f"of wall-clock time ---")
        result = measure_throughput(client, args.measure_seconds, args.stall)
        shoot(client, args.shots_dir / "02_after_measure_window.png")
        print(json.dumps({k: v for k, v in result.items()
                           if k not in ("start_counts", "end_counts")},
                          indent=2))
        print("start_counts:", json.dumps(result["start_counts"]))
        print("end_counts:", json.dumps(result["end_counts"]))

        coverage_after = coverage_snapshot(client)
        print("\n--- Tier-3 / static coverage ---")
        print("before gameplay:", json.dumps(coverage_before))
        print("after gameplay: ", json.dumps(coverage_after))

        if args.dispatch_misses_log is not None:
            exists_after = args.dispatch_misses_log.exists()
            print(f"\n--- dispatch_misses.log ({args.dispatch_misses_log}) ---")
            print(f"existed before actions: {dispatch_log_existed_before}")
            print(f"exists after gameplay window: {exists_after}")
            if exists_after:
                text = args.dispatch_misses_log.read_text(
                    encoding="utf-8", errors="replace")
                lines = text.splitlines()
                print(f"line count: {len(lines)}")
                print("first 20 lines:")
                for line in lines[:20]:
                    print("  " + line)
                if len(lines) > 20:
                    print(f"  ... ({len(lines) - 20} more lines)")
    finally:
        client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
