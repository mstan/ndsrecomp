#!/usr/bin/env python3
"""beads-yjp.68: the producer-codegen load gate, through the real loader.

A live shard is a native DLL that binds directly to runner data symbols and
was compiled against the runtime struct layouts of whatever recompiler
produced it. recompiler/src/codegen_identity.h states the consequence: running
one whose codegen assumptions differ from the runner's is silent memory
corruption. So a cached shard whose producer codegen version is not exactly
ndsrecomp::kCodegenVersion must be refused OUTRIGHT -- no partial row set, no
gap filling -- and moved out of the scanned part of the cache so the next
launch does not pay to reject it again.

The gate lives in live_overlay.cpp::prepare_bank_dll(), the one funnel every
cached shard passes through. Only a real DLL reaches it, which is why this test
builds two of them -- one at kCodegenVersion-1, one at kCodegenVersion -- and
drives them through the actual loader in live_overlay_preflight_test
(--load-cache-json), rather than stubbing the predicate the way the first
attempt at this bug did.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import compile_live_shards as tool  # noqa: E402

ROM_SHA1 = "test"

# The generated bank a shard wraps: one dispatch row over four zero bytes,
# with the dependency closure the loader's preflight requires. The runner data
# symbols are IMPORTED, not defined here -- a shard that defined its own copies
# would fail the linked-import check instead of reaching the codegen gate, so
# the DLL binds to the test executable exactly as a field shard binds to
# nds_runner.exe.
STUB = r"""
#include "runtime_arm.h"

static void body(void) {}
static const unsigned char expected[4] = {0, 0, 0, 0};
static const NdsStaticValidationRange dependencies[] = {
    {0x02000000u, sizeof(expected), expected},
};
static const NdsStaticValidation validation = {
    0x02000000u, sizeof(expected), expected, dependencies, 1u};
const NdsDispatchEntry g_dispatch_%(bank)s[] = {
    {0x02000000u, 0u, body, &validation},
};
const unsigned g_dispatch_%(bank)s_len = 1u;
"""

SUFFIX = ".dll" if sys.platform == "win32" else ".so"


def runner_codegen_version(header: Path) -> int:
    """kCodegenVersion as the runner sees it, read from the same header."""
    text = header.read_text(encoding="utf-8")
    match = re.search(
        r"inline\s+constexpr\s+unsigned\s+kCodegenVersion\s*=\s*(\d+)\s*;",
        text)
    if not match:
        raise AssertionError(f"cannot read kCodegenVersion from {header}")
    return int(match.group(1))


def build_shard(work: Path, cc: str, implib_dir: Path, runner_test: Path,
                codegen_version: int) -> Path:
    """One published shard in its own cache directory, at a given version."""
    bank = "nds_live_arm9_02000000"
    cache = work / f"cache-v{codegen_version}"
    source = work / f"source-v{codegen_version}"
    backend = cache / "gcc"
    source.mkdir(parents=True)
    backend.mkdir(parents=True)

    stub = source / "stub.c"
    stub.write_text(STUB % {"bank": bank}, encoding="utf-8", newline="\n")
    wrapper = source / f"{bank}_live.c"
    tool.write_wrapper(wrapper, bank, f"candidate-v{codegen_version}",
                       f"generation-v{codegen_version}", ROM_SHA1, 9,
                       codegen_version)

    final = backend / f"{bank}_candidate-v{codegen_version}{SUFFIX}"
    stage = final.with_suffix(f".stage{SUFFIX}")
    command = [
        cc, "-shared", "-O0", "-g0", "-DNDS_STATIC_CPU=0",
        "-I", str(ROOT / "recompiler" / "armv4t"),
        "-I", str(ROOT / "external" / "arm-recomp-core" / "common"),
        "-o", str(stage), str(stub), str(wrapper),
    ]
    if sys.platform == "win32":
        command.extend([f"-L{implib_dir}", f"-l{runner_test.stem}"])
    else:
        command.insert(2, "-fPIC")
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    if result.returncode != 0:
        print(result.stdout, end="")
        raise AssertionError(
            f"shard for codegen-v{codegen_version} failed to link")
    os.replace(stage, final)
    return cache


def load(runner_test: Path, cache: Path) -> tuple[dict, str]:
    """Run the real loader over `cache`; return (status JSON, stderr)."""
    result = subprocess.run(
        [str(runner_test), "--load-cache-json", str(cache)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        raise AssertionError(
            f"loader harness exited {result.returncode}")
    line = next((piece for piece in reversed(result.stdout.splitlines())
                 if piece.startswith("{")), "")
    if not line:
        print(result.stdout, end="")
        raise AssertionError("loader harness printed no status JSON")
    return json.loads(line), result.stderr


def quarantined(cache: Path) -> list[Path]:
    root = cache / "quarantine"
    if not root.is_dir():
        return []
    return sorted(p for p in root.rglob(f"*{SUFFIX}"))


def registered(status: dict) -> list[dict]:
    """Banks actually serving rows out of the dispatch index."""
    return [bank for bank in status.get("loaded", [])
            if bank.get("registered") and not bank.get("superseded")]


def scanned(cache: Path) -> list[Path]:
    """Shards still in the part of the cache the loader walks."""
    return sorted(p for p in cache.rglob(f"*{SUFFIX}")
                  if "quarantine" not in p.relative_to(cache).parts)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--runner-test", type=Path, required=True)
    parser.add_argument("--implib-dir", type=Path, required=True)
    parser.add_argument("--codegen-identity-header", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()

    current = runner_codegen_version(args.codegen_identity_header)
    if current < 1:
        raise AssertionError("kCodegenVersion must be >= 1 for this test")
    stale = current - 1

    args.work = args.work.resolve()
    if args.work.exists():
        shutil.rmtree(args.work)
    args.work.mkdir(parents=True)

    failures: list[str] = []

    def check(name: str, condition: bool, detail: str = "") -> None:
        print(f"{'PASS' if condition else 'FAIL'}: {name}"
              f"{'' if condition else ' -- ' + detail}", flush=True)
        if not condition:
            failures.append(name)

    # ---- the stale half: refused outright, and moved aside ----------------
    cache = build_shard(args.work, args.cc, args.implib_dir, args.runner_test,
                        stale)
    before = scanned(cache)
    status, stderr = load(args.runner_test, cache)
    reasons = status.get("reject_reasons", {})
    check("stale_shard_publishes_no_bank", status.get("banks_loaded") == 0,
          f"banks_loaded={status.get('banks_loaded')}")
    check("stale_shard_registers_no_rows",
          not status.get("loaded") and not registered(status),
          f"loaded={status.get('loaded')}")
    check("stale_shard_counted_rejected", status.get("banks_rejected") == 1,
          f"banks_rejected={status.get('banks_rejected')}")
    check("stale_shard_reason_is_codegen_mismatch",
          reasons.get("load_codegen_mismatch") == 1,
          f"load_codegen_mismatch={reasons.get('load_codegen_mismatch')} "
          f"in {reasons}")
    check("stale_shard_error_names_both_versions",
          f"codegen-v{stale}" in str(status.get("last_error", "")) and
          f"codegen-v{current}" in str(status.get("last_error", "")),
          str(status.get("last_error")))
    check("stale_shard_quarantined", len(quarantined(cache)) == 1,
          f"quarantine holds {quarantined(cache)}")
    check("stale_shard_quarantine_names_producer_version",
          any(f"nds-codegen-v{stale}" in p.parts for p in quarantined(cache)),
          f"{[str(p) for p in quarantined(cache)]}")
    check("stale_shard_left_the_scanned_cache",
          before and not scanned(cache),
          f"still scanned: {[str(p) for p in scanned(cache)]}")
    check("stale_shard_logged_one_summary_line",
          stderr.count("quarantined 1 cached shard(s)") == 1,
          stderr.strip() or "(no stderr)")

    # A second launch must not re-load, re-reject or re-log the quarantined
    # shard: that is the whole point of moving it.
    status2, stderr2 = load(args.runner_test, cache)
    check("quarantine_is_not_rescanned",
          status2.get("banks_rejected") == 0 and
          status2.get("banks_loaded") == 0 and
          status2.get("reject_reasons", {}).get(
              "load_codegen_mismatch", 0) == 0,
          json.dumps(status2.get("reject_reasons", {})))
    check("quarantine_is_not_relogged", "quarantined" not in stderr2,
          stderr2.strip() or "(no stderr)")

    # ---- the current half: passes the gate and is adopted -----------------
    fresh = build_shard(args.work, args.cc, args.implib_dir, args.runner_test,
                        current)
    status3, _ = load(args.runner_test, fresh)
    reasons3 = status3.get("reject_reasons", {})
    check("current_shard_passes_the_gate",
          reasons3.get("load_codegen_mismatch", 0) == 0,
          json.dumps(reasons3))
    check("current_shard_is_adopted", status3.get("banks_loaded") == 1 and
          len(registered(status3)) == 1,
          f"banks_loaded={status3.get('banks_loaded')} "
          f"loaded={json.dumps(status3.get('loaded'))} "
          f"last_error={status3.get('last_error')}")
    check("current_shard_reports_its_codegen_version",
          [bank.get("codegen_version") for bank in registered(status3)] ==
          [current],
          json.dumps(status3.get("loaded", [])))
    check("current_shard_not_quarantined", not quarantined(fresh),
          f"{[str(p) for p in quarantined(fresh)]}")

    # ---- the producer side: one source of truth ---------------------------
    # The number a wrapper publishes must be the recompiler's kCodegenVersion,
    # never this script's own SHARD_CODEGEN_VERSION escape hatch. They are
    # unrelated counters that happened to coincide.
    emitted = args.work / "wrapper-source-of-truth.c"
    tool.write_wrapper(emitted, "nds_live_arm9_02000000", "c", "g", ROM_SHA1,
                       9, current)
    text = emitted.read_text(encoding="utf-8")
    check("wrapper_publishes_the_recompiler_codegen_version",
          f"return {current}u;" in text, text)
    check("wrapper_does_not_publish_shard_codegen_version",
          tool.SHARD_CODEGEN_VERSION == current or
          f"return {tool.SHARD_CODEGEN_VERSION}u;" not in text,
          f"SHARD_CODEGEN_VERSION={tool.SHARD_CODEGEN_VERSION} leaked "
          "into the wrapper")

    if failures:
        print(f"\nFAILED: {', '.join(failures)}", file=sys.stderr)
        return 1
    print("\nPASS: producer-codegen load gate quarantines stale shards and "
          "admits current ones")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
