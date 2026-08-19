#!/usr/bin/env python3
"""Compile generation-bound NDS RAM code pages into persistent live DLLs.

The coverage manifest associates each dispatch/resume observation with the
exact page bytes resident at that moment. Each DLL publishes one exact
dependency closure covering every emitted native body. Direct transfers remain
inside that atomically validated closure; cross-page transfers return through
candidate lookup.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


ABI_VERSION = 4
PAGE_SIZE = 4096


def load_json(path: Path) -> object:
    return json.loads(path.read_text(encoding="utf-8"))


def atomic_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8", newline="\n")
    os.replace(temporary, path)


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


def canonical_entries(page: dict, ranges: list[tuple[int, int]]) -> list[dict]:
    merged: dict[tuple[int, str], dict] = {}
    for entry in page.get("entry_points", []):
        addr = int(entry["addr"], 16)
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


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def provider_identity(args: argparse.Namespace) -> str:
    runtime_header = (
        args.ndsrecomp_root / "recompiler" / "armv4t" / "runtime_arm.h"
    )
    if not runtime_header.is_file():
        raise RuntimeError(f"runtime ABI header does not exist: {runtime_header}")
    try:
        gcc_machine = subprocess.check_output(
            [str(args.gcc), "-dumpmachine"], text=True).strip()
        gcc_version = subprocess.check_output(
            [str(args.gcc), "-dumpfullversion", "-dumpversion"],
            text=True).strip()
    except (OSError, subprocess.CalledProcessError) as error:
        raise RuntimeError(f"cannot identify live-shard compiler: {error}")
    value = {
        "schema": 1,
        "abi": ABI_VERSION,
        "provider_sha256": file_sha256(Path(__file__)),
        "recompiler_sha256": file_sha256(args.recompiler),
        "runtime_header_sha256": file_sha256(runtime_header),
        "gcc_machine": gcc_machine,
        "gcc_version": gcc_version,
        "generated_opt": args.generated_opt,
        "max_function_bytes": args.max_function_bytes,
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


def write_wrapper(path: Path, bank: str, candidate_id: str,
                  generation_id: str, rom_sha1: str, cpu: int) -> None:
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
    ]), encoding="utf-8", newline="\n")


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
        dll = Path(indexed.get("dll", ""))
        if dll.is_file():
            print(f"capture {key}: already compiled as {dll}", flush=True)
            return "skipped", dll

    cpu_name = "arm9" if cpu == 9 else "arm7"
    bank = f"nds_live_{cpu_name}_{addr:08x}"
    work = args.cache / "work" / key
    src_dir = work / "src"
    if src_dir.exists():
        shutil.rmtree(src_dir)
    src_dir.mkdir(parents=True)
    image = work / "page.bin"
    config = work / "page.toml"
    image.write_bytes(raw)
    write_config(config, cpu, addr, page_sha1, entries)

    command = [
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
    if run(command).returncode != 0:
        return "failed", None

    candidate_id = generated_identity(
        src_dir, bank, cpu, args.rom_sha1, args.provider_id)
    bank_sources = generated_bank_sources(src_dir, bank)
    if any('runtime_unimplemented_op("' in path.read_text(
            encoding="utf-8") for path in bank_sources
            if path.name != f"{bank}_dispatch.c"):
        index.setdefault("captures", {})[key] = {
            "status": "unsupported",
            "cpu": cpu,
            "page": f"0x{addr:08X}",
            "page_sha1": page_sha1,
            "entries": len(entries),
        }
        atomic_json(args.index, index)
        print(f"capture {key}: generated body contains an unsupported opcode; "
              "kept in Tier 3", flush=True)
        return "skipped", None
    dll_dir = args.cache / "gcc"
    dll_dir.mkdir(parents=True, exist_ok=True)
    dll = dll_dir / f"{bank}_{candidate_id}.dll"
    if not dll.is_file():
        wrapper = src_dir / f"{bank}_live.c"
        write_wrapper(wrapper, bank, candidate_id, page_sha1,
                      args.rom_sha1, cpu)
        bank_sources = generated_bank_sources(src_dir, bank)
        sources = [*bank_sources, wrapper]
        if len(bank_sources) < 2 or not all(path.is_file() for path in sources):
            raise RuntimeError(f"generated source set is incomplete for {bank}")
        stage = dll.with_suffix(".stage.dll")
        stage.unlink(missing_ok=True)
        gcc = [
            args.gcc, "-shared", args.generated_opt, "-g0",
            f"-DNDS_STATIC_CPU={0 if cpu == 9 else 1}",
            "-I", str(args.ndsrecomp_root / "recompiler" / "armv4t"),
            "-I", str(src_dir),
            "-Wl,--enable-auto-import",
            "-o", str(stage),
            *[str(path) for path in sources],
            str(args.runner_import_lib),
        ]
        if run(gcc).returncode != 0:
            stage.unlink(missing_ok=True)
            return "failed", None
        os.replace(stage, dll)

    index.setdefault("captures", {})[key] = {
        "candidate_id": candidate_id,
        "provider_id": args.provider_id,
        "generation_id": page_sha1,
        "cpu": cpu,
        "page": f"0x{addr:08X}",
        "page_sha1": page_sha1,
        "entries": len(entries),
        "dll": dll.resolve().as_posix(),
    }
    atomic_json(args.index, index)
    print(f"NDS_SHARD_PUBLISHED {dll.resolve().as_posix()}", flush=True)
    return "ok", dll


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
    parser.add_argument("--ndsrecomp-root", type=Path, required=True)
    parser.add_argument("--runner-build", type=Path, required=True)
    parser.add_argument("--recompiler", type=Path, required=True)
    parser.add_argument("--gcc", default="gcc")
    parser.add_argument("--generated-opt", default="-O2")
    parser.add_argument("--max-function-bytes", type=int, default=512)
    parser.add_argument("--max-pages", type=int, default=6,
                        help="maximum new page candidates compiled per run")
    parser.add_argument("--min-hits", type=int, default=1)
    parser.add_argument("--cpu", type=int, choices=(7, 9), action="append")
    parser.add_argument("--exclude-range", type=parse_range, action="append",
                        default=[])
    args = parser.parse_args()

    if not args.manifest or not args.manifest.is_file():
        raise SystemExit(f"manifest does not exist: {args.manifest}")
    if not args.cache:
        raise SystemExit("cache path is required")
    if not args.rom_sha1:
        raise SystemExit("ROM SHA-1 is required")
    if not args.recompiler.is_file():
        raise SystemExit(f"recompiler does not exist: {args.recompiler}")
    args.cache.mkdir(parents=True, exist_ok=True)
    args.index = args.cache / "live-index.json"
    args.runner_import_lib = find_import_lib(args.runner_build)
    args.provider_id = provider_identity(args)

    manifest = load_json(args.manifest)
    if (not isinstance(manifest, dict) or
            manifest.get("kind") != "ndsrecomp-tier3-coverage" or
            int(manifest.get("schema", 0)) < 3):
        raise SystemExit("live sharding requires a schema-3 coverage manifest")
    if str(manifest.get("rom_sha1", "")).lower() != args.rom_sha1.lower():
        raise SystemExit("coverage manifest ROM SHA-1 does not match the runner")

    index = load_json(args.index) if args.index.is_file() else {
        "schema": 2, "rom_sha1": args.rom_sha1, "captures": {}}
    if index.get("rom_sha1") != args.rom_sha1:
        raise SystemExit("live cache index belongs to a different ROM")
    if int(index.get("schema", 0)) not in (1, 2):
        raise SystemExit("live cache index schema is unsupported")
    index["schema"] = 2

    allowed_cpus = set(args.cpu or (7, 9))
    candidates = []
    for page in manifest.get("pages", {}).get("entries", []):
        cpu = int(page["cpu"])
        if cpu not in allowed_cpus:
            continue
        entries = canonical_entries(page, args.exclude_range)
        hits = sum(entry["hits"] for entry in entries)
        if entries and hits >= args.min_hits:
            candidates.append((hits, int(page["executions"]), page, entries))
    candidates.sort(
        key=lambda item: (-item[0], -item[1], int(item[2]["addr"], 16),
                          item[2]["sha1"]))

    ok = failed = skipped = attempted = 0
    for _hits, _executions, page, entries in candidates:
        key = work_identity(int(page["cpu"]), int(page["addr"], 16),
                            str(page["sha1"]).lower(), entries,
                            args.provider_id)
        indexed = index.get("captures", {}).get(key)
        if indexed and (indexed.get("status") == "unsupported" or
                        Path(indexed.get("dll", "")).is_file()):
            skipped += 1
            continue
        if attempted >= args.max_pages:
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
            skipped += 1
        else:
            failed += 1

    print(f"NDS_SHARD_RESULT ok={ok} failed={failed} skipped={skipped}",
          flush=True)
    return 2 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
