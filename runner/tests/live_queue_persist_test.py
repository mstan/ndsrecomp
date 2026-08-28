#!/usr/bin/env python3
"""Persistence and ordering of the live-overlay pending-work queue.

beads-yjp.51. Three properties, each of which a player install depends on:

  ORDERING     Candidates are compiled hottest-first by EXECUTION WEIGHT. The
               field evidence is that uncovered hot pages, not numerous ones,
               drive the interpreter storms, so a page carrying the frame must
               outrank a page with more distinct entry points but little work.

  PERSISTENCE  The pending queue is a schema-versioned document written beside
               live-index.json under the same live-index.lock, and it must be
               tolerant of every way it can be wrong: absent, truncated, from
               another ROM, from another provider identity, from a future
               schema, or carrying malformed rows. A bad queue costs a
               rediscovery; it must never be able to stop shards compiling.

  RESUME       A queue written by a process that is then KILLED is still
               readable afterwards -- both the file (atomic replace) and the
               lock (kernel-released on process death) -- and the candidates
               it names come back with their payload and their accumulated
               weight, so the next launch continues the backlog instead of
               waiting for the guest to re-execute the same pages.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import textwrap
import time
from pathlib import Path
from types import SimpleNamespace

TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))

import compile_live_shards as tool  # noqa: E402

ROM_SHA1 = "0123456789abcdef0123456789abcdef01234567"
PROVIDER = "provider0000000000aa"


def make_args(cache: Path) -> SimpleNamespace:
    return SimpleNamespace(
        cache=cache,
        index=cache / "live-index.json",
        queue=cache / tool.QUEUE_NAME,
        rom_sha1=ROM_SHA1,
        provider_id=PROVIDER,
        exclude_range=[],
        include_roots=False,
        min_hits=1,
        merge_cache_snapshots=False,
        manifest=cache / "snapshots" / "current.json",
    )


def page(addr: int, executions: int, entry_hits: list[int],
         cpu: int = 9) -> dict:
    """One coverage page whose payload really does hash to its sha1."""
    raw = bytes(((addr >> 4) + i) & 0xFF for i in range(tool.PAGE_SIZE))
    return {
        "cpu": cpu,
        "addr": f"0x{addr:08X}",
        "sha1": hashlib.sha1(raw).hexdigest(),
        "data": base64.b64encode(raw).decode("ascii"),
        "executions": executions,
        "entry_points": [
            {"addr": f"0x{addr + 4 * i:08X}", "mode": "arm", "hits": hits,
             "kind": "call"}
            for i, hits in enumerate(entry_hits)
        ],
    }


def manifest(pages: list[dict]) -> dict:
    return {
        "schema": 4,
        "kind": "ndsrecomp-tier3-coverage",
        "rom_sha1": ROM_SHA1,
        "pages": {"entries": pages},
    }


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


# ---------------------------------------------------------------- ordering


def test_ordering(cache: Path) -> None:
    args = make_args(cache)
    manifest_path = cache / "snapshots" / "manifest-000001.json"
    # cold:    lots of entry points, almost no execution -> must sort LAST
    # hot:     one entry point carrying the frame          -> must sort FIRST
    # middle:  in between
    pages = [
        page(0x02001000, executions=10, entry_hits=[50] * 8),   # hits 400
        page(0x02002000, executions=90000, entry_hits=[3]),     # hits 3
        page(0x02003000, executions=4000, entry_hits=[20, 20]),  # hits 40
    ]
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest(pages)), encoding="utf-8")

    ordered = tool.collect_candidates(
        args, [(manifest_path, manifest(pages))], {7, 9}, {})
    addrs = [int(item[2]["addr"], 16) for item in ordered]
    check(addrs == [0x02002000, 0x02003000, 0x02001000],
          f"hottest page must compile first by execution weight, got {addrs}")
    # And the ordering must not be an accident of discovery order: feed the
    # same pages in the opposite order and demand the same result.
    reordered = tool.collect_candidates(
        args, [(manifest_path, manifest(list(reversed(pages))))], {7, 9}, {})
    check([int(i[2]["addr"], 16) for i in reordered] == addrs,
          "candidate order must not depend on discovery order")
    check(all(item[4] == str(manifest_path) for item in ordered),
          "each candidate must record the manifest holding its payload")


def test_carried_weight_beats_the_recency_window(cache: Path) -> None:
    """A page that aged out of the hot window keeps its accumulated weight.

    The runner's snapshot is a recency window, so a page that was hot a minute
    ago can reappear with almost no executions. Without the carried weight it
    would sink below genuinely colder pages and never be compiled.
    """
    args = make_args(cache)
    manifest_path = cache / "snapshots" / "manifest-000002.json"
    aged = page(0x02004000, executions=1, entry_hits=[2])
    fresh = page(0x02005000, executions=500, entry_hits=[9])
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest([aged, fresh])),
                             encoding="utf-8")

    without = tool.collect_candidates(
        args, [(manifest_path, manifest([aged, fresh]))], {7, 9}, {})
    check(int(without[0][2]["addr"], 16) == 0x02005000,
          "without carried weight the fresh page leads")

    carried = {
        (9, 0x02004000, aged["sha1"]): {
            "cpu": 9, "addr": 0x02004000, "page_sha1": aged["sha1"],
            "executions": 90000, "hits": 400, "manifest": str(manifest_path),
        }
    }
    with_carry = tool.collect_candidates(
        args, [(manifest_path, manifest([aged, fresh]))], {7, 9}, carried)
    check(int(with_carry[0][2]["addr"], 16) == 0x02004000,
          "the carried execution weight must restore the aged-out hot page")


# ------------------------------------------------------------- persistence


def test_roundtrip_and_schema(cache: Path) -> None:
    args = make_args(cache)
    pending = [
        {"cpu": 9, "addr": 0x02002000, "page_sha1": "a" * 40,
         "executions": 900, "hits": 3, "manifest": "m.json"},
        {"cpu": 7, "addr": 0x03800000, "page_sha1": "b" * 40,
         "executions": 10, "hits": 400, "manifest": "m.json"},
    ]
    tool.store_queue(args, pending)
    raw = json.loads(args.queue.read_text(encoding="utf-8"))
    check(raw["schema"] == tool.QUEUE_SCHEMA,
          "the queue document must carry its schema version")
    check(raw["rom_sha1"] == ROM_SHA1 and raw["provider_id"] == PROVIDER,
          "the queue must be bound to its ROM and provider identity")
    check(raw["pending_count"] == 2,
          "pending_count is the scalar the runner reads; it must be present "
          "and correct")
    check([e["addr"] for e in raw["pending"]] == ["0x02002000", "0x03800000"],
          "queue order must be preserved verbatim on disk")

    back = tool.load_queue(args)
    check([e["addr"] for e in back] == [0x02002000, 0x03800000],
          "round-trip must preserve the pending order")
    check(back[0]["executions"] == 900 and back[1]["hits"] == 400,
          "round-trip must preserve the accumulated weights")


def test_corrupt_tolerance(cache: Path) -> None:
    args = make_args(cache)
    good = [{"cpu": 9, "addr": 0x02002000, "page_sha1": "a" * 40,
             "executions": 1, "hits": 1, "manifest": "m.json"}]

    args.queue.unlink(missing_ok=True)
    check(tool.load_queue(args) == [], "an absent queue reads as empty")

    cases = {
        "truncated json": '{"schema": 1, "rom_sha1": "',
        "not an object": '[1, 2, 3]',
        "empty file": '',
        "binary garbage": '\x00\x01\x02 not json at all',
    }
    for name, text in cases.items():
        args.queue.write_text(text, encoding="utf-8")
        check(tool.load_queue(args) == [], f"{name} must read as empty")

    tool.store_queue(args, good)
    for name, mutate in {
        "future schema": lambda d: d.update(schema=tool.QUEUE_SCHEMA + 1),
        "older schema": lambda d: d.update(schema=0),
        "another ROM": lambda d: d.update(rom_sha1="f" * 40),
        "another provider": lambda d: d.update(provider_id="somethingelse"),
        "pending not a list": lambda d: d.update(pending={}),
    }.items():
        doc = json.loads(args.queue.read_text(encoding="utf-8"))
        mutate(doc)
        args.queue.write_text(json.dumps(doc), encoding="utf-8")
        check(tool.load_queue(args) == [], f"{name} must read as empty")

    # Individual malformed rows are dropped without taking the queue with
    # them: one bad entry must not cost the whole backlog.
    doc = tool.empty_queue(ROM_SHA1, PROVIDER)
    doc["pending"] = [
        {"cpu": 9, "addr": "0x02002000", "page_sha1": "a" * 40,
         "executions": 5, "hits": 5, "manifest": "m.json"},
        {"cpu": 5, "addr": "0x02003000", "page_sha1": "a" * 40},   # bad cpu
        {"cpu": 9, "addr": "0x02004000", "page_sha1": "zz"},       # bad sha1
        "not an object",
        {"cpu": 9},                                                # truncated
    ]
    doc["pending_count"] = len(doc["pending"])
    args.queue.write_text(json.dumps(doc), encoding="utf-8")
    survivors = tool.load_queue(args)
    check(len(survivors) == 1 and survivors[0]["addr"] == 0x02002000,
          f"only the well-formed row should survive, got {survivors}")


# ------------------------------------------------------------------ resume


def test_resume_reloads_the_payload(cache: Path) -> None:
    args = make_args(cache)
    manifest_path = cache / "snapshots" / "manifest-000003.json"
    hot = page(0x02006000, executions=70000, entry_hits=[11])
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest([hot])), encoding="utf-8")

    queued = [{"cpu": 9, "addr": 0x02006000, "page_sha1": hot["sha1"],
               "executions": 70000, "hits": 11,
               "manifest": str(manifest_path)}]
    resumed = tool.resume_manifests(queued, set())
    check(len(resumed) == 1, "the referenced snapshot must be reloaded")
    check(resumed[0][1]["pages"]["entries"][0]["sha1"] == hot["sha1"],
          "the resumed manifest must carry the page payload back")

    # A snapshot the queue names but that no longer exists drops only its own
    # candidate; a pruned cache must not be fatal.
    missing = [{"cpu": 9, "addr": 0x02007000, "page_sha1": "c" * 40,
                "executions": 1, "hits": 1,
                "manifest": str(cache / "snapshots" / "gone.json")}]
    check(tool.resume_manifests(missing, set()) == [],
          "a missing snapshot must be skipped, not raised")

    # Already-loaded manifests are not loaded twice.
    check(tool.resume_manifests(queued, {manifest_path.resolve()}) == [],
          "a manifest already in the merge set must not be re-added")


_KILL_CHILD = textwrap.dedent(
    """
    import json, sys, time
    from pathlib import Path
    sys.path.insert(0, sys.argv[1])
    import compile_live_shards as tool
    from types import SimpleNamespace
    cache = Path(sys.argv[2])
    args = SimpleNamespace(cache=cache, queue=cache / tool.QUEUE_NAME,
                           rom_sha1=sys.argv[3], provider_id=sys.argv[4])
    tool.store_queue(args, [
        {"cpu": 9, "addr": 0x02008000, "page_sha1": "d" * 40,
         "executions": 1234, "hits": 7, "manifest": "m.json"},
        {"cpu": 9, "addr": 0x02009000, "page_sha1": "e" * 40,
         "executions": 12, "hits": 7, "manifest": "m.json"},
    ])
    # Now take the shared lock and die holding it. The kernel owns the
    # byte-range lock, so the parent must still be able to acquire it.
    with tool.exclusive_file_lock(cache / tool.INDEX_LOCK_NAME):
        print("HELD", flush=True)
        time.sleep(600)
    """
)


def test_survives_a_process_kill(cache: Path) -> None:
    args = make_args(cache)
    args.queue.unlink(missing_ok=True)
    child = subprocess.Popen(
        [sys.executable, "-c", _KILL_CHILD, str(TOOLS), str(cache),
         ROM_SHA1, PROVIDER],
        stdout=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 30.0
        line = ""
        while time.monotonic() < deadline:
            line = child.stdout.readline()
            if line.strip() == "HELD":
                break
        check(line.strip() == "HELD",
              "the child never reported holding the live-index lock")
        child.kill()
        child.wait(timeout=30)
    finally:
        if child.poll() is None:
            child.kill()

    resumed = tool.load_queue(args)
    check([e["addr"] for e in resumed] == [0x02008000, 0x02009000],
          f"the queue must survive a hard kill of its writer, got {resumed}")
    check(resumed[0]["executions"] == 1234,
          "weights must survive the kill too")
    # The runner reads pending_count out of the same file with a scalar scan;
    # prove the scalar is where it is expected to be.
    doc = json.loads(args.queue.read_text(encoding="utf-8"))
    check(doc["pending_count"] == 2,
          "pending_count must be readable after the kill")


def test_prune_keeps_what_resume_needs(cache: Path) -> None:
    """Snapshot pruning must never delete a manifest the queue depends on.

    The adaptive cadence runs the compiler many times a minute and each run
    writes a ~400 KiB snapshot, so unbounded accumulation had to be bounded.
    The keep-set is exactly the current manifest plus every manifest a still
    pending candidate names -- deleting one of those would turn a resumable
    backlog into a silent rediscovery.
    """
    args = make_args(cache)
    snapshots = cache / "snapshots"
    snapshots.mkdir(parents=True, exist_ok=True)
    current = args.manifest
    referenced = snapshots / "manifest-0000dead-000001.json"
    orphan_a = snapshots / "manifest-0000dead-000002.json"
    orphan_b = snapshots / "manifest-0000beef-000001.json"
    for path in (current, referenced, orphan_a, orphan_b):
        path.write_text(json.dumps(manifest([])), encoding="utf-8")

    pending = [{"cpu": 9, "addr": 0x0200A000, "page_sha1": "a" * 40,
                "executions": 1, "hits": 1, "manifest": str(referenced)}]
    removed = tool.prune_snapshots(args, pending)
    check(removed == 2, f"both orphans should be pruned, removed={removed}")
    check(current.is_file(), "the current manifest must survive pruning")
    check(referenced.is_file(),
          "a manifest a pending candidate names must survive pruning")
    check(not orphan_a.is_file() and not orphan_b.is_file(),
          "unreferenced snapshots should be gone")

    # An empty backlog still keeps the current manifest, and only that.
    orphan_a.write_text(json.dumps(manifest([])), encoding="utf-8")
    tool.prune_snapshots(args, [])
    check(current.is_file() and not referenced.is_file(),
          "with nothing pending only the current manifest is kept")

    # --merge-cache-snapshots deliberately mines the whole history; pruning
    # must stand down completely.
    for path in (referenced, orphan_a):
        path.write_text(json.dumps(manifest([])), encoding="utf-8")
    args.merge_cache_snapshots = True
    check(tool.prune_snapshots(args, []) == 0,
          "pruning must not run under --merge-cache-snapshots")
    check(referenced.is_file() and orphan_a.is_file(),
          "history must be intact under --merge-cache-snapshots")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="nds-live-queue-") as temporary:
        root = Path(temporary)
        for name, case in [
            ("ordering", test_ordering),
            ("carried weight", test_carried_weight_beats_the_recency_window),
            ("round-trip", test_roundtrip_and_schema),
            ("corrupt tolerance", test_corrupt_tolerance),
            ("resume", test_resume_reloads_the_payload),
            ("process kill", test_survives_a_process_kill),
            ("snapshot pruning", test_prune_keeps_what_resume_needs),
        ]:
            cache = root / name.replace(" ", "-")
            cache.mkdir(parents=True, exist_ok=True)
            case(cache)
            print(f"ok: {name}", flush=True)
    print("PASS: live-overlay pending queue persists, resumes and orders "
          "hottest-first")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
