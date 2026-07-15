# Firmware boot — Phase 0 ground truth

Source: `bios/firmware.bin`, sha-1 `ae22de59…` (matches known-good).
Decoded by `tools/fw_inspect.py`.

## Header (verified offsets, melonDS FirmwareHeader)

| field | value |
|---|---|
| 0x00 ARM9 GUI code offset | `0x3076` |
| 0x02 ARM7 wifi code offset | `0x1EB6` |
| 0x04 GUI/wifi checksum | `0xE934` |
| 0x06 boot code checksum | `0xABBB` |
| 0x08 identifier | `"MACP"` |
| 0x0C ARM9 boot ROM addr | `0x0030` |
| 0x0E ARM9 boot RAM addr | `0x6100` |
| 0x10 ARM7 boot RAM addr | `0x0034` |
| 0x12 ARM7 boot ROM addr | `0x0008` |
| 0x14 shift amounts | `0x0C71` |
| 0x24 user settings offset | `0x0DB3` → flash `0x6D98` |
| 0x36 MAC | device-specific value (redacted; Nintendo OUI verified) |

Genuine retail firmware (real Nintendo MAC, 256 KB, per-console
settings). Not a synthesized/homebrew image.

## Boot-part decode (CONFIRMED — DeSmuME src/firmware.cpp)

```
shift   = header[0x14]                       # = 0x0C71 here
s_a9rom = (shift >> 0) & 7   # = 1
s_a9ram = (shift >> 3) & 7   # = 6
s_a7rom = (shift >> 6) & 7   # = 1
s_a7ram = (shift >> 9) & 7   # = 6
ARM9 ROM = header[0x0C] << (2 + s_a9rom)              # 0x0030<<3 = flash 0x180
ARM7 ROM = header[0x12] << (2 + s_a7rom)              # 0x0008<<3 = flash 0x40
ARM9 RAM = 0x02800000 - (header[0x0E] << (2+s_a9ram)) # 0x02800000-0x610000 = 0x021F0000
ARM7 RAM = 0x03810000 - (header[0x10] << (2+s_a7ram)) # 0x03810000-0x3400   = 0x0380CC00
```

Both destinations land in valid regions (ARM9 → main RAM, ARM7 → ARM7
WRAM), confirming the formula. The streams at those ROM offsets are
**compressed/encrypted** (a bit-flag + back-reference scheme the ARM7
BIOS decodes). We do **not** decompress offline: the recompiled ARM7
BIOS does it at runtime (LLE). Offline decompression is only needed to
promote the menu to recompiled banks (Phase 3), not to boot.

| part | flash offset | RAM destination |
|---|---|---|
| ARM9 boot | `0x000180` | `0x021F0000` (main RAM) |
| ARM7 boot | `0x000040` | `0x0380CC00` (ARM7 WRAM) |
| ARM9 GUI/menu | `0x0183B0` | (loaded by ARM9 boot) |
| ARM7 wifi | `0x00F5B0` | (loaded by ARM7 boot) |
| user settings | `0x006D98` | — |

## Architectural clarification — offline extraction is NOT a boot prerequisite

We do **LLE** boot: the recompiled **ARM7 BIOS executes the real header
parse + part copy/decompress** into RAM, exactly as hardware does
(mirrors psxrecomp, where the BIOS copies the shell to RAM 0x80030000
and runs it). Therefore:

- To **boot**, we do not need to decompress the firmware parts offline.
  We map `firmware.bin` as the SPI flash device, recompile both BIOSes,
  and let the recompiled BIOS copy/relocate the parts. The copied code
  then runs via the **dirty-RAM ARM interpreter** until promoted.
- Offline part extraction (`fw_inspect` → decompressed ARM9/ARM7
  binaries) is needed only for (a) Ghidra analysis and (b) **promoting**
  the menu code from interpreted dirty-RAM to recompiled banks
  (Phase 3+), shrinking the interpreted set. It is an optimization of
  the LLE path, never a replacement for it.

So the boot bring-up order is: recompile BIOSes → SPI flash device →
BIOS copies parts → dirty-RAM interp runs the menu → (later) recompile
the menu regions as banks for speed/coverage.

## Boot sequence (to LLE)

1. Power-on: ARM7 resets to BIOS `0x00000000`, ARM9 to BIOS
   `0xFFFF0000`. Both BIOSes run their reset path.
2. ARM7 BIOS drives SPI: reads the firmware header, validates the
   checksums, copies the ARM9 boot/GUI part to its RAM dest and the
   ARM7 boot/wifi part to its RAM dest (decompressing as needed).
3. The two cores hand off via IPCSYNC / IPC FIFO and jump to the copied
   entry points; the firmware menu (ARM9) + ARM7 helper begin.
4. Menu draws via 2D engines A/B, reads touch via the ARM7 TSC over SPI.

Cross-check every step against melonDS rings, earliest divergence first.
