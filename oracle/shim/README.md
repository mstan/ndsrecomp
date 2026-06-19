# oracle/shim/ — the `nds_oracle` melonDS reference shim

Headless melonDS frontend that boots the **real** BIOS + firmware to the menu
and serves the `../../TCP.md` debug protocol on `127.0.0.1:19843`, so the diff
harness (`../diff_*.py`, `../find_first_diverge.py`) can sync the native
runtime against melonDS on **hardware events** (never frame indices).

melonDS is GPLv3 and stays a **separate binary** — never linked into the native
runner. This is tool-use, not distribution of melonDS.

## Files

- `platform_oracle.cpp` — headless `melonDS::Platform::*` backing (stdio file
  I/O, `std` threads/mutex/semaphore, time; Net/MP/Camera/Addon/save-writeback
  stubbed). The core ships no Platform impl; every frontend provides its own.
- `oracle_hooks.h` — the always-on hardware-event counters (`OracleCounters`)
  that back `event_counts` / `run_to_event`.
- `nds_oracle.cpp` — *(in progress)* arg parsing, dump loading, the `OracleNDS`
  subclass (IO-write overrides counting IPCSYNC/FIFO traffic), the TCP server,
  and the command dispatch.

## Build wiring (in progress)

These sources are tracked here, not buried in the gitignored melonDS clone.
The plan: `setup-melonds.sh` stages them into the clone (or an out-of-tree
CMake links `core`), gated by `-DBUILD_ORACLE_SHIM=ON`. The only edits to
melonDS itself — the `NDS::SetIRQ` event-counter hook and the root-CMake option
— ship as `../patches/*.patch`. See `../patches/README.md` for the verified
melonDS `1.0rc` hook points.

## Status

WIP. `platform_oracle.cpp` + `oracle_hooks.h` complete; `nds_oracle.cpp`, the
CMake target, and the melonDS patches are not yet finished. Nothing here is
wired into a build yet, so it is inert — safe to commit as work-in-progress.
