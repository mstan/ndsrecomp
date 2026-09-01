#!/usr/bin/env python3
"""Compile generation-bound NDS RAM code pages into persistent live libraries.

The coverage manifest associates each dispatch/resume observation with the
exact page bytes resident at that moment. Each DLL publishes one exact
dependency closure covering every emitted native body. Direct transfers remain
inside that atomically validated closure; cross-page transfers return through
candidate lookup.
"""

from __future__ import annotations

import argparse
import ast
import base64
import collections
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from contextlib import contextmanager
from pathlib import Path


ABI_VERSION = 6
PAGE_SIZE = 4096

# Backends that can build a live shard. "gcc" is the dev/CI toolchain; "tcc" is
# the bundled, toolchain-free player fallback staged by tools/make_release.ps1.
# Each owns a cache namespace (<cache>/gcc, <cache>/tcc) and hashes into a
# distinct provider identity, so their shards never alias one another.
BACKENDS = ("gcc", "tcc")
SHARED_LIBRARY_SUFFIX = (
    ".dll" if os.name == "nt" else
    ".so" if sys.platform.startswith("linux") else "")


def load_json(path: Path) -> object:
    return json.loads(path.read_text(encoding="utf-8"))


def atomic_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8", newline="\n")
    os.replace(temporary, path)


INDEX_LOCK_NAME = "live-index.lock"

# Pending-work queue, persisted next to live-index.json and guarded by the SAME
# live-index.lock. One lock for both files is deliberate: the queue is derived
# from the index (a candidate is pending exactly while the index does not
# satisfy it), so a reader that took them under two different locks could see a
# candidate as both compiled and pending, or as neither.
#
# Why this file exists at all: candidate discovery used to be redone from
# scratch every run out of the coverage snapshots, and the runner's snapshot is
# a RECENCY window (coverage_manifest_write_live_snapshot keeps the 64
# most-recently-seen pages). A hot page that stops being touched for a few
# seconds falls out of the window and its queued work is forgotten, so a player
# session could end with dozens of hot pages never compiled even though they
# had been observed. The queue makes pending work durable across both runs and
# process lifetimes, and carries each candidate's accumulated execution weight
# forward so ordering does not reset with the window.
QUEUE_NAME = "live-queue.json"
QUEUE_SCHEMA = 1


@contextmanager
def exclusive_file_lock(lock_path: Path, timeout: float | None = 120.0):
    """Kernel-owned cross-process lock on one permanent byte-range file.

    Mirrors psxrecomp tools/compile_overlays.py::_exclusive_file_lock. The
    lock file is intentionally permanent: OS byte-range locks are released
    by the kernel when a process dies, whereas deleting the file creates an
    inode/handle split that lets two processes both believe they own it.
    """
    lock_path = lock_path.resolve()
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    lock_file = open(lock_path, "a+b")
    if lock_path.stat().st_size == 0:
        lock_file.write(b"\0")
        lock_file.flush()
    deadline = (time.monotonic() + timeout if timeout is not None else None)
    acquired = False
    try:
        while not acquired:
            try:
                lock_file.seek(0)
                if os.name == "nt":
                    import msvcrt
                    msvcrt.locking(lock_file.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(lock_file.fileno(),
                                fcntl.LOCK_EX | fcntl.LOCK_NB)
                acquired = True
            except OSError:
                if deadline is not None and time.monotonic() >= deadline:
                    raise TimeoutError(
                        f"timed out waiting for live index lock {lock_path}")
                time.sleep(0.025)
        yield
    finally:
        if acquired:
            lock_file.seek(0)
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(lock_file.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
        lock_file.close()


def empty_index(rom_sha1: str) -> dict:
    return {"schema": 2, "rom_sha1": rom_sha1, "captures": {}}


def empty_queue(rom_sha1: str, provider_id: str) -> dict:
    return {
        "schema": QUEUE_SCHEMA,
        "rom_sha1": rom_sha1,
        "provider_id": provider_id,
        "pending_count": 0,
        "pending": [],
    }


def queue_is_usable(value: object, rom_sha1: str, provider_id: str) -> bool:
    """Every reason to distrust a persisted queue, in one place.

    Tolerance is the requirement here, not strictness: the queue is pure
    scheduling advice. Anything unreadable, from another ROM, from another
    provider identity (different compiler or recompiler, so the work identities
    would not match anyway), or from a schema this build does not know, is
    discarded and rebuilt from the manifests. A corrupt queue must never be
    able to stop a player's shards from being compiled.
    """
    if not isinstance(value, dict):
        return False
    if int(value.get("schema", 0)) != QUEUE_SCHEMA:
        return False
    if str(value.get("rom_sha1", "")).lower() != rom_sha1.lower():
        return False
    if str(value.get("provider_id", "")) != provider_id:
        return False
    return isinstance(value.get("pending"), list)


def _sanitize_queue_entry(raw: object) -> dict | None:
    if not isinstance(raw, dict):
        return None
    try:
        cpu = int(raw["cpu"])
        addr = int(raw["addr"], 16) if isinstance(raw["addr"], str) \
            else int(raw["addr"])
        page_sha1 = str(raw["page_sha1"]).lower()
        executions = int(raw.get("executions", 0))
        hits = int(raw.get("hits", 0))
        manifest = str(raw.get("manifest", ""))
    except (KeyError, TypeError, ValueError):
        return None
    if cpu not in (7, 9) or not (0 <= addr < 0x1_0000_0000):
        return None
    if len(page_sha1) != 40 or any(c not in "0123456789abcdef"
                                   for c in page_sha1):
        return None
    return {
        "cpu": cpu,
        "addr": addr,
        "page_sha1": page_sha1,
        "executions": max(0, executions),
        "hits": max(0, hits),
        "manifest": manifest,
    }


def load_queue(args: argparse.Namespace) -> list[dict]:
    """Read the persisted pending queue. Never raises; empty on any doubt."""
    try:
        with exclusive_file_lock(args.cache / INDEX_LOCK_NAME):
            value = load_json(args.queue) if args.queue.is_file() else None
    except (OSError, json.JSONDecodeError, TimeoutError, ValueError):
        return []
    if not queue_is_usable(value, args.rom_sha1, args.provider_id):
        return []
    out = []
    for raw in value["pending"]:
        entry = _sanitize_queue_entry(raw)
        if entry is not None:
            out.append(entry)
    return out


def store_queue(args: argparse.Namespace, pending: list[dict]) -> None:
    value = empty_queue(args.rom_sha1, args.provider_id)
    value["pending_count"] = len(pending)
    value["pending"] = [{
        "cpu": entry["cpu"],
        "addr": f"0x{entry['addr']:08X}",
        "page_sha1": entry["page_sha1"],
        "executions": entry["executions"],
        "hits": entry["hits"],
        "manifest": entry["manifest"],
    } for entry in pending]
    try:
        with exclusive_file_lock(args.cache / INDEX_LOCK_NAME):
            atomic_json(args.queue, value)
    except (OSError, TimeoutError):
        # Losing the queue costs a rediscovery, never correctness.
        pass


def prune_snapshots(args: argparse.Namespace, pending: list[dict]) -> int:
    """Keep only the snapshots the queue still needs, plus the current one.

    The runner writes one ~400 KiB snapshot per compiler run and nothing ever
    removed them. That was tolerable at one run per 30 s; it is not once the
    adaptive cadence runs many times a minute. The safe keep-set is exactly
    what resume depends on: the manifest this run was handed, and every
    manifest a still-pending candidate names. Anything else can only be
    re-derived work that is already either compiled or dropped.

    Skipped entirely under --merge-cache-snapshots, which deliberately mines
    the whole accumulated history.
    """
    if args.merge_cache_snapshots:
        return 0
    snapshots = args.cache / "snapshots"
    if not snapshots.is_dir():
        return 0
    keep: set[Path] = set()
    for candidate in [args.manifest, *(Path(e["manifest"]) for e in pending
                                       if e.get("manifest"))]:
        try:
            keep.add(Path(candidate).resolve())
        except OSError:
            continue
    removed = 0
    for path in snapshots.glob("manifest-*.json"):
        try:
            if path.resolve() in keep:
                continue
            path.unlink()
            removed += 1
        except OSError:
            # Another runner sharing this cache may hold it open. Leaving a
            # snapshot behind costs disk, never correctness.
            continue
    return removed


def record_capture(args: argparse.Namespace, index: dict, key: str,
                   entry: dict) -> None:
    """Merge one capture into the shared index under an exclusive lock.

    Two runners can share one cache directory, so the index is never written
    from a snapshot read at process start: the read-modify-write is redone
    inside the lock against whatever is on disk right now, and `index` (this
    process's working view) is refreshed from the merged result. Entries this
    process holds but that are absent from disk are carried forward; on a key
    collision the on-disk entry wins, since it names a DLL already published.
    """
    with exclusive_file_lock(args.cache / INDEX_LOCK_NAME):
        current = load_json(args.index) if args.index.is_file() else None
        if (not isinstance(current, dict) or
                current.get("rom_sha1") != args.rom_sha1 or
                int(current.get("schema", 0)) not in (1, 2)):
            current = empty_index(args.rom_sha1)
        current["schema"] = 2
        captures = current.setdefault("captures", {})
        for other_key, other in index.get("captures", {}).items():
            captures.setdefault(other_key, other)
        captures[key] = entry
        atomic_json(args.index, current)
    index.clear()
    index.update(current)


def run(command: list[str]) -> subprocess.CompletedProcess:
    print("+ " + " ".join(command), flush=True)
    return subprocess.run(command, text=True)


def find_import_lib(runner_build: Path) -> Path:
    for name in (
            "libnds_runner.dll.a", "nds_runner.exe.a",
            "libnds_runner.exe.a"):
        candidate = runner_build / name
        if candidate.is_file():
            return candidate
    matches = sorted(runner_build.rglob("*nds_runner*.a"))
    if matches:
        return matches[0]
    raise RuntimeError(
        f"could not find the nds_runner import library under {runner_build}")


def parse_range(text: str) -> tuple[int, int]:
    pieces = text.split(":", 1)
    if len(pieces) != 2:
        raise argparse.ArgumentTypeError("range must be START:END")
    start, end = (int(piece, 0) for piece in pieces)
    if start < 0 or end <= start or end > 0x1_0000_0000:
        raise argparse.ArgumentTypeError("invalid address range")
    return start, end


def excluded(addr: int, ranges: list[tuple[int, int]]) -> bool:
    return any(start <= addr < end for start, end in ranges)


def root_entries(page: dict) -> list[dict]:
    base = int(page["addr"], 16)
    blocks = page.get("root_hits") or []
    per_block = len(blocks)
    found: list[tuple[int, str]] = []
    for key, stride in (("root_arm", 4), ("root_thumb", 2)):
        blob = page.get(key)
        if not blob:
            continue
        raw = base64.b64decode(blob)
        mode = "arm" if stride == 4 else "thumb"
        for byte_index, byte in enumerate(raw):
            if not byte:
                continue
            for bit in range(8):
                if byte & (1 << bit):
                    found.append(
                        (base + ((byte_index * 8 + bit) * stride), mode))
    if not found:
        return []

    if not per_block:
        return [
            {"addr": addr, "mode": mode, "hits": 0, "kind": "root"}
            for addr, mode in found
        ]

    def block_of(addr: int) -> int:
        return min(((addr - base) * per_block) // PAGE_SIZE, per_block - 1)

    per = collections.Counter(block_of(addr) for addr, _mode in found)
    remainder = {block: blocks[block] % count for block, count in per.items()}
    entries = []
    for addr, mode in sorted(found):
        block = block_of(addr)
        hits = blocks[block] // per[block]
        if remainder.get(block):
            hits += 1
            remainder[block] -= 1
        entries.append(
            {"addr": addr, "mode": mode, "hits": hits, "kind": "root"})
    return entries


def canonical_entries(
        page: dict, ranges: list[tuple[int, int]], include_roots: bool
) -> list[dict]:
    merged: dict[tuple[int, str], dict] = {}
    source_entries = list(page.get("entry_points", []))
    if include_roots:
        source_entries.extend(root_entries(page))
    for entry in source_entries:
        raw_addr = entry["addr"]
        addr = int(raw_addr, 16) if isinstance(raw_addr, str) else int(raw_addr)
        mode = entry.get("mode", "arm")
        if mode not in ("arm", "thumb") or excluded(addr, ranges):
            continue
        key = (addr, mode)
        current = merged.setdefault(key, {
            "addr": addr,
            "mode": mode,
            "hits": 0,
            "kinds": set(),
        })
        current["hits"] += int(entry.get("hits", 0))
        current["kinds"].add(str(entry.get("kind", "root")))
    result = []
    for current in merged.values():
        result.append({
            "addr": current["addr"],
            "mode": current["mode"],
            "hits": current["hits"],
            "kinds": sorted(current["kinds"]),
        })
    return sorted(result, key=lambda item: (item["addr"], item["mode"]))


def page_key(page: dict) -> tuple[int, int, str]:
    return (
        int(page["cpu"]),
        int(page["addr"], 16),
        str(page["sha1"]).lower(),
    )


def merge_entries(left: list[dict], right: list[dict]) -> list[dict]:
    merged: dict[tuple[int, str], dict] = {}
    for entry in [*left, *right]:
        addr = int(entry["addr"])
        mode = str(entry["mode"])
        key = (addr, mode)
        current = merged.setdefault(key, {
            "addr": addr,
            "mode": mode,
            "hits": 0,
            "kinds": set(),
        })
        current["hits"] += int(entry.get("hits", 0))
        current["kinds"].update(str(kind) for kind in entry.get("kinds", []))
    return sorted(({
        "addr": item["addr"],
        "mode": item["mode"],
        "hits": item["hits"],
        "kinds": sorted(item["kinds"]),
    } for item in merged.values()), key=lambda item: (item["addr"], item["mode"]))


def generation_entries(index: dict) -> dict[tuple[int, int, str], list[dict]]:
    """Entry roots already compiled into a shard, per page BYTE GENERATION.

    beads-yjp.53. A page's candidate set is built from the roots that are
    still reaching Tier 3, and an entry already served natively stops
    appearing there -- so the next capture of the SAME page bytes carries a
    SMALLER root set than the shard already in the cache. The runtime keyed
    supersede on the generation, so that smaller candidate replaced the larger
    one and its extra rows went back to the interpreter, where the next
    capture rediscovered them: an oscillation, visible in a player's own index
    (page 0x0205B000 recorded 4 entries and then 3; 0x02034000 3 and then 2)
    and in the shipped release cache (ten same-generation candidates for
    0x02115000 with alternating root sets).

    Folding the already-published roots back in makes every new candidate a
    superset of the resident one, so coverage for a generation only ever
    grows. It also removes the recompile entirely when the merged set is
    unchanged: work_identity() folds the entry set, so the key then matches
    the index record and the page is skipped.
    """
    out: dict[tuple[int, int, str], list[dict]] = {}
    for record in index.get("captures", {}).values():
        if not isinstance(record, dict):
            continue
        roots = record.get("entry_roots")
        if not isinstance(roots, list) or not roots:
            continue
        try:
            key = (int(record["cpu"]), int(str(record["page"]), 16),
                   str(record["page_sha1"]).lower())
        except (KeyError, TypeError, ValueError):
            continue
        merged = out.setdefault(key, [])
        for root in roots:
            if not isinstance(root, dict):
                continue
            try:
                addr = int(root["addr"])
            except (KeyError, TypeError, ValueError):
                continue
            mode = str(root.get("mode", "arm"))
            if mode not in ("arm", "thumb"):
                continue
            merged.append({"addr": addr, "mode": mode, "hits": 0,
                           "kinds": ["published"]})
    return out


def collect_candidates(
        args: argparse.Namespace, manifests: list[tuple[Path, dict]],
        allowed_cpus: set[int], carried: dict[tuple[int, int, str], dict],
        published: dict[tuple[int, int, str], list[dict]] | None = None
) -> list[tuple[int, int, dict, list[dict], str]]:
    by_page: dict[tuple[int, int, str], dict] = {}
    for manifest_path, manifest in manifests:
        if str(manifest.get("rom_sha1", "")).lower() != args.rom_sha1.lower():
            continue
        for page in manifest.get("pages", {}).get("entries", []):
            cpu = int(page["cpu"])
            if cpu not in allowed_cpus:
                continue
            entries = canonical_entries(
                page, args.exclude_range, args.include_roots)
            if not entries:
                continue
            key = page_key(page)
            current = by_page.setdefault(key, {
                "page": page,
                "entries": [],
                "executions": 0,
                # A root bitmap names PCs that actually fell through to the
                # interpreter for this exact page generation. Keep its
                # positive observation count separate from ordinary entry
                # hits: --min-hits filters noisy call/indirect candidates,
                # but must not delay first-encounter root-only code until it
                # has been interpreted that many times.
                "root_hits": 0,
                # Where the 4 KiB payload can be read back from on a later
                # run. Snapshots are permanent under <cache>/snapshots, so
                # this is what makes a queued candidate resumable rather than
                # merely remembered.
                "manifest": str(manifest_path),
            })
            current["entries"] = merge_entries(current["entries"], entries)
            current["executions"] += int(page.get("executions", 0))
            if args.include_roots:
                current["root_hits"] += sum(
                    int(entry.get("hits", 0))
                    for entry in root_entries(page)
                    if not excluded(int(entry["addr"]), args.exclude_range))

    candidates = []
    for key, current in by_page.items():
        entries = current["entries"]
        hits = sum(entry["hits"] for entry in entries)
        # beads-yjp.53: never regress the root set for a generation already
        # published. Hits are summed BEFORE the merge so a page does not clear
        # --min-hits on the strength of roots nobody executed this session.
        if published:
            already = published.get(key)
            if already:
                entries = merge_entries(entries, already)
        # Weight carried forward from the persisted queue. A page that was hot
        # in an earlier session but has just aged out of the recency window
        # must not sink to the bottom of the order; the queue is the only place
        # that accumulated evidence survives.
        carry = carried.get(key)
        if carry:
            hits = max(hits, carry["hits"])
            current["executions"] = max(current["executions"],
                                        carry["executions"])
        # Root-only observations are the evidence that unknown code has
        # already reached Tier 3. Requiring the general hot-entry threshold
        # here made --include-roots ineffective on first encounter: the
        # runtime repeatedly invoked the compiler, but every newly executed
        # page was discarded below the release policy's --min-hits 8 gate.
        # A zero-hit bitmap does not qualify, nor does a low-hit call/indirect
        # page. max-pages and the durable queue still bound each compile run.
        root_encountered = (
            args.include_roots and current["root_hits"] > 0)
        if hits >= args.min_hits or root_encountered:
            candidates.append((hits, current["executions"],
                               current["page"], entries, current["manifest"]))
    # Hottest FIRST, by execution weight. Execution count is the page's share
    # of guest work; entry-point hit count is how many distinct observations
    # landed on it. The old order made hits primary, which ranks a page with
    # many lightly-used entry points above a page whose single entry point
    # carries the frame. The field evidence (beads-6vqh) is that uncovered HOT
    # pages are what drive the interpreter storms, so execution weight leads
    # and hits stays as the tie-break.
    candidates.sort(
        key=lambda item: (-item[1], -item[0], int(item[2]["addr"], 16),
                          item[2]["sha1"]))
    return candidates


def cache_manifests(cache: Path, current: Path) -> list[tuple[Path, dict]]:
    snapshots = cache / "snapshots"
    if not snapshots.is_dir():
        return []
    current_resolved = current.resolve()
    manifests = []
    for path in sorted(snapshots.glob("manifest-*.json")):
        try:
            if path.resolve() == current_resolved:
                continue
            manifest = load_json(path)
        except (OSError, json.JSONDecodeError):
            continue
        if isinstance(manifest, dict):
            manifests.append((path, manifest))
    return manifests


def resume_manifests(queued: list[dict],
                     already: set[Path]) -> list[tuple[Path, dict]]:
    """Load exactly the snapshots the persisted queue still depends on.

    This is what turns the queue from a memo into a resume: the payload for a
    queued candidate is re-read from the manifest that produced it, so the next
    process starts compiling the same work rather than waiting for the guest to
    re-execute those pages and rediscover them.
    """
    out: list[tuple[Path, dict]] = []
    seen: set[Path] = set()
    for entry in queued:
        raw = entry.get("manifest") or ""
        if not raw:
            continue
        path = Path(raw)
        try:
            resolved = path.resolve()
        except OSError:
            continue
        if resolved in already or resolved in seen:
            continue
        seen.add(resolved)
        try:
            manifest = load_json(path)
        except (OSError, json.JSONDecodeError):
            # A pruned or truncated snapshot drops only its own candidates;
            # they come back the next time the guest executes those pages.
            continue
        if isinstance(manifest, dict):
            out.append((path, manifest))
    return out


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def runtime_headers(args: argparse.Namespace) -> list[Path]:
    """Every runtime ABI header a shard can include, in a stable order.

    Hashing only runtime_arm.h used to be enough, but the shared ARM core
    extraction moved the type definitions it includes into
    external/arm-recomp-core/common/runtime_arm_types.h. An ABI change over
    there would not have invalidated a single cached shard.
    """
    found: dict[str, Path] = {}
    for directory in args.runtime_include:
        if not directory.is_dir():
            raise RuntimeError(
                f"runtime include directory does not exist: {directory}")
        for path in sorted(directory.glob("*.h")):
            found.setdefault(path.name, path)
    if "runtime_arm.h" not in found:
        raise RuntimeError(
            "runtime_arm.h was not found in any --runtime-include directory: "
            + ", ".join(str(d) for d in args.runtime_include))
    return [found[name] for name in sorted(found)]


def compiler_identity(args: argparse.Namespace) -> dict:
    """Identify the backend well enough that its shards never share a cache.

    A tcc-built and a gcc-built shard for the same page are different native
    code from different code generators; they are namespaced on disk AND must
    hash differently, or a cache warmed by one backend is served as if the
    other had produced it.
    """
    if args.compiler == "tcc":
        try:
            # tcc prints "tcc version X (target)" on stderr and exits 0.
            result = subprocess.run([str(args.tcc), "-v"],
                                    capture_output=True, text=True)
        except OSError as error:
            raise RuntimeError(f"cannot identify live-shard compiler: {error}")
        banner = (result.stdout or "") + (result.stderr or "")
        banner = banner.strip().splitlines()[0].strip() if banner.strip() else ""
        if not banner:
            raise RuntimeError(
                f"cannot identify live-shard compiler: {args.tcc} -v was silent")
        return {"compiler": "tcc", "tcc_banner": banner,
                "tcc_sha256": file_sha256(Path(args.tcc))}
    try:
        machine = subprocess.check_output(
            [str(args.gcc), "-dumpmachine"], text=True).strip()
        version = subprocess.check_output(
            [str(args.gcc), "-dumpfullversion", "-dumpversion"],
            text=True).strip()
    except (OSError, subprocess.CalledProcessError) as error:
        raise RuntimeError(f"cannot identify live-shard compiler: {error}")
    return {"compiler": "gcc", "gcc_machine": machine, "gcc_version": version}


def recompiler_codegen_identity(args: argparse.Namespace) -> str:
    """What the recompiler says about its own generated-C emission.

    NOT a hash of the executable. A PE carries a link timestamp and build-path
    residue, so hashing its bytes made a no-op rebuild of the recompiler a new
    provider identity and discarded every player's accumulated shard cache;
    v0.6.5 could not carry the v0.6.4 prebuilt cache forward for exactly that
    reason (beads-yjp.56).

    The binary reports a declared version instead
    (recompiler/src/codegen_identity.h), pinned by recompiler/tests/
    codegen_golden_test.cpp so emission cannot move without the version moving.

    Fails closed. A recompiler that does not answer this question is not one
    whose shards may share a cache namespace with one that does -- silently
    substituting a default would let two different code generators publish
    under one identity, and a live shard links straight into the runner's
    data symbols, so that is memory corruption rather than a bad frame.
    """
    try:
        result = subprocess.run([str(args.recompiler), "--codegen-identity"],
                                capture_output=True, text=True)
    except OSError as error:
        raise RuntimeError(
            f"cannot ask {args.recompiler} for its codegen identity: {error}")
    value = (result.stdout or "").strip().splitlines()
    value = value[0].strip() if value else ""
    if result.returncode != 0 or not re.fullmatch(r"nds-codegen-v[0-9]+",
                                                  value):
        raise RuntimeError(
            f"{args.recompiler} did not report a codegen identity "
            f"(exit {result.returncode}, output {value!r}). It is too old for "
            "this shard pipeline: rebuild the recompiler.")
    return value


def recompiler_codegen_version(args: argparse.Namespace) -> int:
    """The producer codegen version a shard publishes to the runner.

    beads-yjp.68: THE single source of truth. The runner refuses to execute a
    shard whose producer codegen version differs from its own
    ndsrecomp::kCodegenVersion (recompiler/src/codegen_identity.h), so the
    number a shard reports must be exactly that counter -- parsed out of the
    `nds-codegen-vN` string the recompiler itself reports.

    It must NOT be SHARD_CODEGEN_VERSION. That is this script's own
    escape-hatch counter for emission changes codegen_fingerprint() cannot
    see; it lives in a different namespace, is bumped for different reasons,
    and only coincided with kCodegenVersion by accident. Publishing it as the
    producer codegen version made the runner's gate compare two unrelated
    counters.
    """
    identity = recompiler_codegen_identity(args)
    # recompiler_codegen_identity() already enforced `nds-codegen-v[0-9]+`.
    return int(identity[len("nds-codegen-v"):])


# Bump when THIS script's contribution to a shard's compiled output changes in
# a way codegen_fingerprint() cannot see -- for example when a recompiler flag
# emitted below keeps its spelling but changes meaning in the recompiler. The
# fingerprint already covers every edit to the emission functions themselves,
# so this is the escape hatch, not the mechanism.
#
# NOT the number a shard publishes as its producer codegen version -- see
# recompiler_codegen_version() above.
SHARD_CODEGEN_VERSION = 1

# The emission surface: everything whose behaviour can change a byte of a
# compiled shard. The fingerprint is the transitive closure of module-level
# names reachable from these, computed from the AST rather than declared, so a
# helper extracted out of one of them is covered automatically instead of
# silently escaping the hash.
#
# Deliberately NOT here, because they select WHAT is compiled and never HOW:
# candidate discovery and ordering, the pending-work queue, snapshot merging,
# the cache index, futility accounting, and main()'s orchestration. Whatever
# they select reaches the output only as `entries`, and work_identity() folds
# the selected entries directly -- so two runs that pick different work already
# get different candidates without the provider identity moving.
CODEGEN_ENTRY_POINTS = (
    "shard_bank_name",
    "recompiler_command",
    "write_config",
    "write_wrapper",
    "shard_source_set",
    "generated_bank_sources",
    "generated_identity",
    "compile_shard_dll",
    "find_import_lib",
)


def _module_members(tree: ast.Module) -> dict[str, list[ast.stmt]]:
    members: dict[str, list[ast.stmt]] = {}
    for node in tree.body:
        names: list[str] = []
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef,
                             ast.ClassDef)):
            names.append(node.name)
        elif isinstance(node, ast.Assign):
            names.extend(target.id for target in node.targets
                         if isinstance(target, ast.Name))
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target,
                                                            ast.Name):
            names.append(node.target.id)
        for name in names:
            members.setdefault(name, []).append(node)
    return members


def _normalized_source(source_lines: list[str], node: ast.stmt) -> str:
    """The node's own source, minus what cannot change behaviour.

    Comment lines, blank lines, trailing whitespace and a leading docstring are
    dropped. Everything else is kept VERBATIM: normalizing further (ast.unparse
    or ast.dump) would make the fingerprint depend on the Python version that
    computed it, and the packager, the dev box and a player's bundled toolchain
    do not run the same interpreter. Keeping raw text costs an occasional
    invalidation for a renamed local; folding an interpreter version into the
    identity would cost every player their cache on a Python upgrade.
    """
    start = node.lineno
    for decorator in getattr(node, "decorator_list", []):
        start = min(start, decorator.lineno)
    end = node.end_lineno or start
    skip: set[int] = set()
    body = getattr(node, "body", None)
    if body and isinstance(body[0], ast.Expr) and isinstance(
            body[0].value, ast.Constant) and isinstance(
                body[0].value.value, str):
        skip.update(range(body[0].lineno, (body[0].end_lineno or
                                           body[0].lineno) + 1))
    kept = []
    for number in range(start, end + 1):
        if number in skip:
            continue
        line = source_lines[number - 1].rstrip()
        stripped = line.lstrip()
        if not stripped or stripped.startswith("#"):
            continue
        kept.append(line)
    return "\n".join(kept)


def codegen_closure() -> list[tuple[str, str]]:
    """(name, normalized source) for the whole reachable emission surface.

    Reachability is COMPUTED from the AST, which is the point: a comment
    convention or a hand-kept list rots on the first refactor, and a rotted
    list means an emission change that no longer moves the identity.
    """
    # utf-8-sig, and rstrip() per line below: a byte-order mark or a CRLF flip
    # is a checkout artifact on this repo (PowerShell writes both), never a
    # semantic change, and neither may move a player's cache.
    source = Path(__file__).read_text(encoding="utf-8-sig")
    tree = ast.parse(source)
    lines = source.splitlines()
    members = _module_members(tree)
    missing = [name for name in CODEGEN_ENTRY_POINTS if name not in members]
    if missing:
        raise RuntimeError(
            "CODEGEN_ENTRY_POINTS names things this module does not define: "
            + ", ".join(missing))
    reached: set[str] = set()
    pending = list(CODEGEN_ENTRY_POINTS)
    while pending:
        name = pending.pop()
        if name in reached:
            continue
        reached.add(name)
        for node in members[name]:
            for inner in ast.walk(node):
                if (isinstance(inner, ast.Name) and inner.id in members
                        and inner.id not in reached):
                    pending.append(inner.id)
    return [
        (name, "\n".join(_normalized_source(lines, node)
                         for node in members[name]))
        for name in sorted(reached)
    ]


def codegen_fingerprint() -> str:
    digest = hashlib.sha256(b"nds-shard-codegen-1\0")
    digest.update(f"{SHARD_CODEGEN_VERSION}\0".encode("ascii"))
    for name, text in codegen_closure():
        digest.update(name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(text.encode("utf-8"))
        digest.update(b"\0")
    return digest.hexdigest()


def provider_identity(args: argparse.Namespace) -> str:
    """Everything that can change a shard's compiled output -- and nothing else.

    A shard is a native DLL that binds directly to runner data symbols and is
    compiled against the runtime struct layouts, so loading one whose codegen
    assumptions differ is silent memory corruption. That is why every field
    below is here. It is equally why the fields that USED to be here are gone:
    an identity that moves for reasons unrelated to the output throws away
    every player's accumulated cache on a rebuild or a logging tweak, which is
    what happened between v0.6.4 and v0.6.5 (beads-yjp.52, beads-yjp.56).

    Removed, with cause:
      * the whole-script SHA -- replaced by codegen_fingerprint(), the closure
        of emission code reachable from CODEGEN_ENTRY_POINTS.
      * the recompiler executable's SHA -- replaced by the semantic version the
        binary reports, because a PE's bytes move on every rebuild.
      * include_roots / merge_cache_snapshots -- RUN-MODE flags. They select
        which observations become candidates; the observations themselves land
        in work_identity() and in the emitted config, so different selections
        already produce different candidates. Folding them here only meant a
        --merge-cache-snapshots re-warm run published under an identity the
        packager could not compute, which defeated the one supported way to
        re-warm a release cache.
    """
    headers = runtime_headers(args)
    value = {
        "schema": 3,
        "abi": ABI_VERSION,
        "shard_codegen": codegen_fingerprint(),
        "recompiler_codegen": recompiler_codegen_identity(args),
        "runtime_header_sha256": {
            path.name: file_sha256(path) for path in headers
        },
        "generated_opt": args.generated_opt,
        "max_function_bytes": args.max_function_bytes,
        **compiler_identity(args),
    }
    return hashlib.sha256(json.dumps(
        value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()[:20]


def work_identity(cpu: int, addr: int, page_sha1: str,
                  entries: list[dict], provider_id: str) -> str:
    value = {
        "schema": 1,
        "provider_id": provider_id,
        "cpu": cpu,
        "addr": addr,
        "page_sha1": page_sha1,
        # Hit counts and observation kinds are scheduling evidence, not code
        # identity. Recompiling because a hot root's counter increased would
        # create an endless successful compile loop for unchanged code.
        "entries": [
            {"addr": entry["addr"], "mode": entry["mode"]}
            for entry in entries
        ],
    }
    return hashlib.sha256(json.dumps(
        value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()[:20]


def shard_bank_name(cpu: int, addr: int) -> str:
    """The bank name, which namespaces every emitted symbol in the shard.

    Part of the emission surface, not orchestration: change it and every
    generated body, the dispatch table symbol and the wrapper's bank_id all
    change with it.
    """
    return f"nds_live_{'arm9' if cpu == 9 else 'arm7'}_{addr:08x}"


def recompiler_command(args: argparse.Namespace, config: Path, image: Path,
                       src_dir: Path, bank: str) -> list[str]:
    """The exact recompiler invocation every live shard is generated by.

    Each flag here selects emission semantics -- one shard per page keyed by
    source address, a split threshold, live-byte validation, the validated
    dependency closure that permits direct transfers inside it, and
    fallthrough coalescing. Dropping or adding one changes the generated C,
    so this list is part of the codegen fingerprint.
    """
    return [
        str(args.recompiler),
        "--config", str(config),
        "--bin", str(image),
        "--out", str(src_dir),
        "--bank", bank,
        "--shards", "1",
        "--stable-address-shards",
        "--max-function-bytes", str(args.max_function_bytes),
        "--validate-live-bytes",
        "--validated-live-direct-calls",
        "--coalesce-fallthroughs",
    ]


def write_config(path: Path, cpu: int, addr: int, digest: str,
                 entries: list[dict]) -> None:
    cpu_name = "arm9" if cpu == 9 else "arm7"
    lines = [
        "# Runtime-generated live overlay config. Do not commit.",
        "[program]",
        f'name = "NDS live {cpu_name} page 0x{addr:08X}"',
        f'id = "nds_live_{cpu_name}_{addr:08x}"',
        f'cpu = "{cpu_name}"',
        f'isa = "{"armv5te" if cpu == 9 else "armv4t"}"',
        f"load_address = 0x{addr:08X}",
        f"size = 0x{PAGE_SIZE:08X}",
        f"entry_pc = 0x{entries[0]['addr']:08X}",
        "authoritative_entry_points = false",
        "",
        "[identity]",
        f'sha1 = "{digest}"',
        "",
    ]
    for entry in entries:
        lines.extend([
            "[[entry_point]]",
            f"addr = 0x{entry['addr']:08X}",
            f'mode = "{entry["mode"]}"',
            'kind = "runtime_generation_observed"',
            f"# hits = {entry['hits']}; kinds = {','.join(entry['kinds'])}",
            "",
        ])
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def generated_identity(src_dir: Path, bank: str, cpu: int,
                       rom_sha1: str, provider_id: str) -> str:
    digest = hashlib.sha256()
    digest.update(f"nds-live-abi-{ABI_VERSION}\n".encode("ascii"))
    digest.update(f"provider={provider_id}\n".encode("ascii"))
    digest.update(f"cpu={cpu}\nrom={rom_sha1}\n".encode("ascii"))
    sources = generated_bank_sources(src_dir, bank)
    header = src_dir / f"{bank}.h"
    if not sources or not header.is_file():
        raise RuntimeError(f"generated source set is incomplete for {bank}")
    for path in [header, *sources]:
        digest.update(path.name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()[:20]


def generated_bank_sources(src_dir: Path, bank: str) -> list[Path]:
    sources = []
    unnumbered = src_dir / f"{bank}.c"
    if unnumbered.is_file():
        sources.append(unnumbered)
    sources.extend(sorted(src_dir.glob(f"{bank}_[0-9][0-9].c")))
    dispatch = src_dir / f"{bank}_dispatch.c"
    if dispatch.is_file():
        sources.append(dispatch)
    return sources


def shard_wrapper_path(src_dir: Path, bank: str) -> Path:
    return src_dir / f"{bank}_live.c"


def shard_source_set(src_dir: Path, bank: str) -> list[Path]:
    """Every translation unit linked into the shard DLL, in link order.

    Emission surface, not orchestration: this is the exact set of C the
    backend compiles. Adding or dropping one changes the DLL.
    """
    return [*generated_bank_sources(src_dir, bank),
            shard_wrapper_path(src_dir, bank)]


def write_wrapper(path: Path, bank: str, candidate_id: str,
                  generation_id: str, rom_sha1: str, cpu: int,
                  codegen_version: int) -> None:
    cpu_token = "NDS_ARM9" if cpu == 9 else "NDS_ARM7"
    exc_base = "0xFFFF0000u" if cpu == 9 else "0x00000000u"
    path.write_text("\n".join([
        '#include "runtime_arm.h"',
        "",
        f"extern const NdsDispatchEntry g_dispatch_{bank}[];",
        f"extern const unsigned g_dispatch_{bank}_len;",
        "",
        "#ifdef _WIN32",
        "#define NDS_LIVE_EXPORT __declspec(dllexport)",
        "#else",
        "#define NDS_LIVE_EXPORT __attribute__((visibility(\"default\")))",
        "#endif",
        "",
        "NDS_LIVE_EXPORT const NdsLiveBankInfo* nds_live_bank_info(void) {",
        "    static NdsLiveBankInfo info;",
        "    info.abi_version = NDS_LIVE_BANK_ABI_VERSION;",
        "    info.flags = NDS_LIVE_BANK_FLAG_DEPENDENCY_CLOSURE;",
        f'    info.bank_id = "{bank}";',
        f'    info.candidate_id = "{candidate_id}";',
        f'    info.title_sha1 = "{rom_sha1}";',
        f"    info.cpu = {cpu_token};",
        # Report what the bodies were ACTUALLY compiled with rather than
        # re-deriving it from `cpu`: the point of the field is to catch the
        # build passing the two independently and disagreeing. No fallback
        # default -- a shard built without -DNDS_STATIC_CPU must fail to
        # compile, not publish a guessed identity.
        "    info.static_cpu = (uint32_t)(NDS_STATIC_CPU);",
        f"    info.exc_base = {exc_base};",
        f"    info.dispatch = g_dispatch_{bank};",
        f"    info.dispatch_len = g_dispatch_{bank}_len;",
        "    info.linked_g_cpu = &g_cpu;",
        "    info.linked_busf_main = &g_busf_main;",
        "    info.linked_busf_itcm = &g_busf_itcm;",
        "    info.linked_runtime_cycles = &g_runtime_cycles;",
        "    return &info;",
        "}",
        "",
        "NDS_LIVE_EXPORT const char* nds_live_generation_id(void) {",
        f'    return "{generation_id}";',
        "}",
        "",
        # beads-yjp.68: the recompiler's own kCodegenVersion, not this
        # script's SHARD_CODEGEN_VERSION. The runner quarantines any shard
        # whose value differs from ndsrecomp::kCodegenVersion, so the two
        # sides must be reading the same counter.
        "NDS_LIVE_EXPORT uint32_t nds_live_codegen_version(void) {",
        f"    return {codegen_version}u;",
        "}",
        "",
    ]), encoding="utf-8", newline="\n")


# ---- TinyCC (tcc) backend â€” the toolchain-free player fallback -------------
#
# tcc bundles its own headers and linker, so it is self-contained: a player box
# needs no gcc, no binutils and no MSYS. Two accommodations are required, and
# both are confined to throwaway copies so the REAL runtime headers (which the
# provider identity hashes) stay byte-identical across backends:
#
#   1. tcc 0.9.27 does not skip a UTF-8 BOM. Strip it from the copies.
#   2. tcc has no equivalent of gcc's -Wl,--enable-auto-import, so every
#      runtime DATA symbol the generated bodies touch (g_cpu, g_busf_main,
#      g_busf_itcm, g_runtime_cycles, ...) fails to link with
#      "undefined symbol 'g_cpu', missing __declspec(dllimport)?". Marking the
#      extern declarations dllimport in the copied headers is what makes the
#      import thunks get emitted. Functions are marked too, which is both
#      harmless and correct â€” they all come from the runner image as well.
#
# NOTE the transform is applied ONLY to the runtime include dirs, never to the
# generated shard's own source dir: that one declares symbols the shard itself
# defines (g_dispatch_<bank>), and importing those would be wrong.
#
# tcc 0.9.27 needs no __builtin_* shim for this codebase: the generated bodies
# use none, and runtime_arm.h names __builtin_clz only in a comment explaining
# why runtime_clz() exists instead. If that changes, add the shim as a prefix
# on the disposable generated source rather than in the headers.

_TCC_EXTERN_RE = re.compile(rb'^(extern[ \t]+)(?!"C")', re.M)

# Runtime data symbols that MUST come out of the transform as dllimport. This
# is a tripwire, not the mechanism: if a header refactor moves these behind a
# macro the regex stops matching and every tcc shard would silently fall back
# to a link error (or worse, a private copy), so assert them up front.
_TCC_REQUIRED_IMPORTS = (
    "g_cpu", "g_busf_main", "g_busf_itcm", "g_runtime_cycles",
)


def _strip_bom(data: bytes) -> bytes:
    return data[3:] if data[:3] == b"\xef\xbb\xbf" else data


def tcc_include_dir(args: argparse.Namespace) -> Path:
    """Memoized TinyCC-compatible copy of the runtime headers.

    Keyed by a digest of the real header contents, so it self-invalidates when
    a header changes and can safely persist in the cache across runs.
    """
    headers = runtime_headers(args)
    digest = hashlib.sha256(
        f"nds-live-tcc-include-2\0{os.name}\0".encode("ascii"))
    payload: list[tuple[str, bytes]] = []
    for path in headers:
        data = _strip_bom(path.read_bytes())
        if os.name == "nt":
            marked, count = _TCC_EXTERN_RE.subn(
                rb"\1__declspec(dllimport) ", data)
        else:
            marked, count = data, 0
        payload.append((path.name, marked))
        digest.update(path.name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(data)
        digest.update(f"\0{count}\0".encode("ascii"))
    out = args.cache / "tcc" / f"include-{digest.hexdigest()[:16]}"
    marker = out / ".complete"
    if not marker.is_file():
        staging = Path(tempfile.mkdtemp(
            prefix="include-", dir=str(args.cache / "tcc")))
        for name, data in payload:
            (staging / name).write_bytes(data)
        (staging / ".complete").write_bytes(b"")
        if out.exists():
            shutil.rmtree(out, ignore_errors=True)
        try:
            os.replace(staging, out)
        except OSError:
            # Another process published the same content-keyed directory.
            shutil.rmtree(staging, ignore_errors=True)
            if not marker.is_file():
                raise
    blob = b"\n".join(
        _strip_bom((out / name).read_bytes()) for name, _ in payload)
    missing = ([] if os.name != "nt" else [
        symbol for symbol in _TCC_REQUIRED_IMPORTS
        if re.search(rb"__declspec\(dllimport\)[^;]*?\b"
                     + symbol.encode("ascii") + rb"\b", blob) is None
    ])
    if missing:
        raise RuntimeError(
            "tcc header transform did not mark these runtime data symbols "
            "dllimport (runtime_arm.h shape changed?): " + ", ".join(missing))
    return out


def tcc_import_dir(args: argparse.Namespace) -> Path:
    """Memoized tcc import library (.def) for the runner image.

    tcc cannot read MinGW's libnds_runner.dll.a ("invalid object file"), but it
    can generate and consume its own .def. `tcc -impdef` reads the export table
    straight out of the runner executable, so no gcc/binutils is involved and
    the import name automatically tracks whatever the shipped exe is called.
    """
    exe = args.runner_exe
    if not exe or not exe.is_file():
        raise RuntimeError(
            "the tcc backend needs --runner-exe pointing at the runner "
            f"executable whose exports the shards import (got: {exe})")
    stem = exe.stem
    key = hashlib.sha256(
        f"nds-live-tcc-impdef-1\0{exe.name}\0".encode("utf-8")
        + file_sha256(exe).encode("ascii")).hexdigest()[:16]
    out = args.cache / "tcc" / f"imp-{key}"
    target = out / f"{stem}.def"
    if not target.is_file():
        out.mkdir(parents=True, exist_ok=True)
        staged = out / f"{stem}.def.{os.getpid()}.tmp"
        result = subprocess.run(
            [str(args.tcc), "-impdef", str(exe), "-o", str(staged)],
            capture_output=True, text=True)
        if result.returncode != 0 or not staged.is_file():
            staged.unlink(missing_ok=True)
            raise RuntimeError(
                f"tcc -impdef failed for {exe} (exit {result.returncode}): "
                + (result.stderr or result.stdout or "").strip())
        os.replace(staged, target)
    return out


def compile_shard_dll(args: argparse.Namespace, sources: list[Path],
                      src_dir: Path, stage: Path, cpu: int) -> bool:
    """Build one shard DLL with the selected backend.

    Both backends must see the SAME preprocessor state. NDS_STATIC_CPU is the
    only define that reaches the generated bodies, and it is load-bearing:
    runtime_arm.h folds g_nds_active on it, and the runner rejects a bank whose
    reported static_cpu disagrees with its cpu. Flag drift between backends is
    the classic way to ship a tcc tier that links but runs the wrong timing
    model, so the define list is built once here rather than per backend.
    """
    static_cpu = 0 if cpu == 9 else 1
    defines = [f"-DNDS_STATIC_CPU={static_cpu}"]
    if args.compiler == "tcc":
        includes = [tcc_include_dir(args), src_dir]
        command = [
            str(args.tcc), "-shared", *defines,
            *[f"-I{path}" for path in includes],
            "-o", str(stage),
            *[str(path) for path in sources],
        ]
        if os.name == "nt":
            import_dir = tcc_import_dir(args)
            command.extend(
                [f"-L{import_dir}", f"-l{args.runner_exe.stem}"])
        ok = run(command).returncode == 0
        # tcc -shared drops an export .def beside the output. It is a build
        # artifact, not part of the published pair; leaving it behind litters
        # the cache namespace the loader scans.
        stage.with_suffix(".def").unlink(missing_ok=True)
        return ok
    command = [
        str(args.gcc), "-shared", args.generated_opt, "-g0", *defines,
        *[f"-I{path}" for path in [*args.runtime_include, src_dir]],
        "-o", str(stage),
        *[str(path) for path in sources],
    ]
    if os.name == "nt":
        command.extend(["-Wl,--enable-auto-import",
                        str(args.runner_import_lib)])
    else:
        command.insert(2, "-fPIC")
    return run(command).returncode == 0


def compile_page(args: argparse.Namespace, page: dict, entries: list[dict],
                 index: dict) -> tuple[str, Path | None]:
    cpu = int(page["cpu"])
    addr = int(page["addr"], 16)
    page_sha1 = str(page["sha1"]).lower()
    raw = base64.b64decode(page["data"], validate=True)
    if len(raw) != PAGE_SIZE:
        raise RuntimeError(
            f"page 0x{addr:08X} has {len(raw)} bytes, expected {PAGE_SIZE}")
    if hashlib.sha1(raw).hexdigest() != page_sha1:
        raise RuntimeError(f"page 0x{addr:08X} SHA-1 does not match payload")

    key = work_identity(cpu, addr, page_sha1, entries, args.provider_id)
    indexed = index.get("captures", {}).get(key)
    if indexed:
        if indexed.get("status") == "unsupported":
            print(f"capture {key}: previously rejected unsupported opcode",
                  flush=True)
            return "skipped", None
        if indexed.get("status") == "compile-failed":
            print(f"capture {key}: previously failed to build under "
                  f"{indexed.get('compiler', '?')}", flush=True)
            return "skipped", None
        dll = Path(indexed.get("dll", ""))
        if dll.is_file():
            print(f"capture {key}: already compiled as {dll}", flush=True)
            return "skipped", dll

    bank = shard_bank_name(cpu, addr)
    work = args.cache / "work" / key
    src_dir = work / "src"
    if src_dir.exists():
        shutil.rmtree(src_dir)
    src_dir.mkdir(parents=True)
    image = work / "page.bin"
    config = work / "page.toml"
    image.write_bytes(raw)
    write_config(config, cpu, addr, page_sha1, entries)

    if run(recompiler_command(args, config, image, src_dir,
                              bank)).returncode != 0:
        return "failed", None

    candidate_id = generated_identity(
        src_dir, bank, cpu, args.rom_sha1, args.provider_id)
    bank_sources = generated_bank_sources(src_dir, bank)
    if any('runtime_unimplemented_op("' in path.read_text(
            encoding="utf-8") for path in bank_sources
            if path.name != f"{bank}_dispatch.c"):
        record_capture(args, index, key, {
            "status": "unsupported",
            "cpu": cpu,
            "page": f"0x{addr:08X}",
            "page_sha1": page_sha1,
            "entries": len(entries),
        })
        print(f"capture {key}: generated body contains an unsupported opcode; "
              "kept in Tier 3", flush=True)
        return "skipped", None
    # Each backend owns a cache namespace. The loader scans both and prefers
    # gcc for the same generation, so a player box still LOADS the gcc shards
    # shipped in the prebuilt cache and only fills the gaps with tcc.
    library_dir = args.cache / args.compiler
    library_dir.mkdir(parents=True, exist_ok=True)
    library = library_dir / f"{bank}_{candidate_id}{SHARED_LIBRARY_SUFFIX}"
    if not library.is_file():
        write_wrapper(shard_wrapper_path(src_dir, bank), bank, candidate_id,
                      page_sha1, args.rom_sha1, cpu,
                      args.recompiler_codegen_version)
        bank_sources = generated_bank_sources(src_dir, bank)
        sources = shard_source_set(src_dir, bank)
        if len(bank_sources) < 2 or not all(path.is_file() for path in sources):
            raise RuntimeError(f"generated source set is incomplete for {bank}")
        stage = library.with_suffix(f".stage{SHARED_LIBRARY_SUFFIX}")
        stage.unlink(missing_ok=True)
        if not compile_shard_dll(args, sources, src_dir, stage, cpu):
            stage.unlink(missing_ok=True)
            # Account for the failure per shard instead of retrying this exact
            # page every trigger forever. The key folds provider_id, which
            # folds the backend, so a gcc pass over the same cache is still
            # free to attempt (and publish) the page that tcc could not build.
            record_capture(args, index, key, {
                "status": "compile-failed",
                "compiler": args.compiler,
                "cpu": cpu,
                "page": f"0x{addr:08X}",
                "page_sha1": page_sha1,
                "entries": len(entries),
            })
            print(f"capture {key}: {args.compiler} could not build this page; "
                  "kept in Tier 3", flush=True)
            return "skipped", None
        os.replace(stage, library)

    record_capture(args, index, key, {
        "candidate_id": candidate_id,
        "provider_id": args.provider_id,
        "generation_id": page_sha1,
        "cpu": cpu,
        "page": f"0x{addr:08X}",
        "page_sha1": page_sha1,
        "entries": len(entries),
        # beads-yjp.53: the roots this shard actually translated, so a later
        # capture of the same page bytes can be forced to be a superset of it
        # instead of replacing it with a smaller set. `entries` above stays as
        # the count it always was; this is an additive field an older reader
        # ignores.
        "entry_roots": [
            {"addr": int(entry["addr"]), "mode": str(entry["mode"])}
            for entry in entries
        ],
        "dll": library.resolve().as_posix(),
    })
    print(f"NDS_SHARD_PUBLISHED {library.resolve().as_posix()}", flush=True)
    return "ok", library


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path,
                        default=(Path(os.environ["NDS_LIVE_OVERLAY_MANIFEST"])
                                 if os.environ.get("NDS_LIVE_OVERLAY_MANIFEST")
                                 else None))
    parser.add_argument("--cache", type=Path,
                        default=(Path(os.environ["NDS_LIVE_OVERLAY_CACHE"])
                                 if os.environ.get("NDS_LIVE_OVERLAY_CACHE")
                                 else None))
    parser.add_argument("--rom-sha1",
                        default=os.environ.get("NDS_LIVE_OVERLAY_ROM_SHA1", ""))
    # Optional because the bundled player toolchain is not a source checkout:
    # it ships flattened headers and passes --runtime-include instead.
    parser.add_argument("--ndsrecomp-root", type=Path)
    parser.add_argument("--runtime-include", type=Path, action="append",
                        default=[],
                        help="directory of runtime ABI headers the generated "
                             "shard includes; repeatable. Defaults to the "
                             "in-tree pair under --ndsrecomp-root.")
    # Only the gcc backend links the MinGW import library out of a runner
    # build tree; tcc derives its own .def from --runner-exe.
    parser.add_argument("--runner-build", type=Path)
    parser.add_argument("--runner-exe", type=Path,
                        help="runner executable whose exports the shards "
                             "import (required by the tcc backend)")
    parser.add_argument("--recompiler", type=Path, required=True)
    parser.add_argument("--compiler", choices=BACKENDS, default="gcc",
                        help="shard backend: gcc (dev/CI) or tcc (bundled, "
                             "toolchain-free player fallback)")
    parser.add_argument("--gcc", default="gcc")
    parser.add_argument("--tcc", default="tcc")
    parser.add_argument("--generated-opt", default="-O2")
    parser.add_argument("--max-function-bytes", type=int, default=512)
    # No literal default: the runner decides the batch cap per run and passes
    # it in NDS_LIVE_OVERLAY_MAX_PAGES, raising it while a backlog is pending
    # and dropping back to the conservative base once the queue drains. An
    # explicit --max-pages still wins, for dev/CI callers that drive the tool
    # directly.
    parser.add_argument("--max-pages", type=int, default=None,
                        help="maximum new page candidates compiled per run "
                             "(default: $NDS_LIVE_OVERLAY_MAX_PAGES, else 6)")
    parser.add_argument("--min-hits", type=int, default=1)
    parser.add_argument("--cpu", type=int, choices=(7, 9), action="append")
    parser.add_argument("--include-roots", action="store_true",
                        help="also compile root-map PCs from interpreted "
                             "spans, not only call/indirect entry points")
    parser.add_argument("--merge-cache-snapshots", action="store_true",
                        help="merge entry/root observations from cached live "
                             "snapshot manifests for the same page bytes")
    parser.add_argument("--merge-manifest", type=Path, action="append",
                        default=[],
                        help="additional coverage manifest to merge before "
                             "candidate selection")
    parser.add_argument("--exclude-range", type=parse_range, action="append",
                        default=[])
    args = parser.parse_args()

    if not SHARED_LIBRARY_SUFFIX:
        raise SystemExit("live shard compilation supports Windows and Linux")

    if not args.manifest or not args.manifest.is_file():
        raise SystemExit(f"manifest does not exist: {args.manifest}")
    if not args.cache:
        raise SystemExit("cache path is required")
    if not args.rom_sha1:
        raise SystemExit("ROM SHA-1 is required")
    if not args.recompiler.is_file():
        raise SystemExit(f"recompiler does not exist: {args.recompiler}")
    if not args.runtime_include:
        if not args.ndsrecomp_root:
            raise SystemExit(
                "pass --runtime-include or --ndsrecomp-root so the runtime "
                "ABI headers can be found")
        # runtime_arm.h lives in the recompiler tree but includes
        # runtime_arm_types.h out of the shared ARM core submodule; a shard
        # needs BOTH on the include path or it does not preprocess at all.
        args.runtime_include = [
            args.ndsrecomp_root / "recompiler" / "armv4t",
            args.ndsrecomp_root / "external" / "arm-recomp-core" / "common",
        ]
    args.runtime_include = [
        path for path in args.runtime_include if path.is_dir()]
    if not args.runtime_include:
        raise SystemExit("no runtime include directory exists")
    if args.compiler == "tcc" and os.name == "nt":
        if not args.runner_exe or not args.runner_exe.is_file():
            raise SystemExit(
                "the tcc backend needs --runner-exe (the runner executable "
                f"whose exports the shards import); got {args.runner_exe}")
    elif args.compiler == "gcc" and os.name == "nt" and not args.runner_build:
        raise SystemExit("the gcc backend needs --runner-build")
    if args.max_pages is None:
        try:
            args.max_pages = int(
                os.environ.get("NDS_LIVE_OVERLAY_MAX_PAGES", "") or 6)
        except ValueError:
            args.max_pages = 6
    args.max_pages = max(1, args.max_pages)
    args.cache.mkdir(parents=True, exist_ok=True)
    args.index = args.cache / "live-index.json"
    args.queue = args.cache / QUEUE_NAME
    args.runner_import_lib = (
        find_import_lib(args.runner_build)
        if args.compiler == "gcc" and os.name == "nt" else None)
    if args.compiler == "tcc":
        (args.cache / "tcc").mkdir(parents=True, exist_ok=True)
    args.provider_id = provider_identity(args)
    # beads-yjp.68: resolved once, here, so every wrapper this run emits
    # publishes the same producer codegen version the runner will compare
    # against its own ndsrecomp::kCodegenVersion.
    args.recompiler_codegen_version = recompiler_codegen_version(args)

    manifest = load_json(args.manifest)
    if (not isinstance(manifest, dict) or
            manifest.get("kind") != "ndsrecomp-tier3-coverage" or
            int(manifest.get("schema", 0)) < 3):
        raise SystemExit("live sharding requires a schema-3 coverage manifest")
    if str(manifest.get("rom_sha1", "")).lower() != args.rom_sha1.lower():
        raise SystemExit("coverage manifest ROM SHA-1 does not match the runner")

    with exclusive_file_lock(args.cache / INDEX_LOCK_NAME):
        index = (load_json(args.index) if args.index.is_file()
                 else empty_index(args.rom_sha1))
    if index.get("rom_sha1") != args.rom_sha1:
        raise SystemExit("live cache index belongs to a different ROM")
    if int(index.get("schema", 0)) not in (1, 2):
        raise SystemExit("live cache index schema is unsupported")
    index["schema"] = 2

    allowed_cpus = set(args.cpu or (7, 9))
    manifests: list[tuple[Path, dict]] = [(args.manifest, manifest)]
    for path in args.merge_manifest:
        loaded = load_json(path)
        if not isinstance(loaded, dict):
            raise SystemExit(f"merge manifest is not an object: {path}")
        manifests.append((path, loaded))
    if args.merge_cache_snapshots:
        manifests.extend(cache_manifests(args.cache, args.manifest))

    # Resume before discovering. Anything the previous run left pending is
    # brought back with its payload and its accumulated weight, so a fresh
    # process picks the backlog up where it stopped instead of waiting for the
    # guest to re-execute the same pages.
    queued = load_queue(args)
    known: set[Path] = set()
    for path, _ in manifests:
        try:
            known.add(Path(path).resolve())
        except OSError:
            continue
    manifests.extend(resume_manifests(queued, known))
    carried = {
        (entry["cpu"], entry["addr"], entry["page_sha1"]): entry
        for entry in queued
    }
    if queued:
        print(f"resumed {len(queued)} pending candidate(s) from "
              f"{args.queue}", flush=True)

    candidates = collect_candidates(args, manifests, allowed_cpus, carried,
                                    generation_entries(index))

    ok = failed = skipped = attempted = 0
    pending: list[dict] = []

    def remember(page: dict, hits: int, executions: int,
                 manifest_path: str) -> None:
        pending.append({
            "cpu": int(page["cpu"]),
            "addr": int(page["addr"], 16),
            "page_sha1": str(page["sha1"]).lower(),
            "executions": executions,
            "hits": hits,
            "manifest": manifest_path,
        })

    for hits, executions, page, entries, manifest_path in candidates:
        key = work_identity(int(page["cpu"]), int(page["addr"], 16),
                            str(page["sha1"]).lower(), entries,
                            args.provider_id)
        indexed = index.get("captures", {}).get(key)
        if indexed and (indexed.get("status") in ("unsupported",
                                                  "compile-failed") or
                        Path(indexed.get("dll", "")).is_file()):
            skipped += 1
            continue
        if attempted >= args.max_pages:
            # Over the batch cap: this is the backlog, and it is what the
            # queue exists to carry. Candidates stay in the order decided
            # above, so the next batch takes the next-hottest work.
            remember(page, hits, executions, manifest_path)
            continue
        attempted += 1
        try:
            outcome, _dll = compile_page(args, page, entries, index)
        except Exception as error:
            print(f"live shard failed: {error}", file=sys.stderr, flush=True)
            outcome = "failed"
        if outcome == "ok":
            ok += 1
        elif outcome == "skipped":
            # Either already published or permanently recorded as unsupported
            # / compile-failed in the index. Both are terminal: not pending.
            skipped += 1
        else:
            # A transient failure (the recompiler itself errored) leaves no
            # index record, so keep it queued rather than dropping the work.
            failed += 1
            remember(page, hits, executions, manifest_path)

    store_queue(args, pending)
    pruned = prune_snapshots(args, pending)
    if pruned:
        print(f"pruned {pruned} snapshot manifest(s) no longer referenced by "
              "the pending queue", flush=True)
    print(f"NDS_SHARD_RESULT ok={ok} failed={failed} skipped={skipped} "
          f"pending={len(pending)} cap={args.max_pages}", flush=True)
    # Emitted separately as well so the runner's log scanner reads one scalar
    # off a stable marker instead of parsing the human-facing result line.
    print(f"NDS_SHARD_PENDING {len(pending)}", flush=True)
    return 2 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
