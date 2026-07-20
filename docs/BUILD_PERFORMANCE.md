# Build performance

Large generated Nintendo DS banks are intentionally split into ordinary C
translation units. Some titles and firmware captures still produce hundreds of
megabytes of generated source, so unconstrained Ninja builds can otherwise run
many memory-heavy compiler frontends at once.

## Default safeguards

Ninja builds use one shared compile pool with a default depth of four. Heavy
bank archives and runner links use a link pool with depth one. The limits apply
even when `cmake --build` is invoked with a larger `--parallel` value. They are
configurable at configure time:

```powershell
& C:\msys64\mingw64\bin\cmake.exe -S runner -B runner/build `
  -DNDSRECOMP_COMPILE_JOBS=4 -DNDSRECOMP_LINK_JOBS=1
```

The compiler cache policy is `AUTO` by default. It prefers `sccache`, then
`ccache`, without replacing a launcher supplied by a parent project or user:

```powershell
# Disable automatic caching
-DNDSRECOMP_COMPILER_CACHE=OFF

# Require a particular launcher
-DNDSRECOMP_COMPILER_CACHE=C:/msys64/mingw64/bin/ccache.exe
```

Per-title generation commands should use a successful-generation stamp as the
primary custom-command output and list generated sources as byproducts. This
keeps an unchanged generator dependency from rerunning on every build while
still regenerating a missing source.

## Initial measurements (2026-07-18)

- The pre-change SM64DS/framework corpus contained 466 generated C files and
  about 1.32 GiB of source; the largest body unit was 57.28 MiB.
- A full GCC cache-populating rebuild invoked with `--parallel 16` never
  exceeded four NDS `cc1.exe` workers. Peak observed compiler working set was
  about 3.15 GiB. The externally controlled run was stopped at its ten-minute
  harness limit, so it is not a completed build-time result.
- After successful stamps were established, an unchanged SM64DS generation
  target reported `ninja: no work to do` in 0.072 seconds.

Measure clean, unchanged, and one-shard incremental builds separately. Use the
same source snapshot, job depth, cache policy, and build type for compiler
comparisons.
