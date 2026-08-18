# Live Overlay PSX-vs-NDS Adversarial Review

Date: 2026-08-17

Scope:
- PSX reference: `F:\Projects\psxrecomp\ApeEscapeRecomp\psxrecomp-v4`
- NDS core: `F:\Projects\ndsrecomp\ndsrecomp-live-overlay-provider`
- MPH title: `F:\Projects\ndsrecomp\metroidprimehuntersrecomp-live-overlay-provider`

## Resolution

This document records the adversarial review that drove the implementation;
the "Current Failure" and "Missing" sections below are historical, not the
final state. The completed implementation now includes ABI-v4 exact dependency
closures, candidate chains, deterministic publication, write-generation
revalidation, owner-backed resume entries, dirty-RAM-only Tier 3 fallback,
content-addressed persistent caching, and same-session background publication.

The `0x90900004` failure was eliminated by ABI-checked DLL data imports. The
separate direct-call validation defect was fixed by allowing direct native
transfers only inside an atomically validated complete dependency closure.
Cold MPH UI, Multi-Card bot combat, same-session Tier 3-to-native handoff, warm
cache reuse, and all sibling smoke tests are recorded in
`docs/live-overlay-safe-execution-validation.md`.

## Current Failure

The NDS live overlay path can compile and load MPH overlay DLLs on a base-only
runner, but after native execution starts it later halts on an ARM9 dispatch miss
near `0x90900004` / `0x90900000`, with LR around `0x01FF8EA4`.

That target is not an MPH overlay address. It is a bogus control-flow target.
The likely failure happened earlier: live native execution corrupted a function
pointer or return/callback state, and the later dispatch miss is only the point
where the corruption became visible.

## Source-Checked Correction

After checking the implementation, the direct-call bypass is a real invariant
to preserve but it is not the current default live-DLL behavior.

`recompiler/src/main.cpp` passes `allow_direct_calls = !validate_live_bytes`.
`tools/live_overlay_compile.py` builds live MPH DLLs with
`--validate-live-bytes`, so current safe live builds route inter-function
`B/BL` edges back through runtime dispatch. Generated safe live sources confirm
this by emitting `runtime_dispatch_literal_call/branch`, not direct
`mph_arm9_live_...()` calls.

The stronger proven crash cause recorded in `beads-yjp.29` is MinGW data import
binding: the live DLL can bind exported runtime data such as `g_cpu` to an import
thunk instead of the runner's storage. That makes generated code read thunk
bytes as CPU state; `g_cpu.R[15]` becomes the `0x9090000N` padding signature and
the first native body falls into a fake dispatch miss. This explains the
observed `0x90900004` directly.

Implemented safeguards in this branch:
- `runtime_exports.def` marks exported runtime data symbols as `DATA`.
- `NdsLiveBankInfo` is ABI version 2 and reports the data addresses the DLL
  linked against.
- `live_overlay.cpp` rejects any live DLL whose reported `&g_cpu`,
  `&g_busf_main`, `&g_busf_itcm`, or `&g_runtime_cycles` does not match the
  runner's own storage.

The validation-boundary issue remains a design requirement for future candidate
chains: no live native body may execute unless that body's candidate identity
matched live memory, or the runtime atomically validated an explicit dependency
closure containing it.

## Validation-Boundary Finding

Relevant NDS code:
- `runner/src/runtime_arm.cpp`: `lookup_in()` validates only the matched
  `NdsDispatchEntry` bytes before returning its function.
- `runner/src/runtime_arm.cpp`: `StaticGuardScope` guards only that selected
  validation range during execution.
- `recompiler/armv4t/arm_codegen.cpp`: when direct calls are enabled and a
  branch/call target has a generated name in the current bank, codegen emits a
  direct C call instead of routing through `runtime_dispatch`.
- `tools/live_overlay_compile.py`: current live MPH builds may compile the full
  static overlay body but filter dispatch rows down to only seed entries.

That combination would be unsafe if direct calls were enabled for live-byte
banks. This branch keeps forced dispatch as the default and adds the explicit
diagnostic flag `--unsafe-live-direct-calls` / `--unsafe-direct-calls` to test
the hypothesis without shipping it.

## PSX Mechanisms Missing or Partial in NDS

PSX has several safety layers that NDS does not currently have:

1. Per-candidate identity:
   Each function candidate has an entry, exact code ranges, and a CRC over those
   ranges. Validation is per compiled function identity, not just per page or
   per DLL.

2. Candidate chains:
   The same guest address can have several candidate implementations from
   different overlay generations. Dispatch walks the chain and picks the one
   whose live bytes match.

3. Interior/alias ownership:
   A PC observed at runtime is treated as reachability evidence, not automatically
   as a function boundary. If it is inside another function, PSX either makes it
   an alias/resume point into a validated owner or rejects it to interpreter.

4. Fail-closed publication:
   A shard is published only after manifest validation, export validation, range
   validation, and capacity checks. Ambiguous or malformed entries do not become
   native.

5. Interpreter authority:
   PSX falls back to dirty-RAM interpreter for valid dirty RAM misses. It does
   not interpret unmapped/bogus targets.

6. Native-off / diff instrumentation:
   PSX can select a candidate but route it to interpreter, and it has shadow-diff
   tooling to compare native against interpreter from the same state.

## Dispatch Miss Answer

NDS should fall back to interpreter only when the target is valid executable RAM
that the guest actually wrote. The central NDS dispatch path already does this:
it tries native first, then checks write provenance and enters Tier 3.

It should not fall back for `0x90900004`. That address is not valid copied code.
Interpreting it would hide the real bug.

One caveat: generated function-local "bad entry" paths that call
`runtime_dispatch_miss()` directly may be too fatal for live overlay work. Those
should probably route through a fallback-aware helper that can reject the current
candidate and re-enter central dispatch/Tier 3 when the target is valid RAM.

## Adversarial Notes on the Sol Review

The Sol review's architectural concern is correct, but its leading explanation
was stale for this worktree. Safe live builds already disable direct
inter-function calls through the `--validate-live-bytes` path. The current
`0x90900004` failure should be treated as the import-thunk data binding bug
unless a newer run proves otherwise.

The direct-call bypass is still worth testing because `--unsafe-live-direct-calls`
now deliberately recreates that shape for an A/B run. A direct callee mismatch
would explain a bogus callback pointer, but the `0x90900004` miss itself is no
longer ambiguous: the thunk-byte signature is direct evidence of bad data import
binding.

The recommendation to "just keep all dispatch rows" is useful as a diagnostic,
but it is not sufficient as a correctness model. Full dispatch rows still need
per-candidate byte validation and safe ordering when multiple overlay
generations share addresses.

## Current Diagnostic Matrix

Use one deterministic checkpoint and four cache directories:

- safe/sparse: default live builder
- safe/full: add `--full-dispatch-rows`
- unsafe/sparse: add `--unsafe-direct-calls`
- unsafe/full: add both flags

Interpretation should account for the import-thunk fix first. If safe/sparse
still fails after ABI 2 import checks pass, then use the new
`live_overlay_diagnostics` TCP command to inspect:
- transfer records with source PC, target, LR, CPSR, and transfer kind
- live candidate records with validation range, provenance result, byte-match
  result, bank id, and publication generation
- writes around `0x027E0000..0x027E0040`, including old/new values

## Smallest Next Implementation Plan

1. Run the four matrix variants from the same checkpoint after the ABI 2
   import-binding fix.

2. Replace regex dispatch-row deletion with a real admission model:
   keep rows whose code identity is independently valid, make interior entries
   aliases into a validated owner, and reject ambiguous entries to Tier 3.

3. Add loader preflight:
   sorted dispatch table, duplicate `(addr, thumb)` rejection, validation range
   sanity, bundle identity, and deterministic newest-valid candidate preference.
   Do not globally reject duplicate `(addr, thumb)` across different published
   candidates.

4. Add tests:
   - root bytes match but direct callee bytes differ; stale callee must not run
   - mid-function seed accepted only with owner identity
   - valid written RAM miss enters Tier 3
   - clean/unmapped miss such as `0x90900004` remains fatal
   - duplicate/unsorted/malformed live table rejected

## External Claude/Fable Prompt

Use this prompt for another adversarial pass:

```text
Adversarial implementation review only. Do not edit files.

Compare PSX live overlay implementation against NDS live overlay provider.
Focus on actual code behavior, not docs.

Repos:
- PSX reference: F:\Projects\psxrecomp\psxrecomp
- NDS core: F:\Projects\ndsrecomp\ndsrecomp-live-overlay-provider
- MPH title: F:\Projects\ndsrecomp\metroidprimehuntersrecomp-live-overlay-provider

Known NDS state:
- NDS live provider compiles MPH overlay DLLs from coverage and loads them at runtime.
- Base-only MPH runner can compile/load ov000 and ov004 live DLLs.
- After live native execution starts, it crashes with ARM9 dispatch miss around
  0x90900004 / 0x90900000, LR 0x01FF8EA4.
- Static AOT for same overlays works.
- A sparse boundary bug was found: live seed configs generated different function
  ranges from static configs. The current MPH tool defaults to static boundaries
  where checked-in config exists, then filters dispatch rows to seed entries.
  That moved the crash later but did not fix it.
- User suspects this matches an old PSX issue: shards too narrow / bad
  boundaries, then fixed by widening shard ownership and making interior entries
  aliases or rejecting them.

Inspect implementation files, especially:
PSX:
- runtime/src/overlay_loader.c
- runtime/src/autocompile.c
- runtime/src/dirty_ram_interp.c
- runtime/include/overlay_api.h
- tools/compile_overlays.py
- runtime/tests/test_autocompile_publication.c
- runtime/tests/test_overlay_decodable_fallback.py
- runtime/tests/test_dirty_text_admission_guards.py
- recompiler/tests/test_aot_overlay_discovery.py

NDS:
- runner/src/live_overlay.cpp
- runner/src/runtime_arm.cpp
- runner/src/tier3.cpp
- recompiler/armv4t/arm_codegen.cpp
- recompiler/armv4t/runtime_arm.h
- runner/src/runtime_exports.def

MPH:
- tools/live_overlay_compile.py
- tools/seed_overlay_from_coverage.py

Deliver:
1. Most likely root causes, adversarially ordered.
2. Exact PSX mechanisms missing or only partially ported to NDS.
3. Whether NDS dispatch misses should fall back to interpreter, with caveats.
4. Concrete smallest next implementation steps/tests.
5. Call out any explanation from the main agent that seems wrong or overconfident.

Include file/line references where possible. Keep it practical.
```
