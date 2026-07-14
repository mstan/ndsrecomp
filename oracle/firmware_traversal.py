"""Deterministic native-vs-melonDS firmware-menu traversal.

Actions come from firmware_traversal.json.  Both machines are always advanced
concurrently to absolute hardware-event ordinals; input is changed only while
both are stopped at the preceding ordinal.  Any failed stop, architectural
counter/RTC mismatch, or framebuffer byte mismatch terminates immediately.

This is a comparison harness, not a synchronization mechanism for the guest:
the runtime remains one dual-CPU hardware-event scheduler, and the observers
are queried only after the requested event has already happened.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

from _client import DebugClient, first_divergence


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = Path(__file__).with_name("firmware_traversal.json")


class TraversalFailure(RuntimeError):
    pass


class PairedMachines:
    def __init__(self, native_port: int, oracle_port: int, timeout: float):
        self.native = DebugClient(port=native_port, timeout=timeout)
        self.oracle = DebugClient(port=oracle_port, timeout=timeout)
        self.pool = ThreadPoolExecutor(max_workers=2)

    def close(self) -> None:
        self.native.close()
        self.oracle.close()
        self.pool.shutdown()

    def command(self, name: str, **args: Any) -> tuple[Any, Any]:
        futures = [
            self.pool.submit(client.cmd, name, **args)
            for client in (self.native, self.oracle)
        ]
        return futures[0].result(), futures[1].result()

    def framebuffers(self, engine: str) -> tuple[bytes, bytes]:
        futures = [
            self.pool.submit(client.framebuffer, engine)
            for client in (self.native, self.oracle)
        ]
        native = futures[0].result()
        oracle = futures[1].result()
        if native[:2] != oracle[:2]:
            raise TraversalFailure(
                f"framebuffer {engine} geometry differs: "
                f"native={native[:2]} oracle={oracle[:2]}"
            )
        return native[2], oracle[2]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha1(data: bytes) -> str:
    return hashlib.sha1(data).hexdigest()


def capture_static_variant(
    pair: PairedMachines,
    out_dir: Path,
    scenario_name: str,
) -> Path:
    """Persist the exact LLE-produced executable images at scenario end.

    These files are recompiler inputs only. Runtime dispatch still validates
    every emitted function against live guest RAM, so capturing a late
    generation cannot make it active before the firmware installs those
    bytes.
    """
    safe_name = re.sub(r"[^A-Za-z0-9_]+", "_", scenario_name).strip("_")
    if not safe_name:
        raise TraversalFailure("scenario name cannot form a capture id")
    out_dir.mkdir(parents=True, exist_ok=True)

    mainram = pair.native.read_region("mainram")
    itcm = pair.native.read_region("itcm")
    shared = pair.native.read_region("wramshared")
    wram7 = pair.native.read_region("wram7")
    if (len(mainram), len(itcm), len(shared), len(wram7)) != (
        0x400000, 0x8000, 0x8000, 0x10000
    ):
        raise TraversalFailure(
            "unexpected capture geometry: "
            f"mainram={len(mainram):#x} itcm={len(itcm):#x} "
            f"shared={len(shared):#x} wram7={len(wram7):#x}"
        )

    # ARM9's high ITCM mirror is contiguous with main RAM. ARM7 needs the
    # same main-RAM bytes compiled for ARMv4T plus its discontiguous WRAM;
    # the config maps the appended WRAM source back to 0x037F8000.
    images = {
        f"fw_arm9_{safe_name}": itcm + mainram,
        f"fw_arm7_{safe_name}": mainram + shared + wram7,
    }
    banks: list[dict[str, Any]] = []
    for bank_id, data in images.items():
        path = out_dir / f"{bank_id}.bin"
        path.write_bytes(data)
        banks.append({
            "id": bank_id,
            "cpu": 9 if bank_id.startswith("fw_arm9_") else 7,
            "path": str(path.resolve()),
            "size": len(data),
            "sha1": sha1(data),
            "sha256": sha256(data),
        })

    coverage = pair.native.cmd("tier3_coverage", max=262144)["entries"]
    roots = {
        str(cpu): next(
            (item for item in coverage
             if int(item["cpu"]) == cpu and int(item["kind"]) == 1),
            None,
        )
        for cpu in (9, 7)
    }
    metadata = {
        "format": 1,
        "method": "native LLE scenario traversal capture",
        "scenario": scenario_name,
        "safe_name": safe_name,
        "event_counts": pair.native.cmd("event_counts"),
        "static_coverage": pair.native.cmd("static_coverage"),
        "coverage": coverage,
        "roots": roots,
        "banks": banks,
        "source_layout": {
            "arm9": {
                "0x01FF8000": "ARM9 ITCM high mirror (32 KiB)",
                "0x02000000": "main RAM (4 MiB)",
            },
            "arm7": {
                "0x02000000": "main RAM (4 MiB), compiled as ARMv4T",
                "0x02400000": "source copy of shared WRAM (32 KiB)",
                "0x02408000": "source copy of ARM7 WRAM (64 KiB)",
            },
        },
    }
    manifest_path = out_dir / f"{safe_name}_capture.json"
    manifest_path.write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return manifest_path


def check_frame(
    pair: PairedMachines,
    engine: str,
    expected: str | None,
    where: str,
) -> str:
    native, oracle = pair.framebuffers(engine)
    if native != oracle:
        diff = first_divergence(native, oracle)
        assert diff is not None
        index, native_byte, oracle_byte = diff
        pixel = index // 3
        raise TraversalFailure(
            f"{where}: framebuffer {engine} differs at "
            f"({pixel % 256},{pixel // 256}) channel={index % 3}: "
            f"native={native_byte} oracle={oracle_byte}; "
            f"sha256 native={sha256(native)} oracle={sha256(oracle)}"
        )
    digest = sha256(native)
    if expected is not None and digest != expected:
        raise TraversalFailure(
            f"{where}: framebuffer {engine} agrees between machines but "
            f"does not match the manifest: got {digest}, expected {expected}"
        )
    return digest


def checkpoint(
    pair: PairedMachines,
    action: dict[str, Any],
    ignored_counters: set[str],
    where: str,
) -> dict[str, Any]:
    result: dict[str, Any] = {}
    if action.get("counts", True):
        native, oracle = pair.command("event_counts")
        differing = {
            key: [native[key], oracle[key]]
            for key in sorted(native.keys() & oracle.keys())
            if key not in ignored_counters and native[key] != oracle[key]
        }
        if differing:
            raise TraversalFailure(f"{where}: event counters differ: {differing}")
        result["counts"] = native

    if action.get("rtc", True):
        native, oracle = pair.command("rtc_state")
        if native != oracle:
            raise TraversalFailure(
                f"{where}: RTC differs: native={native} oracle={oracle}"
            )
        expected_rtc = action.get("expected_rtc", {})
        unexpected = {
            key: [native.get(key), value]
            for key, value in expected_rtc.items()
            if native.get(key) != value
        }
        if unexpected:
            raise TraversalFailure(
                f"{where}: RTC agrees between machines but does not match "
                f"the manifest: {unexpected}"
            )
        result["rtc"] = native

    frame_hashes: dict[str, str] = {}
    expected = action.get("expected_frames", {})
    for engine in action.get("frames", ["A", "B"]):
        frame_hashes[engine] = check_frame(
            pair, engine, expected.get(engine), where
        )
    if frame_hashes:
        result["frames"] = frame_hashes
    return result


def run_action(
    pair: PairedMachines,
    action: dict[str, Any],
    ignored_counters: set[str],
    index: int,
) -> dict[str, Any] | None:
    kind = action["action"]
    where = f"action {index} ({kind})"

    if kind == "reset":
        native, oracle = pair.command("reset")
        if not native.get("ok") or not oracle.get("ok"):
            raise TraversalFailure(f"{where}: reset failed: {native}, {oracle}")
        return None

    if kind == "keys":
        native, oracle = pair.command("keys", mask=int(action["mask"]))
        if not native.get("ok") or not oracle.get("ok"):
            raise TraversalFailure(f"{where}: keys failed: {native}, {oracle}")
        return None

    if kind == "touch":
        native, oracle = pair.command(
            "touch",
            x=int(action["x"]),
            y=int(action["y"]),
            down=bool(action["down"]),
        )
        if not native.get("ok") or not oracle.get("ok"):
            raise TraversalFailure(f"{where}: touch failed: {native}, {oracle}")
        return None

    if kind == "run":
        event = str(action["event"])
        count = int(action["count"])
        native, oracle = pair.command("run_to_event", event=event, count=count)
        if not native.get("reached") or not oracle.get("reached"):
            raise TraversalFailure(
                f"{where}: failed to reach {event}={count}: "
                f"native={native} oracle={oracle}"
            )
        return {"event": event, "count": count}

    if kind == "run_until_stall":
        event = str(action["event"])
        count = int(action["count"])
        native, oracle = pair.command("run_to_event", event=event, count=count)
        if (native.get("reached") or oracle.get("reached") or
                not native.get("stalled") or not oracle.get("stalled")):
            raise TraversalFailure(
                f"{where}: expected both machines to stop before "
                f"{event}={count}: native={native} oracle={oracle}"
            )
        native_counts = native.get("counts", {})
        oracle_counts = oracle.get("counts", {})
        differing = {
            key: [native_counts[key], oracle_counts[key]]
            for key in sorted(native_counts.keys() & oracle_counts.keys())
            if key not in ignored_counters and
            native_counts[key] != oracle_counts[key]
        }
        if differing:
            raise TraversalFailure(
                f"{where}: terminal counters differ: {differing}"
            )
        expected = action.get("expected_counts", {})
        unexpected = {
            key: [native_counts.get(key), value]
            for key, value in expected.items()
            if native_counts.get(key) != value
        }
        if unexpected:
            raise TraversalFailure(
                f"{where}: terminal counters do not match the manifest: "
                f"{unexpected}"
            )
        return {
            "event": event,
            "unreached_count": count,
            "terminal_counts": native_counts,
        }

    if kind == "checkpoint":
        return checkpoint(pair, action, ignored_counters, where)

    if kind == "scan_vblank9":
        first = int(action["first"])
        last = int(action["last"])
        engines = action.get("frames", ["A", "B"])
        for ordinal in range(first, last + 1):
            native, oracle = pair.command(
                "run_to_event", event="vblank9", count=ordinal
            )
            if not native.get("reached") or not oracle.get("reached"):
                raise TraversalFailure(
                    f"{where}: failed to reach vblank9={ordinal}: "
                    f"native={native} oracle={oracle}"
                )
            for engine in engines:
                check_frame(pair, engine, None, f"{where}, vblank9={ordinal}")
            if action.get("counts", True):
                native_counts, oracle_counts = pair.command("event_counts")
                differing = {
                    key: [native_counts[key], oracle_counts[key]]
                    for key in sorted(native_counts.keys() & oracle_counts.keys())
                    if key not in ignored_counters and
                    native_counts[key] != oracle_counts[key]
                }
                if differing:
                    raise TraversalFailure(
                        f"{where}, vblank9={ordinal}: event counters differ: "
                        f"{differing}"
                    )
            if action.get("rtc", True):
                native_rtc, oracle_rtc = pair.command("rtc_state")
                if native_rtc != oracle_rtc:
                    raise TraversalFailure(
                        f"{where}, vblank9={ordinal}: RTC differs: "
                        f"native={native_rtc} oracle={oracle_rtc}"
                    )
        return {"first": first, "last": last, "frames": engines}

    raise TraversalFailure(f"{where}: unknown action {kind!r}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--scenario", default="calibration_save")
    parser.add_argument("--native-port", type=int, default=19842)
    parser.add_argument("--oracle-port", type=int, default=19843)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument(
        "--skip-frame-scan",
        action="store_true",
        help="skip expensive scan_vblank9 actions (checkpoints still compare)",
    )
    parser.add_argument(
        "--capture-static-dir",
        type=Path,
        help=("after a passing traversal, capture LLE-produced RAM images and "
              "Tier-3 coverage for content-validated static variants"),
    )
    parser.add_argument(
        "--require-zero-tier3",
        action="store_true",
        help=("fail unless both CPUs complete the traversal with zero Tier-3 "
              "entries/instructions and zero rejected clean-RAM targets"),
    )
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    try:
        scenario = manifest["scenarios"][args.scenario]
    except KeyError as exc:
        raise SystemExit(f"unknown scenario {args.scenario!r}") from exc

    ignored = set(manifest.get("ignored_counter_differences", []))
    pair = PairedMachines(args.native_port, args.oracle_port, args.timeout)
    report: list[dict[str, Any]] = []
    capture_path: Path | None = None
    static_coverage: dict[str, Any] | None = None
    try:
        for index, action in enumerate(scenario["actions"], 1):
            if args.skip_frame_scan and action["action"] == "scan_vblank9":
                # Preserve the scenario's absolute input timeline.  Skipping
                # a scan means omitting the per-frame comparisons, not
                # applying the next input hundreds of VBlanks early.
                action = {
                    "action": "run",
                    "event": "vblank9",
                    "count": int(action["last"]),
                }
            result = run_action(pair, action, ignored, index)
            if result is not None:
                report.append({"index": index, "action": action["action"], **result})
            print(f"[{index:02d}] {action['action']}: ok", flush=True)
        static_coverage = pair.native.cmd("static_coverage")
        if args.require_zero_tier3:
            nonzero = {
                key: int(value)
                for key, value in static_coverage.items()
                if key.startswith(("tier3_", "clean_ram_rejects"))
                and int(value) != 0
            }
            if nonzero:
                raise TraversalFailure(
                    "release static-coverage gate is nonzero: "
                    + json.dumps(nonzero, sort_keys=True)
                )
        if args.capture_static_dir is not None:
            capture_path = capture_static_variant(
                pair, args.capture_static_dir, args.scenario
            )
            print(f"[capture] {capture_path}", flush=True)
    except TraversalFailure as exc:
        print(f"FAIL: {exc}", flush=True)
        return 1
    finally:
        pair.close()

    print(
        json.dumps(
            {
                "status": "pass",
                "scenario": args.scenario,
                "description": scenario.get("description", ""),
                "static_capture": str(capture_path) if capture_path else None,
                "static_coverage": static_coverage,
                "report": report,
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
