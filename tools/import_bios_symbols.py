#!/usr/bin/env python3
"""import_bios_symbols.py — emit per-BIOS recompile config TOMLs from the
PikalaxALT/ndsbios disassembly.

ndsbios reassembles to binaries that SHA-1-match our `bios/biosnds7.rom`
and `bios/biosnds9.rom` (verified by its own `bios.sha1`, and by
`make compare`). We import its global LABEL set as `[[entry_point]]`
entries (addr + ARM/Thumb mode + name) and declare the BIOS SWI jump
tables as `[[jump_table]]` directives. Entry points the global-symbol set
does not cover (exception vectors, indirect-only handlers) are added from
the curated EXTRA_ENTRIES table — the importer is the single source of
truth, so hand-editing the generated TOMLs is never needed.

Pipeline (mirrors gbarecomp/tools/import_bios_symbols):
  1. Build the disasm:  make -C third_party/ndsbios all
  2. `arm-none-eabi-nm -n bios{7,9}.elf` → (addr, name) for global
     text symbols (type 'T') — the function entries the author marked.
  3. Parse `bios{7,9}.s` for `(arm|thumb|non_word_aligned_thumb)_func_start
     NAME` to assign ARM/Thumb mode per name.
  4. Emit `bios/biosnds7.toml` and `bios/biosnds9.toml`.

Idempotent: rerunning overwrites the TOMLs.
"""
from __future__ import annotations
import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

FUNC_START_RE = re.compile(
    r"^\s*(arm|thumb|non_word_aligned_thumb)_func_start\s+(\S+)")

# Entry points the ndsbios global-symbol set does NOT cover but which the
# runtime genuinely dispatches to. These are Tier-0 config facts: addresses
# reached ONLY by the CPU exception mechanism or by indirect/computed branches
# the static finder cannot resolve, so they can never be auto-discovered.
# Each is (addr, name, mode, kind). Identified by the Ghidra analysis
# (docs/bios_analysis.md). Folding them here keeps the importer the single
# source of truth — a re-run no longer drops hand-added entries.
#
# NOT listed here on purpose: the ARM9 IRQ-return epilogue at 0xFFFF0290. It
# is reached only by the firmware handler's `bx lr` after the BIOS does
# `adr lr,0xFFFF0290; ldr pc,[dtcm+0x3FFC]`, and the finder's Tier-2
# landing-pad auto-discovery now seeds it from exactly that idiom (it appears
# as lpad_FFFF0290). Declaring it would be redundant.
EXTRA_ENTRIES = {
    "biosnds7": [
        # Runtime-confirmed computed call from the BIOS IRQ dispatcher. The
        # same bytes are reachable in both ARM (reset vector) and Thumb state.
        (0x00000000, "reset_vector_00000000", "arm", "reset_vector"),
        (0x00000000, "runtime_thumb_00000000", "thumb", "runtime_confirmed"),
        # Exception vectors — entered by the CPU on SWI/IRQ, never by a static
        # branch, so the finder cannot see them.
        (0x00000008, "swi_vector_entry", "arm",   "exception_vector"),
        (0x00000018, "irq_vector_entry", "arm",   "exception_vector"),
        # Firmware cartridge detection calls this interior Thumb helper from
        # RAM (LR 0x037FAE5D); it is not a global ndsbios symbol.
        (0x00001498, "lpad_00001498",     "thumb", "runtime_confirmed"),
        # Static emission of the 0x1498 helper can return through this
        # computed interior continuation.
        (0x000014DE, "lpad_000014DE",     "thumb", "runtime_confirmed"),
        (0x000014EC, "lpad_000014EC",     "thumb", "runtime_confirmed"),
        (0x0000153A, "lpad_0000153A",     "thumb", "runtime_confirmed"),
        # Remaining cases in the same BIOS jump table at 0x148C.
        (0x00001548, "lpad_00001548",     "thumb", "computed_case"),
        (0x00001592, "lpad_00001592",     "thumb", "computed_case"),
        # Oracle-validated interior landing: firmware computed branch from
        # 0x037FAECE (LR 0x037FAED1) reaches Thumb 0x15A0. Verified at the
        # identical retired-instruction boundary insn7=40,428,506.
        (0x000015A0, "lpad_000015A0",     "thumb", "runtime_confirmed"),
        # IRQ handler entries reached only indirectly (installed-handler tables
        # / computed dispatch).
        (0x00001CAA, "irqh_00001CAA",    "thumb", "indirect_handler"),
        # Cartridge-transfer IRQ path branches into the middle of the shared
        # handler body after acknowledging IF bit 19.
        (0x00001CB6, "lpad_00001CB6",    "thumb", "runtime_confirmed"),
        (0x00001D1A, "irqh_00001D1A",    "thumb", "indirect_handler"),
        (0x0000201A, "irqh_0000201A",    "thumb", "indirect_handler"),
        (0x00002DD4, "irqh_00002DD4",    "arm",   "indirect_handler"),
    ],
    "biosnds9": [
        (0xFFFF0008, "swi_vector",       "arm",   "exception_vector"),
        (0xFFFF0018, "irq_vector_entry", "arm",   "exception_vector"),
        (0xFFFF01BC, "irqh_FFFF01BC",    "thumb", "indirect_handler"),
        (0xFFFF09FC, "booth_FFFF09FC",   "arm",   "indirect_target"),
    ],
}

# Per-binary facts: ELF/asm names, base, CPU, identity, and the SWI jump
# table located during the Ghidra analysis (see docs/bios_analysis.md).
BINARIES = [
    dict(
        elf="bios7.elf", asm="bios7.s", out="biosnds7.toml",
        name="NDS ARM7 BIOS", ident="biosnds7", cpu="arm7", isa="armv4t",
        base=0x00000000, size=0x4000,
        sha1="24f67bdea115a2c847c8813a262502ee1607b7df",
        swi_table=(0x00002E38, 34)),
    dict(
        elf="bios9.elf", asm="bios9.s", out="biosnds9.toml",
        name="NDS ARM9 BIOS", ident="biosnds9", cpu="arm9", isa="armv5te",
        base=0xFFFF0000, size=0x1000,
        sha1="bfaac75f101c135e32e2aaf541de6b1be4c8c62d",
        swi_table=(0xFFFF02FC, 32)),
]


def nm_globals(elf: pathlib.Path) -> list[tuple[int, str]]:
    out = subprocess.check_output(
        ["arm-none-eabi-nm", "-n", str(elf)], text=True, encoding="utf-8")
    rows = []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] == "T":          # global text symbol
            try:
                rows.append((int(p[0], 16), p[2]))
            except ValueError:
                pass
    return rows


def modes_from_asm(asm: pathlib.Path) -> dict[str, str]:
    m = {}
    for line in asm.read_text(encoding="utf-8", errors="replace").splitlines():
        g = FUNC_START_RE.match(line)
        if g:
            m[g.group(2)] = "arm" if g.group(1) == "arm" else "thumb"
    return m


def emit_toml(path: pathlib.Path, b: dict,
              funcs: list[tuple[int, str, str]]) -> None:
    L = []
    L += [f"# {b['out']} — recompile config for the {b['name']}.",
          "#",
          "# Generated by tools/import_bios_symbols.py from the",
          "# PikalaxALT/ndsbios disassembly (reassembles SHA-1-identical to",
          f"# bios/{b['ident']}.rom). Re-run the importer; do not hand-edit.",
          ""]
    L += ["[program]",
          f'name         = "{b["name"]}"',
          f'id           = "{b["ident"]}"',
          f'cpu          = "{b["cpu"]}"',
          f'isa          = "{b["isa"]}"',
          f"load_address = 0x{b['base']:08X}",
          f"size         = 0x{b['size']:08X}",
          f"entry_pc     = 0x{b['base']:08X}",
          "",
          "[identity]",
          f'sha1 = "{b["sha1"]}"',
          ""]
    L += ["# ── Function entries (from ndsbios global symbols) ──────────────"]
    arm = sum(1 for _, _, m in funcs if m == "arm")
    th = sum(1 for _, _, m in funcs if m == "thumb")
    L += [f"# {len(funcs)} entries: arm={arm} thumb={th}", ""]
    for addr, name, mode in funcs:
        L += ["[[entry_point]]",
              f"addr = 0x{addr:08X}",
              f'mode = "{mode}"',
              f'name = "{name}"',
              ""]

    extras = EXTRA_ENTRIES.get(b["ident"], [])
    if extras:
        L += ["# ── Non-symbol entry points (Tier-0 config facts) ───────────────",
              "# Exception vectors + indirect-only handlers the static finder",
              "# cannot reach. See EXTRA_ENTRIES in import_bios_symbols.py.",
              ""]
        for addr, name, mode, kind in extras:
            L += ["[[entry_point]]",
                  f"addr = 0x{addr:08X}",
                  f'mode = "{mode}"',
                  f'name = "{name}"',
                  f'kind = "{kind}"',
                  ""]

    sa, sc = b["swi_table"]
    L += ["# ── SWI dispatch table ──────────────────────────────────────────",
          "# Indexed by SWI#*4; ABS32 entries, bit0 = Thumb. Located via the",
          "# Ghidra analysis (docs/bios_analysis.md).",
          "",
          "[[jump_table]]",
          f"addr         = 0x{sa:08X}",
          "stride       = 4",
          f"count        = {sc}",
          'format       = "abs32"',
          'entries_mode = "auto"  # bit 0 of each entry encodes Thumb/ARM',
          'name         = "swi_jump_table"',
          ""]
    path.write_text("\n".join(L) + "\n", encoding="utf-8", newline="\n")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--disasm", type=pathlib.Path,
                    default=ROOT / "third_party" / "ndsbios")
    ap.add_argument("--outdir", type=pathlib.Path, default=ROOT / "bios")
    args = ap.parse_args()

    for b in BINARIES:
        elf = args.disasm / b["elf"]
        asm = args.disasm / b["asm"]
        if not elf.exists():
            print(f"error: {elf} missing — build first: "
                  f"make -C {args.disasm} all", file=sys.stderr)
            return 1
        rows = nm_globals(elf)
        modes = modes_from_asm(asm)
        funcs = [(a, n, modes.get(n, "arm")) for a, n in rows]
        out = args.outdir / b["out"]
        emit_toml(out, b, funcs)
        arm = sum(1 for _, _, m in funcs if m == "arm")
        th = sum(1 for _, _, m in funcs if m == "thumb")
        print(f"==> {out.name}: {len(funcs)} funcs (arm={arm} thumb={th})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
