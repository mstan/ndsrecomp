#!/usr/bin/env python3
"""Merge player-submitted Tier-3 coverage manifests into recompiler seeds.

beads-yjp.28. The runner writes one manifest per session (see
runner/src/coverage_manifest.cpp). This turns any number of them -- from any
number of people, across any number of sessions -- into:

  * ROM-ALIAS FINDINGS: captured pages whose bytes are verbatim ROM content
    resident at a load address no bank declares. These are the highest-value
    output by a wide margin and need no dumped bytes at all, only the base.
    The first real submission was 93.5% this: the whole ARM7 module runs from
    two runtime aliases (0x037F7E50, 0x027CFBC4) while the only ARM7 bank is
    compiled at the header address 0x02380000, so all of it was interpreted.
  * a merged entry-point TOML for the call and indirect-branch targets, which
    are the addresses that behave like function entries, and
  * a seed at the START of every contiguous interpreted span, which is the only
    output that can reach code the finder never discovered at all (computed
    branches and jump tables stop it, so the span sits in a hole BETWEEN
    compiled functions and no call-target seed can ever name it).
  * one content-validated bank per captured code generation, with the bytes
    written alongside it, for genuinely runtime-materialized code (patched
    pages, decompressed-in-place regions) that exists nowhere in the ROM.
  * a ranked report of the interpreted spans that carry the most execution, so
    "what should be recompiled next" is answered by the data, not guessed.

Merging is monotonic: run it again with more manifests and the corpus only
grows. Nothing is silently dropped -- every address this tool refuses to
promote is counted and reported by reason.

Manifests arrive from other people's machines, so everything in them is
verified before use: each page's SHA-1 is recomputed from its own bytes, and
manifests whose ROM SHA-1 disagrees with the target are rejected outright
rather than blended into the corpus. They are also large -- the first was
202 MB -- so they are read incrementally, never json.load'ed whole.
"""

from __future__ import annotations

import argparse
import base64
import collections
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from coverage_stream import stream_manifest          # noqa: E402
from rom_alias import RomImages, resolve, undeclared  # noqa: E402

# Kinds that behave like function entries. A `root` is where native code fell
# into the interpreter, which sounds like the same thing but is not: the
# interpreter also re-enters at whatever instruction an IRQ or DMA stall
# interrupted, so over a long session roots accumulate at nearly every
# instruction of every hot interpreted function. Measured on the first real
# submission, 92% of root addresses have another root exactly one instruction
# away and the longest unbroken run is 415 instructions.
#
# So roots are an execution map, not a seed list -- but exactly ONE address per
# contiguous run of them IS a seed, and it is the only seed that can reach the
# code the interpreter is actually running (beads-yjp.55). See select_span_seeds.
PROMOTABLE_KINDS = ("call", "indirect")

# The interpreter's own PC while it executed an observed call. By construction
# an address no bank owned, so it is an interpreted-code observation exactly
# like a root -- and a better-attributed one, because it is bound to a concrete
# instruction the manifest saw retire rather than to a bitmap bit. The runner
# folds every caller past kMaxCallersPerEntry into this sentinel.
CALLER_SENTINEL = 0xFFFFFFFF

# Stride of one instruction, per mode. Roots are recorded at this granularity,
# so two roots one stride apart are consecutive instructions of one run.
MODE_STRIDE = {"arm": 4, "thumb": 2}

ARRAY_KEYS = ("entry_points_arm9", "entry_points_arm7", "pages.entries",
              "root_map")

SUPPORTED_SCHEMAS = (2, 3, 4)


def root_addresses(record, base):
    """Yield (addr, mode, hits) from a schema-4 root bitmap record.

    Schema 4 stores roots as two bitmaps over the page -- ARM at word stride,
    Thumb at halfword stride -- plus 16 per-256-byte-block hit counters, rather
    than one JSON record per address. The address set and the mode survive
    exactly; what is approximated is the per-address hit count, which is
    reported at block resolution. Nothing consumed per-address root hits: the
    range report sums them and the overlay seeder only annotates with them.
    """
    blocks = record.get("root_hits") or []
    per_block = len(blocks)
    found: list[tuple[int, str]] = []
    for key, stride in (("root_arm", 4), ("root_thumb", 2)):
        blob = record.get(key)
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

    if not per_block:
        for addr, mode in found:
            yield addr, mode, 0
        return

    # Spread each block's count across the addresses set inside it, so that
    # summing any set of addresses stays proportional and summing a whole block
    # reproduces its count exactly. Handing every address the full block total
    # instead would inflate a span's cost by its own length.
    def block_of(addr):
        return min(((addr - base) * per_block) // 4096, per_block - 1)

    per = collections.Counter(block_of(addr) for addr, _ in found)
    remainder = {b: blocks[b] % n for b, n in per.items()}
    for addr, mode in sorted(found):
        b = block_of(addr)
        share = blocks[b] // per[b]
        if remainder.get(b):
            share += 1
            remainder[b] -= 1
        yield addr, mode, share


def load_manifest(path: Path, rom_sha1: str | None, problems: list[str]):
    """Stream one manifest, returning (header, entries, pages) or None.

    The header scalars arrive before the arrays, so the ROM-SHA-1 and schema
    gates are applied as soon as they are known: a mismatched manifest costs a
    few hundred bytes of reading rather than a full parse of someone else's
    two-hundred-megabyte dump.
    """
    header: dict = {}
    entries: list[tuple[int, dict]] = []
    pages: list[dict] = []
    root_map: list[dict] = []
    checked = False
    try:
        for kind, key, value in stream_manifest(path, ARRAY_KEYS):
            if kind == "scalar":
                header[key] = value
                continue
            if not checked:
                reason = _reject_reason(header, rom_sha1)
                if reason:
                    problems.append(f"{path.name}: {reason}")
                    return None
                checked = True
            if key == "entry_points_arm9":
                entries.append((9, value))
            elif key == "entry_points_arm7":
                entries.append((7, value))
            elif key == "root_map":
                root_map.append(value)
            else:
                pages.append(value)
    except (OSError, ValueError) as error:
        problems.append(f"{path.name}: unreadable ({error})")
        return None
    if not checked:
        reason = _reject_reason(header, rom_sha1)
        if reason:
            problems.append(f"{path.name}: {reason}")
            return None
    return header, entries, pages, root_map


def _reject_reason(header: dict, rom_sha1: str | None) -> str | None:
    if header.get("kind") != "ndsrecomp-tier3-coverage":
        return "not an ndsrecomp coverage manifest"
    if header.get("schema") not in SUPPORTED_SCHEMAS:
        return (f"schema {header.get('schema')!r}, expected one of "
                f"{', '.join(str(s) for s in SUPPORTED_SCHEMAS)}")
    got = str(header.get("rom_sha1", "")).lower()
    if rom_sha1 and got != rom_sha1:
        return (f"ROM SHA-1 {got or '(none)'} does not match the target "
                f"{rom_sha1}; refusing to merge a different dump")
    return None


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("manifests", nargs="+", type=Path)
    parser.add_argument("--out", type=Path, required=True,
                        help="output directory for the seeds and images")
    parser.add_argument("--images", type=Path, default=None,
                        help="prepared ROM image directory (arm9.bin, "
                             "arm7.bin, overlays/, overlays.json). Supplying "
                             "it enables ROM-alias resolution, which is where "
                             "most of the value in a manifest actually is")
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
    parser.add_argument("--no-promote-ranges", dest="promote_ranges",
                        action="store_false",
                        help="reproduce the pre-yjp.55 policy: report the "
                             "interpreted spans but promote nothing from them, "
                             "so only call/indirect targets become seeds")
    parser.add_argument("--no-promote-callers", dest="promote_callers",
                        action="store_false",
                        help="ignore the caller field of call/indirect records "
                             "instead of treating it as an interpreted-code "
                             "observation")
    parser.add_argument("--min-span-hits", type=int, default=1,
                        help="only promote a span start whose span carries at "
                             "least this many interpreter entries")
    parser.add_argument("--max-span-seeds", type=int, default=0,
                        help="promote at most this many span starts per CPU, "
                             "hottest span first (0 = every qualifying span)")
    parser.set_defaults(promote_ranges=True, promote_callers=True)
    args = parser.parse_args()

    problems: list[str] = []
    rom_sha1 = args.rom_sha1.lower() if args.rom_sha1 else None

    entries: dict[tuple[int, int, str], dict] = {}
    # (cpu, addr, sha1) -> {"bytes", "entries": {(pc, mode): slot},
    #                       "roots": {(pc, mode): hits}, "executions"}
    versions: dict[tuple[int, int, str], dict] = {}
    # cpu -> (addr, mode) -> interpreter entries. Mode is carried because a
    # span start becomes a seed and a seed without a mode cannot be compiled;
    # keying on the address alone silently merged ARM and Thumb runs.
    roots: dict[int, dict[tuple[int, str], int]] = {9: {}, 7: {}}
    callers: dict[int, dict[tuple[int, str], int]] = {9: {}, 7: {}}
    rejected: collections.Counter = collections.Counter()
    accepted = 0
    build_ids: set[str] = set()
    truncated = False

    for path in args.manifests:
        loaded = load_manifest(path, rom_sha1, problems)
        if loaded is None:
            continue
        header, manifest_entries, manifest_pages, manifest_roots = loaded
        if rom_sha1 is None:
            rom_sha1 = str(header.get("rom_sha1", "")).lower()
        accepted += 1
        if header.get("build_id"):
            build_ids.add(str(header["build_id"]))

        dropped = int(header.get("pages.dropped", 0))
        if dropped > 0:
            truncated = True
            problems.append(
                f"{path.name}: the runner hit its page-store cap, so this "
                f"manifest is INCOMPLETE ({dropped} pages were refused)")

        # schema 4 carries roots as bitmaps. The session-wide map survives
        # page eviction and part rotation, which the per-address list only did
        # because tier3's own map never forgot an address.
        for record in manifest_roots:
            cpu = int(record["cpu"])
            base = int(record["addr"], 16)
            for addr, mode, hits in root_addresses(record, base):
                key = (addr, mode)
                roots[cpu][key] = roots[cpu].get(key, 0) + hits

        page_size = int(header.get("pages.page_size", 4096))
        for page in manifest_pages:
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
            slot = versions.setdefault(
                (cpu, addr, actual),
                {"bytes": raw, "entries": {}, "roots": {}, "executions": 0})
            slot["executions"] += int(page.get("executions", 0))
            # Schema 4 repeats the root bitmap per captured page version, which
            # is the only GENERATION-BOUND root attribution in the manifest --
            # the session-wide root_map is keyed by page base alone, so it
            # cannot say which overlay body was resident. A bank assembled from
            # this version needs the former; the span report needs the latter.
            # Nothing read this before, so a bank could only ever be seeded
            # with call targets even when the manifest knew exactly which of
            # its own instructions the interpreter had been entered at.
            for root_pc, root_mode, root_hits in root_addresses(page, addr):
                root_key = (root_pc, root_mode)
                slot["roots"][root_key] = (
                    slot["roots"].get(root_key, 0) + root_hits)
            # schema 3 associates each entry with the exact page generation it
            # was observed under. schema 2 carried no such association, which
            # is why the old assembler had to guess by pairing equal version
            # indices across addresses; with schema 3 there is nothing to guess.
            for entry in page.get("entry_points", []):
                kind = entry.get("kind")
                pc = int(entry["addr"], 16)
                hits = int(entry.get("hits", 0))
                mode = entry["mode"]
                if kind == "root":
                    root_key = (pc, mode)
                    roots[cpu][root_key] = roots[cpu].get(root_key, 0) + hits
                    slot["roots"][root_key] = (
                        slot["roots"].get(root_key, 0) + hits)
                    continue
                if kind not in PROMOTABLE_KINDS:
                    rejected[f"page entry: kind={kind}"] += 1
                    continue
                key = (pc, mode)
                held = slot["entries"].setdefault(
                    key, {"hits": 0, "kinds": set()})
                held["hits"] += hits
                held["kinds"].add(kind)
                note_caller(callers, cpu, entry, hits)

        for cpu, entry in manifest_entries:
            kind = entry.get("kind")
            pc = int(entry["addr"], 16)
            hits = int(entry.get("hits", 0))
            if kind == "root":
                # Schema 2/3 carried roots one JSON record per address here;
                # schema 4 moved them to root_map. Either way they now feed the
                # span promoter rather than being counted as a refusal.
                root_key = (pc, entry["mode"])
                roots[cpu][root_key] = max(roots[cpu].get(root_key, 0), hits)
                continue
            if kind not in PROMOTABLE_KINDS:
                rejected[f"entry: kind={kind}"] += 1
                continue
            slot = entries.setdefault(
                (cpu, pc, entry["mode"]), {"hits": 0, "kinds": set()})
            slot["hits"] += hits
            slot["kinds"].add(kind)
            note_caller(callers, cpu, entry, hits)

    if not accepted:
        for problem in problems:
            print(f"  ! {problem}", file=sys.stderr)
        print("no usable manifests", file=sys.stderr)
        return 1

    dropped_low_hits = 0
    for key in list(entries):
        if entries[key]["hits"] < args.min_hits:
            del entries[key]
            dropped_low_hits += 1

    # ---- interpreted spans -> seeds (beads-yjp.55) ------------------------
    # A caller is an address the interpreter was executing, so it belongs in
    # the same map as the roots: folded in BEFORE the runs are cut, it either
    # extends a run it is adjacent to (costing no extra seed) or forms its own.
    interpreted = {cpu: dict(hits) for cpu, hits in roots.items()}
    if args.promote_callers:
        for cpu, observed in callers.items():
            for key, hits in observed.items():
                interpreted[cpu][key] = interpreted[cpu].get(key, 0) + hits

    hot = hot_ranges(interpreted)
    promoted, span_stats = select_span_seeds(
        hot, promote=args.promote_ranges, min_hits=args.min_span_hits,
        limit=args.max_span_seeds)
    for (cpu, addr, mode), hits in sorted(promoted.items()):
        slot = entries.setdefault((cpu, addr, mode),
                                  {"hits": 0, "kinds": set()})
        slot["hits"] += hits
        slot["kinds"].add("span")
    # A span start on a page whose bytes exist nowhere in the ROM is a seed for
    # the assembled bank rather than for a module, and the manifest says which
    # page GENERATION it was interpreted under -- so attribute it there instead
    # of letting the run fall back to address containment.
    for (cpu, _page_addr, _sha), slot in versions.items():
        for (pc, mode), hits in slot["roots"].items():
            if (cpu, pc, mode) not in promoted:
                continue
            held = slot["entries"].setdefault(
                (pc, mode), {"hits": 0, "kinds": set()})
            held["hits"] += hits
            held["kinds"].add("span")

    args.out.mkdir(parents=True, exist_ok=True)

    # ---- ROM-alias resolution --------------------------------------------
    alias_findings: list[dict] = []
    aliased_pages = 0
    page_target: dict[tuple[int, int], tuple[str, int]] = {}
    if args.images:
        images = RomImages(args.images)
        aliases, unresolved = resolve(
            ((cpu, addr, slot["bytes"])
             for (cpu, addr, _sha), slot in versions.items()), images)
        aliased_pages = sum(len(v["addrs"]) for v in aliases.values())
        alias_findings = undeclared(aliases, images)
        for (cpu, module, base), slot in aliases.items():
            for addr in slot["addrs"]:
                page_target[(cpu, addr)] = (module, base)
        unresolved_keys = [
            key for key, slot in versions.items()
            if images.locate(slot["bytes"]) is None]
    else:
        problems.append(
            "--images was not supplied, so no ROM-alias resolution ran; on the "
            "first real submission that would have hidden 93.5% of the finding")
        unresolved_keys = list(versions)

    # ---- banks for genuinely runtime-materialized code --------------------
    images_dir = args.out / "images"
    banks_written = 0
    entries_in_banks = 0
    runs = assemble_runs(versions, unresolved_keys, entries)
    if runs:
        images_dir.mkdir(parents=True, exist_ok=True)
    for cpu, start, blob, run_entries in runs:
        if not run_entries:
            continue
        entries_in_banks += len(run_entries)
        identity = hashlib.sha1(blob).hexdigest()
        stem = f"{args.bank_prefix}_arm{cpu}_{start:08x}_{identity[:8]}"
        (images_dir / f"{stem}.bin").write_bytes(blob)
        write_bank_toml(images_dir / f"{stem}.toml", stem, cpu,
                        args.isa9 if cpu == 9 else args.isa7,
                        start, len(blob), identity, run_entries, rom_sha1)
        banks_written += 1

    write_entry_toml(args.out / "entry_points.toml",
                     sorted(entries.items()), rom_sha1, accepted,
                     sorted(build_ids))

    # A flat seed list is not actionable on its own: an address is only a seed
    # for the bank whose image holds those bytes at that address. Split the
    # merged list by the module and base each seed's page resolved to, so each
    # file drops straight into the matching bank config.
    seeds_dir = args.out / "seeds"
    by_target: dict[str, list] = collections.defaultdict(list)
    for key, slot in sorted(entries.items()):
        cpu, addr, _mode = key
        target = page_target.get((cpu, addr & ~0xFFF))
        name = (f"arm{cpu}_{Path(target[0]).stem}_{target[1]:08x}"
                if target else f"arm{cpu}_unresolved")
        by_target[name].append((key, slot))
    if by_target:
        seeds_dir.mkdir(parents=True, exist_ok=True)
    for name, items in sorted(by_target.items()):
        write_entry_toml(seeds_dir / f"{name}.toml", items, rom_sha1,
                         accepted, sorted(build_ids))

    (args.out / "interpreted-ranges.json").write_text(
        json.dumps(hot, indent=2), encoding="utf-8", newline="\n")

    report = {
        "manifests_accepted": accepted,
        "manifests_rejected": len(problems),
        "manifests_incomplete": truncated,
        "rom_sha1": rom_sha1,
        "build_ids": sorted(build_ids),
        "rom_alias_findings": [describe_alias(f) for f in alias_findings],
        "pages_resolved_to_rom": aliased_pages,
        "page_versions_total": len(versions),
        "page_versions_not_in_rom": len(unresolved_keys),
        "entry_points_merged": len(entries),
        "entry_points_dropped_below_min_hits": dropped_low_hits,
        "entry_points_placed_in_banks": entries_in_banks,
        "entry_points_by_target": {name: len(items)
                                   for name, items in sorted(by_target.items())},
        "banks_written": banks_written,
        "interpreted_ranges": {
            "arm9": len(hot.get("arm9", [])),
            "arm7": len(hot.get("arm7", [])),
        },
        "span_promotion": {
            "enabled": args.promote_ranges,
            "callers_folded_in": args.promote_callers,
            "distinct_callers": sum(len(v) for v in callers.values()),
            "min_span_hits": args.min_span_hits,
            "max_span_seeds": args.max_span_seeds,
            "seeds": len(promoted),
            "by_cpu": span_stats,
        },
        "rejected": dict(rejected),
        "problems": problems,
    }
    (args.out / "ingest-report.json").write_text(
        json.dumps(report, indent=2), encoding="utf-8", newline="\n")
    print(json.dumps(report, indent=2))
    return 0


def describe_alias(finding: dict) -> dict:
    declared = finding["declared_base"]
    return {
        "cpu": f"arm{finding['cpu']}",
        "module": finding["module"],
        "resident_base": f"0x{finding['base']:08X}",
        "declared_base": (f"0x{declared:08X}" if declared is not None
                          else None),
        "pages": finding["pages"],
        "guest_span": (f"0x{finding['guest_span'][0]:08X}.."
                       f"0x{finding['guest_span'][1]:08X}"),
        "module_span": (f"0x{finding['module_span'][0]:X}.."
                        f"0x{finding['module_span'][1]:X}"),
        "note": "resident at a base no bank declares; recompile the module "
                "at this base, no dumped bytes required",
    }


def assemble_runs(versions, unresolved_keys, session_entries=None):
    """Group unresolved page versions into contiguous, unambiguous runs.

    A run may only be extended across a page boundary when the next address has
    exactly ONE captured version. With several versions at an address there is
    nothing in the manifest that says which one was resident alongside the
    previous page, and the old assembler's answer -- pair equal indices in
    SHA-1 sort order -- concatenates unrelated overlay generations into one
    image. That is fail-safe at runtime, because dispatch validates each
    function against live bytes, but it silently wastes the coverage. Stopping
    at the ambiguity costs a shorter run and loses nothing.

    A schema-2 manifest has no page-scoped entries at all, so a run assembled
    from one would carry no seeds and be dropped. For those, fall back to the
    session-wide entries that fall inside the run's address range -- which is
    all schema 2 ever supported, and the only reason the old assembler had to
    attribute by address in the first place.
    """
    version_count: collections.Counter = collections.Counter()
    for cpu, addr, _sha in versions:
        version_count[(cpu, addr)] += 1

    by_cpu: dict[int, list] = collections.defaultdict(list)
    for key in unresolved_keys:
        cpu, addr, sha = key
        by_cpu[cpu].append((addr, sha, versions[key]))

    runs = []
    for cpu, items in sorted(by_cpu.items()):
        items.sort(key=lambda item: (item[0], item[1]))
        index = 0
        while index < len(items):
            addr, _sha, slot = items[index]
            page_size = len(slot["bytes"])
            chunks = [slot["bytes"]]
            run_entries = {key: dict(held, kinds=set(held["kinds"]))
                           for key, held in slot["entries"].items()}
            start = addr
            previous = addr
            index += 1
            # Only an address with a single captured version can be appended:
            # anything else would be a guess about what was resident with it.
            while (index < len(items)
                   and version_count[(cpu, previous)] == 1):
                next_addr, _next_sha, next_slot = items[index]
                if next_addr != previous + page_size:
                    break
                if version_count[(cpu, next_addr)] != 1:
                    break
                chunks.append(next_slot["bytes"])
                for key, held in next_slot["entries"].items():
                    merged = run_entries.setdefault(
                        key, {"hits": 0, "kinds": set()})
                    merged["hits"] += held["hits"]
                    merged["kinds"] |= held["kinds"]
                previous = next_addr
                index += 1
            blob = b"".join(chunks)
            if not run_entries and session_entries:
                run_entries = {
                    (pc, mode): held
                    for (ecpu, pc, mode), held in session_entries.items()
                    if ecpu == cpu and start <= pc < start + len(blob)}
            ordered = sorted(
                (pc, mode, held) for (pc, mode), held in run_entries.items()
                if start <= pc < start + len(blob))
            runs.append((cpu, start, blob, ordered))
    return runs


def note_caller(callers, cpu: int, entry: dict, hits: int) -> None:
    """Record the interpreted call site behind one call/indirect record.

    The runner stores the interpreter's own PC alongside every transfer it
    observed. That address is by construction code no bank owned -- the
    interpreter was executing it -- so it is an interpreted-code observation of
    exactly the same kind as a root, and a better-attributed one: it names a
    concrete instruction the manifest watched retire instead of a bitmap bit.
    Every promoter in both repos read the target and dropped this field.

    The mode recorded on the record is the TARGET's. That is the caller's mode
    too for a plain BL, and can differ for BLX/BX; a seed carrying the wrong
    mode is inert rather than wrong, because dispatch keys on (pc, thumb) and
    such a row is simply never looked up, and the bytes are still validated.
    """
    raw = entry.get("caller")
    if raw is None:
        return
    addr = int(raw, 16)
    # kMaxCallersPerEntry overflow is folded into one record under this
    # sentinel; its hit count is exact but its address is not an address.
    if addr == CALLER_SENTINEL or addr == 0:
        return
    mode = entry["mode"]
    if addr % MODE_STRIDE.get(mode, 4):
        return
    callers[cpu][(addr, mode)] = callers[cpu].get((addr, mode), 0) + hits


def select_span_seeds(hot, *, promote: bool, min_hits: int, limit: int):
    """Choose one seed per contiguous interpreted span: its START address.

    This is the promotion that reaches code no call-target seed can (yjp.55).
    A call/indirect target is by definition an address some compiled function
    branched to, so the finder had already discovered it; the code the
    interpreter is actually RUNNING sits in the holes between compiled
    functions, where a computed branch or jump table stopped discovery, and the
    only record of it is the root map.

    Exactly one seed per span, not one per root, and the reason is structural
    rather than a size preference: `write_bank_dispatch` emits one dispatch row
    per INSTRUCTION across [fn.addr, fn.end_addr), so a single seed at a span
    start already yields an owning row for every instruction the finder walks
    from there. Seeding the interior roots as well would be strictly harmful --
    the finder gives each seed its own independent Function record and then
    trims the previous one's `end_addr` down to meet it, so every extra seed
    chops the body into a shorter one and suppresses the fallthrough coalescing
    the dispatch cost depends on.

    Ranking is by span cost, so a bounded `limit` spends the seed budget on the
    spans that carry the interpretation rather than on the long tail.
    """
    seeds: dict[tuple[int, int, str], int] = {}
    stats: dict[str, dict] = {}
    for name, spans in sorted(hot.items()):
        cpu = int(name[3:])
        qualifying = [s for s in spans if s["entries"] >= min_hits]
        chosen = qualifying[:limit] if limit > 0 else qualifying
        if promote:
            for span in chosen:
                key = (cpu, int(span["start"], 16), span["mode"])
                seeds[key] = seeds.get(key, 0) + span["entries"]
        stats[name] = {
            "spans": len(spans),
            "spans_above_min_hits": len(qualifying),
            "spans_promoted": len(chosen) if promote else 0,
            "entries_promoted": sum(s["entries"] for s in chosen) if promote
                                else 0,
            "entries_total": sum(s["entries"] for s in spans),
        }
    return seeds, stats


def hot_ranges(interpreted):
    """Merge interpreted addresses into contiguous spans, hottest first.

    Roots are where the interpreter was entered, and callers are where it was
    running. Individually they are poor seeds, but merged they are the honest
    answer to "which code is still being interpreted, and how much does it
    cost", which is the question a coverage submission exists to answer -- and
    each run's START is the one address in it worth seeding.

    Runs are cut per MODE and at instruction stride. Merging an ARM run with a
    Thumb one because their addresses interleave would produce a span whose
    start has no single answer to "which mode do I compile this as".
    """
    out: dict[str, list] = {}
    for cpu, hits in interpreted.items():
        spans = []
        for mode in sorted({mode for _addr, mode in hits}):
            stride = MODE_STRIDE.get(mode, 4)
            run_start = None
            previous = None
            total = 0
            for addr in sorted(a for a, m in hits if m == mode):
                if previous is not None and addr - previous <= stride:
                    total += hits[(addr, mode)]
                    previous = addr
                    continue
                if run_start is not None:
                    spans.append(_span(run_start, previous, total, mode,
                                       stride))
                run_start = addr
                previous = addr
                total = hits[(addr, mode)]
            if run_start is not None:
                spans.append(_span(run_start, previous, total, mode, stride))
        spans.sort(key=lambda span: (-span["entries"], span["start"]))
        out[f"arm{cpu}"] = spans
    return out


def _span(start: int, last: int, entries: int, mode: str,
          stride: int) -> dict:
    return {"start": f"0x{start:08X}", "end": f"0x{last + stride:08X}",
            "bytes": last + stride - start, "mode": mode, "entries": entries}


def write_bank_toml(path: Path, bank_id: str, cpu: int, isa: str,
                    load_address: int, size: int, identity: str,
                    run_entries, rom_sha1: str | None) -> None:
    lines = [
        "# AUTO-GENERATED by tools/ingest_coverage_manifests.py.",
        "# Assembled from player-submitted Tier-3 coverage manifests; every",
        "# page was SHA-1 verified against its own bytes before assembly, and",
        "# the bank is content-validated against live guest bytes at dispatch.",
        "# These bytes exist nowhere in the ROM -- code that does is reported",
        "# as a load-base finding instead, which needs no dumped bytes.",
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
        "# Every Tier-3 call and indirect-branch target observed, plus the",
        "# START of every contiguous interpreted span (seen as `span`), which",
        "# is the only seed that reaches code the finder never discovered. An",
        "# address here is a seed only for the bank whose image actually holds",
        "# bytes at this address; see ingest-report.json for which module and",
        "# load base each captured page resolved to.",
        "",
    ]
    for (cpu, addr, mode), slot in addressed:
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
