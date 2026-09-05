"""Measure the MPH adventure route with every guest instruction interpreted.

Phase 0 of the tier-3 interpreter performance campaign (beads-yjp.42). This is
a modified copy of metroidprimehuntersrecomp-mph-perf/tools/measure_mph_scenario.py
-- the mph-perf worktree is read-only reference here, and the change needed is
structural rather than a new flag, so the copy lives with the selector it
drives. Route landmarks are copied VERBATIM so a forced number and a faithful
number describe the same guest work.

WHY THIS IS NOT JUST `NDS_FORCE_TIER3=1 measure_mph_scenario.py`
---------------------------------------------------------------
The adventure route reaches gameplay by playing 7800 guest VBlanks of boot,
logos and FMV, then replaying a menu scenario. Interpreting all of that would
cost roughly an hour per repetition and would measure menus, not gameplay. So
the route is navigated FAITHFULLY at native speed, and the selector is flipped
through the debug server (`force_tier3`) once the measured phases begin. Env
`NDS_FORCE_TIER3=1` (whole-process forcing from reset) remains available on the
runner for anyone who wants the boot path itself interpreted.

TIER VERIFICATION IS A GATE, NOT A NOTE
---------------------------------------
The one lesson imported from the psxrecomp interp-perf session: a number is
worthless until you have proven the tier you think you measured is the tier
that ran. That session chased a phantom 40 FPS figure produced by shards that
had silently failed to compile. So every phase here is checked against two
independent witnesses and a run that fails EITHER is marked invalid and
contributes no number:

  1. dispatch_stats.forced_tier3 matches the requested mode, and
     forced_tier3_misses actually advanced during the phase (a flag that is set
     but never converts a lookup proves nothing).
  2. static_coverage.tier3_insns9 delta / event_counts.insn9 delta exceeds
     --min-tier3-share (default 0.90) in forced mode, and falls below
     --max-tier3-share (default 0.05) in the faithful control.

Witness 2 is the load-bearing one: it is measured on the guest instruction
stream itself and cannot be satisfied by a mis-set flag.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import subprocess
import sys
import time
from typing import Any

FRAMEWORK_ROOT = pathlib.Path(__file__).resolve().parents[1]
WORKSPACE_ROOT = FRAMEWORK_ROOT.parent
sys.path.insert(0, str(FRAMEWORK_ROOT / "tools"))

import scenario_bench as bench  # noqa: E402

# The MPH target worktree supplies the route assets (scenario JSON, game.toml,
# ROM). Read-only: nothing here writes into it.
TARGET_ROOT = WORKSPACE_ROOT / "metroidprimehuntersrecomp-mph-perf"
SCENARIO_DIR = TARGET_ROOT / "scenarios"

# --- Route landmarks, copied verbatim from measure_mph_scenario.py ----------
# Calibrated 2026-08-25 against build-mph-release-050, --boot direct. Do not
# retune these for the interpreter: an instruction landmark describes guest
# work, which is exactly what must stay constant between the forced and
# faithful legs. Host seconds are the output, not the input.
TITLE_VBLANK = 7800
TITLE_TAP = (128, 96)
TITLE_SETTLE_VBLANKS = 180
DEFAULT_BOOT = "direct"
ADVENTURE_INSN_PHASES = (
    ("adventure_settle", 5_000_000),
    ("adventure_walk", 40_000_000),
    ("adventure_steady", 125_000_000),
)
SCENARIO = "adventure_start.json"
HOLD_KEY = "up"


def selector_state(client: Any) -> dict[str, Any]:
    """Read the selector without changing it."""
    return client.cmd("force_tier3")


def set_selector(client: Any, on: bool) -> dict[str, Any]:
    state = client.cmd("force_tier3", on=1 if on else 0)
    if bool(state.get("forced_tier3")) != on:
        raise RuntimeError(
            f"force_tier3 did not take: asked {on}, runner reports {state}"
        )
    return state


def verify_phase(
    phase: dict[str, Any],
    forced: bool,
    min_share: float,
    max_share: float,
) -> None:
    """Attach the tier verdict to a phase; raise if the phase is not valid.

    Both CPUs are reported, but the gate is on ARM9: it is the CPU that runs
    the game code the campaign is about, and the ARM7 spends most of a frame
    halted in its BIOS idle loop, so an ARM7 share is not a stable witness.
    """
    insn9 = phase["insn9"]
    tier3_insn9 = phase["tier3_insns9"]
    share = tier3_insn9 / insn9 if insn9 else 0.0
    phase["tier3_insn9_share"] = share
    phase["tier3_insn7_share"] = (
        phase["tier3_insns7"] / phase["insn7"] if phase["insn7"] else 0.0
    )
    problems: list[str] = []
    if phase["forced_tier3"] is not forced:
        problems.append(
            f"dispatch_stats.forced_tier3={phase['forced_tier3']}, expected {forced}"
        )
    if forced:
        if phase["forced_tier3_misses_delta"] <= 0:
            problems.append(
                "selector converted no lookups during the phase "
                "(forced_tier3_misses did not advance)"
            )
        if share < min_share:
            problems.append(
                f"tier3 ARM9 instruction share {share:.4f} < {min_share}: "
                "native code still carried this phase"
            )
    else:
        if share > max_share:
            problems.append(
                f"tier3 ARM9 instruction share {share:.4f} > {max_share}: "
                "the faithful control was not running natively"
            )
    phase["tier_verified"] = not problems
    phase["tier_problems"] = problems
    if problems:
        raise RuntimeError(
            f"tier verification failed for {phase['label']}: " + "; ".join(problems)
        )


def measure_to_insn9(
    client: Any,
    process: subprocess.Popen[bytes],
    label: str,
    target_insn9: int,
    requested_insn9: int,
    timeout_seconds: float,
    forced: bool,
    min_share: float,
    max_share: float,
) -> dict[str, Any]:
    before_front = bench.live_stats(client)
    before_profile = bench.profile_snapshot(client)
    before_sel = selector_state(client)
    # event_counts (insn9) and static_coverage (tier3_insns9) are two separate
    # round trips, and the guest keeps executing between them. Reading them
    # adjacently at BOTH ends makes the skew cancel to first order; reading
    # them far apart does not, and produced tier3 shares above 1.0 -- an
    # impossible value that would have discredited a real result. The residual
    # is measured below rather than assumed away.
    before_cov = client.cmd("static_coverage")
    before_counts = client.event_counts()

    bench.wait_until_insn9(client, target_insn9, timeout_seconds, process)

    after_front = bench.live_stats(client)
    after_profile = bench.profile_snapshot(client)
    after_cov = client.cmd("static_coverage")
    end_counts = client.event_counts()
    # One extra round trip bounds how much guest execution a single command
    # costs, so the reported share carries its own error bar.
    skew_counts = client.event_counts()
    after_sel = selector_state(client)

    result = bench.summarize_window(
        label, before_front, after_front, before_profile, after_profile
    )
    result["requested_insn9"] = requested_insn9
    result["insn9"] = end_counts["insn9"] - before_counts["insn9"]
    result["insn7"] = end_counts["insn7"] - before_counts["insn7"]
    result["vblank9"] = end_counts["vblank9"] - before_counts["vblank9"]
    result["tier3_insns9"] = after_cov["tier3_insns9"] - before_cov["tier3_insns9"]
    result["tier3_insns7"] = after_cov["tier3_insns7"] - before_cov["tier3_insns7"]
    result["tier3_entries9"] = after_cov["tier3_entries9"] - before_cov["tier3_entries9"]
    result["counter_skew_insn9"] = skew_counts["insn9"] - end_counts["insn9"]
    result["forced_tier3"] = bool(after_sel["forced_tier3"])
    result["forced_tier3_misses_delta"] = (
        after_sel["forced_tier3_misses"] - before_sel["forced_tier3_misses"]
    )
    # Presented FPS is frame-limited: the frontend keeps presenting at ~60 even
    # when the guest has fallen far behind, so FPS alone hides an interpreter
    # that cannot keep up. Guest VBlanks per host second against the DS's own
    # 59.8261 Hz is the honest speed number, and insn9/s is the raw
    # interpreter throughput the campaign is trying to move.
    seconds = result["seconds"] or 1e-9
    result["guest_vblanks_per_second"] = result["vblank9"] / seconds
    result["guest_speed_ratio"] = result["guest_vblanks_per_second"] / 59.8261
    result["insn9_per_second"] = result["insn9"] / seconds
    verify_phase(result, forced, min_share, max_share)
    print(
        f"{label}: {result['insn9']} ARM9 insns ({result['tier3_insn9_share']:.4f} "
        f"tier3) and {result['frames']} frames in {result['seconds']:.3f}s, "
        f"{result['fps']:.3f} FPS, emu {result['phase_ms_per_frame']['emu']:.2f} "
        f"ms/frame, guest speed {result['guest_speed_ratio']:.4f}x, "
        f"{result['insn9_per_second'] / 1e6:.2f} M insn9/s, "
        # summarize_window strips the _ns suffix from the scheduler keys.
        f"sched arm9={result.get('scheduler_sample_share', {}).get('arm9', 0):.3f} "
        f"arm7={result.get('scheduler_sample_share', {}).get('arm7', 0):.3f}",
        flush=True,
    )
    return result


def navigate_route(
    client: Any,
    process: subprocess.Popen[bytes],
    hold_frames: int,
    settle_frames: int,
    boot_timeout: float,
) -> None:
    bench.wait_until_vblank9(client, TITLE_VBLANK, boot_timeout, process)
    bench.touch(client, *TITLE_TAP, hold_frames, process)
    bench.advance_vblanks(client, TITLE_SETTLE_VBLANKS, process)
    actions = bench.load_actions(SCENARIO_DIR / SCENARIO)
    bench.replay_actions(
        client,
        actions,
        hold_frames=hold_frames,
        settle_frames=settle_frames,
        process=process,
        on_step=lambda index, label: print(
            f"  route step {index}/{len(actions)}: {label}", flush=True
        ),
    )


def run_repetition(
    args: argparse.Namespace, mode: str, repetition: int
) -> dict[str, Any]:
    forced = mode == "forced"
    stem = f"adventure-{mode}-{repetition:02d}"
    save_path = None
    if args.save_source is not None:
        save_path = args.output / f"{stem}.sav"
        shutil.copy2(args.save_source, save_path)

    extra_args = ["--boot", args.boot] + list(args.runner_arg)
    process, stdout_file, stderr_file = bench.launch_runtime(
        args.exe,
        args.bios,
        args.rom,
        args.port,
        args.output / f"{stem}.stdout.log",
        args.output / f"{stem}.stderr.log",
        config=args.config,
        save_path=save_path,
        startup_mode="automatic",
        profiled=bool(args.profile),
        threaded=bool(args.threaded),
        renderer=args.renderer,
        extra_args=extra_args,
    )
    run: dict[str, Any] = {
        "mode": mode,
        "route": "adventure",
        "repetition": repetition,
        "pid": process.pid,
        "valid": False,
        "phases": [],
    }
    client = None
    try:
        client = bench.wait_for_client(process, args.port)

        # The route itself is navigated faithfully in BOTH modes, so the two
        # legs enter the measured phases from the identical guest state.
        initial = selector_state(client)
        run["selector_at_launch"] = initial
        if initial["forced_tier3"]:
            raise RuntimeError(
                "selector was already on at launch; the route would be "
                f"navigated interpreted: {initial}"
            )
        navigate_route(
            client, process, args.hold_frames, args.settle_frames, args.boot_timeout
        )

        client.cmd("keys", mask=bench.KEYS_RELEASED & ~(1 << bench.KEY_BITS[HOLD_KEY]))
        try:
            if forced:
                run["selector_engaged"] = set_selector(client, True)
                # Forcing applies at dispatch boundaries. A recompiled function
                # already on the host stack keeps running until it returns, so
                # drain a short window before the first measured phase rather
                # than letting that native tail depress phase 1's tier share.
                anchor = client.event_counts()["insn9"]
                bench.wait_until_insn9(
                    client, anchor + args.drain_insn9, args.phase_timeout, process
                )
                run["drain_insn9"] = args.drain_insn9

            anchor = client.event_counts()["insn9"]
            previous = 0
            for label, cumulative in ADVENTURE_INSN_PHASES:
                run["phases"].append(
                    measure_to_insn9(
                        client,
                        process,
                        label,
                        anchor + cumulative,
                        cumulative - previous,
                        args.phase_timeout,
                        forced,
                        args.min_tier3_share,
                        args.max_tier3_share,
                    )
                )
                previous = cumulative
                if label == args.stop_after_phase:
                    break
        finally:
            client.cmd("keys", mask=bench.KEYS_RELEASED)

        run["final_event_counts"] = client.event_counts()
        run["final_dispatch_stats"] = client.cmd("dispatch_stats")
        run["final_static_coverage"] = client.cmd("static_coverage")
        run["valid"] = True
        try:
            if client.cmd("frontend_exit").get("requested"):
                process.wait(timeout=30.0)
        except (ConnectionError, RuntimeError, subprocess.TimeoutExpired):
            pass
        return run
    except bench.RunnerDied as error:
        run["error"] = f"runner died: {error}"
        print(f"[discarded] {mode} repetition {repetition}: {run['error']}", flush=True)
        return run
    except (TimeoutError, RuntimeError, ConnectionError, OSError) as error:
        run["error"] = f"{type(error).__name__}: {error}"
        print(f"[discarded] {mode} repetition {repetition}: {run['error']}", flush=True)
        return run
    finally:
        if client is not None:
            client.close()
        # Only ever this PID: the workspace runs concurrent sessions of the
        # same executable and an image-name kill would take theirs down too.
        bench.terminate_runtime(process)
        run["returncode"] = process.returncode
        stdout_file.close()
        stderr_file.close()


def summarize(runs: list[dict[str, Any]]) -> dict[str, Any]:
    summary: dict[str, Any] = {}
    for mode in ("forced", "normal"):
        valid = [r for r in runs if r["mode"] == mode and r["valid"]]
        phases: dict[str, dict[str, Any]] = {}
        for run in valid:
            for phase in run["phases"]:
                row = phases.setdefault(
                    phase["label"],
                    {
                        "label": phase["label"],
                        "fps_values": [],
                        "emu_ms_values": [],
                        "tier3_share_values": [],
                        "seconds_values": [],
                        "speed_ratio_values": [],
                        "insn9_per_second_values": [],
                    },
                )
                row["fps_values"].append(phase["fps"])
                row["emu_ms_values"].append(phase["phase_ms_per_frame"]["emu"])
                row["tier3_share_values"].append(phase["tier3_insn9_share"])
                row["seconds_values"].append(phase["seconds"])
                row["speed_ratio_values"].append(phase["guest_speed_ratio"])
                row["insn9_per_second_values"].append(phase["insn9_per_second"])
                row.setdefault("scheduler_sample_share", []).append(
                    phase.get("scheduler_sample_share", {})
                )
        for row in phases.values():
            for key in (
                "fps",
                "emu_ms",
                "tier3_share",
                "seconds",
                "speed_ratio",
                "insn9_per_second",
            ):
                values = sorted(row[f"{key}_values"])
                row[f"median_{key}"] = (
                    values[len(values) // 2] if values else None
                )
        summary[mode] = {
            "run_count": len(valid),
            "discarded_runs": len([r for r in runs if r["mode"] == mode])
            - len(valid),
            "phases": list(phases.values()),
        }
    forced_phases = {p["label"]: p for p in summary["forced"]["phases"]}
    normal_phases = {p["label"]: p for p in summary["normal"]["phases"]}
    summary["slowdown"] = [
        {
            "label": label,
            "normal_median_fps": normal_phases[label]["median_fps"],
            "forced_median_fps": forced_phases[label]["median_fps"],
            "fps_ratio": (
                normal_phases[label]["median_fps"] / forced_phases[label]["median_fps"]
                if forced_phases[label]["median_fps"]
                else None
            ),
            "normal_median_emu_ms": normal_phases[label]["median_emu_ms"],
            "forced_median_emu_ms": forced_phases[label]["median_emu_ms"],
            "normal_median_speed_ratio": normal_phases[label]["median_speed_ratio"],
            "forced_median_speed_ratio": forced_phases[label]["median_speed_ratio"],
            "speed_ratio_slowdown": (
                normal_phases[label]["median_speed_ratio"]
                / forced_phases[label]["median_speed_ratio"]
                if forced_phases[label]["median_speed_ratio"]
                else None
            ),
            "normal_median_insn9_per_second": normal_phases[label][
                "median_insn9_per_second"
            ],
            "forced_median_insn9_per_second": forced_phases[label][
                "median_insn9_per_second"
            ],
        }
        for label in forced_phases
        if label in normal_phases
    ]
    return summary


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--exe",
        type=pathlib.Path,
        default=FRAMEWORK_ROOT / "runner/build-interp-perf/nds_runner.exe",
    )
    parser.add_argument("--bios", type=pathlib.Path, default=WORKSPACE_ROOT / "ndsrecomp" / "bios")
    parser.add_argument(
        "--rom",
        type=pathlib.Path,
        default=WORKSPACE_ROOT
        / "metroidprimehuntersrecomp"
        / "Metroid Prime Hunters.nds",
    )
    parser.add_argument("--config", type=pathlib.Path, default=TARGET_ROOT / "game.toml")
    parser.add_argument("--output", type=pathlib.Path, default=None)
    # 19920+ per the campaign's port allocation; 19842/19843 are the oracle and
    # 19870/19871 belong to the mph-perf harnesses.
    parser.add_argument("--port", type=int, default=19920)
    parser.add_argument("--forced-repetitions", type=int, default=3)
    parser.add_argument("--normal-repetitions", type=int, default=1)
    parser.add_argument("--boot", choices=("direct", "lle"), default=DEFAULT_BOOT)
    parser.add_argument("--threaded", type=int, choices=(0, 1), default=1)
    parser.add_argument(
        "--renderer", choices=("auto", "soft", "compute"), default="auto"
    )
    parser.add_argument("--save-source", type=pathlib.Path, default=None)
    parser.add_argument("--hold-frames", type=int, default=3)
    parser.add_argument("--settle-frames", type=int, default=45)
    parser.add_argument("--boot-timeout", type=float, default=600.0)
    parser.add_argument("--phase-timeout", type=float, default=3600.0)
    parser.add_argument("--stop-after-phase", default=None)
    parser.add_argument("--drain-insn9", type=int, default=250_000)
    parser.add_argument("--min-tier3-share", type=float, default=0.90)
    parser.add_argument("--max-tier3-share", type=float, default=0.05)
    parser.add_argument("--runner-arg", action="append", default=[])
    parser.add_argument("--tag", default=None)
    # Arms NDS_PROFILE_GPU/NDS_PROFILE_SCHED. Costs a few percent, applied
    # equally to both legs. Worth it here: the scheduler shares are what
    # separate "the interpreter is fast enough" from "guest CPU execution is a
    # minority of frame time either way", and those have opposite implications
    # for the whole campaign.
    parser.add_argument("--profile", action="store_true")
    args = parser.parse_args()
    if args.output is None:
        stamp = time.strftime("%Y%m%d-%H%M%S")
        suffix = f"-{args.tag}" if args.tag else ""
        args.output = FRAMEWORK_ROOT / "perf-results" / f"{stamp}-forced-tier3{suffix}"
    return args


def main() -> int:
    args = parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report: dict[str, Any] = {
        "schema": "mph-forced-tier3/1",
        "created_local": time.strftime("%Y-%m-%d %H:%M:%S"),
        "bead": "beads-yjp.42",
        "route": {
            "name": "adventure",
            "scenario": SCENARIO,
            "title_vblank": TITLE_VBLANK,
            "title_tap": list(TITLE_TAP),
            "insn9_phases": [
                {"label": label, "cumulative_insn9": value}
                for label, value in ADVENTURE_INSN_PHASES
            ],
            "hold_key": HOLD_KEY,
        },
        "build": {
            "executable": str(args.exe),
            "executable_sha256": bench.sha256_file(args.exe),
            "config": str(args.config),
            "rom": str(args.rom),
            "framework_root": str(FRAMEWORK_ROOT),
            "boot": args.boot,
            "renderer": args.renderer,
            "profiled": bool(args.profile),
        },
        "verification": {
            "min_tier3_share": args.min_tier3_share,
            "max_tier3_share": args.max_tier3_share,
            "drain_insn9": args.drain_insn9,
        },
        "host": bench.host_description(),
        "runs": [],
    }

    # Faithful control first: if the runner cannot reach the route natively,
    # nothing about the forced numbers would be interpretable anyway.
    plan = [("normal", i) for i in range(1, args.normal_repetitions + 1)]
    plan += [("forced", i) for i in range(1, args.forced_repetitions + 1)]
    for mode, repetition in plan:
        print(f"\n=== {mode} repetition {repetition} ===", flush=True)
        report["runs"].append(run_repetition(args, mode, repetition))
        report["summary"] = summarize(report["runs"])
        bench.write_json(args.output / "report.partial.json", report)

    report["summary"] = summarize(report["runs"])
    bench.write_json(args.output / "report.json", report)
    (args.output / "report.partial.json").unlink(missing_ok=True)
    print("\n" + json.dumps(report["summary"], indent=2), flush=True)
    print(f"\nreport: {args.output / 'report.json'}", flush=True)
    invalid = [r for r in report["runs"] if not r["valid"]]
    return 1 if invalid else 0


if __name__ == "__main__":
    raise SystemExit(main())
