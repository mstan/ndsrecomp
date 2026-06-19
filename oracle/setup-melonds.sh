#!/usr/bin/env bash
# oracle/setup-melonds.sh — clone melonDS at a pinned tag and apply our
# nds_oracle patch. Idempotent (skips clone/apply when already done).
#
# Usage: from the ndsrecomp/ root, run `bash oracle/setup-melonds.sh`.
# The build itself is driven from PowerShell afterward (see the echoed
# instructions). melonDS lives in third_party/ and is gitignored.

set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
MELON_DIR="$REPO_ROOT/third_party/melonDS"
# Pin to a release tag so the oracle is reproducible. Bump deliberately.
# (melonDS tags are lowercase; 1.0rc is the last release before the 1.0+
# instance-API refactor, so it matches the global NDS:: hook points in
# patches/README.md.)
MELON_TAG="1.0rc"
PATCHES_DIR="$REPO_ROOT/oracle/patches"

if [ ! -d "$MELON_DIR/.git" ]; then
    echo "==> Cloning melonDS $MELON_TAG into $MELON_DIR"
    git clone --depth 1 --branch "$MELON_TAG" \
        https://github.com/melonDS-emu/melonDS.git "$MELON_DIR"
else
    echo "==> melonDS already cloned at $MELON_DIR"
fi

# Apply patches that haven't landed yet. Each patch is detected by a
# sentinel so re-runs are safe. (Patches authored against the clone;
# see patches/README.md for the hook points.)
cd "$MELON_DIR"
shopt -s nullglob
for p in "$PATCHES_DIR"/*.patch; do
    if git apply --check "$p" >/dev/null 2>&1; then
        echo "==> Applying $(basename "$p")"
        git apply "$p"
    else
        echo "==> $(basename "$p") already applied (or N/A) — skipping"
    fi
done

cat <<'EOF'
==> melonDS source ready. The oracle shim builds OUT OF TREE (its sources live
    in oracle/shim/, tracked in this repo; only the SetIRQ hook above is applied
    to the clone). From the ndsrecomp root (mingw64 g++ + Ninja on PATH):

    cmake -B oracle/shim/build -S oracle/shim -DMELONDS_DIR=third_party/melonDS -G Ninja
    cmake --build oracle/shim/build --target nds_oracle

Then run the oracle pointed at the same dumps as the native runtime:

    oracle/shim/build/nds_oracle.exe \
        --bios9 bios/biosnds9.rom --bios7 bios/biosnds7.rom \
        --firmware bios/firmware.bin --boot firmware --port 19843

And from oracle/:  python diff_mem.py --region mainram --event vblank9:30
EOF
