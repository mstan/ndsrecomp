#!/usr/bin/env bash
# ghidra/import_bios.sh — headless import + analyze + seed-export of the
# DS BIOSes. Idempotent: re-importing an existing program is skipped by
# analyzeHeadless unless -overwrite is passed.
#
# Usage: bash ghidra/import_bios.sh <GHIDRA_INSTALL_DIR>
#   e.g. bash ghidra/import_bios.sh /c/ghidra_11.0
#
# Produces ghidra/NDSBIOS.gpr and generated/ghidra_function_starts_*.json

set -euo pipefail

GHIDRA="${1:-}"
if [ -z "$GHIDRA" ]; then
    echo "usage: import_bios.sh <GHIDRA_INSTALL_DIR>" >&2
    exit 2
fi

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
HEADLESS="$GHIDRA/support/analyzeHeadless"
[ -x "$HEADLESS" ] || HEADLESS="$GHIDRA/support/analyzeHeadless.bat"

PROJ_DIR="$REPO_ROOT/ghidra"
PROJ_NAME="NDSBIOS"
SCRIPT_DIR="$REPO_ROOT/ghidra"
BIOS="$REPO_ROOT/bios"
OUT="$REPO_ROOT/generated"
mkdir -p "$OUT"

import_one() {
    local file="$1" lang="$2" base="$3" tag="$4"
    echo "==> importing $file  ($lang @ $base)"
    "$HEADLESS" "$PROJ_DIR" "$PROJ_NAME" \
        -import "$file" \
        -processor "$lang" \
        -loader BinaryLoader -loader-baseAddr "$base" \
        -scriptPath "$SCRIPT_DIR" \
        -postScript export_functions.py "$OUT/ghidra_function_starts_$tag.json" \
        -overwrite
}

# ARM7 BIOS — ARM7TDMI / ARMv4T, maps at 0x00000000
import_one "$BIOS/biosnds7.rom" "ARM:LE:32:v4t" "0x00000000" "arm7"
# ARM9 BIOS — ARM946E-S / ARMv5TE, maps at 0xFFFF0000
import_one "$BIOS/biosnds9.rom" "ARM:LE:32:v5t" "0xFFFF0000" "arm9"

echo "==> done. seeds in $OUT/ghidra_function_starts_arm7.json and _arm9.json"
