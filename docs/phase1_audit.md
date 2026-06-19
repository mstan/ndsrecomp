# Phase 1 — ARM core port: build + execution-driven gap audit

The recompiler builds and runs on the real BIOSes. Instead of guessing
which instructions to implement, `nds_recompile --audit` decode-walks
every discovered function and reports the IrOp histogram + the codegen
gaps + the undefined encodings. This is the gap list that drives Phase 1.

## Build

```
cmake -G Ninja -B recompiler/build recompiler   # mingw64 g++ 15.2, ninja
cmake --build recompiler/build                  # -> nds_recompile.exe
recompiler/build/nds_recompile --config bios/biosnds7.toml \
    --bin bios/biosnds7.rom --audit
```

`recompiler/` = ported `armv4t/` core (decoder/IR/codegen) + portable
`finder/` (function_finder + config, namespace renamed to `ndsrecomp`) +
`support/` (sha1) + `third_party/toml.hpp` + `src/main.cpp` (NDS driver).

## ARM7 BIOS audit — ZERO codegen gaps ✅

583 functions (97 arm / 486 thumb), 75,473 instructions decoded.
**codegen NOT-IMPLEMENTED: none.** Every instruction the ARM7 BIOS uses
(incl. LDM/STM, MUL, MRS/MSR, LDRSB/LDRSH, SWI, the Thumb BL halves)
lowers. The few "undefined" words are literal-pool / ARM↔Thumb-veneer
bytes the linear walk over-ran into — not real gaps.

## ARM9 BIOS audit — CP15 (MCR/MRC) decode DONE ✅

These are **CP15** accesses — the ARM9 system-control coprocessor
(cache, MPU, ITCM/DTCM):

| encoding | instruction | meaning |
|---|---|---|
| `0xEE010F10` | `mcr p15,0,Rd,c1,c0,0` | CP15 **control register** (MPU/cache enable) |
| `0xEE070F90` | `mcr p15,0,Rd,c7,c0,4` | wait-for-interrupt / cache op |
| `0xEE07EF90` | `mcr p15,0,LR,c7,…` | cache/TCM op |
| `0xEE090F11` | `mcr p15,0,Rd,c9,c1,0` | **DTCM** base/size |
| `0xEE11CF10` `0xEE19xF11` | `mrc p15,…` | read control / TCM / cache regs |

The decoder previously returned Undefined for the `0xEE…` coprocessor
space, and the finder treats undefined as a body terminator, so ARM9
discovery was truncated at every `mcr`/`mrc` (94-func undercount). The
coprocessor space (`cond 1110…`) now decodes: bit4=1 → MCR/MRC, bit4=0 →
CDP (`recompiler/armv4t/arm_decode.cpp:decode_coprocessor`). MCR/MRC/CDP
lower (`emit_coprocessor`) to `runtime_coproc_write` / `runtime_coproc_read`
/ `runtime_coproc_cdp` (declared in `runtime_arm.h`), which Phase 2's CP15
model backs.

**Post-fix audit:** 97 functions (arm=47 / thumb=50), 10,686 instructions,
**codegen NOT-IMPLEMENTED: none.** Histogram now shows `mcr`=15, `mrc`=7,
`cdp`=0 (the BIOS's coprocessor ops are all MCR/MRC — every listed
encoding has bit4=1). The +3 functions vs. the truncated 94 are the
early-boot CP15 setup (reset / MPU / TCM / cache init) the walk previously
cut off. Remaining undefined ARM/THUMB words (distinct=3 arm, 1 thumb,
all low-count) are literal-pool / ARM↔Thumb-veneer bytes the linear walk
over-ran — e.g. `0x4770DBFC` is THUMB `bx lr` + `b.mi` read as one ARM
word. Not real gaps.

> LDC/STC (`cond 110x`) are intentionally left Undefined: they target no
> DS coprocessor (CP15 supports MCR/MRC only), so a real occurrence should
> surface loudly rather than decode silently. The ARM7 (ARMv4T) has no
> coprocessor at all; the 2 `mrc` it now shows are pool data sampled by
> the linear walk. ARM7-vs-ARM9 coprocessor *legality* is a runtime
> concern (an ARM7 `runtime_coproc_*` would trap), not a decode one.

## Phase 1 work list (now empirical, not guessed)

1. ~~**MCR/MRC decode + IR + codegen**~~ — DONE. Coprocessor space decodes
   (MCR/MRC/CDP), lowers to `runtime_coproc_{write,read,cdp}`, ARM9
   discovery un-truncated, zero codegen gaps.
2. ~~**Remaining ARMv5TE ops**~~ — DONE. BLX (reg+imm), CLZ,
   QADD/QSUB/QDADD/QDSUB (CPSR.Q), the signed-multiply family
   (SMLA/SMLAW/SMULW/SMLAL/SMUL xy), LDRD/STRD, PLD (no-op) all decode →
   IR → codegen. The saturating + signed-multiply ops previously
   **mis-decoded silently** as DP `AND`/`SUB` (S=0) — now intercepted in
   the ARMv5 misc space before the DP fallthrough. BLX is mode-switch
   aware in the finder. Post-fix ARM9 audit: 99 funcs, `blx`=6, zero
   codegen gaps; remaining undefined are pool data (incl. `110x`
   LDC/STC-shaped words, intentionally left loud). Covered by
   `recompiler/tests/armv5te_decode_test.cpp` (19 encodings + negative
   tests that real CMP/MSR are not stolen by the interception).
3. **CP15 model** in the runtime — control register, MPU regions, TCM
   (ITCM/DTCM) base/size. TCM placement changes what addresses mean, so
   this is load-bearing for the ARM9 bus (Phase 2). **Gated on standing
   up `runner/`** (empty; Phase 2). The codegen already emits
   `runtime_coproc_{write,read,cdp}` calls for it to back.
4. ~~Wire C bank emission (`--out`) + dispatch table~~ — DONE. `--out <dir>
   --bank <name>` emits `generated/<bank>.{c,h}` + `<bank>_dispatch.c`
   (ported from gbarecomp's proven emitter; plain C, per-bank symbols
   `g_dispatch_<bank>`). Emitted `arm9_bios` (99 fns) + `arm7_bios`
   (583 fns); **all generated TUs compile clean** as C11 against
   `runtime_arm.h` (only benign `-Wunused-variable` from the uniform
   `_post` temp — generated TUs build with `-Wno-unused-*`). This is the
   first end-to-end compile of the codegen output (the audit only checks
   the `not_implemented` flag). The ARMv5TE emitters are separately
   compile-checked via `armv5te_decode_test <out.c>`.

## Phase 1 status: the static ARM core is complete

Both BIOSes: discovery clean, zero codegen gaps, banks emit + compile.
The remaining Phase-1-shaped items are gated on Phase 2 (the `runner/`):
CP15 runtime model, the interpreter-oracle ARMv5TE ops, and per-CPU
dispatch wiring. The firmware is **not statically auditable** — its boot
streams are compressed/encrypted and decoded at runtime by the
recompiled ARM7 BIOS (LLE), so `fw_arm9`/`fw_arm7` banks come from
runtime dirty-RAM promotion (Phase 2/3), not a static pass.

> Not yet done (deferred, low priority): THUMB-state BLX (the `11101`
> BL-suffix form) — the ARM9 THUMB BIOS audit shows no gaps, so this is
> only needed if firmware uses it; flagged for the firmware audit pass.
> The interpreter oracle (`interpreter.cpp`, not compiled into the tool)
> still needs these ARMv5TE ops for Phase-2 state-diffing; its `default`
> case aborts on them today.

## Note on the audit walk

Per-function linear decode stops at the first unconditional terminator,
so it samples bodies rather than fully covering them (literal pools after
terminators are skipped). Good enough to surface the gap classes; full
coverage comes with the real CFG-following emit pass.
