#pragma once

#include <cstdint>

void nds_vram_reset();
void nds_vram_map(unsigned bank, uint8_t value);
uint8_t nds_vramcnt(unsigned bank);
uint8_t nds_vramstat();

bool nds_video_address(uint32_t addr);
uint32_t nds_video_read(int cpu, uint32_t addr, uint32_t width);
void nds_video_write(int cpu, uint32_t addr, uint32_t value, uint32_t width);

bool nds_video_get_region(const char* name, const uint8_t** ptr, uint32_t* len);

// Renderer-facing mapped views. Addresses are offsets within the named engine
// space and widths are 1/2/4 bytes.
uint32_t nds_vram_read_bg(int engine, uint32_t addr, uint32_t width);
uint32_t nds_vram_read_obj(int engine, uint32_t addr, uint32_t width);
uint32_t nds_vram_read_bg_extpal(int engine, uint32_t addr, uint32_t width);
uint32_t nds_vram_read_obj_extpal(int engine, uint32_t addr, uint32_t width);
