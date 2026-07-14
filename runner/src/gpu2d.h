#pragma once

#include <cstdint>

void nds_gpu2d_reset();
// Console power-off clears both physical front/back buffers without resetting
// GPU register state, matching melonDS GPU::Stop().
void nds_gpu2d_stop();
uint32_t nds_gpu2d_read(uint32_t addr, uint32_t width);
void nds_gpu2d_write(uint32_t addr, uint32_t value, uint32_t width);
void nds_gpu2d_render_scanline(int line);
void nds_gpu2d_render_frame();
void nds_gpu2d_finish_frame();
const uint32_t* nds_gpu2d_framebuffer(int screen);
