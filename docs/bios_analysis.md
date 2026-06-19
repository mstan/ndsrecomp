# BIOS analysis (Ghidra, via MCP on port 2222)

Both BIOSes loaded as Raw Binary at their hardware bases. Seeds exported
to `generated/ghidra_function_starts_{arm7,arm9}.json`. Mode (ARM/Thumb)
on the SWI handlers is authoritative — taken from the jump-table pointer
LSB.

## ARM7 BIOS (`biosnds7.rom`, `ARM:LE:32:v4t`, base 0x00000000, 16 KB)

Fully auto-analyzed: **155 functions**. Vector table auto-labeled.

| what | address |
|---|---|
| Reset vector | `0x0` → `b 0x20` → `arm7_reset_boot` @ `0x2CD0` |
| SWI vector | `0x8` → `arm7_swi_dispatch` @ `0x2DDC` |
| SWI jump table | `0x2E38` (34 slots, 0x00–0x21) |
| ARM↔Thumb veneer table | `0x330C`–`0x33A0` |

`arm7_reset_boot` writes IME=0 (`0x04000208`), seeds the ARM7 IRQ vector
at `0x03FFFFFC`, then boots — this is the firmware-boot driver (Phase 3).
`arm7_swi_dispatch`: `table[(*(u16)(lr-2) & 0xFF)*4]()`.

SWI handlers present (mode): 0x00 SoftReset(a), 0x03 WaitByLoop(t),
0x04 IntrWait(a), 0x05 VBlankIntrWait(a), 0x06 Halt(a), 0x07 Sleep(a),
0x08 SoundBias(t), 0x09 Div(a), 0x0B CpuSet(t), 0x0C CpuFastSet(a),
0x0D SquareRoot(a), 0x0E GetCRC16(t), 0x0F IsDebugger(t),
0x10 BitUnPack(a), 0x11 LZ77UnComp/wram(a) @0x3220,
0x12 LZ77UnComp/vram(t) @0x2A2A, 0x13 HuffUnComp(t), 0x14 RLUnComp/wram(t),
0x15 RLUnComp/vram(t), 0x1A GetSineTable(t), 0x1B GetPitchTable(t),
0x1C GetVolumeTable(t), + 0x1D/0x1F/0x20/0x21. (0x1A–0x1C sound SWIs are
ARM7-only.)

## ARM9 BIOS (`biosnds9.rom`, `ARM:LE:32:v5t`, base 0xFFFF0000, 4 KB)

Imported over MCP, then GUI Auto-Analyze (31 functions). The SWI
handlers reached only via the jump table aren't auto-discovered (the
analyzer can't see `0xFFFF02FC` is a code-pointer table) — added by hand
from the table read. **41 seeds** total; all 20 SWI handlers captured
with authoritative mode from the table pointer LSB. A few handlers
(`0x04/0x05/0x06`, `0x0D`, `0x10`) couldn't be split into separate
Ghidra functions (merged into a neighbor body or in not-yet-disassembled
bytes) — cosmetic only; the seed list has them. High vectors at
`0xFFFF0000`.

| what | address |
|---|---|
| Reset vector | `0xFFFF0000` → `b 0xFFFF0110` → `arm9_reset_boot` |
| SWI vector | `0xFFFF0008` → `arm9_swi_dispatch` @ `0xFFFF0298` |
| IRQ vector | `0xFFFF0018` → `arm9_irq_handler` @ `0xFFFF0274` |
| SWI jump table | `0xFFFF02FC` (32 slots, 0x00–0x1F) |

`arm9_swi_dispatch` uses **`blx r12`** (ARMv5) + `mrs/msr spsr` — concrete
proof the recompiler needs the ARMv5TE delta (BLX etc.) for ARM9. SWIs
invoked from Thumb (`ldrh [lr,-2]`).

SWI handlers present (mode): 0x00 SoftReset(a), 0x03 WaitByLoop(t),
0x04 IntrWait(a), 0x05 VBlankIntrWait(a), 0x06 Halt(a), 0x09 Div(a),
0x0B CpuSet(t), 0x0C CpuFastSet(a), 0x0D Sqrt(a), 0x0E GetCRC16(t),
0x0F IsDebugger(t), 0x10 BitUnPack(a), 0x11 LZ77UnComp/wram(a),
0x12 LZ77UnComp/vram(t), 0x13 HuffUnComp(t), 0x14 RLUnComp/wram(t),
0x15 RLUnComp/vram(t), 0x16 Diff8bitUnFilter(t), 0x18 Diff16bitUnFilter(t),
0x1F CustomPost/GetBootProcs(t). No sound SWIs (ARM9 has none).

## Implication for Phase 1

The ARM9 path exercises ARMv5TE (BLX, SPSR transfers) from the very first
SWI, so the v5 delta + CP15 are not optional — they're hit on the boot
path. The shared decompression SWIs (LZ77/Huff/RL) are how the firmware
parts get expanded into RAM; recompiling them well matters for Phase 3.
