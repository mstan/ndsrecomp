#!/usr/bin/env python3
"""End-to-end test for generation-bound live shard compilation and caching."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path


ROM_SHA1 = "0123456789abcdef0123456789abcdef01234567"
BASE = 0x02000000
PAGE_SIZE = 4096


def arm_program(value: int) -> bytes:
    page = bytearray(PAGE_SIZE)
    # BL 0x02000020; BX LR
    page[0:8] = bytes.fromhex("060000eb1eff2fe1")
    # MOV r0,#value; BX LR
    page[0x20:0x28] = bytes((value, 0x00, 0xA0, 0xE3,
                             0x1E, 0xFF, 0x2F, 0xE1))
    # An unconnected function used to prove a later revision can expand the
    # same resident generation without colliding with its older revision.
    page[0x40:0x48] = bytes.fromhex("0300a0e31eff2fe1")
    return bytes(page)


def manifest(path: Path, page: bytes, roots: list[int], hits: int = 100) -> None:
    digest = hashlib.sha1(page).hexdigest()
    value = {
        "schema": 3,
        "kind": "ndsrecomp-tier3-coverage",
        "rom_sha1": ROM_SHA1,
        "rom_name": "synthetic.nds",
        "build_id": "live-shard-cache-test",
        "static_coverage": {},
        "entry_points_arm9": [],
        "entry_points_arm7": [],
        "pages": {
            "captured": 1,
            "dropped": 0,
            "replaced": 0,
            "bytes": PAGE_SIZE,
            "revisits": 0,
            "page_size": PAGE_SIZE,
            "entries": [{
                "addr": f"0x{BASE:08X}",
                "cpu": 9,
                "sha1": digest,
                "executions": hits,
                "entry_points": [{
                    "addr": f"0x{root:08X}",
                    "mode": "arm",
                    "kind": "root",
                    "hits": hits,
                    "caller": "0x00000000",
                } for root in roots],
                "data": base64.b64encode(page).decode("ascii"),
            }],
        },
    }
    path.write_text(json.dumps(value), encoding="utf-8", newline="\n")


def run_tool(args: argparse.Namespace, manifest_path: Path,
             expect_ok: bool = True,
             generated_opt: str = "-O2") -> subprocess.CompletedProcess[str]:
    command = [
        sys.executable, str(args.tool),
        "--manifest", str(manifest_path),
        "--cache", str(args.cache),
        "--rom-sha1", ROM_SHA1,
        "--ndsrecomp-root", str(args.ndsrecomp_root),
        "--runner-build", str(args.runner_build),
        "--recompiler", str(args.recompiler),
        "--gcc", str(args.gcc),
        f"--generated-opt={generated_opt}",
        "--max-pages", "8",
        "--min-hits", "1",
    ]
    result = subprocess.run(
        command, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT)
    print(result.stdout, end="")
    if expect_ok and result.returncode != 0:
        raise AssertionError(f"live compiler failed with {result.returncode}")
    if not expect_ok and result.returncode == 0:
        raise AssertionError("malformed manifest unexpectedly succeeded")
    return result


def dlls(cache: Path) -> list[Path]:
    return sorted((cache / "gcc").glob("*.dll"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--ndsrecomp-root", type=Path, required=True)
    parser.add_argument("--runner-build", type=Path, required=True)
    parser.add_argument("--recompiler", type=Path, required=True)
    parser.add_argument("--gcc", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    args.work = args.work.resolve()
    args.cache = args.work / "cache"
    if args.work.exists():
        shutil.rmtree(args.work)
    args.work.mkdir(parents=True)

    gen_a = args.work / "generation-a.json"
    gen_b = args.work / "generation-b.json"
    gen_a_hot = args.work / "generation-a-hot.json"
    gen_a_expanded = args.work / "generation-a-expanded.json"
    manifest(gen_a, arm_program(1), [BASE])
    manifest(gen_b, arm_program(2), [BASE])
    manifest(gen_a_hot, arm_program(1), [BASE], hits=999999)
    manifest(gen_a_expanded, arm_program(1), [BASE, BASE + 0x40])

    result = run_tool(args, gen_a)
    assert "NDS_SHARD_RESULT ok=1 failed=0" in result.stdout
    assert len(dlls(args.cache)) == 1

    result = run_tool(args, gen_b)
    assert "NDS_SHARD_RESULT ok=1 failed=0" in result.stdout
    assert len(dlls(args.cache)) == 2

    before = {path: path.stat().st_mtime_ns for path in dlls(args.cache)}
    result = run_tool(args, gen_a_hot)
    assert "NDS_SHARD_RESULT ok=0 failed=0" in result.stdout
    assert before == {path: path.stat().st_mtime_ns for path in dlls(args.cache)}

    result = run_tool(args, gen_a_expanded)
    assert "NDS_SHARD_RESULT ok=1 failed=0" in result.stdout
    assert len(dlls(args.cache)) == 3
    assert not list(args.cache.rglob("*.stage.dll"))

    index = json.loads((args.cache / "live-index.json").read_text(
        encoding="utf-8"))
    assert index["schema"] == 2 and len(index["captures"]) == 3
    generations = {item["generation_id"]
                   for item in index["captures"].values()}
    assert len(generations) == 2

    generated = "\n".join(
        path.read_text(encoding="utf-8")
        for path in (args.cache / "work").rglob("*.c"))
    assert "nds_live_arm9_02000000_afunc_02000020();" in generated
    assert "NdsStaticValidationRange" in generated
    assert "g_validation_closure_" in generated
    assert "nds_live_generation_id" in generated

    # beads-yjp.53: a later capture of an ALREADY published generation only
    # sees the roots still reaching Tier 3, so its observed set can be
    # SMALLER than the shard in the cache. Compiling that set produced a
    # same-generation candidate that the runtime then let supersede the larger
    # one, dropping rows and sending that code back to the interpreter -- where
    # the next capture rediscovered it. The published roots are merged back in,
    # so the work identity matches the expanded candidate already in the cache
    # and nothing is compiled at all.
    gen_a_shrunk = args.work / "generation-a-shrunk.json"
    manifest(gen_a_shrunk, arm_program(1), [BASE + 0x40])
    before = {path: path.stat().st_mtime_ns for path in dlls(args.cache)}
    result = run_tool(args, gen_a_shrunk)
    assert "NDS_SHARD_RESULT ok=0 failed=0" in result.stdout, result.stdout
    assert len(dlls(args.cache)) == 3
    assert before == {path: path.stat().st_mtime_ns for path in dlls(args.cache)}
    index = json.loads((args.cache / "live-index.json").read_text(
        encoding="utf-8"))
    published_roots = {
        tuple(sorted(int(root["addr"])
                     for root in item.get("entry_roots", [])))
        for item in index["captures"].values()
    }
    assert (BASE, BASE + 0x40) in published_roots, published_roots

    # A toolchain/options change is a new provider identity even when the
    # guest bytes and roots are unchanged. It must produce a distinct DLL,
    # then reuse that exact provider candidate on the next run.
    result = run_tool(args, gen_a, generated_opt="-O0")
    assert "NDS_SHARD_RESULT ok=1 failed=0" in result.stdout
    assert len(dlls(args.cache)) == 4
    result = run_tool(args, gen_a, generated_opt="-O0")
    assert "NDS_SHARD_RESULT ok=0 failed=0" in result.stdout
    assert len(dlls(args.cache)) == 4
    index = json.loads((args.cache / "live-index.json").read_text(
        encoding="utf-8"))
    provider_ids = {item.get("provider_id")
                    for item in index["captures"].values()}
    assert None not in provider_ids and len(provider_ids) == 2

    bad = json.loads(gen_a.read_text(encoding="utf-8"))
    bad["schema"] = 2
    bad_path = args.work / "bad-schema.json"
    bad_path.write_text(json.dumps(bad), encoding="utf-8")
    run_tool(args, bad_path, expect_ok=False)

    print("PASS: live shard cache preserves generations and reuses warm work")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
