#!/usr/bin/env python3
"""Multi-instance safety of the live shard cache index.

Two runners can share one live-overlay cache directory, so the index
read-modify-write in tools/compile_live_shards.py has to be serialized by a
cross-process lock. This spawns several concurrent writers against one index
and asserts that every capture survives -- then repeats the run with the lock
neutered and asserts that the same workload does lose entries, so the test
cannot pass for the wrong reason.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import shutil
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))

import compile_live_shards as tool  # noqa: E402

ROM_SHA1 = "0123456789abcdef0123456789abcdef01234567"
WORKERS = 4
PER_WORKER = 40


def worker(cache: Path, worker_id: int, unlocked: bool) -> int:
    if unlocked:
        # Neuter only the lock, then widen the read-modify-write window so the
        # race is reliably observable instead of merely probable.
        tool.exclusive_file_lock = (
            lambda *_args, **_kwargs: contextlib.nullcontext())
        real_load_json = tool.load_json

        def slow_load_json(path: Path) -> object:
            value = real_load_json(path)
            import time
            time.sleep(0.002)
            return value

        tool.load_json = slow_load_json

    args = SimpleNamespace(cache=cache, index=cache / "live-index.json",
                           rom_sha1=ROM_SHA1)
    index = tool.empty_index(ROM_SHA1)
    for i in range(PER_WORKER):
        key = f"worker{worker_id}-capture{i:03d}"
        tool.record_capture(args, index, key, {
            "candidate_id": key,
            "cpu": 9,
            "page": "0x02000000",
            "entries": 1,
        })
    return 0


def captures(cache: Path, tolerate_corruption: bool = False) -> set[str]:
    text = (cache / "live-index.json").read_text(encoding="utf-8")
    try:
        index = json.loads(text)
    except json.JSONDecodeError:
        # Only the unlocked control can get here: concurrent unserialized
        # writers can leave the shared index as non-JSON, which is itself
        # evidence the lock is load-bearing.
        if tolerate_corruption:
            return set()
        raise
    assert index["schema"] == 2, "index schema should be normalized to 2"
    assert index["rom_sha1"] == ROM_SHA1, "index should keep its ROM identity"
    return set(index["captures"])


def run_workers(cache: Path, unlocked: bool) -> tuple[set[str], int]:
    if cache.exists():
        shutil.rmtree(cache)
    cache.mkdir(parents=True)
    children = [
        subprocess.Popen(
            [sys.executable, str(Path(__file__).resolve()),
             "--worker", str(worker_id), "--cache", str(cache)]
            + (["--unlocked"] if unlocked else []),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for worker_id in range(WORKERS)
    ]
    failures = 0
    for child in children:
        output = child.communicate()[0]
        if child.returncode == 0:
            continue
        failures += 1
        if not unlocked:
            print(output, end="")
            raise AssertionError(
                f"index writer failed with {child.returncode}")
    return captures(cache, tolerate_corruption=unlocked), failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path)
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--worker", type=int)
    parser.add_argument("--unlocked", action="store_true")
    args = parser.parse_args()

    if args.worker is not None:
        return worker(args.cache, args.worker, args.unlocked)

    work = (args.work or (Path(__file__).resolve().parent /
                          "live-index-lock-work")).resolve()
    work.mkdir(parents=True, exist_ok=True)

    expected = {f"worker{w}-capture{i:03d}"
                for w in range(WORKERS) for i in range(PER_WORKER)}

    locked, _ = run_workers(work / "locked", unlocked=False)
    missing = expected - locked
    assert not missing, (
        f"the exclusive index lock lost {len(missing)} captures: "
        f"{sorted(missing)[:5]}")
    assert locked == expected, "the index gained unexpected captures"

    # Control: the same workload without the lock must either lose entries or
    # fail outright (on Windows two concurrent os.replace calls onto one index
    # raise PermissionError). Either outcome proves the workload is genuinely
    # contended and the lock is load-bearing rather than decorative.
    unlocked, failures = run_workers(work / "unlocked", unlocked=True)
    assert unlocked - expected == set(), "control run invented captures"
    lost = len(expected - unlocked)
    if not failures and not lost:
        raise AssertionError(
            "the unlocked control run lost nothing and failed nothing; the "
            "workload is not contended enough to prove the lock is needed")

    print(f"PASS: live index lock kept {len(locked)} concurrent captures "
          f"(unlocked control lost {lost} across {failures} failed writers)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
