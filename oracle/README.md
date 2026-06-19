# oracle/ — melonDS-backed reference

The behavioral source of truth for ndsrecomp is **melonDS**, built as a
SEPARATE binary (`nds_oracle`) that embeds the melonDS core and exposes
the `TCP.md` command set on `127.0.0.1:19843`. The native ndsrecomp
build **never** links melonDS — it is GPL-3.0 and our hard rule is "zero
copyleft emulator dependencies in the native build." License isolation
mirrors how `gbarecomp/oracle` keeps libmgba out of the native binary.

melonDS is the right oracle because it runs the **real** ARM7/ARM9
BIOSes and the **real** firmware boot (the same dumps we feed
ndsrecomp), so a native-vs-oracle diff compares two executions of the
same code — not our code vs a reimplementation.

## Flow

1. `bash oracle/setup-melonds.sh` — clone melonDS at the pinned tag into
   `third_party/melonDS/` (gitignored) and apply `patches/`.
2. Build the core + the `nds_oracle` shim (see `setup-melonds.sh` output
   for the exact cmake invocation).
3. Point the oracle at the same `bios/` dumps and `firmware.bin`.
4. Run a probe from `oracle/` (below). It drives both the native runtime
   and the oracle to the same hardware-event count and diffs state.

## The melonDS-side patch (to author against the clone)

melonDS has no built-in TCP state server in the shape we need. The patch
in `patches/` adds a small `nds_oracle` frontend that:

- loads the BIOS/firmware and boots in **firmware mode** (not direct
  boot — we want the menu),
- pumps the core and maintains the `TCP.md` event counters (VBlank IRQ
  per CPU, IPCSYNC writes, FIFO sends, DMA/timer events),
- serves `regs` / `read_mem` (per-CPU bus view) / `read_region` /
  `framebuffer` (both engines) / `touch` / `keys`.

It is authored against the actual melonDS source after the clone (same
as `gbarecomp/oracle/patches` were authored against mGBA). Until then
`patches/README.md` records the exact hook points.

## Probes

- `diff_mem.py` — run both ends to `vblank9 N`, diff a region
  (`mainram`/`vram?`/`pal?`/`oam`), print first divergence.
- `diff_frame.py` — diff engine-A and engine-B framebuffers at event N;
  dump both as PNG on mismatch.
- `find_first_diverge.py` — bisect over event counts to find the first
  VBlank where any tracked region diverges.

All sync on hardware events, never raw frame numbers (`TCP.md`).
