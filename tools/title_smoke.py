#!/usr/bin/env python3
"""Attract-and-fuzz smoke test for any title, driven over the TCP debug server.

Regression check for framework changes: boot a title, let its attract sequence
play, fuzz some input at it, and report whether anything broke. Title-agnostic
-- point it at a runner built for whichever game.

What counts as a failure, in order of severity:
  * the machine halted (terminal), which is how a dispatch miss or an
    unimplemented op surfaces
  * dispatch_misses.log gained entries (a silent game-breaking bug per
    CLAUDE.md's build loop)
  * every captured frame was black, i.e. it booted but never presented
  * the debug server stopped answering

Screenshots are written for every checkpoint so a human can see what the run
actually looked like rather than trusting a pass/fail.
"""

from __future__ import annotations

import argparse
import json
import random
import socket
import subprocess
import sys
import time
from pathlib import Path

KEY_BITS = {"a": 0, "b": 1, "select": 2, "start": 3, "right": 4, "left": 5,
            "up": 6, "down": 7, "r": 8, "l": 9, "x": 10, "y": 11}
# Active-low; the server default of 0x3FF would leave X and Y held down.
RELEASED = 0x0FFF


class DebugClient:
    def __init__(self, port: int, timeout: float = 1800.0) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.buf = b""

    def cmd(self, name: str, **args: object) -> dict:
        args["cmd"] = name
        self.sock.sendall((json.dumps(args) + "\n").encode())
        while b"\n" not in self.buf:
            chunk = self.sock.recv(1 << 16)
            if not chunk:
                raise RuntimeError("debug server closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        reply = json.loads(line)
        if isinstance(reply, dict) and "error" in reply:
            raise RuntimeError(f"{name}: {reply['error']}")
        return reply

    def vblank(self) -> int:
        return int(self.cmd("event_counts")["vblank9"])

    def advance_to(self, target: int) -> None:
        # run_to_event caps at max_rounds and can return part way with
        # exhausted=True; resume until the target is actually reached.
        for _ in range(400):
            reply = self.cmd("run_to_event", event="vblank9", count=target,
                             max_rounds=50_000_000)
            if reply.get("terminal"):
                raise RuntimeError(
                    f"HALTED: arm9={reply.get('reason9')!r} "
                    f"arm7={reply.get('reason7')!r}")
            if reply.get("stalled"):
                raise RuntimeError(f"stalled before vblank9 {target}")
            if self.vblank() >= target:
                return
        raise RuntimeError(f"could not reach vblank9 {target}")

    def advance(self, frames: int) -> None:
        self.advance_to(self.vblank() + frames)

    def shot(self, path: Path) -> float:
        """Save both screens stacked; return the mean luminance of the frame."""
        try:
            from PIL import Image
        except ImportError:
            return -1.0
        frames = []
        for engine in ("A", "B"):
            fb = self.cmd("framebuffer", engine=engine)
            raw = bytes.fromhex(fb["rgb"])
            frames.append(Image.frombytes("RGB", (fb["w"], fb["h"]), raw))
        combined = Image.new("RGB", (frames[0].width,
                                     frames[0].height + frames[1].height))
        combined.paste(frames[0], (0, 0))
        combined.paste(frames[1], (0, frames[0].height))
        combined.save(path)
        grey = combined.convert("L").resize((32, 48))
        return sum(grey.getdata()) / (32 * 48)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--runner", type=Path, required=True)
    ap.add_argument("--bios", type=Path, required=True)
    ap.add_argument("--rom", type=Path, required=True)
    ap.add_argument("--config", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--port", type=int, default=19960)
    ap.add_argument("--label", default="title")
    ap.add_argument("--attract-vblanks", type=int, default=9000)
    ap.add_argument("--checkpoint-every", type=int, default=1500)
    ap.add_argument("--fuzz-steps", type=int, default=40)
    ap.add_argument("--settle", type=int, default=60)
    ap.add_argument("--seed", type=int, default=0x5A17)
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)
    misses = args.runner.parent / "dispatch_misses.log"
    misses_before = misses.stat().st_size if misses.exists() else 0

    cmd = [str(args.runner), str(args.bios), "--serve", "--port", str(args.port),
           "--rom", str(args.rom.resolve()), "--no-save",
           "--startup-mode", "automatic"]
    if args.config:
        cmd += ["--config", str(args.config.resolve())]
    proc = subprocess.Popen(cmd, cwd=str(args.runner.parent),
                            stdout=(args.out / "runner.stdout.log").open("wb"),
                            stderr=(args.out / "runner.stderr.log").open("wb"))

    report: dict = {"label": args.label, "runner": str(args.runner),
                    "rom": args.rom.name, "checkpoints": [], "failures": []}
    luminance: list[float] = []
    try:
        client = None
        for _ in range(240):
            if proc.poll() is not None:
                raise SystemExit(f"runner exited early rc={proc.returncode}")
            try:
                client = DebugClient(args.port)
                break
            except OSError:
                time.sleep(0.5)
        if client is None:
            raise RuntimeError("debug server never came up")
        client.cmd("reset")

        # Attract: let the title run untouched and sample it.
        target = 0
        while target < args.attract_vblanks:
            target = min(target + args.checkpoint_every, args.attract_vblanks)
            client.advance_to(target)
            lum = client.shot(args.out / f"attract-{target:06d}.png")
            luminance.append(lum)
            report["checkpoints"].append(
                {"phase": "attract", "vblank": client.vblank(), "luminance": lum})
            print(f"  attract vblank {client.vblank():>6}  luminance {lum:6.1f}")

        # Fuzz: random taps and button presses.
        for step in range(1, args.fuzz_steps + 1):
            roll = rng.random()
            if roll < 0.45:
                x, y = rng.randrange(0, 256), rng.randrange(0, 192)
                client.cmd("touch", x=x, y=y, down=True)
                client.advance(3)
                client.cmd("touch", x=0, y=0, down=False)
                label = f"touch-{x}-{y}"
            elif roll < 0.9:
                key = rng.choice(list(KEY_BITS))
                client.cmd("keys", mask=RELEASED & ~(1 << KEY_BITS[key]))
                client.advance(3)
                client.cmd("keys", mask=RELEASED)
                label = f"key-{key}"
            else:
                label = "idle"
            client.advance(args.settle)
            if step % 5 == 0 or step == args.fuzz_steps:
                lum = client.shot(args.out / f"fuzz-{step:03d}.png")
                luminance.append(lum)
                report["checkpoints"].append(
                    {"phase": "fuzz", "step": step, "action": label,
                     "vblank": client.vblank(), "luminance": lum})
                print(f"  fuzz {step:>3}/{args.fuzz_steps} {label:<16} "
                      f"vblank {client.vblank():>6}  luminance {lum:6.1f}")

        report["static_coverage"] = client.cmd("static_coverage")
        report["dispatch_stats"] = client.cmd("dispatch_stats")
        report["final_vblank"] = client.vblank()
        # Optional: a pre-change runner has no coverage_manifest command, and
        # this harness has to run against those to establish a baseline. Its
        # absence is information, not a failure.
        manifest = (args.out / "coverage.json").resolve()
        try:
            report["coverage_manifest"] = client.cmd(
                "coverage_manifest", path=manifest.as_posix())
        except RuntimeError as exc:
            report["coverage_manifest"] = {"unavailable": str(exc)}
    except Exception as exc:  # noqa: BLE001 - the failure IS the result
        report["failures"].append(str(exc))
        print(f"  FAILURE: {exc}")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()

    misses_after = misses.stat().st_size if misses.exists() else 0
    if misses_after > misses_before:
        report["failures"].append(
            f"dispatch_misses.log grew by {misses_after - misses_before} bytes")
    lit = [l for l in luminance if l > 2.0]
    if luminance and not lit:
        report["failures"].append("every captured frame was black")
    report["frames_captured"] = len(luminance)
    report["frames_non_black"] = len(lit)
    report["passed"] = not report["failures"]

    (args.out / "report.json").write_text(json.dumps(report, indent=2),
                                          encoding="utf-8", newline="\n")
    print(f"\n{args.label}: {'PASS' if report['passed'] else 'FAIL'}  "
          f"({report['frames_non_black']}/{report['frames_captured']} frames lit)")
    for f in report["failures"]:
        print(f"  ! {f}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
