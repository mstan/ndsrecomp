#!/usr/bin/env python3
"""Merge player-submitted Tier-3 coverage manifests into recompiler seeds.

beads-yjp.28. The runner writes one manifest per session (see
runner/src/coverage_manifest.cpp). This turns any number of them -- from any
number of people, across any number of sessions -- into:

  * a merged entry-point TOML for addresses that live in immutable
    ROM-derived ranges, where the address plus the ROM SHA-1 is enough, and
  * one content-validated bank per contiguous run of captured code pages,
    with the run's bytes written alongside it, for the runtime-resident code
    (ITCM, overlays, ARM7 WRAM) where an address alone identifies nothing.

Merging is monotonic: run it again with more manifests and the corpus only
grows. Nothing is silently dropped -- every address this tool refuses to
promote is counted and reported by reason.

Manifests arrive from other people's machines, so everything in them is
verified before use: each page's SHA-1 is recomputed from its own bytes, and
manifests whose ROM SHA-1 disagrees with the target are rejected outright
rather than blended into the corpus.
"""

from __future__ import annotations

import argparse
import base64
import collections
import hashlib
import json
import sys
from pathlib import Path

# Addresses that are not backed by writable RAM never appear as captured
# pages; they are promotable from the address alone because the ROM SHA-1
# pins their bytes. Everything else needs its resident image.
PROMOTABLE_KINDS = ("call", "indirect")


def load_manifest(path: Path, rom_sha1: str | None, problems: list[str]):
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        problems.append(f"{path.name}: unreadable ({error})")
        return None
    if data.get("kind") != "ndsrecomp-tier3-coverage":
        problems.append(f"{path.name}: not an ndsrecomp coverage manifest")
        return None
    if data.get("schema") != 2:
        problems.append(
            f"{path.name}: schema {data.get('schema')!r}, expected 2")
        return None
    got_rom = str(data.get("rom_sha1", "")).lower()
    if rom_sha1 and got_rom != rom_sha1:
        problems.append(
            f"{path.name}: ROM SHA-1 {got_rom or '(none)'} does not match "
            f"the target {rom_sha1}; refusing to merge a different dump")
        return None
    return data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("manifests", nargs="+", type=Path)
    parser.add_argument("--out", type=Path, required=True,
                        help="output directory for the seeds and images")
    parser.add_argument("--rom-sha1", default=None,
                        help="required ROM SHA-1; manifests that disagree are "
                             "rejected (defaults to the first manifest's)")
    parser.add_argument("--bank-prefix", default="coverage",
                        help="id prefix for the generated bank configs")
    parser.add_argument("--isa9", default="armv5te")
    parser.add_argument("--isa7", default="armv4t")
    parser.add_argument("--min-hits", type=int, default=1,
                        help="drop entry points seen fewer than this many "
                             "times across all manifests")
    args = parser.parse_args()

    problems: list[str] = []
    rom_sha1 = args.rom_sha1.lower() if args.rom_sha1 else None

    # (cpu, addr, mode) -> {hits, kinds, sources}
    entries: dict[tuple[int, int, str], dict] = {}
    # (cpu, addr) -> {sha1: bytes}
    pages: dict[tuple[int, int], dict[str, bytes]] = {}
    rejected = collections.Counter()
    accepted_manifests = 0
    build_ids: set[str] = set()

    for path in args.manifests:
        data = load_manifest(path, rom_sha1, problems)
        if data is None:
            continue
        if rom_sha1 is None:
            rom_sha1 = str(data.get("rom_sha1", "")).lower()
        accepted_manifests += 1
        if data.get("build_id"):
            build_ids.add(str(data["build_id"]))

        if int(data.get("pages", {}).get("dropped", 0)) > 0:
            problems.append(
                f"{path.name}: the runner hit its page-store cap, so this "
                f"manifest is INCOMPLETE ({data['pages']['dropped']} pages "
                f"were refused)")

        page_size = int(data.get("pages", {}).get("page_size", 4096))
        for page in data.get("pages", {}).get("entries", []):
            raw = base64.b64decode(page["data"])
            if len(raw) != page_size:
                rejected["page: wrong length"] += 1
                continue
            actual = hashlib.sha1(raw).hexdigest()
            if actual != page["sha1"]:
                # Never ingest bytes whose own manifest disagrees about them.
                rejected["page: SHA-1 mismatch"] += 1
                continue
            cpu = int(page["cpu"])
            addr = int(page["addr"], 16)
            pages.setdefault((cpu, addr), {})[actual] = raw

        for cpu, key in ((9, "entry_points_arm9"), (7, "entry_points_arm7")):
            for entry in data.get(key, []):
                kind = entry.get("kind")
                if kind not in PROMOTABLE_KINDS:
                    # Roots are scheduler resume points, which usually land
                    # mid-function and make poor seeds.
                    rejected[f"entry: kind={kind}"] += 1
                    continue
                slot = entries.setdefault(
                    (cpu, int(entry["addr"], 16), entry["mode"]),
                    {"hits": 0, "kinds": set()})
                slot["hits"] += int(entry.get("hits", 0))
                slot["kinds"].add(kind)

    if not accepted_manifests:
        for problem in problems:
            print(f"  ! {problem}", file=sys.stderr)
        print("no usable manifests", file=sys.stderr)
        return 1

    dropped_low_hits = 0
    for key in list(entries):
        if entries[key]["hits"] < args.min_hits:
            del entries[key]
            dropped_low_hits += 1

    args.out.mkdir(parents=True, exist_ok=True)
    images_dir = args.out / "images"

    # Assemble contiguous runs of captured pages per (cpu, content choice).
    # A page address can hold several distinct contents across a playthrough
    # (overlay generations); each becomes its own bank, exactly as the
    # existing SM64DS gameplay/boot RAM banks do.
    page_size = 4096
    by_cpu: dict[int, dict[int, dict[str, bytes]]] = collections.defaultdict(dict)
    for (cpu, addr), versions in pages.items():
        by_cpu[cpu][addr] = versions

    banks_written = 0
    covered_by_image = 0
    if pages:
        images_dir.mkdir(parents=True, exist_ok=True)
    for cpu, addr_map in sorted(by_cpu.items()):
        # Generation index: version 0 of every address forms bank 0, version 1
        # forms bank 1, and so on. Addresses with fewer versions simply do not
        # appear in the later banks.
        max_versions = max(len(v) for v in addr_map.values())
        for generation in range(max_versions):
            selected: dict[int, bytes] = {}
            for addr, versions in addr_map.items():
                ordered = [versions[k] for k in sorted(versions)]
                if generation < len(ordered):
                    selected[addr] = ordered[generation]
            if not selected:
                continue
            for run_start, run_bytes in contiguous_runs(selected, page_size):
                run_end = run_start + len(run_bytes)
                run_entries = [
                    (addr, mode, slot)
                    for (ecpu, addr, mode), slot in sorted(entries.items())
                    if ecpu == cpu and run_start <= addr < run_end
                ]
                if not run_entries:
                    continue
                covered_by_image += len(run_entries)
                identity = hashlib.sha1(run_bytes).hexdigest()
                stem = (f"{args.bank_prefix}_arm{cpu}_"
                        f"{run_start:08x}_g{generation}")
                (images_dir / f"{stem}.bin").write_bytes(run_bytes)
                write_bank_toml(
                    images_dir / f"{stem}.toml", stem, cpu,
                    args.isa9 if cpu == 9 else args.isa7,
                    run_start, len(run_bytes), identity, run_entries,
                    rom_sha1)
                banks_written += 1

    # Everything not inside a captured image is promotable by address alone
    # only if it is genuinely immutable; we cannot tell from here, so emit it
    # separately and label it honestly.
    addressed = [
        (cpu, addr, mode, slot)
        for (cpu, addr, mode), slot in sorted(entries.items())
    ]
    write_entry_toml(args.out / "entry_points.toml", addressed, rom_sha1,
                     accepted_manifests, sorted(build_ids))

    report = {
        "manifests_accepted": accepted_manifests,
        "manifests_rejected": len(problems),
        "rom_sha1": rom_sha1,
        "build_ids": sorted(build_ids),
        "entry_points_merged": len(entries),
        "entry_points_inside_captured_images": covered_by_image,
        "entry_points_address_only": len(entries) - covered_by_image,
        "entry_points_dropped_below_min_hits": dropped_low_hits,
        "distinct_page_addresses": len(pages),
        "distinct_page_versions": sum(len(v) for v in pages.values()),
        "banks_written": banks_written,
        "rejected": dict(rejected),
        "problems": problems,
    }
    (args.out / "ingest-report.json").write_text(
        json.dumps(report, indent=2), encoding="utf-8", newline="\n")
    print(json.dumps(report, indent=2))
    return 0


def contiguous_runs(selected: dict[int, bytes], page_size: int):
    """Yield (start_addr, joined_bytes) for each maximal contiguous run."""
    run_start = None
    previous = None
    chunks: list[bytes] = []
    for addr in sorted(selected):
        if previous is not None and addr == previous + page_size:
            chunks.append(selected[addr])
        else:
            if run_start is not None:
                yield run_start, b"".join(chunks)
            run_start = addr
            chunks = [selected[addr]]
        previous = addr
    if run_start is not None:
        yield run_start, b"".join(chunks)


def write_bank_toml(path: Path, bank_id: str, cpu: int, isa: str,
                    load_address: int, size: int, identity: str,
                    run_entries, rom_sha1: str | None) -> None:
    lines = [
        "# AUTO-GENERATED by tools/ingest_coverage_manifests.py.",
        "# Assembled from player-submitted Tier-3 coverage manifests; every",
        "# page was SHA-1 verified against its own bytes before assembly, and",
        "# the bank is content-validated against live guest bytes at dispatch.",
        "",
        "[program]",
        f'name         = "coverage bank arm{cpu} @ 0x{load_address:08X}"',
        f'id           = "{bank_id}"',
        f'cpu          = "arm{cpu}"',
        f'isa          = "{isa}"',
        f"load_address = 0x{load_address:08X}",
        f"size         = 0x{size:08X}",
        f"entry_pc     = 0x{run_entries[0][0]:08X}",
        "",
        "[identity]",
        f'sha1 = "{identity}"',
    ]
    if rom_sha1:
        lines += [f'rom_sha1 = "{rom_sha1}"']
    lines.append("")
    for addr, mode, slot in run_entries:
        lines += [
            "[[entry_point]]",
            f"addr = 0x{addr:08X}",
            f'mode = "{mode}"',
            'kind = "runtime_observed"',
            f"# hits = {slot['hits']}, seen as "
            f"{'/'.join(sorted(slot['kinds']))}",
            "",
        ]
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def write_entry_toml(path: Path, addressed, rom_sha1, manifest_count,
                     build_ids) -> None:
    lines = [
        "# AUTO-GENERATED by tools/ingest_coverage_manifests.py.",
        f"# Merged from {manifest_count} player coverage manifest(s).",
        f"# ROM SHA-1: {rom_sha1}",
        f"# Runner build(s): {', '.join(build_ids) if build_ids else 'unknown'}",
        "#",
        "# Every Tier-3 call/indirect target observed. Addresses inside an",
        "# immutable ROM-derived range are usable as seeds directly; addresses",
        "# in runtime-materialized memory need the matching bank in images/.",
        "",
    ]
    for cpu, addr, mode, slot in addressed:
        lines += [
            "[[entry_point]]",
            f"addr = 0x{addr:08X}",
            f'mode = "{mode}"',
            f'cpu  = "arm{cpu}"',
            'kind = "runtime_observed"',
            f"# hits = {slot['hits']}, seen as "
            f"{'/'.join(sorted(slot['kinds']))}",
            "",
        ]
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    raise SystemExit(main())
