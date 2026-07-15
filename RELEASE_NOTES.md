# ndsrecomp 0.0.1

Initial private source release of the Nintendo DS static-recompiler ecosystem.

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
