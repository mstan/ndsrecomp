# Live Overlay Safe Execution Validation

Date: 2026-08-17

Core worktree: `ndsrecomp-live-overlay-provider`

Title worktree: `metroidprimehuntersrecomp-live-overlay-provider`

## Final Correctness Model

- Live bank ABI v4 carries an exact candidate identity and an optional complete
  dependency closure. A closure covers every native body that direct generated
  transfers can reach.
- Candidate lookup retains multiple implementations at the same guest address
  and ARM/Thumb state. It searches newest first and selects by current live
  bytes and write provenance. Returning to an older byte generation reuses its
  retained candidate.
- A dependency-closure bank validates all of its exact ranges atomically before
  entering native code. Writes revalidate the active closure before native
  execution continues.
- Direct generated transfers are enabled only inside one validated closure.
  Cross-candidate and cross-page transfers return through runtime dispatch.
- Coalesced functions enter their emitted owner body with the exact guest PC in
  R15. The owner resume switch selects the real instruction label; codegen no
  longer references suppressed interior host symbols.
- A missing generated resume entry enters Tier 3 only for mapped written RAM.
  Clean RAM, MMIO, ROM, and invalid targets such as `0x90900004` remain fatal.
- Cache publication uses a stage DLL plus atomic rename. A worker preflights
  complete banks and the emulation thread publishes one complete candidate at
  a time.
- Loader preflight rejects bad ABI, ROM, CPU, data imports, flags, ranges,
  ordering, ownership, and conflicting duplicate rows within a candidate.
  Duplicate address/state keys across distinct candidates remain valid.
- The persistent content identity includes the compiler, runtime ABI header,
  GCC identity, optimization level, function limit, page bytes, and entry set.
- Live capture is delayed for 90 seconds in the MPH launcher. Intro FMV data is
  not admitted as executable code and the intro remains at full speed.

## Deterministic Fault Matrix

Before dependency closures were enabled, the same checkpoint was exercised in
four ABI-v3 diagnostic variants. All four loaded and ran without the former
`0x90900004` import-thunk failure.

| Variant | Inter-function edges | Rows | Native hits | ARM9 Tier 3 |
| --- | --- | ---: | ---: | ---: |
| A | forced dispatch, sparse | 54 | 76,046 | 3,576,614,446 |
| B | forced dispatch, full | 27,980 | 61,335,585 | 3,280,471,471 |
| C | unsafe direct, sparse | 54 | 76,046 | 3,576,604,851 |
| D | unsafe direct, full | 27,980 | 36,900,356 | 3,280,471,471 |

The no-live baseline recorded 3,576,978,303 ARM9 Tier 3 instructions. The
matrix isolated sparse admission from transfer shape, while the earlier crash
was independently traced to MinGW data-import binding and fixed by ABI import
guards.

## Cold-to-Native MPH Evidence

The final validation preserved all prior caches, moved the existing ABI-v4
cache aside, and launched MPH through recomp-ui with an empty cache.

- Cold UI boot loaded zero live banks, rendered the intro instead of a white
  screen, and settled at 59.3-60.4 FPS before live activation.
- At 90 seconds, Tier 3 observations automatically scheduled background
  compilation. The first run published six ARM7 candidates, which accumulated
  native hits in the same process.
- TCP input and screenshots navigated from the main menu through Multi-Card,
  Create Game, Battle, arena selection, three bot confirmations, Start Game,
  and live four-hunter combat.
- Cold routing completed nine compiler runs with zero failures. Candidate
  publication grew from 0 to 54 banks without a rejected bank, crash, halt,
  white screen, or dispatch miss.
- After the first combat encounter, 18 newly compiled candidates were present.
  Replaying deterministic combat input in the same process gave native hits to
  15 of them. Examples:

| Candidate page | Candidate id | Native hits added on replay |
| --- | --- | ---: |
| `0x02080000` | `9b08a10de9c3c5df252a` | 2,004,091 |
| `0x02088000` | `eb1f622f5a21f6c2421b` | 1,241,472 |
| `0x02087000` | `6f8906e604ef07d0b3f1` | 654,749 |
| `0x02059000` | `d3943ad4ab6ce8e1c5db` | 590,360 |
| `0x02046000` | `6f94a9091c20322136a7` | 268,731 |
| `0x01FF8000` | `2054abd238197398e17b` | 159,796 |

The deterministic 60-action replay presented 871 frames in 14.562 seconds,
or 59.81 FPS. Aggregate Tier 3 continued to increase because combat reached
other cold pages; the per-candidate native deltas above are the direct proof
that compiled generations took over in the same process.

## Persistence and Shutdown

- A normal `WM_CLOSE` preserved the cache index and firmware profile. Relaunch
  loaded 60 cached candidates before the 90-second activation point, started
  zero compiler jobs, rejected zero banks, and accumulated 3.60 million native
  hits immediately.
- The final debug-pump shutdown test held a TCP client open while posting
  `WM_CLOSE` to the exact runner PID. The runner exited in 0.302 seconds after
  the listener wake-up fix.
- The retail firmware profile SHA-256 remained
  `00bfe2d81892dcd32975686d0c62a4808a6bf313170ac8e4856193fff616deff`.
- No final run created or grew `dispatch_misses.log`.

## Automated and Regression Tests

- Recompiler CTest: 3/3 passed.
- Runner CTest: 14/14 passed.
- MPH launcher CTest: 1/1 passed.
- Synthetic live-cache lifecycle: generation A, B, A reuse, expanded entry
  revision, O0/O2 provider split, atomic stage publication, and malformed
  manifest rejection passed.
- MPH, SM64DS, and MKDS each passed a 6,000-VBlank attract plus 12-step TCP
  input-fuzz smoke with lit screenshots and no dispatch-miss growth.

Primary ignored QA artifacts are under the MPH worktree's `generated/` tree:

- `abi4-cold-bot-route/`
- `abi4-cold-combat-replay/`
- `abi4-combat-fuzz-warm/`
- `live-provider-final-smoke/`
