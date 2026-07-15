# External references & provenance policy

We are **not** writing an emulator; we statically recompile the DS's own
code to C. But for instruction semantics and hardware behavior we lean on
well-trodden references rather than reinventing. This file records *what*
we reference and *under what license*, so the provenance of distributable
binaries stays clean.

## Licensing policy (set by project owner)

1. **Prefer permissive** sources (MIT / BSD / Zlib / Apache-2.0). MPL-2.0
   sources are weak-copyleft and may be used only while preserving their
   file-level obligations.
2. **Specifications are the cleanest source.** Implementing an instruction
   from the ARM Architecture Reference Manual pseudocode, or DS hardware
   from GBATEK, is *not* a derivative work of any emulator. This is our
   default for the ARM core and the I/O model.
3. **Strong-copyleft (GPL/AGPL) emulators are reference-only for the native
   runtime.** We may inspect them to understand or test a hardware quirk, but
   the committed native implementation must be independently written from
   specifications and measured behavior. GPL patch context is confined to the
   separate optional oracle under `oracle/patches/`.
4. **AGPL is last-resort, proof-only.** If an AGPL reference is the only
   way to confirm a behavior, it may be used transiently to *prove* a
   model is correct, but **must be clean-roomed out before any binary is
   distributed**. Anything touched this way is tagged `AGPL-PROVE` in a
   comment so it is easy to find and excise.

When in doubt, the rule is: **the committed implementation must trace to a
spec or a permissive source, never to copyleft code.**

## ISA: ARMv5TE (ARM9) + ARMv4T (ARM7)

| Reference | License | Use |
|---|---|---|
| **ARM Architecture Reference Manual, ARMv5TE (ARM DDI 0100E)** | Spec (free to read) | **Primary.** Per-instruction pseudocode for CLZ, QADD/QSUB/QDADD/QDSUB (`SignedSat`), SMLA/SMLAW/SMLAL/SMUL xy family, LDRD/STRD, BLX, PLD, MCR/MRC/CDP. We implement decode + semantics from this pseudocode. |
| **vixl** (Linaro) — github.com/Linaro/vixl | BSD-3-Clause (permissive) | Cross-check the **A32/T32 encodings** (assembler + disassembler). Note: vixl's *simulator* is AArch64-only, so it is an encoding reference, not an execution reference. |
| **mGBA** — github.com/mgba-emu/mgba | MPL-2.0 (file-level copyleft) | ARMv4T execution + timing reference (we already cite an "mGBA-derived" cycle model in `arm_ir.cpp`). No ARMv5TE / CP15. It is not vendored or linked here. |
| **QEMU** `target/arm` | GPLv2 | Reference-only (clean-room). Deep ARM coverage; consult for ambiguous corner cases, re-derive from ARM ARM. |

For ARMv5TE we have **not** needed to read any copyleft emulator — the ARM
ARM pseudocode is sufficient and is what the code is written from.

## DS hardware (CP15, 2D engines, SPI, IPC, memory map, firmware)

| Reference | License | Use |
|---|---|---|
| **GBATEK** (Martin Korth) — problemkaputt.de/gbatek.htm | Free documentation | **Primary** for DS hardware: CP15 register map + reset defaults, ITCM/DTCM, MPU regions, VRAM bank control, DISPCNT/POWCNT, IPCSYNC/IPC FIFO, SPI (firmware/touch), memory map. |
| **melonDS** — github.com/melonDS-emu/melonDS | **GPLv3-or-later** | Behavioral/timing reference for the native implementation and the separately built accuracy oracle. It is cloned only into ignored `third_party/`; tracked GPL patch context is confined to `oracle/patches/`. |
| **DeSmuME** — github.com/TASEmulators/desmume | **GPLv2** | Reference-only. Second opinion on hardware quirks; not vendored or linked. |
| **PikalaxALT/ndsbios** | (disassembly; reassembles SHA-1-identical) | Already vendored to `third_party/ndsbios` (gitignored) for symbol import only — not linked into output. |

### How melonDS is used (it is GPLv3 — important)

melonDS is the **oracle**: we run its GPL-covered executable alongside the
native runner and diff state over TCP. That separate-process relationship does
not link melonDS into the native runner. The oracle shim and tracked patch
context are part of the optional melonDS build and must be handled under its
GPL terms; they are not part of the native runner. Native DS behavior is
implemented independently from specifications and measured oracle behavior.

## Attribution

References actually consulted (including the mGBA timing model, vixl
encodings, and ARM ARM pseudocode) are credited here and in relevant source
comments. See the repository-level `THIRD_PARTY_ATTRIBUTION.md` before any
source or binary distribution.
