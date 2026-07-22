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
        runtime_hle_try(&title_routine_descriptor, title_routine_lle,
                        &title_routine_handler))
        return;
    title_routine_lle();
}
```

Both dispatch entries and direct generated calls target the wrapper. Interior-PC
resume dispatches enter the retained LLE body. For content-validated banks, the
existing byte/provenance guard still runs before the wrapper. The wrapper symbol
and descriptor identify the exact bank generation; the selector is not keyed by
bare PC.

Only configured candidates pay selector overhead. The version-1 manifest owns
candidate address/range, mode, bank/content identity, and an optional handler
symbol. Accuracy tier, register and memory footprint, cycle/interrupt contract,
and comparison policy remain handler-owned until the manifest schema can
represent and validate them without weakening the current fail-closed format.
The framework owns wrapper emission, selection, controls, counters, and the
diagnostic ring. Generated C is never edited by hand.

Candidate execution has two independent opt-ins. Configure with
`-DNDS_ENABLE_HLE=ON`, then enable the runtime master and math policies, for
example `NDS_HLE=on NDS_HLE_MATH=on`. A default build emits no candidate
wrapper. An HLE-capable build still starts with both policies off. `on` falls
back on a missing or unsupported handler; `verify` delegates same-input
differential execution to the handler adapter and requires it to leave the LLE
result authoritative. `force` is a testing-only miss detector and halts instead
of hiding an unsupported case. Startup and the debug server's `hle_status`
command expose effective policy, counters, and recent decisions.

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

The first CPU candidate will be selected from content-qualified dynamic data.
The current castle captures do contain the seven named math routines at their
expected addresses (the RAM-bank base is `0x01FF8000`); an earlier contrary
audit decoded them with an incorrect `0x02000000` base. Bare-PC heat is still
insufficient architecture: it cannot distinguish future bank generations and
misses immutable-bank direct calls. Profiling must attribute calls and inclusive
host time to the selected generated wrapper/bank identity. If hot, a fixed-point
matrix/vector routine is the preferred first shape; floating-point division,
square root, length, and normalization follow only when their whole-frame value
justifies their larger error surface.

The first measurement seam is intentionally narrower than a general call-tree
profiler. A committed title manifest may select only an exact, content-validated
whole function that the generator proves is a straight-line leaf with one
unconditional return. In profiling builds only, its public generated symbol
wraps the unchanged private LLE body. Every validated start/interior segment is
counted; one configurable phase out of every `2^N` segments is timed. Clean
normal and unwind segments are both accepted and counted separately; IRQ,
invalid-length, nested-entry, depth, or guard/PC contamination is reported and
excluded. Cumulative snapshots are subtracted around a stopped deterministic
route; there is no arm/reset capture command. Profiler-OFF preprocessing emits
the original public body directly, with object equivalence tested. This
produces an optimistic instrumented body-cost estimate, not a formal removal or
speedup bound. The sample clock and wrapper bookkeeping bias the numerator
upward, while profiler overhead and work outside the wrapper affect the
denominator and may bias the reported share in either direction. Multiple
dispersed phases or an all-segment census are required before triage;
uninstrumented HLE/LLE A/B remains the decision gate.

A general non-leaf profiler would need per-CPU logical guest frames across host
unwinds and descendant resumes. That machinery is deferred until a measured
candidate requires it; wrapper-local wall clocks alone are not accepted for
non-leaf inclusive attribution.

## Optimizer-facing effect metadata

The recompiler also has a conservative, codegen-independent instruction effect
classifier. It distinguishes memory reads/writes, direct and indirect control
flow, processor-state changes, exceptions, and undefined instructions, and can
identify arithmetic-only regions eligible for future local-state lowering.
This is analysis scaffolding only: it does not currently combine instructions,
move timing ticks, or change generated execution. Any future superblock pass
must separately prove scheduler, IRQ, debugger, and cycle-boundary safety before
executing a region atomically.

## Scheduler and serialization boundary

The faithful scheduler rendezvous is currently capped at 64 system cycles,
which gives ARM9 at most 128 cycles before ARM7 catch-up. This is a correctness
boundary, not merely a tuning constant: a wider unconditional lead has already
moved IPC handshakes across peer polling points. Consequently, a handler larger
than one slice cannot be made correct by charging all of its cycles at return.

Handlers declare a proven `minimum_atomic_cycles` for a cheap generic miss when
the current slice cannot possibly contain them. A handler that passes that
preflight still computes and checks its exact or proven-upper-bound cost before
committing state. This avoids doing an expensive semantic prediction followed
by the complete LLE body on the common short-slice rejection path.

Larger semantic regions require one of these stronger contracts:

- a proof that the peer CPU is unable to observe or interfere before the next
  system deadline, allowing a deadline-bounded scheduler lead;
- a resumable HLE continuation with guest-cycle checkpoints for visible writes,
  IRQ delivery, and final register state; or
- an effect transaction whose timestamped writes are committed while ARM7 and
  devices catch up on the ordinary scheduler timeline.

Each path must fall back before mutation when its proof is unavailable. Running
both host CPU threads speculatively, increasing the global rendezvous quantum,
or treating ARM7 as a vector coprocessor is not an accepted shortcut.

The scheduler also has an independent, parity-safe context-switch experiment.
`NDS_FAST_CPU_CONTEXT=1` selects resident per-CPU host call-return stacks rather
than copying the active stack at every rendezvous. It does not change guest CPU
state, timestamps, or ordering; the default copy path remains the same-binary
reference until deterministic endpoint and performance gates pass.

## SM64DS carve order

The first measured leaf, `MulVec3Mat4x3`, accounts for only about 0.6% of prior
castle wrapper heat. It is an ABI/correctness pilot, not a route to 60 FPS by
itself. The next candidates are ordered by expected inclusive work and contract
quality, pending fresh bank-qualified heat measurements:

1. Skeletal animation and bone batches: `ModelComponents::UpdateBones`, channel
   interpolation, and hierarchy matrix composition. These cover roughly
   1.3-2.0 KiB of reachable kernels repeated per bone and operate on bounded
   model/animation arrays without MMIO.
2. Collision query batches: ray, sphere, and ground tests over the registered
   collider set. These are likely castle-hot, but unknown virtual collider
   types must miss to LLE and verification must include touched result objects.
3. The exact fixed-point math cluster, beginning with `MulMat4x3Mat4x3`, then
   vector add/subtract/dot/cross. Division, square root, length, and normalize
   stay separate until DS arithmetic-unit timing/state is modeled.
4. Software OAM construction, bounded before the later hardware load step.
5. Model/GX command submission only after an ordered command-stream verifier
   exists; FIFO stalls and geometry timing make ordinary memory comparison
   insufficient.

Decompression remains useful for load latency but does not target steady-state
frame rate. ARM7/audio HLE requires bank-qualified profiling first; the DS ARM7
normally services audio and I/O rather than performing the title's matrix work.

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
