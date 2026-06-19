# Dispatch & indirect-control-flow architecture

How ndsrecomp resolves indirect branches, jump tables, and the code the
firmware copies into RAM at boot. Modeled on **psxrecomp** (the most
relevant sibling: it boots a BIOS + runtime-materialized code), with the
config vocabulary from nes/snes. Read this before writing codegen or the
runtime dispatch — it is load-bearing.

## Three execution tiers (psxrecomp model)

```
Tier 1  recompiled native C    — statically recompiled banks (BIOSes, fw parts)
   │ (miss: no static fn for this PC)
Tier 2  dirty-RAM JIT shard    — code copied into RAM at boot, JIT-compiled on miss
   │ (JIT declines an opcode, or a write invalidates the shard)
Tier 3  dirty-RAM interpreter  — the always-correct floor (the guest's own bytes)
```

- **Tier 1** is everything we recompile ahead of time: both BIOSes and
  the firmware ARM9/ARM7 parts (once promoted from interpreted RAM).
  Dispatch is a binary search over a per-CPU `{guest_pc -> fn ptr}` table.
- **Tier 3** is the bounded dirty-RAM ARM interpreter (PRINCIPLES.md "The
  one exception"). The firmware copies its menu code into main RAM and
  ARM7 WRAM and runs it; until those regions are recompiled as banks, the
  interpreter runs the **guest's own copied bytes** — never an HLE model.
- **Tier 2** is the optional speed layer between them: on a dispatch miss
  into dirty RAM, JIT the fragment from the **live bytes**, key it by a
  CRC of those bytes, and run it — but only after it passes a **same-state
  differential** against the interpreter (psxrecomp runs N clean passes
  before letting a shard execute natively; a write to the shard's page
  invalidates it). Tier 2 is deferred until Tiers 1+3 boot the menu; it
  changes no observable behavior, only speed.

**Why this shape for the DS:** the firmware boot *is* runtime-
materialized code (parts decompressed into RAM then executed). That is
exactly psxrecomp's shell-copied-to-0x80030000 case. We inherit its
proven correctness floor (interpreter) + content-keyed caching, rather
than inventing one.

## Indirect branches (per CPU, both ISAs)

Generated code never falls through on an indirect target. A computed
transfer lowers to a runtime call:

```c
//  BX Rm / MOV PC,Rm / LDR PC,[..] / POP {..,PC} / LDM {..,pc}
arm_dispatch(cpu, target /* bit0 = Thumb */);   // tail position
arm_dispatch_call(cpu, target, return_pc);      // call position
```

`arm_dispatch` masks bit0 (ARM/Thumb), looks up the per-CPU dispatch
table; on hit calls the native fn; on miss drops to Tier 3/2 for that
PC. Interworking (bit0) is preserved end to end — on ARMv5 `LDR/LDM` to
PC also interworks, unlike v4T.

## Jump tables — discover, don't guess

Two complementary sources, mirroring the siblings:

1. **Static discovery (default).** The function finder backward-scans
   from each computed branch for the table-load idiom (psxrecomp's
   `scan_jr_tables`): on ARM, `LDR PC,[base,idx,LSL#2]` /
   `ADD PC,PC,idx,LSL#2` / `TBB`/`TBH` (v6+, n/a here) and the Thumb
   `LSL/LDR/MOV PC` sequences. Table targets become control-flow edges
   *and* function-discovery seeds.
2. **Config declaration (escape hatch).** When discovery can't recover a
   table (computed base, indirect-only entries), declare it in the
   per-binary TOML. Schema (superset of nes `known_table`/`split_table`
   and snes `indirect_dispatch`):

   ```toml
   [[jump_table]]
   addr         = 0x00002E38   # table base (guest addr)
   stride       = 4
   count        = 34
   format       = "abs32"      # abs32 | rel32 | split8 (lo/hi) | ...
   entries_mode = "auto"       # bit0 of each entry = Thumb (ARM idiom)
   name         = "swi_jump_table"
   ```

   The BIOS SWI tables are already declared this way by
   `tools/import_bios_symbols.py` (ARM7 `0x2E38`×34, ARM9 `0xFFFF02FC`×32).
   Each entry's bit0 gives the handler's mode — authoritative.

A table that *needs* a config hint is a discovery gap to improve in the
finder (PRINCIPLES.md "Hints are not correctness") — fix the class, then
the next binary benefits.

## Dispatch-miss loop (mandatory)

Any indirect target with no Tier-1 fn and no interpretable bytes is a
**dispatch miss** → append to `dispatch_misses.log` (per CPU):

```
extra_func 0xXXXXXXXX  arm|thumb   # cpu=arm9|arm7  via=<site>  pc=<caller>
```

A miss is a silent game-breaking bug. The loop: run → read the log →
either fix the finder (preferred) or add `[[extra_func]]` to the
binary's TOML → regen → rebuild → rerun until the log is empty
(CLAUDE.md BUILD LOOP step 5).

## What we are NOT doing

- No build-time wall (snes's resolve-or-fail) — the interpreter floor
  means an unrecovered indirect tiers *down*, it doesn't fail the build.
- No load-bearing HLE and no interpreter on the Tier-1 hot path — Tier 3
  is only the guest's own copied bytes in dirty RAM.
