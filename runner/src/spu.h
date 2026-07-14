#pragma once

#include <cstdint>

// Nintendo DS ARM7 sound unit. Register widths are bytes (1/2/4), matching
// nds_io_read/write. The mixer advances once per 1024 system cycles.
void nds_spu_reset();
// Console power-off silences queued host output but preserves architectural
// channel/register state, matching melonDS SPU::Stop().
void nds_spu_stop();
uint32_t nds_spu_read(uint32_t addr, uint32_t width);
void nds_spu_write(uint32_t addr, uint32_t value, uint32_t width);
void nds_tick_spu(uint64_t system_cycles);

// Stereo signed-16 output retained for the eventual SDL host. Returns frames.
uint32_t nds_spu_read_output(int16_t* stereo, uint32_t frames);

// Always-on, non-destructive sample trace for native/oracle comparison. Sample
// ordinals start at zero after reset and are independent of the SDL reader.
// A bounded history is retained; callers must request start >= oldest.
uint64_t nds_spu_debug_output_produced();
uint64_t nds_spu_debug_output_oldest();
uint32_t nds_spu_debug_copy_output(uint64_t start, int16_t* stereo,
                                   uint32_t frames);
