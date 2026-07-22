#!/usr/bin/env python3
"""Benchmark a coarse SM64DS cycle window after deterministic title routing.

The existing insn9 benchmark intentionally arms an exact instruction-event
breakpoint for the measured segment. Atomic HLE must reject while that break is
armed, so this variant measures a coarse ARM9 cycle window with no event break.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _client import DebugClient
from sm64ds_perf_benchmark import (
    DEFAULT_PORT,
    DEFAULT_STALL,
    navigate_sm64ds_title,
    port_number,
    positive_int,
    require_reached,
)


DEFAULT_WINDOW_CYCLES9 = 100_000_000


def candidate_key(candidate: dict[str, Any]) -> tuple[str, str, str]:
    return (
        str(candidate.get("id", "")),
        str(candidate.get("bank", "")),
        str(candidate.get("handler", "")),
    )


def subtract_counter_dict(
        after: dict[str, Any], before: dict[str, Any]) -> dict[str, int]:
    delta: dict[str, int] = {}
    for key, value in after.items():
        if isinstance(value, int) and isinstance(before.get(key), int):
            delta[key] = value - before[key]
    return delta


def hle_delta(after: dict[str, Any], before: dict[str, Any]) -> list[dict[str, Any]]:
    before_by_key = {
        candidate_key(candidate): candidate
        for candidate in before.get("candidates", [])
    }
    rows: list[dict[str, Any]] = []
    for candidate in after.get("candidates", []):
        prior = before_by_key.get(candidate_key(candidate), {})
        row: dict[str, Any] = {
            "id": candidate.get("id", ""),
            "bank": candidate.get("bank", ""),
            "handler": candidate.get("handler", ""),
        }
        row.update(subtract_counter_dict(candidate, prior))
        rows.append(row)
    return rows


def function_key(function: dict[str, Any]) -> tuple[str, str, int, int, int, str]:
    return (
        str(function.get("name", "")),
        str(function.get("bank", "")),
        int(function.get("cpu", 0)),
        int(function.get("address", 0)),
        int(function.get("end_address", 0)),
        str(function.get("content_sha1", "")),
    )


def function_heat_delta(
        after: dict[str, Any], before: dict[str, Any]) -> dict[str, Any]:
    before_by_key = {
        function_key(function): function
        for function in before.get("functions", [])
    }
    stride = int(after.get("sample_stride", 0))
    rows: list[dict[str, Any]] = []
    total_samples = 0
    for function in after.get("functions", []):
        prior = before_by_key.get(function_key(function), {})
        samples = int(function.get("samples", 0)) - int(prior.get("samples", 0))
        if samples <= 0:
            continue
        row = {
            "name": function.get("name", ""),
            "bank": function.get("bank", ""),
            "cpu": function.get("cpu", 0),
            "address": function.get("address", 0),
            "end_address": function.get("end_address", 0),
            "thumb": function.get("thumb", 0),
            "content_sha1": function.get("content_sha1", ""),
            "samples": samples,
            "estimated_instructions": samples * stride,
        }
        total_samples += samples
        rows.append(row)
    rows.sort(key=lambda row: (
        -int(row["samples"]), str(row["bank"]), int(row["address"])))
    return {
        "enabled": after.get("enabled", False),
        "sample_log2": after.get("sample_log2", 0),
        "sample_phase": after.get("sample_phase", 0),
        "sample_stride": stride,
        "total_samples": total_samples,
        "functions": rows,
    }


def benchmark(args: argparse.Namespace) -> dict[str, Any]:
    client = DebugClient(host=args.host, port=args.port, timeout=args.timeout)
    try:
        client.ping()
        frontend = client.cmd("frontend_stats")
        if frontend.get("active"):
            raise RuntimeError(
                "the debug server owns an interactive frontend; start the "
                "runner with --serve for a frontend-independent benchmark"
            )

        navigate_started_ns = time.perf_counter_ns()
        navigate_sm64ds_title(client, args.stall)
        navigate_elapsed_ns = time.perf_counter_ns() - navigate_started_ns

        before_counts = client.event_counts()
        before_sched = client.cmd("sched_state")
        before_hle = client.cmd("hle_status")
        before_function_heat = client.cmd("function_heat")
        target_arm9 = int(before_sched["arm9"]) + args.window_cycles9

        started_ns = time.perf_counter_ns()
        if args.rounds:
            target_result = client.cmd("run_rounds", count=args.rounds)
            reached = True
        else:
            target_result = client.cmd("run_cycles", arm9=target_arm9)
            reached = bool(target_result.get("reached"))
        elapsed_ns = time.perf_counter_ns() - started_ns
        if not reached:
            raise RuntimeError(f"SM64DS cycle window failed: {target_result}")

        after_counts = client.event_counts()
        after_sched = client.cmd("sched_state")
        after_hle = client.cmd("hle_status")
        after_function_heat = client.cmd("function_heat")
        if "fast_context" not in after_sched:
            raise RuntimeError("sched_state response lacks fast_context")

        return {
            "schema": "ndsrecomp.sm64ds_cycle_window.v1",
            "route": "reset-title-attract-cycle-window",
            "window_rounds": args.rounds,
            "window_cycles9": args.window_cycles9,
            "target_arm9": target_arm9,
            "navigate_elapsed_s": navigate_elapsed_ns / 1_000_000_000.0,
            "window_elapsed_s": elapsed_ns / 1_000_000_000.0,
            "window_elapsed_ns": elapsed_ns,
            "target_result": target_result,
            "event_delta": subtract_counter_dict(after_counts, before_counts),
            "event_counts": after_counts,
            "sched_state": after_sched,
            "hle_delta": hle_delta(after_hle, before_hle),
            "hle_status": after_hle,
            "coverage": client.cmd("static_coverage"),
            "profile": client.cmd("profile"),
            "hle_profile": client.cmd("hle_heat"),
            "function_heat_delta": function_heat_delta(
                after_function_heat, before_function_heat),
            "function_heat": after_function_heat,
        }
    finally:
        client.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Benchmark an SM64DS post-title ARM9 cycle window without an "
            "exact event breakpoint, allowing atomic HLE to run."
        )
    )
    parser.add_argument("--host", default="127.0.0.1",
                        help="debug server host (default: %(default)s)")
    parser.add_argument("--port", type=port_number, default=DEFAULT_PORT,
                        help="debug server port (default: %(default)s)")
    parser.add_argument(
        "--window-cycles9", type=positive_int, default=DEFAULT_WINDOW_CYCLES9,
        help="ARM9 cycles to run after title routing (default: %(default)s)"
    )
    parser.add_argument(
        "--rounds", type=positive_int, default=0,
        help=(
            "scheduler rounds to run after title routing; when set, this "
            "uses run_rounds instead of run_cycles (default: %(default)s)"
        )
    )
    parser.add_argument(
        "--stall", type=positive_int, default=DEFAULT_STALL,
        help="server event-stall limit while routing (default: %(default)s)"
    )
    parser.add_argument(
        "--timeout", type=float, default=600.0,
        help="socket timeout in seconds (default: %(default)s)"
    )
    parser.add_argument("--pretty", action="store_true",
                        help="pretty-print JSON")
    parser.add_argument("--output", type=Path,
                        help="write JSON to this path instead of stdout")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        result = benchmark(args)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    text = json.dumps(result, indent=2 if args.pretty else None,
                      sort_keys=True)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
