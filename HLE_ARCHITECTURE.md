# Performance HLE Architecture

## Contract

The recompiled/LLE implementation is the accuracy floor and remains linked,
runnable, and forceable. Performance HLE is a set of measured replacements
above that floor. A replacement may become the default path after promotion;
it never deletes its LLE body or changes a miss into a Tier-3 interpreter call.

The process follows the emotion-evolved model:

1. Profile a representative workload and name the cost being replaced.
2. Add a narrow seam while retaining the original implementation as the
   fallback and differential oracle.
3. Start the replacement off by default. Force it on and prove that it is hit.
4. Run same-input differential and mixed HLE/LLE sequences. In verify mode the
   LLE result is authoritative.
5. Run whole-workload correctness gates and interleaved same-binary A/B.
6. Reject a correct replacement if its real gain is noise or moves cost
   elsewhere.
7. Promote a winner independently. Keep a master force-floor control and loud
   miss/fallback diagnostics.

Parity-safe replacements require byte-identical guest-visible state. An
accuracy-affecting replacement additionally requires a per-item error contract,
measured divergences, and explicit user approval before default-on promotion.

## Policy surface

The common vocabulary is:

- `off`: use the faithful floor.
- `on`: use promoted handlers and fall back to LLE for unsupported inputs.
- `force`: testing mode; a configured candidate miss or fallback is fatal.
- `verify`: execute both from the same input, retain the LLE result, and report
  or fail on a contract violation.
- `auto`: select a promoted backend when its host requirements are present;
  otherwise use the faithful backend.

The intended controls are `NDS_HLE=off|on|verify` as a master policy,
`NDS_HLE_MATH=off|on|force|verify` for title CPU routines, and
`NDS_3D_RENDERER=auto|soft|compute|verify` for rendering. Startup output must
print the effective policy. Existing parity-safe host optimizations remain
separate because they do not replace guest semantics.

## CPU/title routine seam

A hook only in `runtime_dispatch` is insufficient: immutable generated banks
may call one another directly, while validated RAM banks can contain multiple
overlay generations at the same CPU address. A PC-only table can therefore
miss calls or select the wrong routine.

Configured candidates instead receive a generated wrapper:

```c
static void title_routine_lle(void) {
    /* the existing generated body, unchanged */
}

void title_routine(void) {
    if (g_cpu.R[15] == ROUTINE_START &&
        runtime_hle_try(&title_routine_descriptor, title_routine_lle))
        return;
    title_routine_lle();
}
```

Both dispatch entries and direct generated calls target the wrapper. Interior-PC
resume dispatches enter the retained LLE body. For content-validated banks, the
existing byte/provenance guard still runs before the wrapper. The wrapper symbol
and descriptor identify the exact bank generation; the selector is not keyed by
bare PC.

Only configured candidates pay selector overhead. A per-title manifest owns
candidate address, CPU/mode, bank identity, handler symbol, accuracy tier,
register and memory footprint, cycle/interrupt contract, and comparison policy.
The framework owns wrapper emission, selection, controls, counters, and the
diagnostic ring. Generated C is never edited by hand.

### Differential execution

Simple verification is limited to pure or bounded-memory routines:

1. Capture declared registers, CPSR, cycles, and bounded input memory.
2. Run HLE and save its declared output.
3. Restore the input.
4. Run the retained recompiled body.
5. Keep the LLE state visible and compare the complete declared contract.

I/O, DMA, callbacks, or unbounded writes require a purpose-built isolation
adapter. Whole-routine cycle charging is not automatically exact: making a
routine atomic can move an IRQ boundary, so timing and interruptibility are part
of every candidate's accuracy tier.

The first CPU candidate will be selected only from content-qualified dynamic
data. Castle overlays replace the original game's named math functions at the
same addresses, so the existing PC-only heat spike is invalid. Profiling must
attribute calls and inclusive host time to the selected static entry/bank
generation. If hot, a fixed-point matrix/vector routine is the preferred first
shape; floating-point division, square root, length, and normalization follow
only when their whole-frame value justifies their larger error surface.

## Renderer seam

Renderer HLE uses melonDS's existing `Renderer3D` backend interface, not the CPU
routine registry. `SoftRenderer` is the faithful forceable floor.
`ComputeRenderer` is the accelerated replacement and must include the runner's
flat-VRAM dirty tracking, GL 4.3 context lifecycle, and synchronized
capture/readback needed by CPU-side GPU2D `GetLine` consumers.

`soft` always selects the floor. Explicit `compute` fails loudly if unavailable.
`auto` may fall back to threaded soft and report why. Renderer verify mode must
render the same latched input through both backends, keep soft output visible,
and compare both framebuffers plus capture/readback-visible state.

This is also the scalable backend for later widescreen, supersampling, and AA;
those enhancements remain separately controlled and are not implied by choosing
the compute performance backend at native DS resolution.

## Diagnostics and promotion gates

Each domain exposes attempts, hits, fallbacks by reason, mismatches, maximum
error, LLE/HLE cycle counts, and attributed host time. A bounded always-on ring
records candidate/bank identity, CPU/mode/PC, input signature, decision, compare
result, and instruction/cycle/VBlank anchors. Query it after the fact; do not
arm-then-capture.

Promotion requires:

- handler differential tests over edge cases, randomized inputs, and pointer
  aliasing where applicable;
- forced-on G3 byte lock for exact replacements, then G1 and G2;
- accuracy/error and long-gameplay validation for approximate replacements;
- interleaved castle A/B min-of-N with hit count and attributed time; and
- a material whole-workload win, not merely a faster microbenchmark.

The master force-floor mode remains available after promotion. Any HLE miss
executes the retained recompiled LLE body, never the dirty-RAM interpreter.
