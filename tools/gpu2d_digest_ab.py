"""Byte-lock two runner configurations against the presented-frame digest ring.

The compositor threading work (beads-yjp.70 phase 2C) moves 2D line rendering
and the adaptive presentation composite onto worker threads. The claim that has
to be proved is that this changes only WHERE the work runs, never WHAT is
presented. This script proves it the only way that survives a shared, noisy
host: it launches the SAME binary twice with different threading environment,
loads the same pinned savestate in each, and compares the always-on
presented-frame digest ring (frontend.h) frame for frame.

Alignment is by digest EPOCH, not by frame index: two independently launched
processes present a different number of frames before the savestate load lands,
so the load -- which bumps the epoch -- is the only shared origin. Within an
epoch the guest runs from identical state with no host input, so digest N after
the load must match digest N after the load in the other leg.

Nothing here arms a capture. The ring records from process start (with
NDS_FRAME_HASH=1 in the environment); this only queries a window out of it.

Usage:
  python gpu2d_digest_ab.py --exe PATH [--frames 300] [--slot 1]
      [--leg LABEL=KEY=VAL,KEY=VAL] ...
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

PINNED_SAVESTATES = Path(
    r"F:\Projects\ndsrecomp\_kanden-fresh\savestates"
    r"\90164d1ac127ee5f9815ea4ae7de798c7b5fc629"
)
DEFAULT_CWD = Path(r"F:\Projects\ndsrecomp\_kanden-fresh")
DEFAULT_ROM = Path(
    r"F:\Projects\ndsrecomp\metroidprimehuntersrecomp\Metroid Prime Hunters.nds"
)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--exe", required=True, type=Path)
    p.add_argument("--out-root", type=Path,
                   default=Path(r"F:\Projects\ndsrecomp\_kanden-legs\runs\digest"))
    p.add_argument("--savestates", type=Path, default=PINNED_SAVESTATES)
    p.add_argument("--cwd", type=Path, default=DEFAULT_CWD)
    p.add_argument("--rom", type=Path, default=DEFAULT_ROM)
    p.add_argument("--base-port", type=int, default=27880)
    p.add_argument("--frames", type=int, default=300)
    p.add_argument("--slot", type=int, default=1)
    p.add_argument("--save-slot", type=int, default=0,
                   help="Slot to save into after --save-after frames (0 = skip)")
    p.add_argument("--save-after", type=int, default=600)
    p.add_argument("--build-id", default="ae9caf6-dirty")
    p.add_argument("--leg", action="append", default=[],
                   help="LABEL=K=V,K=V environment overlay; repeatable")
    p.add_argument("--timeout-sec", type=float, default=420.0)
    return p.parse_args()


def parse_legs(specs: list[str]) -> list[tuple[str, dict[str, str]]]:
    legs: list[tuple[str, dict[str, str]]] = []
    for spec in specs:
        label, _, rest = spec.partition("=")
        env: dict[str, str] = {}
        if rest:
            for item in rest.split(","):
                if not item:
                    continue
                k, _, v = item.partition("=")
                env[k] = v
        legs.append((label, env))
    return legs


def is_port_free(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind(("127.0.0.1", port))
        except OSError:
            return False
    return True


class Client:
    def __init__(self, port: int, timeout_sec: float) -> None:
        deadline = time.monotonic() + timeout_sec
        last: OSError | None = None
        while time.monotonic() < deadline:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), 2.0)
                self.s.settimeout(30.0)
                self.f = self.s.makefile("rwb")
                return
            except OSError as exc:
                last = exc
                time.sleep(0.25)
        raise TimeoutError(f"port {port}: {last}")

    def cmd(self, name: str, **kw: Any) -> dict[str, Any]:
        payload = dict(kw)
        payload["cmd"] = name
        self.f.write((json.dumps(payload) + "\n").encode())
        self.f.flush()
        line = self.f.readline()
        if not line:
            raise ConnectionError("runner closed the debug connection")
        return json.loads(line.decode("utf-8", errors="replace"))

    def key(self, name: str, shift: bool = False) -> None:
        self.cmd("frontend_input", action="key", key=name, down=True,
                 shift=shift)
        time.sleep(0.05)
        self.cmd("frontend_input", action="key", key=name, down=False,
                 shift=shift)

    def close(self) -> None:
        try:
            self.f.close()
            self.s.close()
        except OSError:
            pass


def diagnostics_records(diag: Path) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for path in sorted(diag.glob("performance-*.jsonl")):
        with path.open("r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    out.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
    return out


def load_reported(diag: Path, slot: int) -> bool:
    for r in diagnostics_records(diag):
        if (r.get("kind") == "event" and r.get("event") == "savestate_load"
                and int(r.get("slot", -1)) == slot and bool(r.get("ok"))):
            return True
    return False


def fetch_digests(client: Client, start: int, count: int) -> list[dict[str, Any]]:
    got: list[dict[str, Any]] = []
    while len(got) < count:
        chunk = client.cmd("frame_digests", **{"from": start + len(got),
                                               "count": min(512, count - len(got))})
        entries = chunk.get("digests") or []
        if not entries:
            break
        got.extend(entries)
    return got


def find_epoch_start(client: Client, epoch: int, hint: int) -> int | None:
    """First ring index whose entry has the given epoch, searching around hint."""
    lo = max(0, hint - 600)
    window = fetch_digests(client, lo, 1200)
    for i, entry in enumerate(window):
        if int(entry.get("epoch", 0)) >= epoch:
            return lo + i
    return None


def run_leg(args: argparse.Namespace, label: str, env_overlay: dict[str, str],
            port: int) -> dict[str, Any]:
    run_dir = args.out_root / label
    if run_dir.exists():
        shutil.rmtree(run_dir)
    diag = run_dir / "diagnostics"
    cache = run_dir / "live-overlay-cache"
    states = run_dir / "savestates"
    diag.mkdir(parents=True)
    cache.mkdir(parents=True)
    shutil.copytree(args.savestates, states / args.savestates.name)

    cmd = [
        str(args.exe.resolve()),
        "--interactive", "--port", str(port),
        "--live-overlay-cache", str(cache),
        "--live-overlay-activation-delay-ms", "600000",
        "--diagnostics", "on", "--diagnostics-dir", str(diag),
        "--savestate-dir", str(states),
        str(args.cwd / "bios"),
        "--config", str(args.cwd / "game.toml"),
        "--rom", str(args.rom),
        "--boot", "direct", "--freebios", "--generated-firmware",
        "--internal-resolution", "2", "--antialiasing", "8",
        "--texture-upscale", "2",
        # The governor changes host-side stages over time; a digest comparison
        # needs both legs on the same, fixed configuration.
        "--performance-governor", "off",
    ]
    env = os.environ.copy()
    env["NDS_SAVESTATE_BUILD_ID"] = args.build_id
    env["NDS_FRAME_HASH"] = "1"
    env.update(env_overlay)

    result: dict[str, Any] = {"label": label, "port": port,
                              "env": env_overlay, "run_dir": str(run_dir)}
    proc: subprocess.Popen[Any] | None = None
    client: Client | None = None
    try:
        with (run_dir / "stdout.log").open("wb") as out, \
             (run_dir / "stderr.log").open("wb") as err:
            proc = subprocess.Popen(cmd, cwd=str(args.cwd), env=env,
                                    stdout=out, stderr=err)
            client = Client(port, 60.0)
            # Press the bare load key until diagnostics confirms the load. Bare
            # F<slot> is LOAD; Shift+F<slot> would SAVE over the pinned state.
            deadline = time.monotonic() + 120.0
            while time.monotonic() < deadline and not load_reported(diag, args.slot):
                client.key(f"F{args.slot}")
                time.sleep(0.5)
            result["load_ok"] = load_reported(diag, args.slot)
            if not result["load_ok"]:
                result["error"] = "savestate load never reported"
                return result
            status = client.cmd("frame_digests", **{"from": 0, "count": 1})
            result["ring_enabled"] = bool(status.get("enabled"))
            count_at_load = int(status.get("count", 0))
            start = find_epoch_start(client, 1, count_at_load)
            if start is None:
                result["error"] = "no post-load digest epoch found"
                return result
            result["epoch_start"] = start
            need = start + max(args.frames, args.save_after
                               if args.save_slot else 0) + 8
            deadline = time.monotonic() + args.timeout_sec
            while time.monotonic() < deadline:
                cur = int(client.cmd("frame_digests",
                                     **{"from": 0, "count": 1}).get("count", 0))
                if cur >= need:
                    break
                time.sleep(1.0)
            result["digests"] = fetch_digests(client, start, args.frames)
            if args.save_slot:
                client.key(f"F{args.save_slot}", shift=True)
                time.sleep(2.0)
                saved = sorted((states / args.savestates.name).glob(
                    f"slot{args.save_slot:02d}*"))
                result["saved_files"] = [str(p) for p in saved]
            client.cmd("frontend_exit")
    except Exception as exc:  # keep artifacts inspectable
        result["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        if client is not None:
            client.close()
        if proc is not None:
            try:
                proc.wait(timeout=30.0)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10.0)
            result["returncode"] = proc.returncode
    return result


def compare(legs: list[dict[str, Any]]) -> int:
    base = legs[0]
    rc = 0
    for other in legs[1:]:
        a = base.get("digests") or []
        b = other.get("digests") or []
        n = min(len(a), len(b))
        mismatches = []
        for i in range(n):
            keys = ("top", "bottom", "hd", "tw", "bw", "flags")
            if any(a[i].get(k) != b[i].get(k) for k in keys):
                mismatches.append((i, a[i], b[i]))
        print(f"[{base['label']} vs {other['label']}] compared {n} frames, "
              f"{len(mismatches)} mismatches")
        for i, x, y in mismatches[:10]:
            print(f"  #{i}: {x} != {y}")
        if not n or mismatches:
            rc = 1
    return rc


def main() -> int:
    args = parse_args()
    legs = parse_legs(args.leg) or [
        ("serial", {"NDS_GPU2D_THREADED": "0",
                    "NDS_GPU2D_ADAPTIVE_WORKERS": "0"}),
        ("threaded", {}),
    ]
    args.out_root.mkdir(parents=True, exist_ok=True)
    results = []
    port = args.base_port
    for label, env in legs:
        while not is_port_free(port):
            port += 1
        print(f"--- leg {label} on port {port}: {env}", flush=True)
        results.append(run_leg(args, label, env, port))
        port += 1
        print(json.dumps({k: v for k, v in results[-1].items()
                          if k != "digests"}, indent=2), flush=True)
    (args.out_root / "digest-ab.json").write_text(
        json.dumps(results, indent=2), encoding="utf-8")
    for r in results:
        if r.get("error"):
            print(f"leg {r['label']} failed: {r['error']}")
            return 2
    return compare(results)


if __name__ == "__main__":
    sys.exit(main())
