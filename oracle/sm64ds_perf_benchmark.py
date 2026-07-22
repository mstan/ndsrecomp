#!/usr/bin/env python3
"""Run the deterministic headless SM64DS performance route.

The runner must already be serving the SM64DS ROM through ``--serve``.
This client deliberately refuses an interactive frontend so its elapsed time
measures the reset-to-target emulation route without presentation or audio
pacing.
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


DEFAULT_PORT = 19842
DEFAULT_TARGET_INSN9 = 700_000_000
DEFAULT_STALL = 300_000


def positive_int(value: str) -> int:
    parsed = int(value, 0)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def port_number(value: str) -> int:
    parsed = int(value, 0)
    if not 1 <= parsed <= 65535:
        raise argparse.ArgumentTypeError("must be in the range 1..65535")
    return parsed


def require_reached(stage: str, result: dict[str, Any]) -> None:
    if result.get("reached") and not result.get("terminal"):
        return
    raise RuntimeError(f"SM64DS route failed at {stage}: {result}")


def run_to_vblank(client: DebugClient, count: int, stall: int) -> None:
    result = client.cmd(
        "run_to_event", event="vblank9", count=count, stall=stall
    )
    require_reached(f"vblank9={count}", result)


def navigate_sm64ds_title(client: DebugClient, stall: int) -> None:
    """Follow the byte-locked boot/title/attract route used by G3 probes."""
    client.cmd("reset")
    run_to_vblank(client, 100, stall)
    client.cmd("touch", x=128, y=48, down=True)
    run_to_vblank(client, 102, stall)
    client.cmd("touch", x=128, y=48, down=False)
    run_to_vblank(client, 900, stall)
    client.cmd("touch", x=128, y=120, down=True)
    run_to_vblank(client, 930, stall)
    client.cmd("touch", x=128, y=120, down=False)


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

        started_ns = time.perf_counter_ns()
        navigate_sm64ds_title(client, args.stall)
        target_result = client.cmd(
            "run_to_event", event="insn9", count=args.target_insn9,
            stall=args.stall
        )
        elapsed_ns = time.perf_counter_ns() - started_ns
        require_reached(f"insn9={args.target_insn9}", target_result)

        event_counts = client.event_counts()
        sched_state = client.cmd("sched_state")
        if "fast_context" not in sched_state:
            raise RuntimeError("sched_state response lacks fast_context")

        return {
            "schema": "ndsrecomp.sm64ds_perf.v1",
            "route": "reset-title-attract-insn9",
            "target_insn9": args.target_insn9,
            "elapsed_s": elapsed_ns / 1_000_000_000.0,
            "elapsed_ns": elapsed_ns,
            "elapsed_scope": "headless reset through target insn9",
            "target_result": target_result,
            "event_counts": event_counts,
            "sched_state": sched_state,
            "hle_status": client.cmd("hle_status"),
            "coverage": client.cmd("static_coverage"),
            "profile": client.cmd("profile"),
            "hle_profile": client.cmd("hle_heat"),
        }
    finally:
        client.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Benchmark the deterministic SM64DS boot/title/attract route "
            "against a headless nds_runner debug server."
        )
    )
    parser.add_argument("--host", default="127.0.0.1",
                        help="debug server host (default: %(default)s)")
    parser.add_argument("--port", type=port_number, default=DEFAULT_PORT,
                        help="debug server port (default: %(default)s)")
    parser.add_argument(
        "--target-insn9", type=positive_int, default=DEFAULT_TARGET_INSN9,
        help="absolute ARM9 instruction target (default: %(default)s)"
    )
    parser.add_argument(
        "--stall", type=positive_int, default=DEFAULT_STALL,
        help="server event-stall limit (default: %(default)s)"
    )
    parser.add_argument(
        "--timeout", type=float, default=600.0,
        help="socket timeout in seconds (default: %(default)s)"
    )
    parser.add_argument("--pretty", action="store_true",
                        help="indent the JSON output")
    parser.add_argument("--output", type=Path,
                        help="write JSON to this path instead of stdout")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    record = benchmark(args)
    rendered = json.dumps(
        record, indent=2 if args.pretty else None, sort_keys=True
    ) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")


if __name__ == "__main__":
    main()
