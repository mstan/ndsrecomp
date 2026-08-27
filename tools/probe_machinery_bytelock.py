#!/usr/bin/env python3
"""Instruction-anchored byte-lock between the two NDS_CYCLE_FAST_LIMIT legs.

WHY THIS EXISTS RATHER THAN THE ROUTE HARNESS
---------------------------------------------
The obvious correctness gate for the deadline-bounded machinery
(beads-yjp.42 phase 1) was "run the MPH adventure route in both selector
states and require an identical framebuffer SHA pair". Measured on this
host, that gate does not discriminate: the route is navigated through the
live frontend at real time, so the same binary in the same selector state
produces DIFFERENT final framebuffers and different total insn9 from run to
run. Four runs of `measure_mph_scenario.py --route adventure` (2 with the
deadline on, 2 with it forced off) produced three different total insn9
counts and two different top-screen digests, with the SAME digest appearing
in both legs. A gate that its own control fails cannot certify anything.

The nondeterminism is in the harness, not the candidate: it is present with
the deadline forced OFF, where the runtime is byte-for-byte the pre-change
faithful path.

So this probe replaces the wall-clock anchor with a GUEST anchor. Both legs
are driven through the debug server with `run_to_event insn9 N`, which stops
at the Nth retired ARM9 instruction exactly. Identical guest work in, so any
difference out is the candidate. That is the same discipline as the G3
byte-lock in docs/host_optimization_strategy.md, applied between two
selector states of one binary instead of against the oracle.

At every stop it captures:
  * both framebuffers, SHA-256 (the visible result)
  * both CPUs' full register file + CPSR + SPSR + mode (the architectural
    state the cycle model steers)
  * the whole event_counts block (insn9/insn7/vblank/IRQ/IPC ordinals --
    the cross-CPU sync evidence the dual-CPU rule cares about)
  * dispatch_stats.fast_limit_publishes, so a leg that silently published no
    deadline cannot masquerade as a pass.

Usage:
  py -3 tools/probe_machinery_bytelock.py --start 100000000 --step 100000000 \
      --count 7
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

TOOLS = pathlib.Path(__file__).resolve().parent
ROOT = TOOLS.parent
WORKSPACE = ROOT.parent


class Client:
    def __init__(self, port: int, timeout: float = 1800.0) -> None:
        deadline = time.time() + 120.0
        last = None
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), 5.0)
                break
            except OSError as exc:                      # server not up yet
                last = exc
                time.sleep(0.25)
        else:
            raise SystemExit(f"could not connect to port {port}: {last}")
        self.sock.settimeout(timeout)
        self.buf = b""

    def cmd(self, name: str, **kwargs) -> dict:
        payload = {"cmd": name}
        payload.update(kwargs)
        self.sock.sendall((json.dumps(payload) + "\n").encode())
        while b"\n" not in self.buf:
            chunk = self.sock.recv(1 << 20)
            if not chunk:
                raise SystemExit(f"debug server closed during {name}")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line.decode())

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def digest(obj) -> str:
    return hashlib.sha256(
        json.dumps(obj, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()


def capture(client: Client) -> dict:
    out = {}
    for engine in ("A", "B"):
        fb = client.cmd("framebuffer", engine=engine)
        # The reply carries the raw pixels; hash them rather than keeping
        # 147 KB of hex per stop.
        pixels = fb.get("rgb") or fb.get("pixels") or fb.get("data")
        out[f"fb_{engine}"] = (hashlib.sha256(pixels.encode()).hexdigest()
                               if isinstance(pixels, str) else digest(fb))
    out["regs9"] = client.cmd("regs", cpu=9)
    out["regs7"] = client.cmd("regs", cpu=7)
    out["events"] = client.cmd("event_counts")
    stats = client.cmd("dispatch_stats")
    out["fast_limit_publishes"] = stats.get("fast_limit_publishes")
    out["cycle_fast_limit"] = stats.get("cycle_fast_limit")
    return out


def run_leg(args, enabled: bool, port: int) -> list[dict]:
    env = dict(os.environ)
    # The selector under test is an env var so both legs are the SAME binary.
    # Default is the cycle-fast-limit selector; --selector-env retargets it at
    # any other 0/1 runner toggle (e.g. NDS_GPU2D_THREADED, NDS_3D_THREADED)
    # without forking the probe.
    env[args.selector_env] = "1" if enabled else "0"
    # A byte-lock must not race an audio device or a window: serve mode is
    # headless and steps only when a run_to_event is in flight.
    cmd = [
        str(args.exe), str(args.bios), "--serve", "--port", str(port),
        "--boot", args.boot, "--no-save",
    ]
    # A firmware-only byte-lock (framework runner, no title banks) runs with
    # no cartridge inserted at all.
    if not args.no_rom:
        cmd += ["--rom", str(args.rom)]
    if args.config:
        cmd += ["--config", str(args.config)]
    # --force-tier3 makes this the byte-lock for the Tier-3 half of the
    # deadline: every non-BIOS instruction is interpreted, so the loop's
    # deadline-bounded exit polls are what is being compared.
    cmd += list(args.runner_arg)
    log = args.output / f"leg-{'on' if enabled else 'off'}.log"
    args.output.mkdir(parents=True, exist_ok=True)
    with open(log, "wb") as handle:
        proc = subprocess.Popen(cmd, stdout=handle, stderr=subprocess.STDOUT,
                                env=env)
        try:
            client = Client(port, args.timeout)
            stops = []
            for i in range(args.count):
                target = args.start + i * args.step
                reply = client.cmd("run_to_event", event="insn9",
                                   count=target, max_rounds=100000000)
                if not reply.get("reached", False):
                    print(f"  stop {target}: NOT REACHED {reply}")
                    break
                snap = capture(client)
                snap["stop"] = target
                stops.append(snap)
                print(f"  stop {target}: fbA={snap['fb_A'][:16]} "
                      f"fbB={snap['fb_B'][:16]} "
                      f"insn9={snap['events'].get('insn9')} "
                      f"selector={snap['cycle_fast_limit']} "
                      f"publishes={snap['fast_limit_publishes']}")
            client.close()
        finally:
            proc.terminate()
            try:
                proc.wait(30)
            except subprocess.TimeoutExpired:
                proc.kill()
    return stops


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--exe", type=pathlib.Path,
                   default=ROOT / "runner/build-interp-perf/nds_runner.exe")
    p.add_argument("--bios", type=pathlib.Path,
                   default=WORKSPACE / "ndsrecomp" / "bios")
    p.add_argument("--rom", type=pathlib.Path,
                   default=WORKSPACE / "metroidprimehuntersrecomp" /
                   "Metroid Prime Hunters.nds")
    p.add_argument("--config", type=pathlib.Path, default=None)
    p.add_argument("--boot", default="direct", choices=("direct", "lle"))
    p.add_argument("--no-rom", action="store_true",
                   help="run with no cartridge (firmware-only byte-lock)")
    p.add_argument("--selector-env", default="NDS_CYCLE_FAST_LIMIT",
                   help="name of the 0/1 env toggle the two legs differ on")
    p.add_argument("--port", type=int, default=19950)
    p.add_argument("--start", type=int, default=100_000_000)
    p.add_argument("--step", type=int, default=100_000_000)
    p.add_argument("--count", type=int, default=7)
    p.add_argument("--timeout", type=float, default=3600.0)
    p.add_argument("--runner-arg", action="append", default=[],
                   help="extra nds_runner argument (e.g. --force-tier3)")
    p.add_argument("--output", type=pathlib.Path,
                   default=ROOT / "perf-results" / "machinery-bytelock")
    args = p.parse_args()

    print(f"leg A: {args.selector_env}=0 (faithful, forced)")
    off = run_leg(args, False, args.port)
    print(f"leg B: {args.selector_env}=1 (selector engaged)")
    on = run_leg(args, True, args.port + 1)

    report = {"exe": str(args.exe),
              "selector_env": args.selector_env,
              "exe_sha256": hashlib.sha256(args.exe.read_bytes()).hexdigest(),
              "stops_off": off, "stops_on": on}
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "report.json").write_text(json.dumps(report, indent=2))

    # Compare. Every stop must match on every captured field.
    ok = True
    if len(off) != len(on):
        print(f"MISMATCH: leg lengths {len(off)} vs {len(on)}")
        ok = False
    for a, b in zip(off, on):
        for key in ("fb_A", "fb_B", "regs9", "regs7", "events"):
            if a[key] != b[key]:
                ok = False
                print(f"MISMATCH at insn9={a['stop']} field {key}")
                if key in ("regs9", "regs7", "events"):
                    for k in sorted(set(a[key]) | set(b[key])):
                        if a[key].get(k) != b[key].get(k):
                            print(f"    {k}: off={a[key].get(k)} "
                                  f"on={b[key].get(k)}")
    # The witness: a leg claiming the deadline is on must have published one.
    # This is specific to the cycle-fast-limit selector; when the probe is
    # retargeted at another toggle the witness lives in that toggle's own
    # counters (e.g. profile.gpu2d.threaded_lines) and is checked separately.
    if args.selector_env == "NDS_CYCLE_FAST_LIMIT":
        if on and not any(s["fast_limit_publishes"] for s in on):
            print("INVALID: the enabled leg never published a deadline; "
                  "the selector state proves nothing")
            ok = False
        if off and any(s["fast_limit_publishes"] for s in off):
            print("INVALID: the faithful leg published a deadline")
            ok = False
    else:
        print(f"witness for {args.selector_env} is not the deadline counter; "
              f"verify it in that toggle's own profile counters")

    print("BYTE-LOCK PASS" if ok else "BYTE-LOCK FAIL")
    print(f"report: {args.output / 'report.json'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
