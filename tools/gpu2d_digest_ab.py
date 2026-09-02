"""Byte-lock two runner configurations against the presented-frame digest ring.

The compositor threading work (beads-yjp.70 phase 2C, default flipped on in
70e9f3f) moves 2D line rendering and the adaptive presentation composite onto
worker threads. The claim that has to be proved is that this changes only WHERE
the work runs, never WHAT is presented. This script proves it the only way that
survives a shared, noisy host: it launches the SAME binary twice with different
threading environment, brings each leg to the same start, and compares the
always-on presented-frame digest ring (frontend.h) frame for frame.

Alignment is by digest EPOCH when a savestate is loaded, not by frame index:
two independently launched processes present a different number of frames
before the load lands, so the load -- which bumps the epoch -- is the only
shared origin. Within an epoch the guest runs from identical state with no host
input, so digest N after the load must match digest N after the load in the
other leg. With --slot 0 there is no load; both legs are compared from ring
index 0, which is only sound for a deterministic reset boot (do not pass
--rtc-host in that mode).

Nothing here arms a capture. The ring records from process start (with
NDS_FRAME_HASH=1 in the environment); this only queries a window out of it.
The gpu2d fence/band counters and the frontend phase timers are likewise
cumulative always-on counters, sampled at the window edges and differenced.

This driver is title-agnostic. --title selects a preset (rom/config/bios/cwd/
runner args/savestate dir); every part of a preset can be overridden, and
--runner-arg appends. Presets live in TITLES below.

Usage examples:

  # MPH Kanden fight from the pinned slot 1 (the beads-yjp.70 measurement)
  python gpu2d_digest_ab.py --title mph --exe PATH --frames 300 --slot 1

  # Mario Kart DS: make a pinned savestate first, then A/B from it
  python gpu2d_digest_ab.py --title mkds --exe PATH --prep-slot 1 \
      --prep-frames 1200
  python gpu2d_digest_ab.py --title mkds --exe PATH --slot 1 --frames 300 \
      --save-slot 2 --save-after 600
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

ROOT = Path(r"F:\Projects\ndsrecomp")

# Per-title launch recipe. Everything here is overridable on the command line.
#   rom/config/bios : passed to the runner
#   cwd             : working directory (relative paths in a config resolve here)
#   savestates      : directory that holds the pinned <rom-sha1>/ state dir;
#                     copied per leg so a leg can never write the pinned one
#   args            : title-specific runner arguments (presentation mode etc.)
TITLES: dict[str, dict[str, Any]] = {
    "mph": {
        "rom": ROOT / "metroidprimehuntersrecomp" / "Metroid Prime Hunters.nds",
        "cwd": ROOT / "_kanden-fresh",
        "bios": ROOT / "_kanden-fresh" / "bios",
        "config": ROOT / "_kanden-fresh" / "game.toml",
        "savestates": (ROOT / "_kanden-fresh" / "savestates" /
                       "90164d1ac127ee5f9815ea4ae7de798c7b5fc629"),
        "args": ["--boot", "direct", "--freebios", "--generated-firmware",
                 "--internal-resolution", "2", "--antialiasing", "8",
                 "--texture-upscale", "2"],
    },
    "mkds": {
        "rom": ROOT / "mariokartdsrecomp-p2-all" / "Mario Kart DS.nds",
        "cwd": ROOT / "mariokartdsrecomp-p2-all",
        "bios": ROOT / "ndsrecomp" / "bios",
        "config": ROOT / "mariokartdsrecomp-p2-all" / "game.toml",
        "savestates": None,
        "args": ["--boot", "direct", "--freebios", "--generated-firmware",
                 "--screen-layout", "separate", "--adaptive-widescreen", "top",
                 "--startup-mode", "automatic"],
    },
    "sm64ds": {
        "rom": ROOT / "supermario64dsrecomp-p2-all" / "Super Mario 64 DS.nds",
        "cwd": ROOT / "supermario64dsrecomp-p2-all",
        "bios": ROOT / "ndsrecomp" / "bios",
        "config": ROOT / "supermario64dsrecomp-p2-all" / "game.toml",
        "savestates": None,
        "args": ["--boot", "direct", "--freebios", "--generated-firmware",
                 "--screen-layout", "separate", "--adaptive-widescreen", "top"],
    },
}

DIGEST_KEYS = ("top", "bottom", "hd", "tw", "bw", "flags")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--exe", required=True, type=Path)
    p.add_argument("--title", default="mph", choices=sorted(TITLES))
    p.add_argument("--out-root", type=Path, default=None,
                   help="default: _kanden-legs/runs/digest-<title>")
    p.add_argument("--savestates", type=Path, default=None)
    p.add_argument("--cwd", type=Path, default=None)
    p.add_argument("--rom", type=Path, default=None)
    p.add_argument("--config", type=Path, default=None)
    p.add_argument("--bios", type=Path, default=None)
    p.add_argument("--runner-arg", action="append", default=[],
                   help="extra runner argument, repeatable")
    p.add_argument("--no-preset-args", action="store_true",
                   help="drop the title preset's runner args")
    p.add_argument("--base-port", type=int, default=28300)
    p.add_argument("--frames", type=int, default=300)
    p.add_argument("--slot", type=int, default=1,
                   help="savestate slot to LOAD in each leg; 0 = reset boot")
    p.add_argument("--save-slot", type=int, default=0,
                   help="Slot to save into after --save-after frames (0 = skip)")
    p.add_argument("--save-after", type=int, default=600)
    p.add_argument("--build-id", default=None,
                   help="NDS_SAVESTATE_BUILD_ID override (default: unset)")
    p.add_argument("--leg", action="append", default=[],
                   help="LABEL=K=V,K=V environment overlay; repeatable")
    p.add_argument("--timeout-sec", type=float, default=600.0)
    p.add_argument("--prep-slot", type=int, default=0,
                   help="prep mode: run one leg, save this slot, exit")
    p.add_argument("--prep-frames", type=int, default=1200,
                   help="presented frames to wait before the prep save")
    p.add_argument("--prep-out", type=Path, default=None,
                   help="prep mode: directory to hold <rom-sha1>/ (default: "
                        "<out-root>/pinned)")
    p.add_argument("--prep-env", action="append", default=[],
                   help="prep mode: K=V environment entries")
    return p.parse_args()


def resolve_title(args: argparse.Namespace) -> dict[str, Any]:
    preset = dict(TITLES[args.title])
    cfg: dict[str, Any] = {
        "rom": args.rom or preset["rom"],
        "cwd": args.cwd or preset["cwd"],
        "bios": args.bios or preset["bios"],
        "config": args.config or preset["config"],
        "savestates": args.savestates or preset["savestates"],
        "args": ([] if args.no_preset_args else list(preset["args"]))
        + list(args.runner_arg),
    }
    return cfg


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


def sha1_file(path: Path) -> str:
    h = hashlib.sha1()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class Client:
    def __init__(self, port: int, timeout_sec: float) -> None:
        deadline = time.monotonic() + timeout_sec
        last: OSError | None = None
        while time.monotonic() < deadline:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), 2.0)
                self.s.settimeout(60.0)
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


def savestate_event(diag: Path, slot: int, event: str) -> bool:
    for r in diagnostics_records(diag):
        if (r.get("kind") == "event" and r.get("event") == event
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


def wait_for_frames(client: Client, need: int, timeout_sec: float) -> int:
    deadline = time.monotonic() + timeout_sec
    cur = 0
    while time.monotonic() < deadline:
        cur = int(client.cmd("frame_digests",
                             **{"from": 0, "count": 1}).get("count", 0))
        if cur >= need:
            return cur
        time.sleep(1.0)
    return cur


def dump_framebuffers(client: Client, out_dir: Path, tag: str) -> list[str]:
    """Best-effort PPM dumps of both screens, native and adaptive."""
    written: list[str] = []
    out_dir.mkdir(parents=True, exist_ok=True)
    for engine in ("A", "B"):
        for adaptive in (False, True):
            try:
                r = client.cmd("framebuffer", engine=engine, adaptive=adaptive)
            except Exception:
                continue
            if "rgb" not in r:
                continue
            w, h = int(r["w"]), int(r["h"])
            raw = bytes.fromhex(r["rgb"])
            name = f"{tag}-engine{engine}-{'adaptive' if adaptive else 'native'}.ppm"
            path = out_dir / name
            with path.open("wb") as f:
                f.write(f"P6\n{w} {h}\n255\n".encode())
                f.write(raw)
            written.append(str(path))
    return written


def build_cmd(args: argparse.Namespace, cfg: dict[str, Any], port: int,
              diag: Path, cache: Path, states: Path | None) -> list[str]:
    cmd = [
        str(args.exe.resolve()),
        "--interactive", "--port", str(port),
        "--live-overlay-cache", str(cache),
        "--live-overlay-activation-delay-ms", "600000",
        "--diagnostics", "on", "--diagnostics-dir", str(diag),
    ]
    if states is not None:
        cmd += ["--savestate-dir", str(states)]
    cmd += [
        str(cfg["bios"]),
        "--config", str(cfg["config"]),
        "--rom", str(cfg["rom"]),
    ]
    cmd += list(cfg["args"])
    # The governor changes host-side stages over time; a digest comparison
    # needs both legs on the same, fixed configuration.
    cmd += ["--performance-governor", "off"]
    return cmd


def base_env(args: argparse.Namespace) -> dict[str, str]:
    env = os.environ.copy()
    if args.build_id:
        env["NDS_SAVESTATE_BUILD_ID"] = args.build_id
    env["NDS_FRAME_HASH"] = "1"
    return env


def run_prep(args: argparse.Namespace, cfg: dict[str, Any], port: int) -> dict[str, Any]:
    """Produce a pinned savestate directory both A/B legs can load."""
    out_root = args.out_root
    run_dir = out_root / "prep"
    if run_dir.exists():
        shutil.rmtree(run_dir)
    diag = run_dir / "diagnostics"
    cache = run_dir / "live-overlay-cache"
    diag.mkdir(parents=True)
    cache.mkdir(parents=True)
    states = args.prep_out or (out_root / "pinned")
    if states.exists():
        shutil.rmtree(states)
    states.mkdir(parents=True)

    cmd = build_cmd(args, cfg, port, diag, cache, states)
    env = base_env(args)
    for item in args.prep_env:
        k, _, v = item.partition("=")
        env[k] = v
    result: dict[str, Any] = {"label": "prep", "port": port,
                             "run_dir": str(run_dir), "savestates": str(states)}
    proc: subprocess.Popen[Any] | None = None
    client: Client | None = None
    try:
        with (run_dir / "stdout.log").open("wb") as out, \
             (run_dir / "stderr.log").open("wb") as err:
            proc = subprocess.Popen(cmd, cwd=str(cfg["cwd"]), env=env,
                                    stdout=out, stderr=err)
            client = Client(port, 120.0)
            got = wait_for_frames(client, args.prep_frames, args.timeout_sec)
            result["frames_presented"] = got
            client.key(f"F{args.prep_slot}", shift=True)
            deadline = time.monotonic() + 60.0
            while (time.monotonic() < deadline
                   and not savestate_event(diag, args.prep_slot,
                                           "savestate_save")):
                time.sleep(0.5)
            result["save_ok"] = savestate_event(diag, args.prep_slot,
                                                "savestate_save")
            result["files"] = sorted(str(p) for p in states.rglob("slot*"))
            client.cmd("frontend_exit")
    except Exception as exc:
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


def run_leg(args: argparse.Namespace, cfg: dict[str, Any], label: str,
            env_overlay: dict[str, str], port: int) -> dict[str, Any]:
    run_dir = args.out_root / label
    if run_dir.exists():
        shutil.rmtree(run_dir)
    diag = run_dir / "diagnostics"
    cache = run_dir / "live-overlay-cache"
    states: Path | None = None
    diag.mkdir(parents=True)
    cache.mkdir(parents=True)
    if cfg["savestates"] is not None:
        src = Path(cfg["savestates"])
        states = run_dir / "savestates"
        # Copy the pinned <rom-sha1>/ directory so no leg can write the pinned
        # one, and so a leg's own --save-slot lands in its private copy.
        shutil.copytree(src, states / src.name)

    cmd = build_cmd(args, cfg, port, diag, cache, states)
    env = base_env(args)
    env.update(env_overlay)

    result: dict[str, Any] = {"label": label, "port": port,
                              "env": env_overlay, "run_dir": str(run_dir),
                              "cmd": cmd}
    proc: subprocess.Popen[Any] | None = None
    client: Client | None = None
    try:
        with (run_dir / "stdout.log").open("wb") as out, \
             (run_dir / "stderr.log").open("wb") as err:
            proc = subprocess.Popen(cmd, cwd=str(cfg["cwd"]), env=env,
                                    stdout=out, stderr=err)
            client = Client(port, 120.0)
            if args.slot:
                # Press the bare load key until diagnostics confirms the load.
                # Bare F<slot> is LOAD; Shift+F<slot> would SAVE over it.
                deadline = time.monotonic() + 180.0
                while (time.monotonic() < deadline
                       and not savestate_event(diag, args.slot,
                                               "savestate_load")):
                    client.key(f"F{args.slot}")
                    time.sleep(0.5)
                result["load_ok"] = savestate_event(diag, args.slot,
                                                    "savestate_load")
                if not result["load_ok"]:
                    result["error"] = "savestate load never reported"
                    return result
            status = client.cmd("frame_digests", **{"from": 0, "count": 1})
            result["ring_enabled"] = bool(status.get("enabled"))
            if not result["ring_enabled"]:
                result["error"] = "digest ring disabled (NDS_FRAME_HASH)"
                return result
            count_at_load = int(status.get("count", 0))
            if args.slot:
                start = find_epoch_start(client, 1, count_at_load)
                if start is None:
                    result["error"] = "no post-load digest epoch found"
                    return result
            else:
                start = 0
            result["epoch_start"] = start
            # Cumulative counters at the window's opening edge.
            result["profile_before"] = client.cmd("profile")
            result["stats_before"] = client.cmd("frontend_stats")
            need = start + max(args.frames,
                               args.save_after if args.save_slot else 0) + 8
            result["frames_available"] = wait_for_frames(client, need,
                                                         args.timeout_sec)
            result["profile_after"] = client.cmd("profile")
            result["stats_after"] = client.cmd("frontend_stats")
            result["digests"] = fetch_digests(client, start, args.frames)
            if args.save_slot and states is not None:
                client.key(f"F{args.save_slot}", shift=True)
                deadline = time.monotonic() + 60.0
                while (time.monotonic() < deadline
                       and not savestate_event(diag, args.save_slot,
                                               "savestate_save")):
                    time.sleep(0.5)
                saved = sorted((states / Path(cfg["savestates"]).name).glob(
                    f"slot{args.save_slot:02d}*"))
                result["saved_files"] = [str(p) for p in saved]
                result["saved_hashes"] = {p.name: sha1_file(p)
                                          for p in saved}
                result["saved_sizes"] = {p.name: p.stat().st_size
                                         for p in saved}
            result["framebuffers"] = dump_framebuffers(
                client, run_dir / "fb", "window-end")
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


FENCE_CAUSES = ["vram", "vramcnt", "palette", "oam", "frame", "present",
                "slots", "capture"]


def counter_delta(before: dict[str, Any], after: dict[str, Any]) -> dict[str, Any]:
    b = (before or {}).get("gpu2d") or {}
    a = (after or {}).get("gpu2d") or {}
    out: dict[str, Any] = {}
    for key in ("threaded_lines", "inline_lines", "fence_wait_ns",
                "fence_helped_lines", "staged_captures",
                "adaptive_band_frames", "adaptive_serial_frames",
                "adaptive_helper_lines", "hd_frames"):
        if key in a:
            out[key] = int(a.get(key, 0)) - int(b.get(key, 0))
    for field in ("fence_drains", "fenced_lines"):
        bd, ad = b.get(field) or {}, a.get(field) or {}
        delta = {}
        for cause in set(list(bd) + list(ad)):
            d = int(ad.get(cause, 0)) - int(bd.get(cause, 0))
            if d:
                delta[cause] = d
        out[field] = delta
    return out


def stats_delta(before: dict[str, Any], after: dict[str, Any]) -> dict[str, Any]:
    if not before or not after:
        return {}
    freq = float(after.get("freq") or 1.0)
    frames = int(after.get("frames", 0)) - int(before.get("frames", 0))
    out: dict[str, Any] = {"frames": frames}
    for key in ("emu_ticks", "present_ticks", "adaptive_ticks", "upload_ticks",
                "draw_ticks", "swap_ticks", "drain_ticks"):
        d = int(after.get(key, 0)) - int(before.get(key, 0))
        out[key.replace("_ticks", "_ms_per_frame")] = (
            round(d / freq * 1000.0 / frames, 4) if frames else None)
    wall = (int(after.get("now_ticks", 0)) - int(before.get("now_ticks", 0)))
    out["wall_s"] = round(wall / freq, 3) if freq else None
    out["fps"] = round(frames / (wall / freq), 3) if wall else None
    out["underruns"] = (int(after.get("underruns", 0))
                        - int(before.get("underruns", 0)))
    return out


def compare(legs: list[dict[str, Any]], out_root: Path) -> int:
    base = legs[0]
    rc = 0
    summary: list[dict[str, Any]] = []
    for other in legs[1:]:
        a = base.get("digests") or []
        b = other.get("digests") or []
        n = min(len(a), len(b))
        mismatches = []
        for i in range(n):
            if any(a[i].get(k) != b[i].get(k) for k in DIGEST_KEYS):
                mismatches.append({"index": i, base["label"]: a[i],
                                   other["label"]: b[i]})
        print(f"[{base['label']} vs {other['label']}] compared {n} frames, "
              f"{len(mismatches)} mismatches")
        for m in mismatches[:10]:
            print(f"  #{m['index']}: {m[base['label']]} != {m[other['label']]}")
        summary.append({"a": base["label"], "b": other["label"],
                        "frames": n, "mismatches": mismatches})
        # savestate comparison, when both legs saved one
        ha = base.get("saved_hashes") or {}
        hb = other.get("saved_hashes") or {}
        if ha and hb:
            same = ha == hb
            print(f"  savestate hashes {'IDENTICAL' if same else 'DIFFER'}: "
                  f"{ha} vs {hb}")
            summary[-1]["savestate_identical"] = same
            summary[-1]["savestate_hashes"] = {base["label"]: ha,
                                               other["label"]: hb}
            if not same:
                rc = 1
        if not n or mismatches:
            rc = 1
    (out_root / "digest-ab-summary.json").write_text(
        json.dumps(summary, indent=2), encoding="utf-8")
    # Fence / phase tables for every leg, threaded or not.
    for leg in legs:
        d = counter_delta(leg.get("profile_before"), leg.get("profile_after"))
        s = stats_delta(leg.get("stats_before"), leg.get("stats_after"))
        print(f"--- {leg['label']} gpu2d counters over the window: "
              f"{json.dumps(d)}")
        print(f"--- {leg['label']} frontend phases: {json.dumps(s)}")
        leg["counter_delta"] = d
        leg["stats_delta"] = s
    return rc


def main() -> int:
    args = parse_args()
    cfg = resolve_title(args)
    if args.out_root is None:
        args.out_root = (ROOT / "_kanden-legs" / "runs" /
                         f"digest-{args.title}")
    args.out_root.mkdir(parents=True, exist_ok=True)

    port = args.base_port
    while not is_port_free(port):
        port += 1

    if args.prep_slot:
        res = run_prep(args, cfg, port)
        (args.out_root / "prep.json").write_text(json.dumps(res, indent=2),
                                                 encoding="utf-8")
        print(json.dumps(res, indent=2))
        return 0 if res.get("save_ok") else 2

    if args.slot and cfg["savestates"] is None:
        print("--slot needs a savestate directory: pass --savestates DIR "
              "(or run --prep-slot first)", file=sys.stderr)
        return 2

    legs = parse_legs(args.leg) or [
        ("serial", {"NDS_GPU2D_THREADED": "0",
                    "NDS_GPU2D_ADAPTIVE_WORKERS": "0"}),
        ("threaded", {}),
    ]
    results = []
    for label, env in legs:
        while not is_port_free(port):
            port += 1
        print(f"--- leg {label} on port {port}: {env}", flush=True)
        results.append(run_leg(args, cfg, label, env, port))
        port += 1
        print(json.dumps({k: v for k, v in results[-1].items()
                          if k != "digests"}, indent=2, default=str),
              flush=True)
    rc = 0
    for r in results:
        if r.get("error"):
            print(f"leg {r['label']} failed: {r['error']}")
            rc = 2
    if rc == 0:
        rc = compare(results, args.out_root)
    (args.out_root / "digest-ab.json").write_text(
        json.dumps(results, indent=2, default=str), encoding="utf-8")
    return rc


if __name__ == "__main__":
    sys.exit(main())
