#pragma once

#include <cstdint>

// Bridge to the vendored melonDS GPU3D geometry/rasterizer device model
// (runner/vendor/melonds/, GPL-3.0-or-later — see THIRD_PARTY_ATTRIBUTION.md).
// The guest produces every register/GXFIFO write; this is a device model,
// not HLE.

void nds_gpu3d_reset();

// ARM9 3D register window: DISP3DCNT (0x04000060..63) plus the melonDS
// dispatch range 0x04000320..0x040006A3 (tables, clear/fog, GXFIFO at
// 0x400..0x43F, direct command ports at 0x440..0x5CB, GXSTAT/results at
// 0x600..0x6A3).
bool nds_gpu3d_reg_addr(uint32_t addr);
uint32_t nds_gpu3d_read(uint32_t addr, uint32_t width);
void nds_gpu3d_write(uint32_t addr, uint32_t value, uint32_t width);

// POWCNT1 (ARM9 0x04000304): bit3 enables the geometry engine, bit2 the
// rendering engine, matching melonDS GPU::SetPowerCnt.
void nds_gpu3d_set_power(uint16_t powcnt1);
