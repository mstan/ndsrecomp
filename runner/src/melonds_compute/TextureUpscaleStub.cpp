#include "TextureUpscale.h"

void nds_texture_upscale_set_factor(int) {}

int nds_texture_upscale_factor() {
    return 1;
}

bool nds_texture_upscale_ready() {
    return false;
}

bool nds_texture_upscale_init() {
    return false;
}

void nds_texture_upscale_shutdown() {}

bool nds_texture_upscale_dispatch(uint32_t, uint32_t, uint32_t, uint32_t,
                                  const void*) {
    return false;
}
