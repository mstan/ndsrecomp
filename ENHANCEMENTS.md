# Enhancements

Forward-looking work that is useful but not required for the current
accuracy floor. Performance experiments remain governed by
`docs/host_optimization_strategy.md`: quiet-host interleaved A/B, a 5%
retention gate for added complexity, and full correctness gates for every
retained semantic path.

## Performance and headroom

Acceptance mode is detached top/bottom windows with SM64DS adaptive-top
widescreen enabled. The target is sustained 60 FPS with zero audio underruns
and at most 12.8 ms of emulation time per presented frame; 8.3 ms remains the
aspiration.

### 1. Scoped runner-only PGO

The build now exposes `NDS_PGO_MODE=OFF|GENERATE|USE` for GCC/MinGW.
Instrumentation and profile use apply only to the `nds_runner` target:
generated ARM banks and the portable support libraries remain baseline
objects. Generate mode uses GCC's native object-adjacent profile paths and
atomic counter updates instead of the absolute Windows profile directory that
previously failed.

Completed:

- a Release `GENERATE` build with the SM64DS banks and compute renderer;
- a clean deterministic 1M-cycle proof exit;
- creation of 30 object-adjacent `.gcda` files totaling 197,768 bytes,
  including runtime, scheduler, frontend, GPU2D, GPU3D, and renderer bridge
  translation units.

Still required:

- train the complete firmware/new-file route and saved-File-A gameplay route
  on a quiet host, with both processes exiting normally so counters merge;
- rebuild the same tree with `NDS_PGO_MODE=USE` and audit missing-profile
  warnings;
- compare the profile-use executable against the exact baseline with quiet,
  interleaved A/B in detached/adaptive-top mode;
- reject it if it misses the 5% complexity gate; otherwise run G1/G2/G3 and
  the normal unit/build matrix before retention.

The first attempted full training leg was deliberately excluded: unrelated
PSX and GCN recompilations reduced its initial interval to 12.05 FPS, after
which the debug connection closed. It produced no candidate and no usable
performance claim.

This is the lowest semantic-risk remaining experiment, but a trained local
binary is not automatically a portable release solution.

### 2. GPU2D temporal reuse and dirty state

The post-superblock census measures GPU2D at 1.59--2.32 ms/frame. Existing
row batching and fast paths leave no bounded micro-optimization likely to
clear the retention gate. The next credible design is cached scanline/layer
reuse driven by precise dirty state.

Instrument and classify writes before caching anything:

- aliased VRAM banks and mapping changes;
- palette and OAM writes;
- display registers and affine reference state;
- display capture and 3D-backed lines;
- mid-frame writes that invalidate only later scanlines.

Retain the current renderer as the reference path and force both paths during
G1/G2/G3 validation.

### 3. Adaptive widescreen on GPU-composited 3D

The experimental melonDS ComputeRenderer now supports a 448x192 adaptive
target and an opt-in direct OpenGL top-window presenter. In supported SM64DS
gameplay it keeps 3D GPU-resident and uploads only packed OBJ/HUD metadata;
unsupported scenes and display capture retain the faithful CPU fallback.
Clean unprofiled evidence shows a 23.66% reduction in settled-gameplay
emulation time and clears the 12.8 ms phase target, but regresses the whole
shortened route by 1.23%, so software remains the default.

Promotion work still includes:

- forced DISPCAPCNT, screen-routing, RenderXPos, and transition tests;
- characterize soft/compute image differences and failure fallback;
- validate NVIDIA, AMD, and Intel behavior;
- unprofiled multi-pair ABBA in the exact detached/adaptive mode;
- clear the 5% full-route complexity gate before making OpenGL the default.

### 4. Slow-frame and host-scheduling tails

The retained D1 A/B reaches the 12.8 ms goal in castle/Yoshi, but a later
health run measured 14--15 ms in the same gameplay intervals. Add low-cost
frame-time percentiles and thread wait attribution to distinguish actual guest
work from soft-render-worker, audio, window-swap, and OS scheduling stalls.
Do not substitute process priority for genuine emulation headroom.

## Visual follow-ups

### SM64DS adaptive sky

Actor culling and the minimap are fixed, but the courtyard sky can still show
stretching or black voids at 21:9. Prior generic polygon widening was rejected.
Revisit with title/decomp-aware skybox geometry or draw identification rather
than another global renderer heuristic.
