# ndsrecomp Project Principles

Platform-specific specialization of the cross-project recomp
principles. The platform-agnostic rules (no stubs, fix-the-source,
no edit generated C, always-on rings, disasm-is-not-an-oracle) still
apply; this file adds the DS detail and, above all, the **dual-CPU**
detail that has no precedent in the other projects.

## Ground Truth

- The BIOS dumps and `firmware.bin` (each SHA-1-verified against a
  known-good hash) plus a trusted oracle are the behavioral source of
  truth. Primary oracle: **melonDS**. Secondary: **DeSmuME**.
- Generated C is evidence, not authority. If it is wrong, fix the
  recompiler / runtime / analyzer / config, then regenerate.
- GBATEK, no$gba notes, and public RE give symbols, boundaries,
  register layouts, and the firmware-header/boot format. They are
  reference, **not** an execution oracle. The emulator is.

## Firmware menu must be flawless before any game (HARD GATE)

No cartridge, no commercial game, no "load a ROM real quick" happens
until the console boots to the **interactive firmware menu** and
matches the melonDS oracle:

1. **Visually** — both 256×192 framebuffers (engine A top, engine B
   bottom) match the oracle at every frame from power-on through the
   Health & Safety screen into the main menu.
2. **In-memory** — main RAM, VRAM, palette, OAM byte-identical to the
   oracle at every frame checkpoint. I/O may have documented
   write-only / open-bus jitter, nothing else.
3. **Interactively** — a touch (mouse on the bottom screen) drives the
   TSC sample on the ARM7, the menu reacts, and settings navigate.

**Why:** the BIOS + firmware boot is the simplest thing the console
runs and needs no per-game anything. If it isn't right, every higher
layer is built on a foundation we don't trust. "Close enough" here
compounds into invisible bugs the instant a game loads.

## BIOS + firmware are recompiled, not interpreted (SHOWSTOPPER)

Both BIOSes **and** the firmware-resident ARM9/ARM7 boot+menu code are
recompiled and executed via the dispatch tables — not stubbed, not
HLE'd, not fast-forwarded, and **not interpreted on any runtime hot
path**. Power-on enters the ARM7 BIOS reset (`0x00000000`) and ARM9
BIOS reset (`0xFFFF0000`); the BIOS reads the firmware header, copies
the parts into RAM, and jumps. Our builds do the same, through
recompiled code, from the first PC.

- SWIs land at the BIOS SWI vector and the recompiled BIOS handles
  them. There is no `if (swi == X) hle_...()`.
- IRQs land at the BIOS IRQ vector; the recompiled BIOS dispatcher
  reads `IF`, routes to the registered handler, returns through the
  real epilogue. Per-CPU (`0x04000208` IME etc.) on each side.
- The firmware boot copy is **LLE**: we execute the BIOS code that
  parses the header and copies/relocates the parts. We do not
  hand-wave a "direct boot" that places code and jumps.

**Why:** stubbed SWIs / skipped boots are HLE-by-accident, and a
load-bearing interpreter is the same debt in disguise — it hides every
codegen bug until the day it comes out, then they all fire at once.
PSX/SNES/Genesis/GBA use the same model.

## Interpreter is informative, never load-bearing (SHOWSTOPPER)

An ARM interpreter exists for **one** purpose: an offline oracle to
diff recompiled output against. It is never on a runtime exec path.

**Permitted:** decode-trace diff harnesses, per-IrOp semantic
reference, instruction unit tests, offline smoke oracles.

**Forbidden (means the recompiler is broken, fix it first):** any
runner calling `Interpreter::step` on live state; any "fall back to
interpreter on unknown op"; any "interpret now, recomp later"
milestone; any hybrid where some PCs are recompiled and others
interpreted.

### The one exception: the dirty-RAM interpreter (bounded, intentional)

The firmware copies code into main RAM at boot and executes it. Until
each such region is itself recompiled as a bank, a **small ARM
interpreter runs the guest's own bytes in dirty RAM** — exactly as
`psxrecomp` does. This is not load-bearing HLE: it executes the real
copied instructions, never a hand model of them, and every dirty
region is a candidate to be promoted to a recompiled bank. Track these
regions; shrink the interpreted set over time. It is the *guest's*
code either way.

## Performance HLE is layered above the proven floor

The boot, BIOS, firmware, recompiled CPU, and software-renderer paths above are
the accuracy floor. Once that floor passes the independent-oracle gates, a
measured subsystem or title routine may receive an optimized/HLE replacement.
That replacement may become the normal path, but the original implementation
stays linked, forceable, and authoritative for differential verification and
fallback. An HLE miss returns to the recompiled LLE body, never to Tier-3.

The implementation and promotion contract is in `HLE_ARCHITECTURE.md`. In
particular, replacements are keyed by content/bank identity rather than bare
addresses, begin forced/off-default, prove mixed-tier and whole-workload
behavior, and earn default-on status one item at a time. Accuracy-affecting
items require an explicit measured error contract and user approval.

## Dual-CPU semantics (DS-specific, no precedent in sibling projects)

Treat the ARM9 and ARM7 as **two cooperating CPUs**, not one.

- **Two cores, two ISAs.** ARM9 = ARMv5TE (BLX, CLZ, QADD/QSUB &
  saturated math, SMLAxy/SMULxy/SMLAWy, LDRD/STRD, PLD, MCR/MRC +
  CP15). ARM7 = ARMv4T (the GBA core). Decode and codegen must select
  the right ISA per CPU; do not assume v4T-only.
- **One scheduler, two timelines.** ARM9 runs ~2× the ARM7 clock.
  Interleave on a single event scheduler (mirroring melonDS), and
  resync at every cross-CPU boundary: IPC FIFO/SYNC, shared-WRAM
  access, and any I/O the other side observes. Coarse free-running of
  one CPU past a sync point is a correctness bug, not an optimization.
- **Never pause/step the two CPUs to "compare."** That is arm-then-
  capture in disguise (see DEBUG.md). Free-run, query the always-on
  rings, diff.
- **Shared memory is real.** Main RAM (`0x02000000`, 4 MB) is visible
  to both. Shared WRAM (`0x03000000`) has a runtime-configurable split
  via `WRAMCNT`. Model the split; don't pick "nearest convenient."
- **IPC is first class.** IPCSYNC (`0x04000180`) and IPC FIFO
  (`0x04000184`/`0x04100000`) carry the boot handshake. Their ordering
  and IRQ semantics must match hardware.

## Control-flow semantics (ARM/Thumb, both cores)

- **Interworking is first class.** BX / BLX / `LDR PC` / `LDM {pc}`
  switch ARM↔Thumb on bit 0 of the target. Block discovery follows
  both states. On ARMv5, `LDR/LDM` to PC also interworks (unlike v4T).
- **PC-visible pipeline.** ARM PC reads return `+8`, Thumb `+4`.
- **Condition codes & shifter carry-out** on every ARM data-processing
  op feed CPSR; model both.
- **Banked registers & modes.** SVC/IRQ/FIQ/ABT/UND banked R13/R14
  (+FIQ R8–R12), SPSR per mode, exception entry/return restore CPSR.
- **LDM/STM edge cases.** Empty list, base-in-list, `^` bit,
  write-back interactions — do not simplify.
- **CP15 (ARM9 only).** MCR/MRC p15 configure cache, the **MPU**
  (protection regions, not a full MMU), and **TCM** (ITCM/DTCM)
  base/size. TCM placement changes what addresses mean — model it.

## Runtime boundaries

- Bus primitives are faithful and boring. Region map: ARM7 BIOS, ARM9
  BIOS, main RAM, shared WRAM (split), ARM7 WRAM, ITCM/DTCM, I/O
  (per-CPU), palette, VRAM banks A–I (bank-mapped), OAM, GBA slot.
- Do **not** silence unmapped/unknown reads/writes. Every unknown I/O
  access emits a structured trace artifact. Magic returns are
  forbidden unless documented.
- Save-state restores both CPUs + memory + devices + a well-defined
  generated-code resume boundary.

## Hints are not correctness

Symbol/CFG hints bootstrap discovery; a function/table that *needs* a
hint exposes a discovery gap to fix in the analyzer so the next case
benefits. Per-target config is only for genuine facts (identity, entry
points, documented quirks), never to paper over a recompiler bug.

## Validation

A fix is done only when: (1) root cause explained; (2) the bug class
addressed/audited across sites; (3) generated C regenerated; (4) the
core builds and the runner builds; (5) a deterministic smoke / oracle
comparison exercises it. Smoke scripts and frame logs are
deterministic and rerunnable without manual reconstruction.
