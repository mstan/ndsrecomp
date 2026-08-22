#!/usr/bin/env python3
"""Resolve captured coverage pages back to the ROM images they were copied from.

The first real player submission carried 309 executed code pages. 289 of them
(93.5%) turned out to be byte-identical to bytes already in the ROM -- the ARM9
static module, the ARM7 static module, or a decompressed overlay -- just resident
at an address no bank config declares. The ARM7 case was the whole story of that
session: arm7.bin is loaded at the header address 0x02380000 and recompiled
there, then the guest relocates it to 0x037F7E50 (WRAM) and 0x027CFBC4 (main
RAM) and runs it from those aliases, where no bank exists, so every instruction
of it fell to the interpreter.

That class of gap does not need a player dump to fix -- only the load base, which
is what this module recovers. Matching each captured page against the prepared
ROM images turns a dump into a small list of "this module is also resident at
this base and nothing covers it", which is both far more actionable and far more
trustworthy than a bag of bytes from someone else's machine.
"""

from __future__ import annotations

import collections
import json
import re
from pathlib import Path

# Long enough that a match is not a coincidence, short enough to stay a cheap
# substring scan over a few hundred KB of module.
PROBE = 256


class RomImages:
    """The prepared per-module images for a title, indexed for exact lookup."""

    def __init__(self, images_dir: Path):
        self.dir = images_dir
        self.modules: dict[str, bytes] = {}
        self.declared_base: dict[str, int] = {}
        for name in ("arm9.bin", "arm7.bin"):
            path = images_dir / name
            if path.is_file():
                self.modules[name] = path.read_bytes()
        overlays = images_dir / "overlays"
        if overlays.is_dir():
            for path in sorted(overlays.glob("*.bin")):
                self.modules[f"overlays/{path.name}"] = path.read_bytes()
        manifest = images_dir / "overlays.json"
        if manifest.is_file():
            try:
                for record in json.loads(manifest.read_text(encoding="utf-8")):
                    self.declared_base[f"overlays/{record['file']}"] = \
                        int(str(record["load_address"]), 16)
            except (ValueError, KeyError, TypeError):
                pass
        # The static modules declare their base in the sibling TOML the
        # recompiler is driven from. Without this, every page of the ARM9
        # module resident at its own header address reports as an undeclared
        # alias, burying the real findings under the one thing already correct.
        for module, config in (("arm9.bin", "arm9.toml"),
                               ("arm7.bin", "arm7.toml")):
            path = images_dir / config
            if module not in self.modules or not path.is_file():
                continue
            found = re.search(
                r"^\s*load_address\s*=\s*(0x[0-9A-Fa-f]+)",
                path.read_text(encoding="utf-8", errors="replace"), re.M)
            if found:
                self.declared_base[module] = int(found.group(1), 16)
        # Built lazily: a probe -> [(module, offset)] index over every aligned
        # position, so a page lookup is a dict hit instead of a scan per module.
        self._index: dict[bytes, list[tuple[str, int]]] | None = None

    def declare(self, module: str, base: int) -> None:
        self.declared_base[module] = base

    def _build_index(self) -> None:
        index: dict[bytes, list[tuple[str, int]]] = collections.defaultdict(list)
        for name, blob in self.modules.items():
            # Pages are 4 KiB aligned in guest space but a module's own load
            # base need not be, so index every 4-byte-aligned offset. ARM and
            # Thumb code are both at least halfword aligned; 4 keeps the index
            # small and every observed alias so far is word aligned.
            for offset in range(0, max(0, len(blob) - PROBE), 4):
                index[blob[offset:offset + PROBE]].append((name, offset))
        self._index = index

    def locate(self, page: bytes) -> tuple[str, int] | None:
        """Return (module, offset) when this page is verbatim ROM content."""
        if self._index is None:
            self._build_index()
        assert self._index is not None
        for name, offset in self._index.get(page[:PROBE], ()):
            blob = self.modules[name]
            if blob[offset:offset + len(page)] == page:
                return name, offset
        return None


def resolve(pages, images: RomImages):
    """Classify captured pages against the ROM images.

    `pages` is an iterable of (cpu, addr, bytes). Returns (aliases, unresolved)
    where aliases maps (cpu, module, base) -> {addrs, offsets} and unresolved is
    the list of pages holding code that exists nowhere in the ROM.
    """
    aliases: dict[tuple[int, str, int], dict] = {}
    unresolved: list[tuple[int, int, bytes]] = []
    for cpu, addr, blob in pages:
        found = images.locate(blob)
        if found is None:
            unresolved.append((cpu, addr, blob))
            continue
        module, offset = found
        slot = aliases.setdefault(
            (cpu, module, addr - offset),
            {"addrs": set(), "offsets": set()})
        slot["addrs"].add(addr)
        slot["offsets"].add(offset)
    return aliases, unresolved


def undeclared(aliases, images: RomImages):
    """Aliases whose load base no config declares -- the actionable output."""
    out = []
    for (cpu, module, base), slot in sorted(aliases.items()):
        if images.declared_base.get(module) == base:
            continue
        addrs = sorted(slot["addrs"])
        out.append({
            "cpu": cpu,
            "module": module,
            "base": base,
            "declared_base": images.declared_base.get(module),
            "pages": len(addrs),
            "guest_span": (addrs[0], addrs[-1] + 4096),
            "module_span": (min(slot["offsets"]),
                            max(slot["offsets"]) + 4096),
        })
    return out
