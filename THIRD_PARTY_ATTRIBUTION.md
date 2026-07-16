# Copyright and Third-Party Attribution

Software is copyrighted even when its source is publicly visible. This file
records the origin and licensing posture of code and tooling in this repository;
it is not a legal guarantee of non-infringement.

## Project-owned code

Except for the items described below, the tracked implementation and
documentation are copyright © 2026 Matthew Stan and contributors. No
project-wide license has been declared yet, so no permission is granted beyond
rights provided by applicable law.

## gbarecomp

The portable ARMv4T core and portions of the recompiler driver and function
finder began as ports from the sibling
[`mstan/gbarecomp`](https://github.com/mstan/gbarecomp) project, whose commits
and this repository's commits have the same copyright owner.

- Upstream: https://github.com/mstan/gbarecomp
- Upstream license: PolyForm Noncommercial License 1.0.0
- Local scope: primarily `recompiler/armv4t/`, with adapted recompiler support
  under `recompiler/finder/` and `recompiler/src/`

The port has since gained Nintendo DS-specific ARMv5TE, CP15, dual-CPU timing,
and dispatch behavior. The ported portions remain available under the upstream
PolyForm Noncommercial terms; see the
[official license text](https://polyformproject.org/licenses/noncommercial/1.0.0/).
This provenance statement does not create a license grant for the rest of this
repository.

## melonDS optional oracle

melonDS is copyright its authors and contributors and is distributed under the
GNU General Public License v3.0 or later.

- Upstream: https://github.com/melonDS-emu/melonDS
- Pinned version used by the retained evidence: tag `1.0rc`
- License: GPL-3.0-or-later
- Local scope: `oracle/`

`oracle/setup-melonds.sh` clones melonDS into the ignored `third_party/`
directory. The tracked `oracle/patches/` files contain patch context against
melonDS and are intended solely for that GPL-covered build. The tracked oracle
shim compiles with melonDS into a separate executable; any distribution of
that combined executable must comply with the GPL.

## melonDS vendored GPU3D (runner)

Since 2026-07-16 the native runner additionally vendors the melonDS 3D
geometry engine and software rasterizer as its 3D device model (decision:
accept GPL for the runner executable rather than reimplement the 3D
pipeline; the guest still produces every register and GXFIFO write, so this
remains a device model, not HLE).

- Local scope: `runner/vendor/melonds/`
- Vendored unmodified from tag `1.0rc` (`src/` paths, byte-identical):
  `GPU3D.cpp`, `GPU3D.h`, `GPU3D_Soft.cpp`, `GPU3D_Soft.h`, `FIFO.h`,
  `types.h`, `Savestate.h`, `Savestate.cpp`, `NonStupidBitfield.h`
- Project-written shim headers in the same directory (`NDS.h`, `GPU.h`,
  `Platform.h`) replace the melonDS headers of the same names with the
  minimal interface slice the vendored units consume; as derived interfaces
  they are likewise GPL-3.0-or-later. The bridge `runner/src/gpu3d.cpp`
  implements them against the runner's own device models.
- Consequence: `nds_runner` is a combined work with melonDS. Any
  distribution of the runner binary must comply with GPL-3.0-or-later.
  The recompiler, the generated banks, and all `ndsref`-independent tooling
  remain outside this boundary and do not compile or link melonDS code.

The native implementation uses melonDS as a behavioral and timing reference.
An audit before the first public release found no exact normalized six-line
code block shared between the tracked native recompiler/runtime sources and
the pinned melonDS source tree. That mechanical check cannot prove independent
authorship; provenance comments and the repository history remain the primary
record.

## mGBA

mGBA is copyright © Jeffrey Pfau and contributors and is distributed under the
Mozilla Public License 2.0.

- Upstream: https://github.com/mgba-emu/mgba
- License: MPL-2.0
- Use here: behavioral and ARM7 timing reference inherited in part through the
  portable `gbarecomp` core; mGBA is not vendored, compiled, or linked here

## Specifications and documentation

The ARM Architecture Reference Manual and Martin Korth's GBATEK documentation
are implementation references. DeSmuME is a GPL-2.0 secondary behavioral
cross-check. PikalaxALT/ndsbios may be cloned locally for symbol research but is
ignored and is not distributed in this repository.

See [`docs/references.md`](docs/references.md) for the development provenance
policy. Names such as Nintendo DS, Nintendo, Discord, melonDS, DeSmuME, mGBA,
and ARM belong to their respective owners; references do not imply endorsement.
