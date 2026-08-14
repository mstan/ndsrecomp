#!/usr/bin/env python3
"""Emit bios/freebios9.toml and bios/freebios7.toml from the FreeBIOS
assembly source (beads-yjp.15 increment 3).

FreeBIOS (the DraStic BIOS replacement, BSD-2-Clause) ships WITH its
assembly source: the third_party/freebios submodule
(github.com/mstan/freebios, an unmodified mirror of melonDS's freebios/)
builds byte-identically to its shipped images with a stock devkitARM, so
its symbol table is authoritative — the same footing the retail configs
got from the PikalaxALT/ndsbios disassembly.

  1. Assemble bios_common.S twice (-DBIOS_ARM7 / -DBIOS_ARM9).
  2. objcopy to raw binaries and REFUSE unless they are byte-identical to
     the submodule's drastic_bios_arm{7,9}.bin (symbol addresses are only
     trustworthy against the exact image the runner embeds).
  3. Classify each label as code or data by the first statement after it
     in the source (data directives = data; FreeBIOS is pure ARM, no
     Thumb anywhere).
  4. Emit the configs: the eight exception-vector slots plus every code
     label.

Re-run this importer; do not hand-edit the emitted tomls.
"""

from __future__ import annotations

import hashlib
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SUB = REPO / "third_party" / "freebios"
SRC = SUB / "bios_common.S"
# The devkitPro MSYS environment exports DEVKITARM as /opt/devkitpro/...,
# which is meaningless to Windows Python; fall back to the standard install.
_env_dka = Path(os.environ.get("DEVKITARM", ""))
DEVKITARM = (_env_dka if (_env_dka / "bin").exists()
             else Path(r"C:\devkitPro\devkitARM"))
GCC = DEVKITARM / "bin" / "arm-none-eabi-gcc.exe"
OBJCOPY = DEVKITARM / "bin" / "arm-none-eabi-objcopy.exe"
NM = DEVKITARM / "bin" / "arm-none-eabi-nm.exe"

DATA_DIRECTIVES = re.compile(
    r"^\s*\.(word|short|hword|byte|space|skip|ascii|asciz|incbin|4byte|"
    r"2byte|float|double|fill)\b")
LABEL = re.compile(r"^\s*([A-Za-z_.$][\w.$]*):")

TARGETS = [
    {  # cpu, march flag, define, submodule image, config, program fields
        "define": "BIOS_ARM9", "march": "armv5te",
        "bin": SUB / "drastic_bios_arm9.bin",
        "toml": REPO / "bios" / "freebios9.toml",
        "name": "FreeBIOS ARM9", "id": "freebios9",
        "cpu": "arm9", "isa": "armv5te",
        "load_address": 0xFFFF0000, "size": 0x1000,
    },
    {
        "define": "BIOS_ARM7", "march": "armv4",
        "bin": SUB / "drastic_bios_arm7.bin",
        "toml": REPO / "bios" / "freebios7.toml",
        "name": "FreeBIOS ARM7", "id": "freebios7",
        "cpu": "arm7", "isa": "armv4t",
        "load_address": 0x00000000, "size": 0x4000,
    },
]

VECTORS = [
    (0x00, "_vector_reset"), (0x04, "_vector_undefined"),
    (0x08, "_vector_swi"), (0x0C, "_vector_prefetch_abort"),
    (0x10, "_vector_data_abort"), (0x14, "_vector_reserved"),
    (0x18, "_vector_irq"), (0x1C, "_vector_fiq"),
]


def classify_labels(source_text: str) -> dict[str, str]:
    """label name -> 'code' | 'data', judged by the first statement after
    the label (label-only and comment lines skipped; a data directive means
    the label names a table, not a function)."""
    kinds: dict[str, str] = {}
    pending: list[str] = []
    for raw in source_text.splitlines():
        line = raw.split("@")[0].split("//")[0]
        rest = line
        while True:
            m = LABEL.match(rest)
            if not m:
                break
            pending.append(m.group(1))
            rest = rest[m.end():]
        stmt = rest.strip()
        if not stmt or stmt.startswith("#") or stmt.startswith(".if") or \
                stmt.startswith(".else") or stmt.startswith(".endif"):
            continue
        if pending:
            kind = "data" if DATA_DIRECTIVES.match(stmt) else "code"
            for label in pending:
                kinds.setdefault(label, kind)
            pending = []
    return kinds


def main() -> int:
    for tool in (GCC, OBJCOPY, NM):
        if not tool.exists():
            print(f"missing {tool} (set DEVKITARM)", file=sys.stderr)
            return 1
    source_text = SRC.read_text(encoding="utf-8", errors="replace")
    kinds = classify_labels(source_text)

    for t in TARGETS:
        with tempfile.TemporaryDirectory() as td:
            obj = Path(td) / "freebios.o"
            raw = Path(td) / "freebios.bin"
            subprocess.run(
                [str(GCC), str(SRC), f"-D{t['define']}",
                 f"-march={t['march']}", "-c", "-o", str(obj)],
                check=True)
            subprocess.run(
                [str(OBJCOPY), "-O", "binary", str(obj), str(raw)],
                check=True)
            built = raw.read_bytes()
            vendored = t["bin"].read_bytes()
            if built != vendored:
                print(f"REFUSING: rebuilt {t['id']} does not match "
                      f"{t['bin']}", file=sys.stderr)
                return 1
            nm = subprocess.run([str(NM), "-n", str(obj)],
                                check=True, capture_output=True, text=True)

        entries: dict[int, str] = {addr: name for addr, name in VECTORS}
        for line in nm.stdout.splitlines():
            parts = line.split()
            if len(parts) != 3:
                continue
            addr, _kind, name = parts
            if kinds.get(name) != "code":
                continue
            offset = int(addr, 16)
            entries.setdefault(offset, name)

        sha1 = hashlib.sha1(vendored).hexdigest()
        code_count = len(entries)
        out = [
            f"# {t['toml'].name} — recompile config for the FreeBIOS image",
            f"# (third_party/freebios/{t['bin'].name}, BSD-2-Clause).",
            "#",
            "# Generated by tools/import_freebios_symbols.py from the",
            "# FreeBIOS assembly source (third_party/freebios/"
            "bios_common.S,",
            "# the github.com/mstan/freebios submodule), which reassembles",
            "# byte-identical to the shipped image. Re-run the importer; "
            "do not",
            "# hand-edit.",
            "",
            "[program]",
            f"name         = \"{t['name']}\"",
            f"id           = \"{t['id']}\"",
            f"cpu          = \"{t['cpu']}\"",
            f"isa          = \"{t['isa']}\"",
            f"load_address = 0x{t['load_address']:08X}",
            f"size         = 0x{t['size']:08X}",
            f"entry_pc     = 0x{t['load_address']:08X}",
            "",
            "[identity]",
            f"sha1 = \"{sha1}\"",
            "",
            "# ── Function entries (vectors + FreeBIOS source labels) "
            "──────────",
            f"# {code_count} entries, all arm (FreeBIOS has no Thumb)",
        ]
        for offset in sorted(entries):
            out += [
                "",
                "[[entry_point]]",
                f"addr = 0x{t['load_address'] + offset:08X}",
                "mode = \"arm\"",
                f"name = \"{entries[offset]}\"",
            ]
        t["toml"].write_text("\n".join(out) + "\n", encoding="utf-8",
                             newline="\n")
        print(f"{t['toml'].name}: {code_count} entries "
              f"(image sha1 {sha1}, rebuilt byte-identical)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
