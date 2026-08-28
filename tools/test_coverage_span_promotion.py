#!/usr/bin/env python3
"""Pin the interpreted-span promotion in ingest_coverage_manifests.py.

beads-yjp.55. Before this, the ingest could only ever promote the TARGET of an
interpreted call or indirect branch. Those are addresses some compiled function
already branched to, so the recompiler's finder had discovered them already;
the code the interpreter is actually executing sits in the holes BETWEEN
compiled functions, where a computed branch or a jump table stopped discovery.
The only record of that code is the root map, and nothing consumed it.

Each assertion below fails if the promotion is removed or coarsened:

  * a span start becomes a seed at all;
  * exactly ONE seed per span -- the interior roots must NOT be seeded, because
    the recompiler emits one dispatch row per instruction across a function and
    gives every extra seed its own Function record, trimming the previous one;
  * the seed carries the mode the roots were recorded in, and an ARM run is
    never merged with a Thumb one;
  * a caller is treated as an interpreted-code observation;
  * `--no-promote-ranges` still reproduces the old, seedless policy.
"""

from __future__ import annotations

import base64
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

TOOL = Path(__file__).resolve().parent / "ingest_coverage_manifests.py"
ROM_SHA1 = "0" * 40

# One 4 KiB page. `hole` is a run of five consecutive ARM instructions the
# interpreter was entered at -- the shape of the real MPH finding
# (0x0207B630..0x0207B684). `lone` is an isolated root a long way from it, so
# the two must stay separate spans. `thumb` overlaps `hole` in address space
# but in the other mode, so merging modes would corrupt both.
PAGE = 0x02080000
HOLE = [PAGE + 0x100 + 4 * i for i in range(5)]
LONE = PAGE + 0x800
THUMB = [PAGE + 0x104, PAGE + 0x106]
CALLER = PAGE + 0xC00
TARGET = PAGE + 0xE00


def bitmap(addrs, stride, page_size=4096):
    raw = bytearray(page_size // stride // 8)
    for addr in addrs:
        bit = (addr - PAGE) // stride
        raw[bit // 8] |= 1 << (bit % 8)
    return base64.b64encode(bytes(raw)).decode("ascii")


def manifest(path: Path) -> None:
    hits = [0] * 16
    for addr in HOLE:
        hits[(addr - PAGE) * 16 // 4096] += 200
    hits[(LONE - PAGE) * 16 // 4096] += 7
    record = {
        "addr": f"0x{PAGE:08X}",
        "cpu": 9,
        "root_arm": bitmap(HOLE + [LONE], 4),
        "root_thumb": bitmap(THUMB, 2),
        "root_hits": hits,
    }
    path.write_text(json.dumps({
        "kind": "ndsrecomp-tier3-coverage",
        "schema": 4,
        "rom_sha1": ROM_SHA1,
        "build_id": "test",
        "entry_points_arm9": [{
            "addr": f"0x{TARGET:08X}", "mode": "arm", "kind": "call",
            "hits": 3, "caller": f"0x{CALLER:08X}",
        }],
        "entry_points_arm7": [],
        "root_map": [record],
        "pages": {"captured": 0, "dropped": 0, "replaced": 0, "bytes": 0,
                  "revisits": 0, "page_size": 4096, "entries": []},
    }), encoding="utf-8", newline="\n")


def seeds_of(out: Path) -> dict[tuple[int, str], str]:
    """(addr, mode) -> the `seen as` provenance of every emitted seed.

    Keyed on the mode as well as the address: an ARM seed and a Thumb seed can
    legitimately share an address, and collapsing them would hide exactly the
    mode confusion this test exists to catch.
    """
    text = (out / "entry_points.toml").read_text(encoding="utf-8")
    found: dict[tuple[int, str], str] = {}
    for block in text.split("[[entry_point]]")[1:]:
        addr = re.search(r"addr = 0x([0-9A-Fa-f]+)", block)
        mode = re.search(r'mode = "(\w+)"', block)
        seen = re.search(r"seen as ([\w/]+)", block)
        if addr and mode:
            found[(int(addr.group(1), 16), mode.group(1))] = (
                seen.group(1) if seen else "")
    return found


def run(work: Path, name: str, *extra: str) -> Path:
    out = work / name
    result = subprocess.run(
        [sys.executable, str(TOOL), str(work / "manifest.json"),
         "--out", str(out), "--rom-sha1", ROM_SHA1, *extra],
        capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return out


def check(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")
    print(f"  ok: {message}")


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        manifest(work / "manifest.json")

        out = run(work, "promoted")
        seeds = seeds_of(out)
        spans = json.loads(
            (out / "interpreted-ranges.json").read_text(encoding="utf-8"))
        report = json.loads(
            (out / "ingest-report.json").read_text(encoding="utf-8"))

        check((HOLE[0], "arm") in seeds,
              "the start of an interpreted span is promoted to a seed")
        check(all((addr, "arm") not in seeds for addr in HOLE[1:]),
              "the interior roots of that span are NOT seeded (one seed per "
              "span, or the finder would chop the body at every root)")
        check(seeds.get((HOLE[0], "arm")) == "span",
              "the promoted seed is labelled as coming from a span")
        check((LONE, "arm") in seeds,
              "a span that is only one instruction long is still a seed")
        check((TARGET, "arm") in seeds
              and "call" in seeds[(TARGET, "arm")],
              "call targets are still promoted exactly as before")
        check((CALLER, "arm") in seeds,
              "the caller of an observed call is promoted: the interpreter "
              "was executing there, so it is interpreted code by construction")
        check((THUMB[0], "thumb") in seeds
              and (THUMB[1], "thumb") not in seeds,
              "the Thumb run is seeded once, at its own start, even though it "
              "shares an address with the interior of the ARM run")

        arm9 = spans["arm9"]
        hole_span = next(s for s in arm9
                         if int(s["start"], 16) == HOLE[0])
        check(hole_span["end"] == f"0x{HOLE[-1] + 4:08X}"
              and hole_span["mode"] == "arm",
              "the ARM run spans exactly its five instructions and is "
              "reported in ARM mode")
        thumb_span = next(s for s in arm9 if s["mode"] == "thumb")
        check(int(thumb_span["start"], 16) == THUMB[0],
              "Thumb roots form their own span rather than merging into the "
              "ARM run they interleave with")
        check(sum(1 for s in arm9 if s["mode"] == "thumb") == 1
              and thumb_span["end"] == f"0x{THUMB[-1] + 2:08X}",
              "a Thumb run is cut at halfword stride, not word stride")
        check(report["span_promotion"]["enabled"] is True
              and report["span_promotion"]["seeds"] == len(
                  [s for s in arm9]),
              "the report accounts for every promoted span")

        # Discriminating control: with promotion off, the old policy is back
        # and none of the span starts is reachable as a seed.
        plain = seeds_of(run(work, "plain", "--no-promote-ranges",
                             "--no-promote-callers"))
        check((HOLE[0], "arm") not in plain and (LONE, "arm") not in plain
              and (CALLER, "arm") not in plain,
              "--no-promote-ranges reproduces the pre-yjp.55 policy exactly")
        check((TARGET, "arm") in plain,
              "...while still promoting the call target it always did")

        # A bounded budget spends itself on the hottest span first.
        ranked = seeds_of(run(work, "ranked", "--max-span-seeds", "1",
                              "--no-promote-callers"))
        check((HOLE[0], "arm") in ranked and (LONE, "arm") not in ranked,
              "--max-span-seeds keeps the hottest span and drops the tail")

    print("coverage span promotion: all assertions hold")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
