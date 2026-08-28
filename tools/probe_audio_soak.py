"""Audio-stream byte-lock: fingerprint the SPU output over an attract soak.

Two legs of the SAME binary differing only in one 0/1 runtime env selector are
advanced to the SAME guest event (run_to_event on vblank9), and every PCM frame
the SPU produced along the way is folded into one hash. If a selector perturbs
scheduler pacing, sample generation, or mixing, the fingerprints diverge.

The SPU debug ring is always on and retains ~1M stereo frames (~32 s at
32768 Hz), so this probe CONSUMES the ring rather than arming a capture: it
drains everything produced since its cursor at each checkpoint and never asks
the runner to start or stop recording. Draining every --drain-frames guest
frames keeps the cursor well inside the retention window; the probe FAILS
rather than silently skipping if the ring ever outran it (start no longer
retained).

The witness: a leg pair that produced ZERO audio frames proves nothing, so a
run in which either leg produced no samples is reported INVALID, not PASS.

Usage:
  py -3 tools/probe_audio_soak.py --exe <nds_runner.exe> --bios <dir>
      --rom <rom> --config <game.toml> --toggle-env NDS_DIRECT_LINK
      --frames 4242 --port 19990 --output perf-results/audio-soak
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import socket
import subprocess
import sys
import time


class Client:
    def __init__(self, port: int, timeout: float = 3600.0):
        deadline = time.time() + 120.0
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), 5.0)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.2)
        self.s.settimeout(timeout)
        self.f = self.s.makefile("rwb")

    def cmd(self, name: str, **kw):
        payload = dict(kw)
        payload["cmd"] = name
        self.f.write((json.dumps(payload) + "\n").encode())
        self.f.flush()
        return json.loads(self.f.readline().decode())

    def close(self):
        try:
            self.f.close()
            self.s.close()
        except OSError:
            pass


def drain(client: Client, cursor: int, digest) -> int:
    """Fold every retained PCM frame from `cursor` forward into `digest`."""
    while True:
        reply = client.cmd("audio_samples", start=cursor, count=4096)
        if "error" in reply:
            # The ring wrapped past our cursor: the fingerprint would silently
            # skip samples, so refuse rather than hash a hole.
            raise SystemExit(
                f"audio ring outran the probe at frame {cursor}: "
                f"{reply['error']} (lower --drain-frames)")
        copied = int(reply.get("count", 0))
        if copied == 0:
            return cursor
        digest.update(reply["pcm_s16le"].encode("ascii"))
        cursor += copied


def run_leg(args, enabled: bool, port: int) -> dict:
    env = dict(os.environ)
    env[args.toggle_env] = "1" if enabled else "0"
    cmd = [str(args.exe), str(args.bios), "--serve", "--port", str(port),
           "--boot", args.boot, "--no-save"]
    if args.rom:
        cmd += ["--rom", str(args.rom)]
    if args.config:
        cmd += ["--config", str(args.config)]
    cmd += list(args.runner_arg)

    args.output.mkdir(parents=True, exist_ok=True)
    log = args.output / f"leg-{'on' if enabled else 'off'}.log"
    digest = hashlib.sha256()
    cursor = 0
    produced = 0
    with open(log, "wb") as handle:
        proc = subprocess.Popen(cmd, stdout=handle, stderr=subprocess.STDOUT,
                                env=env)
        try:
            client = Client(port, args.timeout)
            frame = 0
            while frame < args.frames:
                frame = min(frame + args.drain_frames, args.frames)
                reply = client.cmd("run_to_event", event="vblank9",
                                   count=frame, max_rounds=100000000)
                if not reply.get("reached", False):
                    raise SystemExit(f"vblank9={frame} not reached: {reply}")
                cursor = drain(client, cursor, digest)
                produced = int(
                    client.cmd("audio_samples", start=max(cursor - 1, 0),
                               count=1).get("produced", 0))
            counts = client.cmd("event_counts")
            client.close()
        finally:
            proc.terminate()
            try:
                proc.wait(30)
            except subprocess.TimeoutExpired:
                proc.kill()
    return {
        "enabled": enabled,
        "pcm_sha256": digest.hexdigest(),
        "pcm_frames_hashed": cursor,
        "pcm_frames_produced": produced,
        "event_counts": counts,
    }


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--exe", type=pathlib.Path, required=True)
    p.add_argument("--bios", type=pathlib.Path, required=True)
    p.add_argument("--rom", type=pathlib.Path, default=None)
    p.add_argument("--config", type=pathlib.Path, default=None)
    p.add_argument("--boot", default="direct", choices=("direct", "lle"))
    p.add_argument("--toggle-env", "--selector-env", dest="toggle_env",
                   required=True)
    p.add_argument("--frames", type=int, default=4242)
    p.add_argument("--drain-frames", type=int, default=600,
                   help="guest frames between ring drains; must keep the "
                        "cursor inside the ~32 s SPU retention window")
    p.add_argument("--port", type=int, default=19990)
    p.add_argument("--timeout", type=float, default=3600.0)
    p.add_argument("--runner-arg", action="append", default=[])
    p.add_argument("--output", type=pathlib.Path, required=True)
    args = p.parse_args()

    print(f"leg A: {args.toggle_env}=0 (control)")
    off = run_leg(args, False, args.port)
    print(f"  pcm sha256 {off['pcm_sha256']}  frames {off['pcm_frames_hashed']}")
    print(f"leg B: {args.toggle_env}=1 (feature enabled)")
    on = run_leg(args, True, args.port + 1)
    print(f"  pcm sha256 {on['pcm_sha256']}  frames {on['pcm_frames_hashed']}")

    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "report.json").write_text(json.dumps(
        {"exe": str(args.exe), "toggle_env": args.toggle_env,
         "frames": args.frames, "off": off, "on": on}, indent=2))

    ok = True
    # Witness first: a silent run would make any comparison vacuous.
    if off["pcm_frames_hashed"] == 0 or on["pcm_frames_hashed"] == 0:
        print("INVALID: a leg produced no audio; the comparison proves nothing")
        ok = False
    if off["pcm_frames_hashed"] != on["pcm_frames_hashed"]:
        print(f"MISMATCH: frames hashed {off['pcm_frames_hashed']} vs "
              f"{on['pcm_frames_hashed']}")
        ok = False
    if off["pcm_sha256"] != on["pcm_sha256"]:
        print("MISMATCH: PCM fingerprint differs")
        ok = False
    for key in sorted(set(off["event_counts"]) | set(on["event_counts"])):
        a = off["event_counts"].get(key)
        b = on["event_counts"].get(key)
        if a != b:
            print(f"MISMATCH: event {key} off={a} on={b}")
            ok = False

    print("AUDIO SOAK PASS" if ok else "AUDIO SOAK FAIL")
    print(f"report: {args.output / 'report.json'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
