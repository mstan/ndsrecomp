# bios/ — user-provided dumps

These are **your own** Nintendo DS dumps. They are copyrighted and are
**not** committed (see `../.gitignore`). The runtime hash-verifies each
at load and refuses to start on a mismatch.

| file | sha-1 | size | role |
|---|---|---|---|
| `biosnds9.rom` | `bfaac75f101c135e32e2aaf541de6b1be4c8c62d` | 4 KB | ARM9 BIOS — maps at `0xFFFF0000` |
| `biosnds7.rom` | `24f67bdea115a2c847c8813a262502ee1607b7df` | 16 KB | ARM7 BIOS — maps at `0x00000000` |
| `firmware.bin` | `ae22de59fbf3f35ccfbeacaeba6fa87ac5e7b14b` | 256 KB | flash image: header + boot parts + menu + settings |
| `BIOSGBA.ROM` | `300c20df6731a33952ded8c436f7f186d25d3492` | 16 KB | GBA BIOS — for future GBA-mode, out of scope now |

`key.cfg` from the original archive is an emulator keyboard-mapping
string, unrelated to the console — ignored.

Inspect the firmware header with:

```
python ../tools/fw_inspect.py firmware.bin
```
