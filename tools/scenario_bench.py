"""Framework-side helpers for interactive-frontend scenario benchmarks.

The SM64DS harness (supermario64dsrecomp/tools/measure_sm64ds_scenario.py)
established the shape of a repeatable performance measurement:

    fresh process per repetition -> drive a route through the debug TCP
    surface -> diff cumulative counters across a phase -> report
    fps + phase_ms_per_frame + tier-3 + dispatch deltas.

That harness is title-specific in its route only; everything below the route
is generic. This module owns the generic half so a second title (MPH) gets
the identical report shape without copying the SM64DS file.

Two clocks matter and they are NOT interchangeable:

* frontend frames  - host presented frames (frontend_stats.frames). The
  denominator for every per-frame cost, and the only clock that reflects the
  host's speed.
* guest VBlanks / ARM9 instructions (event_counts.vblank9 / insn9) - guest
  work. Route waits and phase landmarks are anchored here so a faster build
  measures the SAME guest workload instead of a shorter one.

Nothing here arms a trace: every counter read is a query against always-on
accumulators, so a probe can join a running session at any point.

While the interactive frontend owns execution the debug server refuses
run_to_event (see runner/src/debug_server.cpp, "frontend owns execution"),
so every wait below is a bounded poll with a runner-liveness check.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import pathlib
import socket
import statistics
import subprocess
import sys
import time
from typing import Any, Callable, Iterable, Sequence


def _framework_root() -> pathlib.Path:
    """Locate the framework checkout that owns oracle/_client.py.

    A worktree pair (target-<topic>, ndsrecomp-<topic>) must resolve to its
    own framework worktree, never to the shared main checkout, so a topic
    branch's protocol changes are the ones under test.
    """
    here = pathlib.Path(__file__).resolve()
    candidate = here.parents[1]
    if (candidate / "oracle" / "_client.py").exists():
        return candidate
    workspace = here.parents[2]
    for name in ("ndsrecomp",):
        fallback = workspace / name
        if (fallback / "oracle" / "_client.py").exists():
            return fallback
    raise RuntimeError("could not locate a framework checkout with oracle/_client.py")


FRAMEWORK_ROOT = _framework_root()


def _load_debug_client() -> type:
    """Import oracle/_client.py by path.

    The oracle directory is not a package, and a target repo's tools/ is not
    on the framework's import path. Loading by spec keeps both harnesses
    independent of how either checkout is laid out.
    """
    path = FRAMEWORK_ROOT / "oracle" / "_client.py"
    spec = importlib.util.spec_from_file_location("ndsrecomp_oracle_client", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.DebugClient


DebugClient = _load_debug_client()


# Active-low KEYINPUT bits. The debug server's own default of 0x3FF leaves
# X and Y held, so every release must write 0x0FFF explicitly.
KEY_BITS = {
    "a": 0, "b": 1, "select": 2, "start": 3, "right": 4, "left": 5,
    "up": 6, "down": 7, "r": 8, "l": 9, "x": 10, "y": 11,
}
KEYS_RELEASED = 0x0FFF


# ---------------------------------------------------------------------------
# pure helpers
# ---------------------------------------------------------------------------


def subtract(after: Any, before: Any) -> Any:
    """Recursive numeric delta over two matching counter snapshots."""
    if isinstance(after, dict):
        return {
            key: subtract(value, before[key])
            for key, value in after.items()
            if key in before
        }
    if isinstance(after, (int, float)):
        return after - before
    return after


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_revision(path: pathlib.Path) -> str | None:
    try:
        result = subprocess.run(
            ["git", "-C", str(path), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return result.stdout.strip()


# ---------------------------------------------------------------------------
# process control
# ---------------------------------------------------------------------------


def launch_runtime(
    executable: pathlib.Path,
    bios: pathlib.Path,
    rom: pathlib.Path,
    port: int,
    stdout_path: pathlib.Path,
    stderr_path: pathlib.Path,
    *,
    config: pathlib.Path | None = None,
    save_path: pathlib.Path | None = None,
    startup_mode: str = "automatic",
    profiled: bool = False,
    threaded: bool = True,
    renderer: str = "auto",
    compute_readback_overlap: bool = True,
    compute_direct_present: bool | None = None,
    extra_args: Sequence[str] = (),
    env_overrides: dict[str, str] | None = None,
) -> tuple[subprocess.Popen[bytes], Any, Any]:
    """Spawn one interactive runner process and return it with its log files.

    The caller owns the returned handles; terminate_runtime() below kills only
    this PID. Never kill by image name: this machine runs concurrent sessions
    of the same executable.
    """
    environment = os.environ.copy()
    # mingw runtime DLLs sit next to the toolchain, not next to the exe.
    environment["PATH"] = r"C:\msys64\mingw64\bin;" + environment.get("PATH", "")
    environment["NDS_FRONTEND_STATS"] = "1"
    environment["NDS_3D_THREADED"] = "1" if threaded else "0"
    environment["NDS_STARTUP_MODE"] = startup_mode
    if renderer == "auto":
        environment.pop("NDS_3D_RENDERER", None)
    else:
        environment["NDS_3D_RENDERER"] = renderer
    environment["NDS_COMPUTE_READBACK_OVERLAP"] = (
        "1" if compute_readback_overlap else "0"
    )
    if compute_direct_present is not None:
        environment["NDS_COMPUTE_DIRECT_PRESENT"] = (
            "1" if compute_direct_present else "0"
        )
    environment.pop("NDS_DEEP_TRACE", None)
    if profiled:
        environment["NDS_PROFILE_GPU"] = "1"
        environment["NDS_PROFILE_SCHED"] = "1"
    else:
        environment.pop("NDS_PROFILE_GPU", None)
        environment.pop("NDS_PROFILE_SCHED", None)
    if env_overrides:
        environment.update(env_overrides)

    command = [
        str(executable),
        str(bios),
        "--interactive",
        "--rom",
        str(rom),
        "--port",
        str(port),
        "--startup-mode",
        startup_mode,
    ]
    if config is not None:
        command += ["--config", str(config)]
    if save_path is not None:
        command += ["--save-path", str(save_path)]
    else:
        command += ["--no-save"]
    command += list(extra_args)

    stdout_file = stdout_path.open("wb")
    stderr_file = stderr_path.open("wb")
    process = subprocess.Popen(
        command,
        cwd=str(executable.parent),
        env=environment,
        stdout=stdout_file,
        stderr=stderr_file,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    return process, stdout_file, stderr_file


def terminate_runtime(process: subprocess.Popen[bytes]) -> None:
    """Stop exactly the PID we spawned."""
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=10.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=10.0)


class RunnerDied(RuntimeError):
    """The measured process exited mid-route; the repetition is void."""


# ---------------------------------------------------------------------------
# connection + counters
# ---------------------------------------------------------------------------


def wait_for_client(
    process: subprocess.Popen[bytes], port: int, timeout_seconds: float = 90.0
) -> Any:
    deadline = time.monotonic() + timeout_seconds
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RunnerDied(f"runtime exited during startup ({process.returncode})")
        try:
            client = DebugClient(port=port, timeout=30.0)
            client.ping()
            return client
        except (ConnectionError, OSError, RuntimeError, socket.timeout) as error:
            last_error = error
            time.sleep(0.1)
    raise TimeoutError(f"runtime TCP server did not start on {port}: {last_error}")


def live_stats(client: Any) -> dict[str, Any]:
    stats = client.cmd("frontend_stats")
    if not stats["active"]:
        raise RuntimeError("runtime does not have an active interactive frontend")
    return stats


def profile_snapshot(client: Any) -> dict[str, Any]:
    """One merged snapshot of every always-on attribution counter.

    profile is the GPU/scheduler bucket set (zeros unless NDS_PROFILE_* armed
    the sampling at process start); the rest are always live. Merging them
    into one dict is what lets subtract() emit per-phase dispatch
    composition, per-class dispatch COST, direct-link behaviour, and tier-3
    deltas from the same diff -- subtract() is recursive, so a nested
    counter surface needs nothing here but its name.

    dispatch_stats answers how OFTEN each class ran; dispatch_timing
    (beads-yjp.44) answers what each one COST; direct_link (beads-yjp.45)
    answers how those transfers actually resolved. A phase delta that
    carries only the first cannot tell a real speedup from a shifted
    composition, which is why all three are captured together.
    """
    snapshot = client.cmd("profile")
    for name, command in (("dispatch", "dispatch_stats"),
                          ("dispatch_timing", "dispatch_timing"),
                          ("direct_link", "direct_link"),
                          ("static_coverage", "static_coverage")):
        try:
            snapshot[name] = client.cmd(command)
        except RuntimeError as error:
            # An older runner simply does not have the command. Skip it
            # rather than fail the whole measurement.
            if not str(error).endswith("unknown cmd"):
                raise
    return snapshot


def _poll(
    process: subprocess.Popen[bytes] | None,
    read: Callable[[], int],
    target: int,
    what: str,
    timeout_seconds: float,
    pace: Callable[[int], float],
) -> None:
    deadline = time.monotonic() + timeout_seconds
    while True:
        current = read()
        remaining = target - current
        if remaining <= 0:
            return
        if process is not None and process.poll() is not None:
            raise RunnerDied(
                f"runtime exited ({process.returncode}) while waiting for "
                f"{what} {target}; stopped at {current}"
            )
        if time.monotonic() >= deadline:
            raise TimeoutError(
                f"runtime did not reach {what} {target}; stopped at {current}"
            )
        time.sleep(pace(remaining))


def wait_until_frame(
    client: Any,
    target: int,
    timeout_seconds: float = 300.0,
    process: subprocess.Popen[bytes] | None = None,
) -> dict[str, Any]:
    _poll(
        process,
        lambda: int(live_stats(client)["frames"]),
        target,
        "presented frame",
        timeout_seconds,
        # Audio pacing caps the frontend near 60 FPS; approach at 90/s so the
        # final poll lands on the boundary instead of overshooting the phase.
        lambda remaining: max(0.03, min(1.0, remaining / 90.0)),
    )
    return live_stats(client)


def wait_until_vblank9(
    client: Any,
    target: int,
    timeout_seconds: float = 900.0,
    process: subprocess.Popen[bytes] | None = None,
) -> dict[str, Any]:
    _poll(
        process,
        lambda: int(client.event_counts()["vblank9"]),
        target,
        "guest VBlank",
        timeout_seconds,
        lambda remaining: max(0.03, min(1.0, remaining / 90.0)),
    )
    return client.event_counts()


def wait_until_insn9(
    client: Any,
    target: int,
    timeout_seconds: float = 300.0,
    process: subprocess.Popen[bytes] | None = None,
) -> dict[str, Any]:
    _poll(
        process,
        lambda: int(client.event_counts()["insn9"]),
        target,
        "ARM9 instruction",
        timeout_seconds,
        # Approach at a conservative 40 M/s so a faster build's final poll
        # does not skip a large fraction of the phase being measured.
        lambda remaining: max(0.03, min(1.0, remaining / 40_000_000.0)),
    )
    return client.event_counts()


# ---------------------------------------------------------------------------
# input
# ---------------------------------------------------------------------------


def advance_vblanks(
    client: Any,
    frames: int,
    process: subprocess.Popen[bytes] | None = None,
    timeout_seconds: float = 900.0,
) -> None:
    target = int(client.event_counts()["vblank9"]) + int(frames)
    wait_until_vblank9(client, target, timeout_seconds, process)


def touch(
    client: Any,
    x: int,
    y: int,
    hold_frames: int = 4,
    process: subprocess.Popen[bytes] | None = None,
) -> None:
    client.cmd("touch", x=int(x), y=int(y), down=True)
    advance_vblanks(client, hold_frames, process)
    client.cmd("touch", x=0, y=0, down=False)


def press(
    client: Any,
    key: str,
    hold_frames: int = 4,
    process: subprocess.Popen[bytes] | None = None,
) -> None:
    """Hold one DS button for hold_frames guest VBlanks, then release.

    The debug `keys` command writes KEYINPUT directly (io.cpp
    nds_set_key_mask). The interactive frontend republishes its own mask only
    when it processes a real SDL input event, so a probe that injects no host
    input keeps ownership of the pad for the whole route.
    """
    bit = KEY_BITS[key]
    client.cmd("keys", mask=KEYS_RELEASED & ~(1 << bit))
    advance_vblanks(client, hold_frames, process)
    client.cmd("keys", mask=KEYS_RELEASED)


def replay_actions(
    client: Any,
    actions: Iterable[dict[str, Any]],
    *,
    hold_frames: int = 3,
    settle_frames: int = 45,
    process: subprocess.Popen[bytes] | None = None,
    on_step: Callable[[int, str], None] | None = None,
) -> None:
    """Replay a scenarios/*.json action list against a live frontend."""
    for index, action in enumerate(actions, 1):
        kind = action["kind"]
        if kind == "touch":
            touch(client, int(action["x"]), int(action["y"]), hold_frames, process)
            label = f"touch-{action['x']}-{action['y']}"
        elif kind == "key":
            press(client, str(action["key"]), hold_frames, process)
            label = f"key-{action['key']}"
        elif kind == "wait":
            advance_vblanks(client, int(action["frames"]), process)
            label = f"wait-{action['frames']}"
        else:
            raise ValueError(f"unknown scenario action kind {kind!r}")
        advance_vblanks(client, settle_frames, process)
        if on_step is not None:
            on_step(index, label)


def load_actions(path: pathlib.Path) -> list[dict[str, Any]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if isinstance(data, dict):
        return list(data.get("actions", []))
    return list(data)


# ---------------------------------------------------------------------------
# visual evidence
# ---------------------------------------------------------------------------


def framebuffer_digest(client: Any) -> dict[str, str]:
    sync = client.cmd("framebuffer_sync")
    if sync.get("active"):
        wait_until_frame(client, int(sync["frames"]) + 1)
    result: dict[str, str] = {}
    for engine in ("A", "B"):
        width, height, rgb = client.framebuffer(engine)
        result[engine] = hashlib.sha256(rgb).hexdigest()
        result[f"{engine}_size"] = f"{width}x{height}"
    return result


def save_screenshot(
    client: Any, path: pathlib.Path, adaptive_top: bool = False
) -> None:
    from PIL import Image  # optional; only visual-evidence runs need it

    width_a, height_a, rgb_a = client.framebuffer("A", adaptive=adaptive_top)
    width_b, height_b, rgb_b = client.framebuffer("B")
    canvas_width = max(width_a, width_b)
    image = Image.new("RGB", (canvas_width, height_a + height_b), (20, 20, 20))
    image.paste(
        Image.frombytes("RGB", (width_a, height_a), rgb_a),
        ((canvas_width - width_a) // 2, 0),
    )
    image.paste(
        Image.frombytes("RGB", (width_b, height_b), rgb_b),
        ((canvas_width - width_b) // 2, height_a),
    )
    image.save(path)


# ---------------------------------------------------------------------------
# measurement
# ---------------------------------------------------------------------------


def summarize_window(
    label: str,
    before_front: dict[str, Any],
    after_front: dict[str, Any],
    before_profile: dict[str, Any],
    after_profile: dict[str, Any],
) -> dict[str, Any]:
    """Derive one phase's report from two counter snapshots.

    Report shape is deliberately identical to the SM64DS harness so the same
    downstream comparison tooling reads both titles.
    """
    front = subtract(after_front, before_front)
    profile = subtract(after_profile, before_profile)
    frames = max(int(front["frames"]), 0)
    frequency = int(after_front["freq"])
    seconds = front["now_ticks"] / frequency if frequency else 0.0
    denominator = max(frames, 1)
    phase_ticks = front["emu_ticks"] + front["present_ticks"] + front["drain_ticks"]
    sched = profile.get("sched", {})
    sampled_ns = sched.get("sampled_round_ns", 0)

    def per_frame_ms(ticks: float) -> float:
        return ticks * 1000.0 / frequency / denominator if frequency else 0.0

    result: dict[str, Any] = {
        "label": label,
        "frames": frames,
        "seconds": seconds,
        "fps": frames / seconds if seconds else 0.0,
        "average_frame_ms": seconds * 1000.0 / denominator,
        "underruns": front["underruns"],
        "phase_ms_per_frame": {
            "emu": per_frame_ms(front["emu_ticks"]),
            "present": per_frame_ms(front["present_ticks"]),
            "audio_drain": per_frame_ms(front["drain_ticks"]),
            "unaccounted": per_frame_ms(front["now_ticks"] - phase_ticks),
        },
        "present_ms_per_frame": {
            key.removesuffix("_ticks"): per_frame_ms(front.get(key, 0))
            for key in ("adaptive_ticks", "upload_ticks", "draw_ticks", "swap_ticks")
        },
        "gpu2d_ms_per_frame": {
            key.removesuffix("_ns"): value / denominator / 1.0e6
            for key, value in profile.get("gpu2d", {}).items()
            if key.endswith("_ns") and isinstance(value, (int, float))
        },
        "gpu3d_ms_per_frame": {
            key.removesuffix("_ns"): value / denominator / 1.0e6
            for key, value in profile.get("gpu3d", {}).items()
            if key.endswith("_ns") and isinstance(value, (int, float))
        },
        "scheduler_sample_share": {
            key.removesuffix("_ns"): value / sampled_ns
            for key, value in sched.items()
            if key.endswith("_ns") and key != "sampled_round_ns" and sampled_ns
        },
        "tier3_delta": profile.get("static_coverage", {}),
        "dispatch_delta": profile.get("dispatch", {}),
        "raw_front_delta": front,
        "raw_profile_delta": profile,
    }
    return result


def finish_phase(
    client: Any,
    result: dict[str, Any],
    screenshot_path: pathlib.Path | None,
    adaptive_screenshot: bool,
) -> dict[str, Any]:
    result["end_event_counts"] = client.event_counts()
    result["end_framebuffer_sha256"] = framebuffer_digest(client)
    if screenshot_path is not None:
        save_screenshot(client, screenshot_path, adaptive_screenshot)
        result["end_screenshot"] = str(screenshot_path)
    return result


def aggregate_runs(runs: list[dict[str, Any]]) -> dict[str, Any]:
    """Median/min/max across the valid repetitions, per phase and overall."""
    valid = [run for run in runs if run.get("valid", True) and run.get("phases")]
    if not valid:
        return {"run_count": 0, "discarded_runs": len(runs), "phases": []}
    labels = [phase["label"] for phase in valid[0]["phases"]]
    phases: list[dict[str, Any]] = []
    for index, label in enumerate(labels):
        fps_values = [
            run["phases"][index]["fps"]
            for run in valid
            if len(run["phases"]) > index
        ]
        if not fps_values:
            continue
        median = statistics.median(fps_values)
        phases.append(
            {
                "label": label,
                "fps_values": fps_values,
                "median_fps": median,
                "min_fps": min(fps_values),
                "max_fps": max(fps_values),
                "median_frame_ms": 1000.0 / median if median else 0.0,
                "median_emu_ms_per_frame": statistics.median(
                    run["phases"][index]["phase_ms_per_frame"]["emu"]
                    for run in valid
                    if len(run["phases"]) > index
                ),
                "median_present_ms_per_frame": statistics.median(
                    run["phases"][index]["phase_ms_per_frame"]["present"]
                    for run in valid
                    if len(run["phases"]) > index
                ),
                "total_underruns": sum(
                    run["phases"][index]["underruns"]
                    for run in valid
                    if len(run["phases"]) > index
                ),
            }
        )
    overall_fps = []
    for run in valid:
        frames = sum(phase["frames"] for phase in run["phases"])
        seconds = sum(phase["seconds"] for phase in run["phases"])
        if seconds:
            overall_fps.append(frames / seconds)
    summary: dict[str, Any] = {
        "run_count": len(valid),
        "discarded_runs": len(runs) - len(valid),
        "phases": phases,
    }
    if overall_fps:
        summary.update(
            {
                "overall_fps_values": overall_fps,
                "overall_median_fps": statistics.median(overall_fps),
                "overall_min_fps": min(overall_fps),
                "overall_max_fps": max(overall_fps),
            }
        )
    return summary


def write_json(path: pathlib.Path, payload: Any) -> None:
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def host_description() -> dict[str, str]:
    import platform

    return {
        "platform": platform.platform(),
        "processor": platform.processor(),
        "python": sys.version,
    }


# ---------------------------------------------------------------------------
# native RIP sampling + symbolization
#
# Shared by the per-title worst-phase profilers. The sampler itself is the
# generic tools/windows_rip_sampler.cpp: it attaches to one PID, suspends and
# reads RIP from every thread on an interval, and writes a CSV plus the
# runtime image base. It stops when its stdin receives a newline, so the
# caller controls the exact window.
#
# Build from PowerShell with the explicit mingw paths below; a git-bash PATH
# silently breaks this compile.
# ---------------------------------------------------------------------------

MINGW_ROOT = pathlib.Path(r"C:\msys64\mingw64\bin")
COMPILER = MINGW_ROOT / "g++.exe"
ADDR2LINE = MINGW_ROOT / "addr2line.exe"
NM = MINGW_ROOT / "nm.exe"


def build_sampler(source: pathlib.Path, executable: pathlib.Path) -> pathlib.Path:
    if executable.exists() and (
        executable.stat().st_mtime_ns >= source.stat().st_mtime_ns
    ):
        return executable
    subprocess.run(
        [str(COMPILER), "-std=c++20", "-O2", "-s", str(source), "-o", str(executable)],
        check=True,
    )
    return executable


def start_sampler(
    sampler_exe: pathlib.Path,
    pid: int,
    samples_path: pathlib.Path,
    interval_us: int,
) -> subprocess.Popen[str]:
    return subprocess.Popen(
        [str(sampler_exe), str(pid), str(samples_path), str(interval_us)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def stop_sampler(sampler: subprocess.Popen[str], timeout: float = 30.0) -> None:
    if sampler.stdin is None:
        raise RuntimeError("sampler was started without a stdin pipe")
    sampler.stdin.write("\n")
    sampler.stdin.flush()
    out, err = sampler.communicate(timeout=timeout)
    if sampler.returncode:
        raise RuntimeError(f"RIP sampler failed ({sampler.returncode}): {out}\n{err}")


def preferred_image_base(executable: pathlib.Path) -> int:
    import struct

    with executable.open("rb") as source:
        source.seek(0x3C)
        pe_offset = struct.unpack("<I", source.read(4))[0]
        source.seek(pe_offset + 24)
        magic = struct.unpack("<H", source.read(2))[0]
        if magic == 0x20B:
            source.seek(pe_offset + 24 + 24)
            return struct.unpack("<Q", source.read(8))[0]
        if magic == 0x10B:
            source.seek(pe_offset + 24 + 28)
            return struct.unpack("<I", source.read(4))[0]
    raise RuntimeError("unsupported PE optional-header format")


def parse_samples(path: pathlib.Path) -> tuple[int, dict[int, Any]]:
    import csv
    from collections import Counter, defaultdict

    runtime_base = 0
    by_thread: dict[int, Counter] = defaultdict(Counter)
    with path.open(newline="", encoding="utf-8") as source:
        lines = source.readlines()
    for line in lines:
        if line.startswith("# runtime_image_base,"):
            runtime_base = int(line.split(",", 1)[1], 16)
    rows = csv.DictReader(line for line in lines if not line.startswith("#"))
    for row in rows:
        by_thread[int(row["thread_id"])][int(row["rip"], 16)] += int(row["samples"])
    if not runtime_base:
        raise RuntimeError("sampler did not report the runtime image base")
    return runtime_base, by_thread


def nearest_symbols(
    executable: pathlib.Path, addresses: list[int]
) -> dict[int, str]:
    """Map each address to the nearest preceding text symbol.

    addr2line alone loses static/inlined frames in a stripped-ish build; the
    nm walk gives every sample a function name even when line info does not.
    """
    if not addresses:
        return {}
    pending = sorted(addresses)
    resolved: dict[int, str] = {}
    index = 0
    previous_name = "??"
    process = subprocess.Popen(
        [str(NM), "-n", "-C", str(executable)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    assert process.stdout is not None
    for line in process.stdout:
        parts = line.rstrip().split(maxsplit=2)
        if len(parts) != 3 or parts[1] not in {"T", "t", "W", "w"}:
            continue
        try:
            symbol_address = int(parts[0], 16)
        except ValueError:
            continue
        while index < len(pending) and pending[index] < symbol_address:
            resolved[pending[index]] = previous_name
            index += 1
        previous_name = parts[2]
    while index < len(pending):
        resolved[pending[index]] = previous_name
        index += 1
    stderr = process.stderr.read() if process.stderr is not None else ""
    if process.wait() != 0:
        raise RuntimeError(f"nm failed: {stderr}")
    return resolved


def symbolize(
    executable: pathlib.Path,
    runtime_base: int,
    preferred_base: int,
    by_thread: dict[int, Any],
) -> dict[str, Any]:
    from collections import Counter

    image_size = executable.stat().st_size
    runner_addresses = sorted(
        {
            rip
            for counts in by_thread.values()
            for rip in counts
            if runtime_base <= rip < runtime_base + image_size
        }
    )
    adjusted = [preferred_base + (rip - runtime_base) for rip in runner_addresses]
    symbols: dict[int, tuple[str, str]] = {}
    if adjusted:
        function_names = nearest_symbols(executable, adjusted)
        result = subprocess.run(
            [str(ADDR2LINE), "-f", "-C", "-e", str(executable)],
            check=True,
            capture_output=True,
            text=True,
            errors="replace",
            input="\n".join(f"0x{address:x}" for address in adjusted) + "\n",
        )
        lines = result.stdout.splitlines()
        for index, rip in enumerate(runner_addresses):
            symbols[rip] = (function_names[adjusted[index]], lines[index * 2 + 1])

    result_threads = []
    aggregate: Counter = Counter()
    for thread_id, counts in sorted(by_thread.items()):
        runner_samples: Counter = Counter()
        total = sum(counts.values())
        for rip, count in counts.items():
            symbol = symbols.get(rip)
            if symbol:
                runner_samples[symbol] += count
                aggregate[symbol] += count
        result_threads.append(
            {
                "thread_id": thread_id,
                "total_samples": total,
                "runner_samples": sum(runner_samples.values()),
                "top_runner_symbols": [
                    {"function": function, "source": source, "samples": count}
                    for (function, source), count in runner_samples.most_common(30)
                ],
            }
        )
    return {
        "threads": result_threads,
        "aggregate_top_runner_symbols": [
            {"function": function, "source": source, "samples": count}
            for (function, source), count in aggregate.most_common(50)
        ],
    }
