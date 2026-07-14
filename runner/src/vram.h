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

// Direct physical palette/OAM views for the software renderer. These remain
// power-gated and are only valid until the next device reset.
const uint8_t* nds_vram_renderer_palette(int engine);
const uint8_t* nds_vram_renderer_oam(int engine);

// Renderer-only flattened address views. A non-null chunk points directly at
// the physical bank backing that 16 KiB virtual window. Legal overlapping
// bank mappings stay null and use the exact OR-combining fallback.
struct NdsVramRendererView {
    const uint8_t* bg[32];
    const uint8_t* obj[16];
};
const NdsVramRendererView* nds_vram_renderer_view(int engine);
