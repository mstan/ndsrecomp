#!/usr/bin/env python3
"""The live-shard provider identity moves exactly when compiled output can.

beads-yjp.52. The identity used to fold the WHOLE of compile_live_shards.py
plus the recompiler executable's bytes, so a logging tweak or a no-op rebuild
invalidated every cached shard for every player -- which is why v0.6.5 could
not carry the v0.6.4 prebuilt cache forward (beads-yjp.56). It also folded
--include-roots and --merge-cache-snapshots, run-mode flags that made a
re-warm run publish under an identity the packager could not compute.

This test is the other half of that change. Narrowing an identity that guards
against loading a natively-linked DLL built by a different code generator is
only safe if the narrowing is proven both ways, so every assertion below comes
in a pair: something that must NOT move it, and something that must. The
failure mode beads-yjp.45 recorded twice is a guard test that passes
vacuously.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import shutil
import stat
import tempfile
from pathlib import Path


FAILURES: list[str] = []


def check(name: str, condition: bool, detail: str = "") -> None:
    if condition:
        print(f"  ok   {name}")
        return
    FAILURES.append(name)
    print(f"  FAIL {name}{(': ' + detail) if detail else ''}")


def load(path: Path):
    spec = importlib.util.spec_from_file_location(
        f"nds_shard_{abs(hash(str(path)))}", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def variant(work: Path, name: str, source_path: Path,
            edits: list[tuple[str, str]], append: str = ""):
    """A copy of the shard compiler with exact textual edits applied."""
    text = source_path.read_text(encoding="utf-8-sig")
    for old, new in edits:
        if old not in text:
            raise AssertionError(
                f"variant {name}: anchor not found in the shard compiler, the "
                f"test needs updating: {old!r}")
        text = text.replace(old, new, 1)
    target = work / f"variant_{name}.py"
    target.write_text(text + append, encoding="utf-8", newline="\n")
    return load(target)


def stub_recompiler(work: Path, name: str, body: str) -> Path:
    """A fake recompiler that answers --codegen-identity however we say."""
    if os.name == "nt":
        path = work / f"{name}.cmd"
        path.write_text(f"@echo off\r\n{body}\r\n", encoding="ascii",
                        newline="")
        return path
    path = work / name
    path.write_text(f"#!/bin/sh\n{body}\n", encoding="ascii", newline="\n")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True,
                        help="tools/compile_live_shards.py")
    parser.add_argument("--recompiler", type=Path, required=True)
    parser.add_argument("--gcc", type=Path, required=True)
    parser.add_argument("--runtime-include", type=Path, action="append",
                        required=True)
    parser.add_argument("--work", type=Path, default=None)
    args = parser.parse_args()

    work = Path(args.work) if args.work else Path(
        tempfile.mkdtemp(prefix="nds-shard-identity-"))
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)

    base = load(args.tool)

    def namespace(**overrides) -> argparse.Namespace:
        value = argparse.Namespace(
            compiler="gcc",
            gcc=str(args.gcc),
            tcc="tcc",
            recompiler=Path(args.recompiler),
            runtime_include=[Path(p) for p in args.runtime_include],
            generated_opt="-O2",
            max_function_bytes=512,
            # Present on purpose. These are the run-mode flags the identity
            # used to fold; a real run always carries them.
            include_roots=False,
            merge_cache_snapshots=False,
        )
        for key, item in overrides.items():
            setattr(value, key, item)
        return value

    reference = base.provider_identity(namespace())
    print(f"reference provider identity: {reference}")
    print(f"reference codegen fingerprint: {base.codegen_fingerprint()}")

    # ---- the declared emission surface is the real one --------------------
    print("\n[closure]")
    closure = dict(base.codegen_closure())
    for name in base.CODEGEN_ENTRY_POINTS:
        check(f"closure_has_entry_point[{name}]", name in closure)
    # Reachability, not just the declared roots: these are pulled in only by
    # being called from an entry point. If the walk ever stops finding them,
    # an edit to the tcc header transform or the wrapper's ABI constants
    # would stop moving the identity.
    for name in ("tcc_include_dir", "tcc_import_dir", "runtime_headers",
                 "file_sha256", "_strip_bom", "_TCC_EXTERN_RE",
                 "_TCC_REQUIRED_IMPORTS", "run", "shard_wrapper_path",
                 "ABI_VERSION", "PAGE_SIZE"):
        check(f"closure_reaches[{name}]", name in closure)
    # Orchestration decides WHAT is compiled. It reaches the output only as
    # `entries`, which work_identity() already folds, so it must stay out.
    for name in ("main", "collect_candidates", "cache_manifests",
                 "resume_manifests", "load_queue", "save_queue",
                 "record_capture", "empty_index", "exclusive_file_lock",
                 "generation_entries", "parse_range", "atomic_json",
                 "provider_identity", "work_identity", "codegen_closure"):
        check(f"closure_excludes[{name}]", name not in closure,
              "orchestration leaked into the emission fingerprint")

    # ---- edits that must NOT move the identity ---------------------------
    print("\n[stable under non-semantic edits]")
    anchor = '    cpu_token = "NDS_ARM9" if cpu == 9 else "NDS_ARM7"'
    comment_only = variant(
        work, "comment_only", args.tool,
        [(anchor, anchor + "\n    # seeded comment: cannot change a byte")],
        append="\n\n# seeded module-level comment\n")
    check("comment_inside_emission_is_free",
          comment_only.codegen_fingerprint() == base.codegen_fingerprint())

    whitespace = variant(
        work, "whitespace", args.tool,
        [(anchor, anchor + "   \n\n")])
    check("whitespace_and_blank_lines_are_free",
          whitespace.codegen_fingerprint() == base.codegen_fingerprint())

    docstring = variant(
        work, "docstring", args.tool,
        [('    """Build one shard DLL with the selected backend.',
          '    """Build one shard DLL with the selected backend (reworded).')])
    check("docstring_edit_is_free",
          docstring.codegen_fingerprint() == base.codegen_fingerprint())

    orchestration = variant(
        work, "orchestration", args.tool,
        [('        print(f"resumed {len(queued)} pending candidate(s) from "',
          '        print(f"RESUMED {len(queued)} pending candidate(s) from "')])
    check("orchestration_logging_edit_is_free",
          orchestration.codegen_fingerprint() == base.codegen_fingerprint())

    check("run_mode_flags_excluded",
          base.provider_identity(namespace(
              include_roots=True, merge_cache_snapshots=True)) == reference,
          "include_roots / merge_cache_snapshots still move the identity")

    # A PE's bytes move on every rebuild (link timestamp, build-path residue).
    # Appending an overlay leaves the program itself runnable and is the
    # cheapest way to prove the identity no longer looks at them.
    fat = work / (Path(args.recompiler).stem + "_fat"
                  + Path(args.recompiler).suffix)
    shutil.copy2(args.recompiler, fat)
    with fat.open("ab") as handle:
        handle.write(b"\0seeded overlay bytes, not code\0" * 64)
    check("recompiler_executable_bytes_excluded",
          base.provider_identity(namespace(recompiler=fat)) == reference,
          "the recompiler PE's bytes still move the identity")

    # ---- edits that MUST move the identity -------------------------------
    print("\n[moves for anything that can change output]")
    emission = variant(
        work, "emission", args.tool,
        [('    exc_base = "0xFFFF0000u" if cpu == 9 else "0x00000000u"',
          '    exc_base = "0xFFFF0000u" if cpu == 9 else "0x00000004u"')])
    check("emission_edit_moves_fingerprint",
          emission.codegen_fingerprint() != base.codegen_fingerprint(),
          "a changed emitted constant did not move the fingerprint")
    check("emission_edit_moves_provider_identity",
          emission.provider_identity(namespace()) != reference)

    flags = variant(
        work, "flags", args.tool,
        [('        "--coalesce-fallthroughs",\n    ]', '    ]')])
    check("recompiler_flag_change_moves_fingerprint",
          flags.codegen_fingerprint() != base.codegen_fingerprint())

    bumped = variant(
        work, "bumped", args.tool,
        [("SHARD_CODEGEN_VERSION = 1", "SHARD_CODEGEN_VERSION = 2")])
    check("shard_codegen_version_bump_moves_identity",
          bumped.provider_identity(namespace()) != reference)

    # Self-policing: a helper extracted OUT of an emission function must be
    # pulled into the closure by reachability, and editing that helper alone
    # must move the fingerprint. This is what a hand-kept list cannot do.
    seeded_a = variant(
        work, "seeded_a", args.tool,
        [('        f\'    info.bank_id = "{bank}";\',',
          '        f\'    info.bank_id = "{bank}{_seeded_marker()}";\',')],
        append='\n\ndef _seeded_marker() -> str:\n    return "seed-a"\n')
    seeded_b = variant(
        work, "seeded_b", args.tool,
        [('        f\'    info.bank_id = "{bank}";\',',
          '        f\'    info.bank_id = "{bank}{_seeded_marker()}";\',')],
        append='\n\ndef _seeded_marker() -> str:\n    return "seed-b"\n')
    check("extracted_helper_enters_closure",
          "_seeded_marker" in dict(seeded_a.codegen_closure()))
    check("extracted_helper_body_moves_fingerprint",
          seeded_a.codegen_fingerprint() != seeded_b.codegen_fingerprint(),
          "reachability walk did not fold a helper's own body")

    check("generated_opt_moves_identity",
          base.provider_identity(namespace(generated_opt="-O0")) != reference)
    check("max_function_bytes_moves_identity",
          base.provider_identity(
              namespace(max_function_bytes=256)) != reference)

    include = work / "include"
    include.mkdir()
    for directory in args.runtime_include:
        for header in Path(directory).glob("*.h"):
            shutil.copy2(header, include / header.name)
    same_headers = base.provider_identity(namespace(runtime_include=[include]))
    with (include / "runtime_arm.h").open("a", encoding="utf-8") as handle:
        handle.write("\n/* seeded ABI change */\n")
    check("runtime_header_change_moves_identity",
          base.provider_identity(
              namespace(runtime_include=[include])) != same_headers)

    # ---- the recompiler's reported codegen identity ----------------------
    print("\n[recompiler codegen identity]")
    reported = base.recompiler_codegen_identity(namespace())
    check("recompiler_reports_codegen_identity",
          reported.startswith("nds-codegen-v"), reported)
    v1 = stub_recompiler(work, "codegen_v1", "echo nds-codegen-v1")
    v2 = stub_recompiler(work, "codegen_v2", "echo nds-codegen-v2")
    check("recompiler_codegen_version_moves_identity",
          base.provider_identity(namespace(recompiler=v1)) !=
          base.provider_identity(namespace(recompiler=v2)))
    silent = stub_recompiler(work, "codegen_silent", "exit /b 2"
                             if os.name == "nt" else "exit 2")
    failed = False
    try:
        base.provider_identity(namespace(recompiler=silent))
    except RuntimeError:
        failed = True
    check("recompiler_without_codegen_identity_fails_closed", failed,
          "an old recompiler silently shared the cache namespace")

    print()
    if FAILURES:
        print(f"FAIL: {len(FAILURES)} assertion(s): " + ", ".join(FAILURES))
        return 1
    print("PASS: provider identity moves exactly with compiled output")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
