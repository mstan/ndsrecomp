# Frame Interpolation Plan

This plan covers ndsrecomp-level frame interpolation. It intentionally does not
uncap emulation or run game logic above the DS frame cadence.

## Goal

Add an opt-in display feature that can present additional host frames between
completed DS frames while the scheduler, input sampling, audio, timers, and game
logic remain locked to one DS frame per `kSystemCyclesPerFrame`.

This is the only safe generic approach for ndsrecomp. Raising the emulated frame
rate would change guest-visible timing and is likely to break games.

## Current Frontend Shape

`runner/src/frontend.cpp` currently:

- runs the scheduler until the next DS frame boundary,
- selects native or adaptive top/bottom framebuffers,
- calls `present_screens(...)` once,
- uses `drain_audio(...)` as the real-time pacing guard.

That means interpolation belongs after a completed DS frame has been produced
and before the next scheduler step begins. It should use the same audio pacing
window that already prevents the frontend from running faster than real time.

## MVP: Temporal Blend

The practical first implementation is a conservative `blend` mode:

1. Keep the previous and current post-compositor framebuffers for each screen.
2. After the current DS frame is complete, render one or more synthetic display
   frames that blend previous/current pixels.
3. Present the real current frame before the next emulated frame begins.
4. Never call `scheduler_run_round()` for synthetic frames.

This can be implemented generically for the SDL texture path because it already
uploads CPU-side ARGB framebuffers for both screens.

The MVP should be default-off and exposed as something like:

```text
--frame-interpolation off|blend
NDS_FRAME_INTERPOLATION=off|blend
```

Suggested user-facing name: "Frame interpolation (experimental)".

## Why This Is Not True Motion Interpolation

A finished NDS framebuffer does not contain motion vectors, object transforms,
camera matrices, depth, or enough layer ownership to reconstruct clean in-between
motion in a general way. A temporal blend can smooth perceived motion on high
refresh displays, but it will ghost fast movement and HUD elements.

Real motion interpolation would need renderer-side support, likely:

- 3D color plus depth or motion metadata from the accelerated renderer,
- separate treatment for 2D/HUD layers,
- title-specific handling for games with custom projection or widescreen hacks.

That is substantially larger than a generic frontend option.

## Compute Renderer Constraint

The direct OpenGL compute path is currently harder than the SDL texture path.
`present_screens(...)` calls `nds_compute_host_present_top(...)`, and that helper
draws/sends the top screen directly to the GL window, including
`SDL_GL_SwapWindow(...)`.

For a first patch, interpolation should be disabled when `presentation.gl_top`
is active, with a one-time log message explaining that the direct compute
presenter needs an offscreen output texture before it can be blended.

A later compute-compatible implementation should refactor the presenter so the
top screen renders into a retained texture first, then the frontend chooses
whether to present current, previous/current blend, or current-only.

## Pacing

The frontend should not busy-loop synthetic frames. It should:

- query the display refresh rate where SDL reports one,
- only attempt extra presents when refresh is comfortably above 60 Hz,
- use the existing audio queue floor as the real-time budget,
- keep pumping SDL events between synthetic presents,
- stop immediately if the audio queue reaches or drops below the pacing floor.

For 120 Hz, one synthetic frame at alpha `0.5` is the useful first target. Higher
refresh rates can add evenly spaced blends only after the 120 Hz path is stable.

## Validation

Required validation before considering this shippable:

- Emulated FPS remains about 60 and game timers do not run faster.
- Audio pacing does not underrun more often with interpolation enabled.
- Input latency is understood and acceptable.
- Stacked and separate screen layouts both work.
- Adaptive widescreen and native 256-wide output both work.
- The option stays ignored or disabled for direct compute presentation until
  that path has an offscreen texture handoff.
- Multiplayer/WFC timing is unchanged, because no guest timing is modified.

## Recommended Phases

1. Add the option and parser (`off|blend`), default off.
2. Add a small frame cache for SDL-presented ARGB frames.
3. Add a renderer helper that can draw a blended texture pair without changing
   guest state.
4. Present at most one synthetic frame on high-refresh displays.
5. Add diagnostics counters for real frames vs synthetic frames.
6. Only then consider compute direct-present support by moving GL output through
   an offscreen texture.

