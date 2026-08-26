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
