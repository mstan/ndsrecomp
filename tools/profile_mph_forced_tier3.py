"""RIP-sample the MPH adventure_steady phase with everything interpreted.

Phase 0 attribution profile for beads-yjp.42. A modified copy of
metroidprimehuntersrecomp-mph-perf/tools/profile_mph_worst_phase.py: same
sampler, same symbolizer, same route, but the selector is engaged before the
sampled window so the profile describes the Tier 3 interpreter rather than the
recompiled banks.

The route is navigated FAITHFULLY (see measure_mph_forced_tier3.py for why),
the selector is flipped at the phase boundary, a drain window lets native
frames on the host stack retire, and only then does sampling start. The tier
share over the sampled window is recorded in the report and gated the same way
the measurement harness gates it -- an attribution profile of the wrong tier is
worse than no profile, because it looks authoritative.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import time
from typing import Any

FRAMEWORK_ROOT = pathlib.Path(__file__).resolve().parents[1]
WORKSPACE_ROOT = FRAMEWORK_ROOT.parent
sys.path.insert(0, str(FRAMEWORK_ROOT / "tools"))

import scenario_bench as bench  # noqa: E402
import measure_mph_forced_tier3 as route  # noqa: E402

# The sampler source is generic and already vendored in the SM64DS/MPH tool
# directories; use the framework copy if one exists, else the SM64DS original.
SAMPLER_SOURCE_CANDIDATES = (
    FRAMEWORK_ROOT / "tools" / "windows_rip_sampler.cpp",
    WORKSPACE_ROOT / "supermario64dsrecomp" / "tools" / "windows_rip_sampler.cpp",
)


def sampler_source() -> pathlib.Path:
    for candidate in SAMPLER_SOURCE_CANDIDATES:
        if candidate.exists():
            return candidate
    raise RuntimeError("no windows_rip_sampler.cpp found")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--exe",
        type=pathlib.Path,
        default=FRAMEWORK_ROOT / "runner/build-interp-perf/nds_runner.exe",
    )
    parser.add_argument(
        "--bios", type=pathlib.Path, default=WORKSPACE_ROOT / "ndsrecomp" / "bios"
    )
    parser.add_argument(
        "--rom",
        type=pathlib.Path,
        default=WORKSPACE_ROOT
        / "metroidprimehuntersrecomp"
        / "Metroid Prime Hunters.nds",
    )
    parser.add_argument(
        "--config", type=pathlib.Path, default=route.TARGET_ROOT / "game.toml"
    )
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--port", type=int, default=19921)
    parser.add_argument("--interval-us", type=int, default=1000)
    parser.add_argument("--boot", default=route.DEFAULT_BOOT)
    parser.add_argument("--threaded", type=int, choices=(0, 1), default=1)
    parser.add_argument("--renderer", default="auto")
    parser.add_argument("--hold-frames", type=int, default=3)
    parser.add_argument("--settle-frames", type=int, default=45)
    parser.add_argument("--boot-timeout", type=float, default=600.0)
    parser.add_argument("--phase-timeout", type=float, default=3600.0)
    parser.add_argument("--drain-insn9", type=int, default=250_000)
    parser.add_argument("--min-tier3-share", type=float, default=0.90)
    # Sample the settled-gameplay phase: the interval between the walk and
    # steady landmarks, which is the largest and least transient window.
    parser.add_argument("--sample-insn9", type=int, default=85_000_000)
    parser.add_argument("--no-force", action="store_true",
                        help="profile the faithful path instead (control)")
    parser.add_argument("--no-symbolize", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    sampler_exe = bench.build_sampler(
        sampler_source(), FRAMEWORK_ROOT / "tools" / "windows_rip_sampler.exe"
    )
    samples_path = args.output / "rip-samples.csv"

    process, stdout_file, stderr_file = bench.launch_runtime(
        args.exe,
        args.bios,
        args.rom,
        args.port,
        args.output / "runner.stdout.log",
        args.output / "runner.stderr.log",
        config=args.config,
        save_path=None,
        startup_mode="automatic",
        profiled=False,
        threaded=bool(args.threaded),
        renderer=args.renderer,
        extra_args=["--boot", args.boot],
    )
    report: dict[str, Any] = {
        "schema": "mph-forced-tier3-profile/1",
        "created_local": time.strftime("%Y-%m-%d %H:%M:%S"),
        "bead": "beads-yjp.42",
        "forced": not args.no_force,
        "executable": str(args.exe),
        "executable_sha256": bench.sha256_file(args.exe),
        "sampler": {"interval_us": args.interval_us},
        "host": bench.host_description(),
    }
    client = None
    sampler = None
    try:
        client = bench.wait_for_client(process, args.port)
        route.navigate_route(
            client, process, args.hold_frames, args.settle_frames, args.boot_timeout
        )
        client.cmd(
            "keys",
            mask=bench.KEYS_RELEASED & ~(1 << bench.KEY_BITS[route.HOLD_KEY]),
        )
        if not args.no_force:
            report["selector_engaged"] = route.set_selector(client, True)
            anchor = client.event_counts()["insn9"]
            bench.wait_until_insn9(
                client, anchor + args.drain_insn9, args.phase_timeout, process
            )
        # Replay the settle+walk landmarks so the sampled window is the same
        # settled gameplay the measurement harness calls adventure_steady.
        anchor = client.event_counts()["insn9"]
        bench.wait_until_insn9(
            client,
            anchor + route.ADVENTURE_INSN_PHASES[1][1],
            args.phase_timeout,
            process,
        )

        before_front = bench.live_stats(client)
        before_profile = bench.profile_snapshot(client)
        before_counts = client.event_counts()
        before_cov = client.cmd("static_coverage")
        before_sel = route.selector_state(client)
        sampler = bench.start_sampler(
            sampler_exe, process.pid, samples_path, args.interval_us
        )
        bench.wait_until_insn9(
            client,
            int(before_counts["insn9"]) + args.sample_insn9,
            args.phase_timeout,
            process,
        )
        bench.stop_sampler(sampler)
        sampler = None
        after_front = bench.live_stats(client)
        after_profile = bench.profile_snapshot(client)
        after_counts = client.event_counts()
        after_cov = client.cmd("static_coverage")
        after_sel = route.selector_state(client)

        phase = bench.summarize_window(
            "forced_tier3_steady" if not args.no_force else "faithful_steady",
            before_front,
            after_front,
            before_profile,
            after_profile,
        )
        phase["insn9"] = after_counts["insn9"] - before_counts["insn9"]
        phase["insn7"] = after_counts["insn7"] - before_counts["insn7"]
        phase["vblank9"] = after_counts["vblank9"] - before_counts["vblank9"]
        phase["tier3_insns9"] = after_cov["tier3_insns9"] - before_cov["tier3_insns9"]
        phase["tier3_insns7"] = after_cov["tier3_insns7"] - before_cov["tier3_insns7"]
        phase["forced_tier3"] = bool(after_sel["forced_tier3"])
        phase["forced_tier3_misses_delta"] = (
            after_sel["forced_tier3_misses"] - before_sel["forced_tier3_misses"]
        )
        report["phase"] = phase
        report["dispatch_stats"] = client.cmd("dispatch_stats")
        # Gate on the same witness the measurement harness uses. Record the
        # verdict rather than aborting: the samples are already on disk and a
        # failed profile is still evidence about what actually ran.
        route.verify_phase(
            phase,
            not args.no_force,
            args.min_tier3_share,
            1.0 if args.no_force else 0.05,
        )
        print(
            f"sampled window: {phase['insn9']} ARM9 insns "
            f"({phase['tier3_insn9_share']:.4f} tier3), {phase['frames']} frames "
            f"in {phase['seconds']:.3f}s, {phase['fps']:.3f} FPS",
            flush=True,
        )
        try:
            if client.cmd("frontend_exit").get("requested"):
                process.wait(timeout=30.0)
        except Exception:
            pass
    finally:
        if sampler is not None:
            try:
                bench.stop_sampler(sampler)
            except Exception:
                pass
        if client is not None:
            client.close()
        bench.terminate_runtime(process)
        stdout_file.close()
        stderr_file.close()

    runtime_base, by_thread = bench.parse_samples(samples_path)
    report["sampler"]["runtime_image_base"] = runtime_base
    preferred = bench.preferred_image_base(args.exe)
    report["sampler"]["preferred_image_base"] = preferred
    if not args.no_symbolize:
        report.update(bench.symbolize(args.exe, runtime_base, preferred, by_thread))
    bench.write_json(args.output / "report.json", report)
    top = report.get("aggregate_top_runner_symbols", [])
    total = sum(row["samples"] for row in top) or 1
    for row in top[:30]:
        print(f"  {row['samples'] / total:7.2%}  {row['samples']:8d}  {row['function']}")
    print(f"\nreport: {args.output / 'report.json'}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
