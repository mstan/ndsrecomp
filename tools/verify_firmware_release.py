#!/usr/bin/env python3
"""Cold-process firmware release matrix with retained identity evidence.

Each scenario gets a new native runner and melonDS oracle process so mutable
firmware flash, terminal power state, and debug counters cannot leak between
tests. The underlying traversal remains the architectural authority: it
advances both machines to identical hardware-event ordinals, compares frames
and checkpoints, and mechanically requires zero Tier-3 work.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import socket
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TRAVERSAL = ROOT / "oracle" / "firmware_traversal.json"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_identity(path: Path) -> dict[str, Any]:
    resolved = path.resolve()
    return {
        "path": str(resolved),
        "size": resolved.stat().st_size,
        "sha256": sha256_file(resolved),
    }


def git_output(*args: str) -> str:
    result = subprocess.run(
        ["git", *args], cwd=ROOT, text=True, capture_output=True, check=False
    )
    return result.stdout.strip()


def port_is_free(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind(("127.0.0.1", port))
        except OSError:
            return False
    return True


def wait_for_server(process: subprocess.Popen[bytes], port: int,
                    timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                f"server for port {port} exited with {process.returncode}"
            )
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.25):
                return
        except OSError:
            time.sleep(0.1)
    raise TimeoutError(f"server did not listen on 127.0.0.1:{port}")


def stop_process(process: subprocess.Popen[bytes] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5.0)


def parse_pass_record(log_path: Path) -> dict[str, Any]:
    for line in reversed(log_path.read_text(encoding="utf-8").splitlines()):
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            continue
        if "status" in record:
            return record
    raise RuntimeError(f"no terminal JSON record in {log_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--scenario", action="append",
        help="scenario to run (repeatable); defaults to the complete manifest",
    )
    parser.add_argument("--native-port", type=int, default=19852)
    parser.add_argument("--oracle-port", type=int, default=19853)
    parser.add_argument(
        "--runner", type=Path,
        default=ROOT / "runner" / "build_probe" / "nds_runner.exe",
    )
    parser.add_argument(
        "--oracle", type=Path,
        default=ROOT / "oracle" / "shim" / "build_probe" / "nds_oracle.exe",
    )
    parser.add_argument("--manifest", type=Path, default=DEFAULT_TRAVERSAL)
    parser.add_argument(
        "--output", type=Path,
        help="evidence directory (default: generated/release_verify/<UTC stamp>)",
    )
    parser.add_argument("--client-timeout", type=float, default=300.0)
    parser.add_argument("--scenario-timeout", type=float, default=1800.0)
    parser.add_argument(
        "--skip-frame-scan", action="store_true",
        help="diagnostic only; a skipped-frame run is not a release proof",
    )
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    available = list(manifest["scenarios"])
    scenarios = args.scenario or available
    unknown = [name for name in scenarios if name not in available]
    if unknown:
        parser.error(f"unknown scenario(s): {', '.join(unknown)}")
    if args.native_port == args.oracle_port:
        parser.error("native and oracle ports must differ")

    for path in (args.runner, args.oracle, args.manifest,
                 ROOT / "bios" / "biosnds9.rom",
                 ROOT / "bios" / "biosnds7.rom",
                 ROOT / "bios" / "firmware.bin"):
        if not path.is_file():
            raise SystemExit(f"required file missing: {path}")
    for port in (args.native_port, args.oracle_port):
        if not port_is_free(port):
            raise SystemExit(
                f"refusing to disturb existing listener on 127.0.0.1:{port}"
            )

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = (args.output or
              ROOT / "generated" / "release_verify" / stamp).resolve()
    output.mkdir(parents=True, exist_ok=False)

    configs = sorted((ROOT / "bios" / "firmware_banks").glob("*.toml"))
    dirty = git_output("status", "--porcelain=v1")
    evidence: dict[str, Any] = {
        "format": 1,
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "status": "running",
        "full_frame_scan": not args.skip_frame_scan,
        "git": {
            "head": git_output("rev-parse", "HEAD"),
            "branch": git_output("branch", "--show-current"),
            "dirty": bool(dirty),
            "status_sha256": hashlib.sha256(dirty.encode()).hexdigest(),
        },
        "identity": {
            "runner": file_identity(args.runner),
            "oracle": file_identity(args.oracle),
            "bios9": file_identity(ROOT / "bios" / "biosnds9.rom"),
            "bios7": file_identity(ROOT / "bios" / "biosnds7.rom"),
            "firmware": file_identity(ROOT / "bios" / "firmware.bin"),
            "traversal": file_identity(args.manifest),
            "firmware_bank_configs": [file_identity(path) for path in configs],
        },
        "ports": {"native": args.native_port, "oracle": args.oracle_port},
        "scenarios": [],
    }
    evidence_path = output / "release_manifest.json"

    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0) if os.name == "nt" else 0
    overall_ok = True
    for index, scenario in enumerate(scenarios, 1):
        print(f"[{index}/{len(scenarios)}] {scenario}: cold start", flush=True)
        native_log = output / f"{scenario}.native.log"
        oracle_log = output / f"{scenario}.oracle.log"
        traversal_log = output / f"{scenario}.traversal.log"
        native: subprocess.Popen[bytes] | None = None
        oracle: subprocess.Popen[bytes] | None = None
        started = time.monotonic()
        result: dict[str, Any] = {"scenario": scenario, "status": "running"}
        try:
            with native_log.open("wb") as native_stream, \
                    oracle_log.open("wb") as oracle_stream:
                native = subprocess.Popen(
                    [str(args.runner), "bios", "--serve", "--port",
                     str(args.native_port)],
                    cwd=ROOT, stdout=native_stream, stderr=subprocess.STDOUT,
                    creationflags=creationflags,
                )
                oracle = subprocess.Popen(
                    [str(args.oracle), "--bios9", "bios/biosnds9.rom",
                     "--bios7", "bios/biosnds7.rom", "--firmware",
                     "bios/firmware.bin", "--boot", "firmware", "--port",
                     str(args.oracle_port)],
                    cwd=ROOT, stdout=oracle_stream, stderr=subprocess.STDOUT,
                    creationflags=creationflags,
                )
                wait_for_server(native, args.native_port, 30.0)
                wait_for_server(oracle, args.oracle_port, 30.0)
                command = [
                    sys.executable, str(ROOT / "oracle" / "firmware_traversal.py"),
                    "--manifest", str(args.manifest), "--scenario", scenario,
                    "--native-port", str(args.native_port), "--oracle-port",
                    str(args.oracle_port), "--timeout", str(args.client_timeout),
                    "--require-zero-tier3",
                ]
                if args.skip_frame_scan:
                    command.append("--skip-frame-scan")
                with traversal_log.open("wb") as traversal_stream:
                    completed = subprocess.run(
                        command, cwd=ROOT, stdout=traversal_stream,
                        stderr=subprocess.STDOUT, timeout=args.scenario_timeout,
                        check=False, creationflags=creationflags,
                    )
                record = parse_pass_record(traversal_log)
                if completed.returncode != 0 or record.get("status") != "pass":
                    raise RuntimeError(
                        f"traversal exit={completed.returncode} "
                        f"status={record.get('status')!r}"
                    )
                result.update({
                    "status": "pass",
                    "duration_seconds": round(time.monotonic() - started, 3),
                    "static_coverage": record.get("static_coverage"),
                    "traversal_log": file_identity(traversal_log),
                })
                print(f"[{index}/{len(scenarios)}] {scenario}: PASS", flush=True)
        except Exception as exc:  # retain the first failure and its logs
            overall_ok = False
            result.update({
                "status": "fail",
                "duration_seconds": round(time.monotonic() - started, 3),
                "error": str(exc),
            })
            print(f"[{index}/{len(scenarios)}] {scenario}: FAIL: {exc}", flush=True)
        finally:
            stop_process(native)
            stop_process(oracle)
        for label, path in (("native_log", native_log),
                            ("oracle_log", oracle_log)):
            if path.is_file():
                result[label] = file_identity(path)
        evidence["scenarios"].append(result)
        evidence["status"] = "running" if overall_ok else "fail"
        evidence_path.write_text(
            json.dumps(evidence, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        if not overall_ok:
            break

    evidence["finished_utc"] = datetime.now(timezone.utc).isoformat()
    evidence["status"] = "pass" if overall_ok else "fail"
    evidence_path.write_text(
        json.dumps(evidence, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"release evidence: {evidence_path}", flush=True)
    return 0 if overall_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
