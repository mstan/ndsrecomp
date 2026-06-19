# External references & provenance policy

We are **not** writing an emulator; we statically recompile the DS's own
code to C. But for instruction semantics and hardware behavior we lean on
well-trodden references rather than reinventing. This file records *what*
we reference and *under what license*, so the provenance of distributable
binaries stays clean.

## Licensing policy (set by project owner)

1. **Prefer permissive** (MIT / BSD / Zlib / Apache-2.0 / MPL-2.0):
   usable directly with attribution.
2. **Specifications are the cleanest source.** Implementing an instruction
   from the ARM Architecture Reference Manual pseudocode, or DS hardware
   from GBATEK, is *not* a derivative work of any emulator. This is our
   default for the ARM core and the I/O model.
3. **Copyleft (GPL/AGPL) emulators are reference-only.** We may read them
   to understand a hardware quirk, but we **clean-room from the spec** —
   re-derive the behavior from ARM ARM / GBATEK and cite the spec, not the
   GPL code. We do not copy GPL/AGPL source into this tree.
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
| **mGBA** — github.com/mgba-emu/mgba | MPL-2.0 (file-level copyleft) | ARMv4T execution + timing reference (we already cite "mGBA-derived" cycle model in `arm_ir.cpp`). No ARMv5TE / CP15. Permissive enough to reference with attribution. |
| **QEMU** `target/arm` | GPLv2 | Reference-only (clean-room). Deep ARM coverage; consult for ambiguous corner cases, re-derive from ARM ARM. |

For ARMv5TE we have **not** needed to read any copyleft emulator — the ARM
ARM pseudocode is sufficient and is what the code is written from.

## DS hardware (CP15, 2D engines, SPI, IPC, memory map, firmware)

| Reference | License | Use |
|---|---|---|
| **GBATEK** (Martin Korth) — problemkaputt.de/gbatek.htm | Free documentation | **Primary** for DS hardware: CP15 register map + reset defaults, ITCM/DTCM, MPU regions, VRAM bank control, DISPCNT/POWCNT, IPCSYNC/IPC FIFO, SPI (firmware/touch), memory map. |
| **melonDS** — github.com/melonDS-emu/melonDS | **GPLv3** | Reference-only (clean-room). The accuracy oracle for the project (visual + in-memory diff), but its source is **not** copied; behaviors are re-derived from GBATEK. |
| **DeSmuME** — github.com/TASEmulators/desmume | **GPLv2** | Reference-only (clean-room). Second opinion on hardware quirks. |
| **PikalaxALT/ndsbios** | (disassembly; reassembles SHA-1-identical) | Already vendored to `third_party/ndsbios` (gitignored) for symbol import only — not linked into output. |

### How melonDS is used (it is GPLv3 — important)

melonDS is the **oracle**: we run it alongside and diff state. That is a
*tool-use* relationship (running a separate program and comparing output),
not linking or copying — it does not pull GPLv3 into our binary. We must
**never** copy melonDS source into the recompiler/runtime, and any DS
behavior we model is written from GBATEK, with melonDS used only to
confirm the model matches.

## Attribution

Permissive references actually consulted (mGBA timing model; vixl
encodings; ARM ARM pseudocode) are credited here and in the relevant
source comments. A consolidated `NOTICE` file will accompany any binary
distribution.
