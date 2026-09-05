#!/usr/bin/env python3
"""Build an ELF live bank with runner imports and prove dlopen adoption."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import compile_live_shards as tool  # noqa: E402


STUB = r"""
#include "runtime_arm.h"

static void body(void) {}
static const unsigned char expected[4] = {0, 0, 0, 0};
static const NdsStaticValidationRange dependencies[] = {
    {0x02000000u, sizeof(expected), expected},
};
static const NdsStaticValidation validation = {
    0x02000000u, sizeof(expected), expected, dependencies, 1u};
const NdsDispatchEntry g_dispatch_nds_live_arm9_02000000[] = {
    {0x02000000u, 0u, body, &validation},
};
const unsigned g_dispatch_nds_live_arm9_02000000_len = 1u;
"""


def runner_codegen_version() -> int:
    """ndsrecomp::kCodegenVersion, read from the header that declares it."""
    header = ROOT / "recompiler" / "src" / "codegen_identity.h"
    match = re.search(
        r"inline\s+constexpr\s+unsigned\s+kCodegenVersion\s*=\s*(\d+)\s*;",
        header.read_text(encoding="utf-8"))
    if not match:
        raise AssertionError(f"cannot read kCodegenVersion from {header}")
    return int(match.group(1))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--runner-test", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()

    if sys.platform != "linux":
        raise SystemExit("this test is Linux-specific")
    if args.work.exists():
        shutil.rmtree(args.work)
    source = args.work / "source"
    cache = args.work / "cache" / "gcc"
    source.mkdir(parents=True)
    cache.mkdir(parents=True)
    bank = "nds_live_arm9_02000000"
    stub = source / "stub.c"
    wrapper = source / f"{bank}_live.c"
    stub.write_text(STUB, encoding="utf-8", newline="\n")
    tool.write_wrapper(wrapper, bank, "linux-loader-test",
                       "linux-generation-test", "test", 9,
                       # beads-yjp.68: the runner refuses any other producer
                       # codegen version outright, so an adoption test has to
                       # publish the current one.
                       runner_codegen_version())
    final = cache / f"{bank}_linux-loader-test.so"
    stage = final.with_suffix(".stage.so")
    command = [
        args.cc, "-shared", "-fPIC", "-O0", "-g0",
        "-DNDS_STATIC_CPU=0",
        "-I", str(ROOT / "recompiler" / "armv4t"),
        "-I", str(ROOT / "external" / "arm-recomp-core" / "common"),
        "-o", str(stage), str(stub), str(wrapper),
    ]
    subprocess.run(command, check=True)
    os.replace(stage, final)
    result = subprocess.run(
        [str(args.runner_test), "--load-cache", str(args.work / "cache")],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, end="")
    if result.returncode != 0:
        raise AssertionError(
            f"Linux live-overlay loader test failed with {result.returncode}")
    print("PASS: Linux ELF shard resolves runner imports and is adopted")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
