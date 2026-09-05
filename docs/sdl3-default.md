# SDL3 default frontend

## Goal

`nds_runner` uses SDL3 as the default interactive host frontend. SDL2 remains
available as an explicit compatibility backend, and headless builds remain
available for non-interactive tooling.

## Build contract

- `NDS_SDL_BACKEND=SDL3` is the default and requires `SDL3::SDL3`.
- `NDS_SDL_BACKEND=SDL2` selects the compatibility backend and requires
  `SDL2::SDL2`.
- `NDS_SDL_BACKEND=NONE` builds without the SDL presentation layer.
- `NDS_ENABLE_COMPUTE_RENDERER=ON` requires `NDS_SDL_BACKEND=SDL3` or `SDL2`
  because the compute renderer owns an SDL-created GL context.

## Implementation notes

The compatibility layer is local to `runner/src/frontend.cpp` and
`runner/src/melonds_compute/ComputeHost.cpp`. The public runner behavior stays
the same: one stacked DS display window, keyboard/gamepad input, mouse/touch
mapping, optional audio, and the existing software/compute renderer switch.

SDL3 uses `SDL_AudioStream` through `SDL_OpenAudioDeviceStream`. SDL2 keeps the
existing callback-backed ring buffer. SDL3 exclusive fullscreen currently maps
to SDL3's fullscreen window state; display-mode selection can be added later if
a title needs true mode switching.

Two host-observable behaviours must survive the backend switch because the
release gates assert on them, so they are ported explicitly rather than left
to the compatibility macros:

- **Audio underruns.** SDL2's mixer callback counts an underrun whenever it
  can serve less than the device requested. SDL3 has no such callback, so
  `SDL_SetAudioStreamGetCallback` carries the same fact: its
  `additional_amount` is the shortfall between what the device is pulling and
  what is queued, and a nonzero value while playback is running is an
  underrun. Without this, `underruns` would be permanently zero under SDL3 and
  `NDS_FRONTEND_REQUIRE_AUDIO=1` would assert nothing. A device whose get
  callback cannot be installed is refused rather than opened blind.
- **Display refresh rate**, the frame-interpolation activation gate
  (`docs/frame_interpolation.md`). `window_refresh_hz()` wraps SDL2's
  `SDL_GetWindowDisplayIndex` + `SDL_GetCurrentDisplayMode(index, &mode)` and
  SDL3's `SDL_GetDisplayForWindow` + `SDL_GetCurrentDisplayMode(id)`. SDL3
  reports `refresh_rate` as a float (e.g. 164.998 on a 165 Hz panel), so it is
  rounded to whole Hz to keep the `>= 100 Hz` comparison behaving identically.

- **Render driver.** `create_renderer()` asks for `"direct3d"` (D3D9) rather
  than taking SDL3's default. SDL3 orders its Windows render drivers
  direct3d11 first; SDL2 ordered direct3d first, and every release through
  v0.5.2 was tuned against that. Measured over 600 presented frames of the
  direct-boot soak, time inside `SDL_RenderPresent`:

  | backend | driver | ms/frame |
  |---|---|---|
  | SDL2 | direct3d (D3D9) | 0.25 |
  | SDL3 | direct3d (D3D9) | 0.29 |
  | SDL3 | direct3d11 (SDL3 default) | 3.70 |
  | SDL3 | direct3d12 | 4.40 |

  It is not vsync — `SDL_RENDER_VSYNC=0` and `=1` measure the same. The
  frontend's pacing clock is the audio queue, so those milliseconds come
  straight out of the frame budget: on D3D11 a 2,400-frame soak underran
  (81 underruns), and on D3D9 the same soak completes clean. The selection
  falls through to SDL3's own choice wherever `direct3d` does not exist
  (Linux, macOS) and then to software, logs the driver it got, and honours
  `NDS_SDL_RENDER_DRIVER` for diagnosis.

Also note that `close_audio()` clears the handle it is given on both backends,
so "was a device ever opened" must be latched before the close — the
`audio_failed` verdict reads that latch, not the closed handle.

Frame interpolation itself is otherwise backend-agnostic: it composes ARGB
pixels in host memory and re-enters `present_screens()`, which is already
routed through the `render_texture()` / `set_render_logical_size()` shims.

## Verified commands

```powershell
& C:\msys64\mingw64\bin\cmake.exe -G Ninja `
  -S F:\Projects\ndsrecomp\worktrees\ndsrecomp-sdl3-default\runner `
  -B F:\Projects\ndsrecomp\worktrees\ndsrecomp-sdl3-default\runner\build-sdl3-default `
  -DCMAKE_BUILD_TYPE=Release `
  -DNDS_ENABLE_COMPUTE_RENDERER=ON `
  -DNDS_BOOTSTRAP_FIRMWARE=ON `
  -DNDS_GENERATED_DIR=F:\Projects\ndsrecomp\ndsrecomp\generated `
  -DCMAKE_PREFIX_PATH=C:\msys64\mingw64\lib\cmake
& C:\msys64\mingw64\bin\cmake.exe --build `
  F:\Projects\ndsrecomp\worktrees\ndsrecomp-sdl3-default\runner\build-sdl3-default `
  --target nds_runner -j 12
```

```powershell
& C:\msys64\mingw64\bin\cmake.exe -G Ninja `
  -S F:\Projects\ndsrecomp\worktrees\ndsrecomp-sdl3-default\runner `
  -B F:\Projects\ndsrecomp\worktrees\ndsrecomp-sdl3-default\runner\build-sdl2-fallback `
  -DCMAKE_BUILD_TYPE=Release `
  -DNDS_SDL_BACKEND=SDL2 `
  -DNDS_ENABLE_COMPUTE_RENDERER=ON `
  -DNDS_BOOTSTRAP_FIRMWARE=ON `
  -DNDS_GENERATED_DIR=F:\Projects\ndsrecomp\ndsrecomp\generated `
  -DCMAKE_PREFIX_PATH=C:\msys64\mingw64\lib\cmake
& C:\msys64\mingw64\bin\cmake.exe --build `
  F:\Projects\ndsrecomp\worktrees\ndsrecomp-sdl3-default\runner\build-sdl2-fallback `
  --target nds_runner -j 12
```
