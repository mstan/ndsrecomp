# ndsrecomp 0.0.1

Very early pre-alpha source snapshot of the Nintendo DS static-recompiler
ecosystem. This release is intended for developers following active bring-up;
it is not a stable or turnkey end-user release.

Highlights:

- Recompiles ARM7TDMI (ARMv4T) and ARM946E-S (ARMv5TE) code into native C.
- Boots the original DS firmware through the Health & Safety screen to an
  interactive main menu using a dual-CPU, event-aligned scheduler.
- Provides SDL video, touch, keyboard, and paced stereo-audio presentation.
- Includes a deterministic, separate-process melonDS oracle and retained
  firmware traversal evidence for accuracy work.
- Covers the exercised firmware paths with static banks and rejects unexpected
  mutable-RAM code in normal release execution.

This is a source-only release. It does not include Nintendo BIOS or firmware
dumps, ROMs, generated recompiled code, screenshots, save data, or binaries
embedding copyrighted Nintendo code or assets. Users must supply their own
hash-verified dumps locally.

There is no commercial game target, stable API, compatibility promise, or
turnkey clean-clone runner bootstrap yet. See `README.md` for the current build
boundary and `THIRD_PARTY_ATTRIBUTION.md` for provenance and licensing notes.
