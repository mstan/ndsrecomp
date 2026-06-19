#!/usr/bin/env python3
"""
fw_inspect.py — inspect a Nintendo DS firmware.bin flash header.

Phase-0 ground-truth tool. It decodes the firmware header fields at the
offsets verified against melonDS's FirmwareHeader struct, and hex-dumps
the candidate boot-part locations so we can confirm the exact
shift/relocation decode against melonDS's Boot() before the recompiler
relies on it.

The part ROM/RAM address decode is confirmed against DeSmuME's
firmware boot (src/firmware.cpp):

    shift   = header[0x14]
    s_a9rom = (shift >> 0) & 7 ;  s_a9ram = (shift >> 3) & 7
    s_a7rom = (shift >> 6) & 7 ;  s_a7ram = (shift >> 9) & 7
    ARM9 ROM = header[0x0C] << (2 + s_a9rom)
    ARM7 ROM = header[0x12] << (2 + s_a7rom)
    ARM9 RAM = 0x02800000 - (header[0x0E] << (2 + s_a9ram))
    ARM7 RAM = 0x03810000 - (header[0x10] << (2 + s_a7ram))

The boot code at those ROM offsets is COMPRESSED/encrypted (a bit-flag +
back-reference scheme decoded by the ARM7 BIOS). This tool reports the
part locations and dumps the raw (still-compressed) stream; it does NOT
decompress, because we boot LLE — the recompiled ARM7 BIOS does the real
decode at runtime. Offline decompression is a Phase-3 concern (promoting
the menu to recompiled banks), not a boot prerequisite.

Header field offsets (bytes, little-endian), per melonDS SPI_Firmware.h:

    0x00  u16  ARM9 GUI code offset       (menu, "part1")
    0x02  u16  ARM7 wifi code offset      ("part2")
    0x04  u16  GUI/wifi code checksum
    0x06  u16  boot code checksum
    0x08  4    identifier ("MAC" + ...)
    0x0C  u16  ARM9 boot code ROM address
    0x0E  u16  ARM9 boot code RAM address
    0x10  u16  ARM7 boot code RAM address
    0x12  u16  ARM7 boot code ROM address
    0x14  u16  shift amounts (per-part address shifts, packed)
    0x16  u16  data/gfx ROM address
    0x24  u16  user settings offset       (flash offset = value << 3)
    0x2C  u16  data/gfx checksum
    0x30  u16  wifi config checksum
    0x32  u16  wifi config length
    0x34  u8   wifi version
    0x36  6    MAC address
"""

import sys
import struct

KNOWN_SHA1 = "ae22de59fbf3f35ccfbeacaeba6fa87ac5e7b14b"


def u16(b, off):
    return struct.unpack_from("<H", b, off)[0]


def hexdump(b, off, n=32):
    out = []
    for row in range(0, n, 16):
        base = off + row
        chunk = b[base:base + 16]
        if not chunk:
            break
        hexs = " ".join(f"{c:02x}" for c in chunk)
        ascii_ = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
        out.append(f"    {base:06x}: {hexs:<47}  {ascii_}")
    return "\n".join(out)


def main():
    if len(sys.argv) < 2:
        print("usage: fw_inspect.py firmware.bin")
        return 1
    path = sys.argv[1]
    with open(path, "rb") as f:
        fw = f.read()

    import hashlib
    sha1 = hashlib.sha1(fw).hexdigest()
    print(f"file        : {path}")
    print(f"size        : {len(fw)} (0x{len(fw):X}) bytes")
    print(f"sha-1       : {sha1}"
          + ("  [matches known-good]" if sha1 == KNOWN_SHA1 else "  [UNKNOWN]"))
    print()

    ident = fw[0x08:0x0C]
    print("=== header (raw) ===")
    print(f"  0x00 ARM9 GUI code offset   : 0x{u16(fw,0x00):04x}"
          f"   (<<3 = flash 0x{u16(fw,0x00)<<3:06x})")
    print(f"  0x02 ARM7 wifi code offset  : 0x{u16(fw,0x02):04x}"
          f"   (<<3 = flash 0x{u16(fw,0x02)<<3:06x})")
    print(f"  0x04 GUI/wifi checksum      : 0x{u16(fw,0x04):04x}")
    print(f"  0x06 boot code checksum     : 0x{u16(fw,0x06):04x}")
    print(f"  0x08 identifier             : {ident!r}")
    print(f"  0x0C ARM9 boot ROM addr     : 0x{u16(fw,0x0C):04x}")
    print(f"  0x0E ARM9 boot RAM addr     : 0x{u16(fw,0x0E):04x}")
    print(f"  0x10 ARM7 boot RAM addr     : 0x{u16(fw,0x10):04x}")
    print(f"  0x12 ARM7 boot ROM addr     : 0x{u16(fw,0x12):04x}")
    shifts = u16(fw, 0x14)
    print(f"  0x14 shift amounts          : 0x{shifts:04x}  "
          f"(bits: a9rom={shifts&7} a9ram={(shifts>>3)&7} "
          f"a7rom={(shifts>>6)&7} a7ram={(shifts>>9)&7})")
    print(f"  0x16 data/gfx ROM addr      : 0x{u16(fw,0x16):04x}")
    print(f"  0x24 user settings offset   : 0x{u16(fw,0x24):04x}"
          f"   (<<3 = flash 0x{u16(fw,0x24)<<3:06x})")
    print(f"  0x34 wifi version           : {fw[0x34]}")
    mac = fw[0x36:0x3C]
    print(f"  0x36 MAC address            : "
          + ":".join(f"{c:02x}" for c in mac))
    print()

    # Confirmed boot-part decode (DeSmuME src/firmware.cpp).
    s_a9rom = (shifts >> 0) & 7
    s_a9ram = (shifts >> 3) & 7
    s_a7rom = (shifts >> 6) & 7
    s_a7ram = (shifts >> 9) & 7
    a9_rom = u16(fw, 0x0C) << (2 + s_a9rom)
    a7_rom = u16(fw, 0x12) << (2 + s_a7rom)
    a9_ram = (0x02800000 - (u16(fw, 0x0E) << (2 + s_a9ram))) & 0xFFFFFFFF
    a7_ram = (0x03810000 - (u16(fw, 0x10) << (2 + s_a7ram))) & 0xFFFFFFFF

    print("=== boot parts (decoded; ROM stream is still compressed) ===")
    print(f"  ARM9 boot  flash 0x{a9_rom:06x}  ->  RAM 0x{a9_ram:08x}  "
          f"(main RAM)")
    print(hexdump(fw, a9_rom, 32))
    print(f"  ARM7 boot  flash 0x{a7_rom:06x}  ->  RAM 0x{a7_ram:08x}  "
          f"(ARM7 WRAM)")
    print(hexdump(fw, a7_rom, 32))
    print()
    print(f"  ARM9 GUI/menu  flash 0x{u16(fw,0x00)<<3:06x}")
    print(f"  ARM7 wifi      flash 0x{u16(fw,0x02)<<3:06x}")
    print(f"  user settings  flash 0x{u16(fw,0x24)<<3:06x}")
    print()
    print("NOTE: the boot streams are compressed/encrypted; the recompiled "
          "ARM7 BIOS decodes them at\nruntime (LLE). Offline decompression "
          "is a Phase-3 concern, not a boot prerequisite.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
