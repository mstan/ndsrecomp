#pragma once

#include <cstdint>

constexpr uint32_t NDS_GPU2D_DISPCNT_WINDOW_ENABLE_MASK = 0xE000u;

// Shared, side-effect-free DS window selection. Keeping this separate from
// the scanline renderer lets the eligibility check use precisely the same
// precedence and register interpretation as native composition.
struct NdsGpu2dWindowState {
    uint32_t dispcnt;
    // Raw MMIO byte order: WINxH/V low byte is the end coordinate, high
    // byte is the start coordinate. WININ/WINOUT retain their byte order.
    const uint8_t* win;
};

constexpr bool nds_gpu2d_window_contains(uint8_t point, uint8_t start,
                                         uint8_t end) {
    // melonDS updates the active latch by clearing at end before setting at
    // start. Equal endpoints therefore describe an empty window; a reversed
    // interval wraps through coordinate zero.
    return start < end ? point >= start && point < end
                       : start > end ? point >= start || point < end
                                     : false;
}

constexpr bool nds_gpu2d_window_rect_contains(uint8_t x, uint8_t y,
                                              uint8_t left, uint8_t right,
                                              uint8_t top, uint8_t bottom) {
    return nds_gpu2d_window_contains(x, left, right) &&
           nds_gpu2d_window_contains(y, top, bottom);
}

constexpr uint8_t nds_gpu2d_window_mask(const NdsGpu2dWindowState& state,
                                         uint8_t x, uint8_t y,
                                         bool obj_window) {
    // WINOUT does not participate until at least one window mechanism is
    // enabled. This matches melonDS DrawScanline_BGOBJ's all-enabled fast
    // mask when DISPCNT[15:13] is clear.
    if (!(state.dispcnt & NDS_GPU2D_DISPCNT_WINDOW_ENABLE_MASK)) return 0x3Fu;
    const uint8_t* const win = state.win;
    if ((state.dispcnt & 0x2000u) &&
        nds_gpu2d_window_rect_contains(x, y, win[1], win[0],
                                       win[5], win[4]))
        return win[8];
    if ((state.dispcnt & 0x4000u) &&
        nds_gpu2d_window_rect_contains(x, y, win[3], win[2],
                                       win[7], win[6]))
        return win[9];
    if ((state.dispcnt & 0x8000u) && obj_window) return win[11];
    return win[10];
}

constexpr bool nds_gpu2d_window_mask_supports_hd(uint8_t mask) {
    // The HD descriptor removes BG0/3D and the shader reinserts it under one
    // global BLDCNT. It can represent a region only when BG0 and normal color
    // effects both remain enabled there.
    return (mask & 0x21u) == 0x21u;
}

constexpr bool nds_gpu2d_window_rect_reachable(uint8_t left, uint8_t right,
                                                uint8_t top, uint8_t bottom) {
    return left != right && top != bottom;
}

constexpr bool nds_gpu2d_windows_support_hd(const NdsGpu2dWindowState& state) {
    if (!(state.dispcnt & NDS_GPU2D_DISPCNT_WINDOW_ENABLE_MASK)) return true;
    const uint8_t* const win = state.win;
    // Outside always has at least one reachable pixel: DS rectangular windows
    // cannot cover the entire 256x192 display.
    if (!nds_gpu2d_window_mask_supports_hd(win[10])) return false;
    if ((state.dispcnt & 0x2000u) &&
        nds_gpu2d_window_rect_reachable(win[1], win[0], win[5], win[4]) &&
        !nds_gpu2d_window_mask_supports_hd(win[8]))
        return false;
    if ((state.dispcnt & 0x4000u) &&
        nds_gpu2d_window_rect_reachable(win[3], win[2], win[7], win[6]) &&
        !nds_gpu2d_window_mask_supports_hd(win[9]))
        return false;
    // OBJ-window coverage is data-dependent. Treat its enabled region as
    // reachable so no unobserved sprite/OAM update can invalidate HD output.
    return !(state.dispcnt & 0x8000u) ||
           nds_gpu2d_window_mask_supports_hd(win[11]);
}
