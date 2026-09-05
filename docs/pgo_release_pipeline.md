# PGO release pipeline (knob C2)

Profile-guided optimization of the runner, trained on scripted scenario
routes. Opt-in, off by default; with it off nothing about the build changes.

The runner's dispatch path is indirect-branch and misprediction heavy and the
shipped binary is large, so block layout and branch probability are exactly
what PGO has to work with. Size-for-speed is authorized for this tree.

## What carries the profile, and what does not

PGO applies to the **runner core only** — the `nds_runner` target's own host
translation units. Generated ARM bank TUs (`nds_banks`), the portable ARM core
(`nds_armv4t`), `nds_support` and `slirp` are never instrumented.

This is a deliberate scope decision, not a shortcut:

- **The build topology makes it nearly free.** The PGO flags are `PRIVATE` to
  `nds_runner`, so switching `NDS_PGO_MODE` inside an existing build directory
  recompiles only runner-core objects. Every generated bank object — the
  overwhelming majority of the build, hundreds of translation units for a
  title like Prime Hunters — is compiled once and reused unchanged by the
  instrumented and the optimized pass alike. Whole-build PGO would instead
  multiply the expensive half of the build by three.
- **Bank TUs are the wrong target.** They are machine-generated straight-line
  translations of guest code whose shape is dictated by the emitter. What PGO
  reorders there is largely already determined, while the profile data volume
  would be one `.gcda` per bank TU.
- **The hot host code is in the runner core.** Dispatch, the Tier-3
  interpreter, the bus, and the scheduler all live in `nds_runner`.
- **Precedent at this scale is bad.** Whole-runner GCC LTO produced a 663 MB
  archive and no linked binary (see `host_optimization_strategy.md`). Bank-wide
  PGO is the same class of scaling risk and buys the least.

## mingw-gcc constraints (verified empirically, gcc 15.2.0)

These are not cautionary notes; each one silently produces a wrong or
untrained binary if ignored.

- **`-fprofile-dir` does not work.** It mangles the object directory into a
  nested path under the profile directory (`prof/C~/Users/.../objdir/x.gcda`).
  At this tree's path depth GCC cannot create it and reports
  `Cannot create directory ... Skip`, writing **no profile at all** while
  still exiting 0. The pipeline therefore does not use it: profiles stay
  beside the object files, which makes the build directory the profile store.
- **All three passes must share one build directory.** This follows from the
  above. It is also the cheap path, since bank objects are then never rebuilt.
- **Profile names track object file names.** GCC derives `x.gcda` from the
  object being produced, so the passes must emit identically named objects.
  Reconfiguring a single build directory satisfies this automatically.
- **`-fprofile-use` with no profile data is not an error.** GCC warns under
  `-Wmissing-profile` and exits 0, producing an unoptimized binary that the
  release would nonetheless label PGO. `NDS_PGO_REQUIRE_PROFILE` (default ON)
  fails configuration instead.
- **`-fprofile-update=atomic` is required.** The runner updates counters from
  the emulation, audio and presentation threads at once; non-atomic counters
  lose and corrupt updates.
- **`-fprofile-correction` is required.** Even with atomic counters, mingw
  profile output across those threads is inconsistent often enough that GCC
  would otherwise reject the profile outright.
- **Terminating the process discards the profile.** GCC flushes counters at
  normal exit only. The runner must be shut down through its `frontend_exit`
  debug command; killing the PID trains nothing. The scenario harness already
  does this on every successful repetition.
- **A compiler cache must be off.** `NDSRECOMP_COMPILER_CACHE=OFF` is enforced:
  a release artifact must not depend on a cache's handling of profile inputs.

## Knobs

| Variable | Default | Meaning |
| --- | --- | --- |
| `NDS_PGO_MODE` | `OFF` | `OFF`, `GENERATE` (instrument) or `USE` (optimize) |
| `NDS_PGO_REQUIRE_PROFILE` | `ON` | Fail configuration if `USE` finds no `.gcda` |
| `NDS_PGO_PARTIAL_TRAINING` | `ON` | Keep untrained functions on normal optimization |

`NDS_PGO_PARTIAL_TRAINING` matters because the training routes are narrow. By
default `-fprofile-use` treats every function the training never executed as
cold and size-optimizes it, which would pessimize multiplayer, Wi-Fi and the
settings menus in exchange for optimizing the one route that was measured.
`-fprofile-partial-training` leaves those functions on normal optimization.

## Sequence

A title repo drives this with one flag. For Prime Hunters:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  tools\build-windows.ps1 -Version 0.6.3 -Pgo
```

That expands to the three passes below, all against one runner build
directory. Any title can follow the same shape.

```powershell
# 1. Instrument. Same arguments as the normal runner configure, plus the two
#    PGO arguments.
cmake -G Ninja -S <framework>\runner -B <runnerBuild> `
  -DCMAKE_BUILD_TYPE=Release -DNDS_SDL_BACKEND=SDL3 `
  -DNDS_BOOTSTRAP_FIRMWARE=ON `
  -DNDS_TITLE_BANK_DIR=<generated\recomp> -DNDS_TITLE_ROM_SHA1=<sha1> `
  -DNDSRECOMP_COMPILER_CACHE=OFF -DNDS_PGO_MODE=GENERATE
cmake --build <runnerBuild> -j <jobs>

# 2. Train on the scripted routes. No human input. Clears stale profiles,
#    refuses to run against a non-instrumented build, and fails if the routes
#    produced no profile data.
powershell -File tools\pgo_train.ps1 -RunnerBuildDir <runnerBuild>

# 3. Optimize. Only runner-core TUs recompile; bank objects are reused.
cmake -G Ninja -S <framework>\runner -B <runnerBuild> `
  ...same arguments... -DNDS_PGO_MODE=USE
cmake --build <runnerBuild> -j <jobs>
```

The profiles are artifacts of step 2. They are never committed; a release
builder reruns the sequence and regenerates them.

## Measured result (2026-08-27, MPH, i9-9900K / Windows 10 19045)

Framework `wt/pgo-pipeline` from `cddba78`, title `wt/pgo-pipeline` from
`dc955a6`. Baseline and candidate are the same source in the same build
directory, differing only in `NDS_PGO_MODE`. Interleaved A/B, five legs per
side, sides alternating and the leading side swapped each round; minimum
across legs per phase, with the median shown to expose noise. Emulation
milliseconds per frame. Negative is faster.

| route | phase | baseline | PGO | delta |
| --- | --- | --- | --- | --- |
| adventure | settle | 5.234 | 4.923 | **-5.95%** |
| adventure | walk | 5.734 | 5.345 | **-6.79%** |
| adventure | steady | 7.109 | 6.626 | **-6.80%** |
| attract | 0600-1200 | 5.132 | 4.986 | -2.84% |
| attract | 1200-1800 | 5.281 | 5.091 | -3.59% |
| attract | 1800-2400 | 5.607 | 5.448 | -2.83% |
| attract | 2400-3000 | 8.772 | 7.925 | **-9.66%** |
| attract | 3000-3600 | 9.502 | 8.532 | **-10.21%** |
| attract | 3600-4200 | 8.816 | 8.106 | **-8.06%** |

Mean -6.51% on adventure, -6.00% on attract: a real win at the low end of the
usual PGO range. The largest gains are the FMV-heavy attract windows, which
also dominate the training profile. Binary size fell slightly, from
211,206,805 to 211,127,468 bytes, so this is not a size-for-speed trade in
practice.

Both routes' medians track their minimums closely, which is the evidence that
the host was quiet; earlier attempts on a contaminated host produced +16.8%
and -2.8% on the same binaries and were discarded. Check for foreign
compiler and runner processes before and after any leg, and treat a large
median-to-minimum spread as contamination rather than as a result.

Gates on the same pair of binaries: `ctest` 18/18 on both; guest state
byte-identical at all seven vblank checkpoints (both register files, mode
registers, every event counter, zero differing pixels on both screens);
release packaging staged the PGO runner (SHA `47cb4098...`) and produced a
0.6.3 ZIP; with the knob off the generated build graph is byte-identical to
the pristine framework commit.

## Training workload

Scripted and deterministic, driven by `tools/measure_mph_scenario.py` over the
debug TCP surface with fixed touch/key/wait action lists and fixed guest
landmarks:

- **`attract`** — boot, title, and the attract-mode FMV windows, measured
  against absolute guest-VBlank landmarks. Covers boot, the BIOS banks, the
  2D/3D presentation path and FMV decode.
- **`adventure`** — the 13-step scripted route that creates a save, skips the
  briefing, lands, and enters Celestial Archives, then holds a movement key
  through three instruction-anchored gameplay phases. Covers the steady-state
  gameplay dispatch path, which is what the release is optimized for.

### Firmware scenario: not applicable to this title's release runner

The intent was to include a firmware route in training. It cannot apply to the
Prime Hunters release runner, for two independent reasons:

1. The release runner is configured `NDS_BOOTSTRAP_FIRMWARE=ON`, which
   compiles **no** firmware banks into the binary at all.
2. No firmware banks (`generated/fw_*.c`) exist in this workspace's framework
   trees to compile even if it were configured otherwise, and `--boot lle` is
   documented as hanging on the available MPH builds.

Since profiles are per-build-directory, a firmware route run against some
other binary could not contribute to this one's profile regardless. The
pipeline itself is route-agnostic — `pgo_train.ps1 -Routes` takes any route
list — so a firmware-capable runner build trains on a firmware scenario with
no changes here. For this title's release path it is genuinely out of scope.
