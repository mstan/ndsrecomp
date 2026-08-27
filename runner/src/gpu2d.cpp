#include "gpu2d.h"
#include "gpu2d_window.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "gpu3d.h"
#include "io.h"
#include "title_patches.h"
#include "vram.h"

namespace {

struct Unit {
    uint32_t dispcnt = 0;
    uint16_t bgcnt[4]{};
    uint16_t bgx[4]{};
    uint16_t bgy[4]{};
    int16_t pa[2]{}, pb[2]{}, pc[2]{}, pd[2]{};
    int32_t refx[2]{}, refy[2]{};
    uint8_t win[12]{};
    uint8_t bg_mosaic_x = 0, bg_mosaic_y = 0;
    uint8_t obj_mosaic_x = 0, obj_mosaic_y = 0;
    uint16_t bldcnt = 0, bldalpha = 0;
    uint8_t eva = 16, evb = 0, evy = 0;
    // BG2/BG3 affine reference registers and the renderer's live scanline
    // accumulators. The latter reload at frame start and advance only while
    // an affine/extended layer is drawn, matching melonDS Unit state.
    int32_t refx_internal[2]{}, refy_internal[2]{};
    uint32_t capture = 0;
    uint16_t master_bright = 0;
    // DISPCAPCNT enable latches at scanline 0 and captures for the whole
    // frame; the enable bit auto-clears at VBlank only if it latched
    // (melonDS Unit::CaptureLatch).
    bool capture_latch = false;
};

struct Pixel {
    uint32_t color = 0; // 6-bit R/G/B in bytes
    uint8_t target = 0; // BLDCNT layer bit
    uint8_t alpha = 0;  // 0=normal, 1..16=semi-transparent OBJ alpha
    uint8_t priority = 4; // 0 is frontmost; backdrop is 4
    uint8_t order = 0; // equal-priority order: OBJ, BG0, BG1, BG2, BG3
    bool valid = false;
    uint8_t alpha5 = 0; // 3D-layer pixel: its 5-bit alpha 1..31 (0 = not 3D)
};

std::array<Unit,2> g_unit{};
using Frame = std::array<uint32_t, 256 * 192>;
constexpr int kMaxAdaptiveWidth = 448;
using AdaptiveFrame = std::array<uint32_t, kMaxAdaptiveWidth * 192>;
// melonDS draws into the back buffer during the active frame and publishes it
// only in GPU::FinishFrame, after VBlank. Keeping the same lifecycle matters
// for instruction-precise framebuffer queries made while VCount is 192..262.
// Physical top/bottom buffers, matching melonDS GPU::AssignFramebuffers.
// POWCNT1 bit 15 routes engine A to the top when set and to the bottom when
// clear. Apply that routing while each scanline is rendered: consulting the
// live register only when a debug/frontend client later reads the completed
// frame can retroactively swap a frame if the guest changes POWCNT1 during
// VBlank.
std::array<std::array<Frame, 2>, 2> g_fb{}; // [buffer][screen], 0xFFRRGGBB
std::array<AdaptiveFrame, 2> g_adaptive_frame{};
// The threaded renderer begins the next 3D frame at VCount 215, before the
// frontend presents the just-completed 2D buffer at the following frame
// boundary. Snapshot the wide lines while 2D consumes them so adaptive
// presentation stays on the same frame and never waits on the next render.
std::array<AdaptiveFrame, 2> g_wide_3d_frame{};
std::array<AdaptiveFrame, 2> g_wide_3d_attr_frame{};
std::array<uint16_t, 2> g_wide_3d_width{};
bool g_adaptive_skybox_fill = false;
bool g_adaptive_center_native = false;
uint32_t g_adaptive_center_max_polygons = 0;
std::array<AdaptiveFrame, 2> g_direct_object_frame{};
NdsGpu2dDirectFrame g_direct_current_frame{};
NdsGpu2dDirectFrame g_direct_present_frame{};
unsigned g_direct_object_write = 0;
bool g_direct_present_enabled = false;
bool g_direct_frame_active = false;

// Internal-resolution (HD) emit. Two native-width surfaces describing the 2D
// stack with the 3D layer removed; see NdsGpu2dHdFrame in gpu2d.h.
bool g_hd_emit_enabled = false;
bool g_hd_frame_valid = false;
std::vector<uint32_t> g_hd_top_pixels;
std::vector<uint32_t> g_hd_below_pixels;
NdsGpu2dHdFrame g_hd_frame{};

uint32_t hd_meta(const Pixel& p, bool effects_enabled) {
    return static_cast<uint32_t>(p.target & 0x3Fu) |
           (effects_enabled ? 0x80u : 0u) |
           (static_cast<uint32_t>(p.alpha) << 8) |
           (static_cast<uint32_t>(p.priority) << 16) |
           (static_cast<uint32_t>(p.order) << 24);
}
bool g_direct_present_frame_active = false;
bool g_frame_capture_active = false;
bool g_present_capture_active = false;
uint32_t g_direct_force_cpu_frames = 0;
bool g_adaptive_hud_anchor = false;
int g_adaptive_hud_center_width = 64;
int g_front = 0;
uint64_t g_render_ns = 0;
uint64_t g_obj_ns = 0;
uint64_t g_engine_ns[2] = {};
uint64_t g_text_lines[2][5] = {};
uint64_t g_no_effect_lines[2] = {};
uint64_t g_render_scanlines = 0;
uint64_t g_direct_frames = 0;
uint64_t g_hd_frames = 0;
uint64_t g_hd_presented = 0;
uint64_t g_direct_overlay_ns = 0;
uint64_t g_direct_class_frames[NDS_GPU2D_DIRECT_CLASS_COUNT] = {};
uint64_t g_direct_class_engine_a_ns[NDS_GPU2D_DIRECT_CLASS_COUNT] = {};
uint64_t g_direct_class_transitions = 0;
uint64_t g_direct_extra_bg_mask_frames[NDS_GPU2D_DIRECT_BG_MASK_COUNT] = {};
uint64_t g_direct_extra_bg_mask_engine_a_ns[
    NDS_GPU2D_DIRECT_BG_MASK_COUNT] = {};
uint64_t g_direct_extra_bg_mode_frames[NDS_GPU2D_DIRECT_BG_MODE_COUNT] = {};
uint64_t g_direct_extra_effect_frames[
    NDS_GPU2D_DIRECT_EFFECT_MODE_COUNT] = {};
uint64_t g_direct_extra_master_bright_frames[
    NDS_GPU2D_DIRECT_EFFECT_MODE_COUNT] = {};
NdsGpu2dDirectClass g_direct_frame_class = NDS_GPU2D_DIRECT_DISABLED;
NdsGpu2dDirectClass g_direct_previous_class = NDS_GPU2D_DIRECT_CLASS_COUNT;
uint8_t g_direct_extra_bg_mask = 0;

bool enabled(int engine);

bool profiling() {
    static const bool enabled = std::getenv("NDS_PROFILE_GPU") != nullptr;
    return enabled;
}

NdsGpu2dDirectClass direct_scene_class() {
    if (!g_direct_present_enabled) return NDS_GPU2D_DIRECT_DISABLED;
    if (!enabled(0)) return NDS_GPU2D_DIRECT_ENGINE_OFF;
    Unit& u = g_unit[0];
    if ((u.capture & 0x80000000u) != 0u || u.capture_latch)
        return NDS_GPU2D_DIRECT_CAPTURE;
    const uint8_t* const palette = nds_vram_renderer_palette(0);
    const uint8_t* const oam = nds_vram_renderer_oam(0);
    const NdsVramRendererView* const vram = nds_vram_renderer_view(0);
    if (!palette || !oam || !vram)
        return NDS_GPU2D_DIRECT_RENDERER_VIEW;
    if (u.dispcnt & 0x80u) return NDS_GPU2D_DIRECT_FORCE_BLANK;
    const uint32_t mode = (u.dispcnt >> 16) & 3u;
    if (mode != 1u) return NDS_GPU2D_DIRECT_DISPLAY_MODE;
    const bool bg0_3d = (u.dispcnt & 0x8u) != 0 &&
                        (u.dispcnt & 0x100u) != 0;
    if (!bg0_3d) return NDS_GPU2D_DIRECT_NO_BG0_3D;
    // The direct OBJ presenter has no DS window mask or OBJ-window coverage.
    // Route every windowed scene through the adaptive compositor instead.
    if (u.dispcnt & NDS_GPU2D_DISPCNT_WINDOW_ENABLE_MASK)
        return NDS_GPU2D_DIRECT_WINDOWS;
    const uint32_t extra_bg_mask = (u.dispcnt >> 9u) & 7u;
    if (extra_bg_mask != 0u) return NDS_GPU2D_DIRECT_EXTRA_BG;
    if (nds_gpu3d_output_width() <= 256u) return NDS_GPU2D_DIRECT_WIDTH;
    if (nds_gpu3d_render_xpos() != 0u)
        return NDS_GPU2D_DIRECT_RENDER_XPOS;
    if (g_adaptive_center_native)
        return NDS_GPU2D_DIRECT_CENTER_NATIVE;
    if (g_adaptive_center_max_polygons != 0u &&
        nds_gpu3d_render_polygon_count() <= g_adaptive_center_max_polygons)
        return NDS_GPU2D_DIRECT_CENTER_NATIVE;
    // The direct presenter currently owns the physical top window only.
    if ((nds_powercontrol9() & 0x8000u) == 0u)
        return NDS_GPU2D_DIRECT_SCREEN_ROUTE;
    return NDS_GPU2D_DIRECT_SUPPORTED;
}

uint16_t view16(const uint8_t* view, uint32_t offset) {
    uint16_t value = 0;
    std::memcpy(&value, view + (offset & 0x3FFu), sizeof(value));
    return value;
}
uint8_t bg_view8(const NdsVramRendererView& view, int engine,
                 uint32_t addr) {
    const uint32_t chunk = (addr >> 14u) & (engine ? 7u : 31u);
    if (const uint8_t* direct = view.bg[chunk])
        return direct[addr & 0x3FFFu];
    return static_cast<uint8_t>(nds_vram_read_bg(engine, addr, 1));
}
uint16_t bg_view16(const NdsVramRendererView& view, int engine,
                   uint32_t addr) {
    const uint32_t chunk = (addr >> 14u) & (engine ? 7u : 31u);
    if (const uint8_t* direct = view.bg[chunk]) {
        uint16_t value = 0;
        std::memcpy(&value, direct + (addr & 0x3FFFu), sizeof(value));
        return value;
    }
    return static_cast<uint16_t>(nds_vram_read_bg(engine, addr, 2));
}
uint8_t obj_view8(const NdsVramRendererView& view, int engine,
                  uint32_t addr) {
    const uint32_t chunk = (addr >> 14u) & (engine ? 7u : 15u);
    if (const uint8_t* direct = view.obj[chunk])
        return direct[addr & 0x3FFFu];
    return static_cast<uint8_t>(nds_vram_read_obj(engine, addr, 1));
}
uint16_t obj_view16(const NdsVramRendererView& view, int engine,
                    uint32_t addr) {
    const uint32_t chunk = (addr >> 14u) & (engine ? 7u : 15u);
    if (const uint8_t* direct = view.obj[chunk]) {
        uint16_t value = 0;
        std::memcpy(&value, direct + (addr & 0x3FFFu), sizeof(value));
        return value;
    }
    return static_cast<uint16_t>(nds_vram_read_obj(engine, addr, 2));
}
uint32_t obj_view32(const NdsVramRendererView& view, int engine,
                    uint32_t addr) {
    // 4-byte rows of 4bpp OBJ tiles are 4-byte aligned inside a 32-byte
    // tile, so a row never crosses a 16 KiB chunk boundary.
    const uint32_t chunk = (addr >> 14u) & (engine ? 7u : 15u);
    if (const uint8_t* direct = view.obj[chunk]) {
        uint32_t value = 0;
        std::memcpy(&value, direct + (addr & 0x3FFFu), sizeof(value));
        return value;
    }
    return nds_vram_read_obj(engine, addr, 4);
}
uint64_t obj_view64(const NdsVramRendererView& view, int engine,
                    uint32_t addr) {
    // 8-byte rows of 8bpp OBJ tiles are 8-byte aligned; same no-crossing rule.
    const uint32_t chunk = (addr >> 14u) & (engine ? 7u : 15u);
    if (const uint8_t* direct = view.obj[chunk]) {
        uint64_t value = 0;
        std::memcpy(&value, direct + (addr & 0x3FFFu), sizeof(value));
        return value;
    }
    return uint64_t{nds_vram_read_obj(engine, addr, 4)} |
           (uint64_t{nds_vram_read_obj(engine, addr + 4u, 4)} << 32);
}
uint32_t bg_view32(const NdsVramRendererView& view, int engine,
                   uint32_t addr) {
    // 4-byte tile rows are 4-byte aligned inside a 32/64-byte tile, so a
    // row never crosses a 16 KiB chunk boundary.
    const uint32_t chunk = (addr >> 14u) & (engine ? 7u : 31u);
    if (const uint8_t* direct = view.bg[chunk]) {
        uint32_t value = 0;
        std::memcpy(&value, direct + (addr & 0x3FFFu), sizeof(value));
        return value;
    }
    return nds_vram_read_bg(engine, addr, 4);
}
uint64_t bg_view64(const NdsVramRendererView& view, int engine,
                   uint32_t addr) {
    // 8-byte rows of 8bpp tiles are 8-byte aligned; same no-crossing rule.
    const uint32_t chunk = (addr >> 14u) & (engine ? 7u : 31u);
    if (const uint8_t* direct = view.bg[chunk]) {
        uint64_t value = 0;
        std::memcpy(&value, direct + (addr & 0x3FFFu), sizeof(value));
        return value;
    }
    return uint64_t{nds_vram_read_bg(engine, addr, 4)} |
           (uint64_t{nds_vram_read_bg(engine, addr + 4u, 4)} << 32);
}
uint32_t rgb6(uint16_t color) {
    return ((color & 0x001Fu) << 1) |
           (((color & 0x03E0u) >> 4) << 8) |
           (((color & 0x7C00u) >> 9) << 16);
}
uint32_t to_rgb32(uint32_t color) {
    const uint32_t r6 = color & 0x3Fu;
    const uint32_t g6 = (color >> 8) & 0x3Fu;
    const uint32_t b6 = (color >> 16) & 0x3Fu;
    const uint32_t r = (r6 << 2) | (r6 >> 4);
    const uint32_t g = (g6 << 2) | (g6 >> 4);
    const uint32_t b = (b6 << 2) | (b6 >> 4);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}
// 15-bit source color -> host pixel, byte-identical to to_rgb32(rgb6(c)).
const uint32_t* rgb32_lut() {
    static const std::array<uint32_t, 32768> lut = [] {
        std::array<uint32_t, 32768> t{};
        for (uint32_t c = 0; c < 32768u; ++c)
            t[c] = to_rgb32(rgb6(static_cast<uint16_t>(c)));
        return t;
    }();
    return lut.data();
}
uint32_t blend(uint32_t a, uint32_t b, uint32_t eva, uint32_t evb) {
    uint32_t r = (((a & 0x3Fu) * eva) + ((b & 0x3Fu) * evb) + 8) >> 4;
    uint32_t g = ((((a >> 8) & 0x3Fu) * eva) + (((b >> 8) & 0x3Fu) * evb) + 8) >> 4;
    uint32_t bl = ((((a >> 16) & 0x3Fu) * eva) + (((b >> 16) & 0x3Fu) * evb) + 8) >> 4;
    return std::min(r,63u) | (std::min(g,63u)<<8) | (std::min(bl,63u)<<16);
}
// The 3D layer blends with 5-bit precision using the pixel's own alpha
// (melonDS ColorBlend5): eva = alpha+1 in 1..32, evb = 32-eva.
uint32_t blend5(uint32_t a, uint32_t b, uint32_t alpha) {
    const uint32_t eva = alpha + 1u;
    if (eva == 32u) return a;
    const uint32_t evb = 32u - eva;
    uint32_t r = (((a & 0x3Fu) * eva) + ((b & 0x3Fu) * evb) + 16u) >> 5;
    uint32_t g = ((((a >> 8) & 0x3Fu) * eva) + (((b >> 8) & 0x3Fu) * evb) + 16u) >> 5;
    uint32_t bl = ((((a >> 16) & 0x3Fu) * eva) + (((b >> 16) & 0x3Fu) * evb) + 16u) >> 5;
    return std::min(r,63u) | (std::min(g,63u)<<8) | (std::min(bl,63u)<<16);
}

uint32_t brighten(uint32_t c, uint32_t f, uint32_t bias = 8u) {
    const uint32_t r = (c & 0x3Fu) + ((((63u-(c&0x3Fu))*f)+bias)>>4);
    const uint32_t g0 = (c>>8)&0x3Fu;
    const uint32_t b0 = (c>>16)&0x3Fu;
    const uint32_t g = g0 + ((((63u-g0)*f)+bias)>>4);
    const uint32_t b = b0 + ((((63u-b0)*f)+bias)>>4);
    return std::min(r,63u)|(std::min(g,63u)<<8)|(std::min(b,63u)<<16);
}
uint32_t darken(uint32_t c, uint32_t f, uint32_t bias = 7u) {
    const uint32_t r0=c&0x3Fu,g0=(c>>8)&0x3Fu,b0=(c>>16)&0x3Fu;
    return (r0-(((r0*f)+bias)>>4)) |
           ((g0-(((g0*f)+bias)>>4))<<8) |
           ((b0-(((b0*f)+bias)>>4))<<16);
}

struct TextLine {
    // Latched register state for this line. Never g_unit: a threaded line
    // render must not observe a register write that landed after its latch.
    const Unit* u = nullptr;
    int engine = 0;
    int bg = 0;
    const uint8_t* palette = nullptr;
    const NdsVramRendererView* vram = nullptr;
    uint16_t cnt = 0;
    uint32_t sy = 0;
    uint32_t char_base = 0;
    uint32_t map_base = 0;
    uint32_t width_extra = 0;
    uint32_t cached_map_addr = UINT32_MAX;
    uint16_t cached_tile = 0;
};

TextLine prepare_text_line(const Unit& u, int engine, int bg, int y,
                           const uint8_t* palette,
                           const NdsVramRendererView* vram) {
    TextLine line{};
    line.u = &u;
    line.engine = engine;
    line.bg = bg;
    line.palette = palette;
    line.vram = vram;
    line.cnt = u.bgcnt[bg];
    const uint32_t size = line.cnt >> 14;
    line.sy = (u.bgy[bg] + y) & 0x1FFu;
    if (!(size & 2u)) line.sy &= 0xFFu;
    if ((line.cnt & 0x40u) && u.bg_mosaic_y)
        line.sy -= y % (u.bg_mosaic_y + 1u);
    line.char_base = (line.cnt & 0x003Cu) << 12;
    line.map_base = (line.cnt & 0x1F00u) << 3;
    if (!engine) {
        line.char_base += (u.dispcnt & 0x07000000u) >> 8;
        line.map_base += (u.dispcnt & 0x38000000u) >> 11;
    }
    line.width_extra = (line.cnt & 0x4000u) ? 0x100u : 0u;
    return line;
}

Pixel text_pixel(TextLine& line, int x) {
    const Unit& u = *line.u;
    const uint16_t cnt = line.cnt;
    const int bg = line.bg;
    uint32_t sx = (u.bgx[bg] + x) & 0x1FFu;
    const uint32_t size = cnt >> 14;
    if (!(size & 1u)) sx &= 0xFFu;
    if ((cnt & 0x40u) && u.bg_mosaic_x) sx -= x % (u.bg_mosaic_x + 1u);
    uint32_t map_addr = line.map_base;
    if (cnt & 0x8000u) {
        map_addr += (line.sy & 0x1F8u) << 3;
        if (cnt & 0x4000u) map_addr += (line.sy & 0x100u) << 3;
    } else map_addr += (line.sy & 0xF8u) << 3;
    map_addr += ((sx & 0xF8u) >> 2) + ((sx & line.width_extra) << 3);
    if (map_addr != line.cached_map_addr) {
        line.cached_map_addr = map_addr;
        line.cached_tile = static_cast<uint16_t>(
            bg_view16(*line.vram, line.engine, map_addr));
    }
    const uint16_t tile = line.cached_tile;
    uint32_t tx = sx & 7u, ty = line.sy & 7u;
    if (tile & 0x0400u) tx = 7u-tx;
    if (tile & 0x0800u) ty = 7u-ty;
    uint8_t index;
    uint16_t color;
    if (cnt & 0x0080u) {
        index = bg_view8(*line.vram, line.engine,
            line.char_base+((tile&0x3FFu)<<6)+(ty<<3)+tx);
        if (!index) return {};
        if (u.dispcnt & 0x40000000u) {
            const uint32_t slot = (bg<2 && (cnt&0x2000u)) ? 2u+bg : static_cast<uint32_t>(bg);
            color = static_cast<uint16_t>(nds_vram_read_bg_extpal(line.engine,(slot<<13)+((tile>>12)<<9)+(index<<1),2));
        } else color = view16(line.palette,index<<1);
    } else {
        const uint8_t packed = bg_view8(*line.vram, line.engine,
            line.char_base+((tile&0x3FFu)<<5)+(ty<<2)+(tx>>1));
        index = (tx&1u) ? packed>>4 : packed&0xFu;
        if (!index) return {};
        color = view16(line.palette,(((tile>>12)&0xFu)<<5)+(index<<1));
    }
    // NOTE: color is returned in raw 15-bit form; the per-tile line decoder
    // below is the primary path and its buffers hold 15-bit colors. This
    // per-pixel routine remains as the exact-semantics fallback for mosaic.
    return {color, static_cast<uint8_t>(1u << bg), 0,
            static_cast<uint8_t>(cnt & 3u), static_cast<uint8_t>(bg + 1), true};
}

// One decoded text-BG scanline. Per-pixel storage is the raw 15-bit color
// with bit 15 as the "opaque" flag; target/priority/order are per-layer
// constants on a text BG so they live once beside the buffer.
struct BgLine {
    std::array<uint16_t, 256> color;  // bit15 = opaque
    uint8_t prio = 0;
    uint8_t target = 0;
    uint8_t order = 0;
};

enum class BgKind : uint8_t {
    None,
    Text,
    Affine,
    Extended,
};

BgKind bg_kind(const Unit& u, int bg) {
    if (bg < 0 || bg > 3) return BgKind::None;
    const uint32_t mode = u.dispcnt & 7u;
    if (bg < 2)
        return mode == 6u ? BgKind::None : BgKind::Text;
    if (bg == 2) {
        switch (mode) {
            case 0:
            case 1:
            case 3:
                return BgKind::Text;
            case 2:
            case 4:
                return BgKind::Affine;
            case 5:
                return BgKind::Extended;
            default:
                return BgKind::None;
        }
    }
    switch (mode) {
        case 0: return BgKind::Text;
        case 1:
        case 2:
            return BgKind::Affine;
        case 3:
        case 4:
        case 5:
            return BgKind::Extended;
        default:
            return BgKind::None;
    }
}

void decode_text_line(const Unit& u, int engine, int bg, int y,
                      const uint8_t* palette,
                      const NdsVramRendererView& vram, BgLine& out) {
    TextLine line = prepare_text_line(u, engine, bg, y, palette, &vram);
    out.prio = static_cast<uint8_t>(line.cnt & 3u);
    out.target = static_cast<uint8_t>(1u << bg);
    out.order = static_cast<uint8_t>(bg + 1);

    if ((line.cnt & 0x40u) && u.bg_mosaic_x) {
        // Mosaic-X resamples per pixel; keep the exact per-pixel path.
        for (int x = 0; x < 256; ++x) {
            const Pixel p = text_pixel(line, x);
            out.color[x] = p.valid
                ? static_cast<uint16_t>(p.color | 0x8000u) : 0u;
        }
        return;
    }

    const uint32_t size = line.cnt >> 14;
    const uint32_t sx_mask = (size & 1u) ? 0x1FFu : 0xFFu;
    const bool color256 = (line.cnt & 0x0080u) != 0;
    const bool extpal = color256 && (u.dispcnt & 0x40000000u);
    const uint32_t extpal_slot =
        (bg < 2 && (line.cnt & 0x2000u)) ? 2u + bg : static_cast<uint32_t>(bg);
    uint32_t row_base = line.map_base;
    if (line.cnt & 0x8000u) {
        row_base += (line.sy & 0x1F8u) << 3;
        if (line.cnt & 0x4000u) row_base += (line.sy & 0x100u) << 3;
    } else {
        row_base += (line.sy & 0xF8u) << 3;
    }

    int x = 0;
    while (x < 256) {
        const uint32_t sx = (u.bgx[bg] + static_cast<uint32_t>(x)) & sx_mask;
        const uint32_t tx = sx & 7u;
        const int run = std::min<int>(static_cast<int>(8u - tx), 256 - x);
        const uint32_t map_addr =
            row_base + ((sx & 0xF8u) >> 2) + ((sx & line.width_extra) << 3);
        const uint16_t tile = static_cast<uint16_t>(
            bg_view16(vram, engine, map_addr));
        const bool hflip = (tile & 0x0400u) != 0;
        const uint32_t ty = (tile & 0x0800u) ? 7u - (line.sy & 7u)
                                             : (line.sy & 7u);
        if (color256) {
            const uint64_t row = bg_view64(vram, engine,
                line.char_base + ((tile & 0x3FFu) << 6) + (ty << 3));
            for (int k = 0; k < run; ++k) {
                const uint32_t px = tx + static_cast<uint32_t>(k);
                const uint32_t sel = hflip ? 7u - px : px;
                const uint8_t index =
                    static_cast<uint8_t>(row >> (sel * 8u));
                if (!index) { out.color[x + k] = 0u; continue; }
                uint16_t color;
                if (extpal) {
                    color = static_cast<uint16_t>(nds_vram_read_bg_extpal(
                        engine,
                        (extpal_slot << 13) +
                            (static_cast<uint32_t>(tile >> 12) << 9) +
                            (uint32_t{index} << 1),
                        2));
                } else {
                    color = view16(palette, uint32_t{index} << 1);
                }
                out.color[x + k] = static_cast<uint16_t>(color | 0x8000u);
            }
        } else {
            const uint32_t row = bg_view32(vram, engine,
                line.char_base + ((tile & 0x3FFu) << 5) + (ty << 2));
            const uint32_t pal_base =
                (static_cast<uint32_t>(tile >> 12) & 0xFu) << 5;
            for (int k = 0; k < run; ++k) {
                const uint32_t px = tx + static_cast<uint32_t>(k);
                const uint32_t sel = hflip ? 7u - px : px;
                const uint8_t index =
                    static_cast<uint8_t>((row >> (sel * 4u)) & 0xFu);
                if (!index) { out.color[x + k] = 0u; continue; }
                out.color[x + k] = static_cast<uint16_t>(
                    view16(palette, pal_base + (uint32_t{index} << 1)) |
                    0x8000u);
            }
        }
        x += run;
    }
}

void begin_affine_line(const Unit& u, int bg, int y, int32_t* x, int32_t* yy) {
    const int affine = bg - 2;
    *x = u.refx_internal[affine];
    *yy = u.refy_internal[affine];
    if ((u.bgcnt[bg] & 0x0040u) && u.bg_mosaic_y) {
        const int mosaic_y = y % (static_cast<int>(u.bg_mosaic_y) + 1);
        *x -= mosaic_y * u.pb[affine];
        *yy -= mosaic_y * u.pd[affine];
    }
}

// BG2/BG3 affine reference accumulators advance once per line, but only on
// lines where that layer is actually decoded. The line render is a pure
// function of its latched Unit copy (which carries the pre-advance value), so
// the advance is applied to the master Unit by the scheduler-thread latch step
// -- never by a renderer. See docs/device_work_parallelization.md F4.
void advance_affine_line(Unit& u, int bg) {
    const int affine = bg - 2;
    u.refx_internal[affine] += u.pb[affine];
    u.refy_internal[affine] += u.pd[affine];
}

void setup_affine_output(const Unit& u, int bg, BgLine& out) {
    out.prio = static_cast<uint8_t>(u.bgcnt[bg] & 3u);
    out.target = static_cast<uint8_t>(1u << bg);
    out.order = static_cast<uint8_t>(bg + 1);
    out.color.fill(0u);
}

void decode_affine_line(const Unit& u, int engine, int bg, int y,
                        const uint8_t* palette,
                        const NdsVramRendererView& vram, BgLine& out) {
    const uint16_t cnt = u.bgcnt[bg];
    setup_affine_output(u, bg, out);

    uint32_t coordmask = 0;
    uint32_t yshift = 0;
    switch (cnt & 0xC000u) {
        case 0x0000u: coordmask = 0x07800u; yshift = 7; break;
        case 0x4000u: coordmask = 0x0F800u; yshift = 8; break;
        case 0x8000u: coordmask = 0x1F800u; yshift = 9; break;
        default:      coordmask = 0x3F800u; yshift = 10; break;
    }
    const uint32_t overflowmask =
        (cnt & 0x2000u) ? 0u : ~(coordmask | 0x7FFu);
    uint32_t char_base = (cnt & 0x003Cu) << 12;
    uint32_t map_base = (cnt & 0x1F00u) << 3;
    if (!engine) {
        char_base += (u.dispcnt & 0x07000000u) >> 8;
        map_base += (u.dispcnt & 0x38000000u) >> 11;
    }
    yshift -= 3u;

    int32_t line_x = 0;
    int32_t line_y = 0;
    begin_affine_line(u, bg, y, &line_x, &line_y);
    const int affine = bg - 2;
    const int mosaic_width =
        (cnt & 0x0040u) ? static_cast<int>(u.bg_mosaic_x) + 1 : 1;
    for (int x = 0; x < 256; ++x) {
        const int sample_x = x - (x % mosaic_width);
        const int32_t final_x = line_x + sample_x * u.pa[affine];
        const int32_t final_y = line_y + sample_x * u.pc[affine];
        if ((static_cast<uint32_t>(final_x | final_y) & overflowmask) != 0u)
            continue;
        const uint32_t map_addr =
            map_base +
            ((((static_cast<uint32_t>(final_y) & coordmask) >> 11u)
               << yshift) +
             ((static_cast<uint32_t>(final_x) & coordmask) >> 11u));
        const uint8_t tile = bg_view8(vram, engine, map_addr);
        const uint32_t tile_x =
            (static_cast<uint32_t>(final_x) >> 8u) & 7u;
        const uint32_t tile_y =
            (static_cast<uint32_t>(final_y) >> 8u) & 7u;
        const uint8_t index = bg_view8(
            vram, engine,
            char_base + (uint32_t{tile} << 6u) + (tile_y << 3u) + tile_x);
        if (!index) continue;
        out.color[x] = static_cast<uint16_t>(
            view16(palette, uint32_t{index} << 1u) | 0x8000u);
    }
}

void decode_extended_line(const Unit& u, int engine, int bg, int y,
                          const uint8_t* palette,
                          const NdsVramRendererView& vram, BgLine& out) {
    const uint16_t cnt = u.bgcnt[bg];
    setup_affine_output(u, bg, out);

    int32_t line_x = 0;
    int32_t line_y = 0;
    begin_affine_line(u, bg, y, &line_x, &line_y);
    const int affine = bg - 2;
    const int mosaic_width =
        (cnt & 0x0040u) ? static_cast<int>(u.bg_mosaic_x) + 1 : 1;

    if (cnt & 0x0080u) {
        uint32_t xmask = 0, ymask = 0, yshift = 0;
        switch (cnt & 0xC000u) {
            case 0x0000u:
                xmask = ymask = 0x07FFFu; yshift = 7; break;
            case 0x4000u:
                xmask = ymask = 0x0FFFFu; yshift = 8; break;
            case 0x8000u:
                xmask = 0x1FFFFu; ymask = 0x0FFFFu; yshift = 9; break;
            default:
                xmask = ymask = 0x1FFFFu; yshift = 9; break;
        }
        const uint32_t overflow_x = (cnt & 0x2000u) ? 0u : ~xmask;
        const uint32_t overflow_y = (cnt & 0x2000u) ? 0u : ~ymask;
        const uint32_t bitmap_base = (cnt & 0x1F00u) << 6u;
        const bool direct = (cnt & 0x0004u) != 0u;
        for (int x = 0; x < 256; ++x) {
            const int sample_x = x - (x % mosaic_width);
            const int32_t final_x = line_x + sample_x * u.pa[affine];
            const int32_t final_y = line_y + sample_x * u.pc[affine];
            if ((static_cast<uint32_t>(final_x) & overflow_x) ||
                (static_cast<uint32_t>(final_y) & overflow_y))
                continue;
            const uint32_t pixel =
                ((static_cast<uint32_t>(final_y) & ymask) >> 8u) *
                    (1u << yshift) +
                ((static_cast<uint32_t>(final_x) & xmask) >> 8u);
            if (direct) {
                const uint16_t color = bg_view16(
                    vram, engine, bitmap_base + (pixel << 1u));
                if (color & 0x8000u) out.color[x] = color;
            } else {
                const uint8_t index =
                    bg_view8(vram, engine, bitmap_base + pixel);
                if (index)
                    out.color[x] = static_cast<uint16_t>(
                        view16(palette, uint32_t{index} << 1u) | 0x8000u);
            }
        }
        return;
    }

    uint32_t coordmask = 0;
    uint32_t yshift = 0;
    switch (cnt & 0xC000u) {
        case 0x0000u: coordmask = 0x07800u; yshift = 7; break;
        case 0x4000u: coordmask = 0x0F800u; yshift = 8; break;
        case 0x8000u: coordmask = 0x1F800u; yshift = 9; break;
        default:      coordmask = 0x3F800u; yshift = 10; break;
    }
    const uint32_t overflowmask =
        (cnt & 0x2000u) ? 0u : ~(coordmask | 0x7FFu);
    uint32_t char_base = (cnt & 0x003Cu) << 12u;
    uint32_t map_base = (cnt & 0x1F00u) << 3u;
    if (!engine) {
        char_base += (u.dispcnt & 0x07000000u) >> 8u;
        map_base += (u.dispcnt & 0x38000000u) >> 11u;
    }
    const bool extpal = (u.dispcnt & 0x40000000u) != 0u;
    yshift -= 3u;
    for (int x = 0; x < 256; ++x) {
        const int sample_x = x - (x % mosaic_width);
        const int32_t final_x = line_x + sample_x * u.pa[affine];
        const int32_t final_y = line_y + sample_x * u.pc[affine];
        if ((static_cast<uint32_t>(final_x | final_y) & overflowmask) != 0u)
            continue;
        const uint32_t map_addr =
            map_base +
            (((((static_cast<uint32_t>(final_y) & coordmask) >> 11u)
                << yshift) +
              ((static_cast<uint32_t>(final_x) & coordmask) >> 11u))
             << 1u);
        const uint16_t tile = bg_view16(vram, engine, map_addr);
        uint32_t tile_x =
            (static_cast<uint32_t>(final_x) >> 8u) & 7u;
        uint32_t tile_y =
            (static_cast<uint32_t>(final_y) >> 8u) & 7u;
        if (tile & 0x0400u) tile_x = 7u - tile_x;
        if (tile & 0x0800u) tile_y = 7u - tile_y;
        const uint8_t index = bg_view8(
            vram, engine,
            char_base + ((uint32_t{tile} & 0x03FFu) << 6u) +
                (tile_y << 3u) + tile_x);
        if (!index) continue;
        uint16_t color = 0;
        if (extpal) {
            color = static_cast<uint16_t>(nds_vram_read_bg_extpal(
                engine,
                (static_cast<uint32_t>(bg) << 13u) +
                    (static_cast<uint32_t>(tile >> 12u) << 9u) +
                    (uint32_t{index} << 1u),
                2));
        } else {
            color = view16(palette, uint32_t{index} << 1u);
        }
        out.color[x] = static_cast<uint16_t>(color | 0x8000u);
    }
}

void decode_bg_line(const Unit& u, int engine, int bg, int y,
                    const uint8_t* palette,
                    const NdsVramRendererView& vram, BgLine& out) {
    switch (bg_kind(u, bg)) {
        case BgKind::Text:
            decode_text_line(u, engine, bg, y, palette, vram, out);
            break;
        case BgKind::Affine:
            decode_affine_line(u, engine, bg, y, palette, vram, out);
            break;
        case BgKind::Extended:
            decode_extended_line(u, engine, bg, y, palette, vram, out);
            break;
        default:
            out.color.fill(0u);
            break;
    }
}

void put_obj(Pixel* line, int width, int x, const Pixel& p) {
    if (x < 0 || x >= width || !p.valid) return;
    // OAM index is resolved by visiting entries from 127 down to 0: an equal
    // priority pixel written later therefore has the lower (winning) index.
    // A numerically worse priority must not replace an existing front pixel.
    if (!line[x].valid || p.priority <= line[x].priority) line[x]=p;
}

void render_obj_line(const Unit& u, int engine, int line_y, Pixel* out,
                     int out_width,
                     const uint8_t* oam, const uint8_t* palette,
                     const NdsVramRendererView& vram,
                     uint8_t* obj_window = nullptr) {
    std::fill_n(out, out_width, Pixel{});
    if (obj_window) std::fill_n(obj_window, out_width, 0u);
    if (!(u.dispcnt&0x1000u)) return;
    static constexpr int widths[16]={8,16,8,8,16,32,8,8,32,32,16,8,64,64,32,8};
    static constexpr int heights[16]={8,8,16,8,16,8,32,8,32,16,32,8,64,32,64,8};
    const uint32_t oam_base=0;
    const uint32_t pal_base=0x200u;
    for (int n=127;n>=0;--n) {
            const uint16_t a0=view16(oam,oam_base+n*8), a1=view16(oam,oam_base+n*8+2), a2=view16(oam,oam_base+n*8+4);
            const int priority=(a2>>10)&3;
            const int shape=(a0>>14)&3,size=(a1>>14)&3, sp=shape|(size<<2);
            int w=widths[sp],h=heights[sp];
            const bool affine=a0&0x0100u;
            if (!affine && (a0&0x0200u)) continue;
            int bw=w,bh=h;
            if (affine && (a0&0x0200u)){bw*=2;bh*=2;}
            const int sy=a0&0xFF;
            int row=(line_y-sy)&0xFF;
            if (row>=bh) continue;
            int sx=static_cast<int16_t>(a1<<7)>>7;
            int output_sx = sx;
            if (out_width > 256) {
                const int extra = (out_width - 256) / 2;
                if (engine == 0 && g_adaptive_hud_anchor) {
                    const int center = sx + bw / 2;
                    const int center_left =
                        (256 - g_adaptive_hud_center_width) / 2;
                    const int center_right =
                        center_left + g_adaptive_hud_center_width;
                    // Keep the title-configured center HUD band together and
                    // move authored left/right OBJ bands to the enhanced
                    // corners.
                    if (center < center_left)
                        output_sx = sx;
                    else if (center >= center_right)
                        output_sx = sx + extra * 2;
                    else
                        output_sx = sx + extra;
                } else {
                    output_sx = sx + extra;
                }
            }
            if (output_sx<=-bw || output_sx>=out_width) continue;
            const int mode=(a0>>10)&3;
            if (mode==2 && !obj_window) continue;
            const bool color256=a0&0x2000u;
            const uint32_t tile=a2&0x3FFu;
            const uint8_t alpha=mode==1?0xFFu:0u;
            int16_t pa=0,pb=0,pc=0,pd=0;
            if(affine){
                const int group=(a1>>9)&0x1F;
                pa=static_cast<int16_t>(view16(oam,oam_base+group*32+6));
                pb=static_cast<int16_t>(view16(oam,oam_base+group*32+14));
                pc=static_cast<int16_t>(view16(oam,oam_base+group*32+22));
                pd=static_cast<int16_t>(view16(oam,oam_base+group*32+30));
            }
            if(!affine && mode!=3 && mode!=2){
                // Per-tile fast path (the decode_text_line treatment): for a
                // regular tile-mode OBJ, py and therefore the tile row are
                // per-line constants and px walks monotonically, so one
                // 32/64-bit row fetch feeds up to 8 pixels. Byte-identical to
                // the per-pixel loop below: same index-0 skip, same palette
                // lookups, one put_obj per pixel. Affine (non-monotonic px),
                // bitmap (different addressing) and OBJ-window coverage keep
                // the generic path.
                const int py=(a1&0x2000u)?h-1-row:row;
                uint32_t tile_index=tile;
                if(u.dispcnt&0x10u) {
                    tile_index <<= (u.dispcnt >> 20) & 3u;
                    tile_index += (py>>3)*(w>>3)*(color256?2u:1u);
                } else {
                    tile_index += (py>>3)*0x20u;
                }
                const bool hflip=a1&0x1000u;
                const bool use_extpal=(u.dispcnt&0x80000000u)!=0u;
                const uint32_t extpal_base=((a2>>12)&0xFu)<<9;
                const uint32_t pal16=pal_base+(((a2>>12)&0xFu)<<5);
                int dx=output_sx<0?-output_sx:0;
                const int dx_end=output_sx+bw>out_width
                    ? out_width-output_sx:bw;
                Pixel p{0,0x10u,alpha,static_cast<uint8_t>(priority),0,true};
                while(dx<dx_end){
                    const int px=hflip?w-1-dx:dx;
                    const int tile_left=hflip?(px&7)+1:8-(px&7);
                    const int run=std::min(tile_left,dx_end-dx);
                    if(color256){
                        const uint32_t addr=(tile_index<<5)+((py&7)<<3)+((px>>3)<<6);
                        const uint64_t rowbits=obj_view64(vram,engine,addr);
                        for(int k=0;k<run;++k){
                            const int pxk=hflip?px-k:px+k;
                            const uint8_t index=static_cast<uint8_t>(rowbits>>((pxk&7)*8));
                            if(!index)continue;
                            uint16_t color;
                            if(use_extpal)color=static_cast<uint16_t>(nds_vram_read_obj_extpal(engine,extpal_base+(index<<1),2));
                            else color=view16(palette,pal_base+(index<<1));
                            p.color=rgb6(color);
                            put_obj(out,out_width,output_sx+dx+k,p);
                        }
                    }else{
                        const uint32_t addr=(tile_index<<5)+((py&7)<<2)+((px>>3)<<5);
                        const uint32_t rowbits=obj_view32(vram,engine,addr);
                        for(int k=0;k<run;++k){
                            const int pxk=hflip?px-k:px+k;
                            const uint8_t index=(rowbits>>((pxk&7)*4))&0xFu;
                            if(!index)continue;
                            p.color=rgb6(view16(palette,pal16+(index<<1)));
                            put_obj(out,out_width,output_sx+dx+k,p);
                        }
                    }
                    dx+=run;
                }
                continue;
            }
            for(int dx=0;dx<bw;++dx){
                int px,py;
                if(affine){
                    px=((dx-bw/2)*pa+(row-bh/2)*pb+(w<<7))>>8;
                    py=((dx-bw/2)*pc+(row-bh/2)*pd+(h<<7))>>8;
                    if(px<0||px>=w||py<0||py>=h) continue;
                }else{
                    px=(a1&0x1000u)?w-1-dx:dx;
                    py=(a1&0x2000u)?h-1-row:row;
                }
                const int screen_x=output_sx+dx;
                if(screen_x<0||screen_x>=out_width)continue;
                uint16_t color=0; uint8_t index=0;
                if(mode==3){
                    const uint32_t bitmap_alpha = (a2 >> 12) & 0xFu;
                    if (!bitmap_alpha) continue;
                    uint32_t base;
                    if(u.dispcnt&0x40u){
                        if(u.dispcnt&0x20u)continue;
                        base=tile<<(7+((u.dispcnt>>22)&1u));
                        base+=(py*w+px)*2;
                    } else if (u.dispcnt & 0x20u) {
                        base=((tile&0x1Fu)<<4)+((tile&0x3E0u)<<7)+(py*256u+px)*2;
                    } else {
                        base=((tile&0xFu)<<4)+((tile&0x3F0u)<<7)+(py*128u+px)*2;
                    }
                    color=obj_view16(vram,engine,base);
                    if(!(color&0x8000u))continue;
                }else{
                    uint32_t tile_index=tile;
                    if(u.dispcnt&0x10u) {
                        tile_index <<= (u.dispcnt >> 20) & 3u;
                        tile_index += (py>>3)*(w>>3)*(color256?2u:1u);
                    } else {
                        tile_index += (py>>3)*0x20u;
                    }
                    if(color256){
                        const uint32_t addr=(tile_index<<5)+((py&7)<<3)+((px>>3)<<6)+(px&7);
                        index=obj_view8(vram,engine,addr);if(!index)continue;
                        if(u.dispcnt&0x80000000u)color=static_cast<uint16_t>(nds_vram_read_obj_extpal(engine,(((a2>>12)&0xFu)<<9)+(index<<1),2));
                        else color=view16(palette,pal_base+(index<<1));
                    }else{
                        const uint32_t addr=(tile_index<<5)+((py&7)<<2)+((px>>3)<<5)+((px&7)>>1);
                        const uint8_t v=obj_view8(vram,engine,addr);index=(px&1)?v>>4:v&0xFu;if(!index)continue;
                        color=view16(palette,pal_base+(((a2>>12)&0xFu)<<5)+(index<<1));
                    }
                }
                if (mode == 2) {
                    // OBJ-window sprites are invisible. Their nonzero tile
                    // indices define coverage; palette values and OBJ
                    // priority do not participate.
                    obj_window[screen_x] = 1u;
                    continue;
                }
                Pixel p{rgb6(color), 0x10u, alpha,
                        static_cast<uint8_t>(priority), 0, true};
                if(mode==3)p.alpha=static_cast<uint8_t>(std::min(16u,((a2>>12)&0xFu)+1u));
                put_obj(out,out_width,screen_x,p);
            }
    }
}

uint32_t compose(const Unit& u, const Pixel& top, const Pixel& below,
                 bool effects_enabled = true) {
    uint32_t c=top.color;
    const uint16_t target2=static_cast<uint16_t>(below.target)<<8;
    if (top.alpha5) {
        // 3D layer on top: whenever the pixel behind is a BLDCNT second
        // target, per-pixel 5-bit blending is forced regardless of the
        // selected color effect (melonDS ColorComposite coloreffect=4).
        if (u.bldcnt & target2) return blend5(c, below.color, top.alpha5);
        // Otherwise the 3D layer acts as BG0 (first-target bit 0x01) for
        // brightness effects only; alpha blend never applies here.
        if (effects_enabled && (u.bldcnt & top.target)) {
            switch ((u.bldcnt >> 6) & 3u) {
                case 2: return brighten(c, u.evy);
                case 3: return darken(c, u.evy);
            }
        }
        return c;
    }
    if(top.alpha && (u.bldcnt&target2)){
        const uint32_t eva=top.alpha==0xFFu?u.eva:top.alpha;
        c=blend(c,below.color,eva,top.alpha==0xFFu?u.evb:16u-eva);
    }else if(effects_enabled && (u.bldcnt&top.target)){
        switch((u.bldcnt>>6)&3u){
            case 1:if(u.bldcnt&target2)c=blend(c,below.color,u.eva,u.evb);break;
            case 2:c=brighten(c,u.evy);break;
            case 3:c=darken(c,u.evy);break;
        }
    }
    return c;
}

// ---- Per-line latch and per-render scratch --------------------------------
// A line render is a pure function of a latched register snapshot plus the
// memory views; it never reads g_unit and never mutates guest state. The
// scheduler thread owns the latch (and therefore the affine accumulators and
// the DISPCAPCNT enable latch). See docs/device_work_parallelization.md.
struct LineScratch {
    std::array<BgLine, 4> bg_lines{};
    std::array<uint32_t, 256> comp6{};
    std::array<Pixel, 256> obj{};
    std::array<uint8_t, 256> obj_window{};
    // Profiling accumulators, merged into the module counters at a drain so
    // concurrent renders cannot tear them.
    uint64_t obj_ns = 0;
    uint64_t engine_ns[2] = {};
    uint64_t text_lines[2][5] = {};
    uint64_t no_effect_lines[2] = {};
    uint64_t scanlines = 0;
    uint64_t render_ns = 0;
    uint64_t direct_class_engine_a_ns[NDS_GPU2D_DIRECT_CLASS_COUNT] = {};
    uint64_t direct_extra_bg_mask_engine_a_ns[
        NDS_GPU2D_DIRECT_BG_MASK_COUNT] = {};
};

struct LineJob {
    Unit unit[2]{};
    const uint8_t* palette[2]{};
    const uint8_t* oam[2]{};
    const NdsVramRendererView* vram[2]{};
    // Owned copy: GPU3D::GetLine returns a pointer into shared scratch that is
    // valid only until the next call, and SoftRenderer::GetLine consumes
    // scanline-semaphore tokens, so only the scheduler thread may call it.
    std::array<uint32_t, 256> line3d_storage{};
    const uint32_t* line3d = nullptr;
    uint32_t buffer = 0;
    uint32_t screen[2]{};
    int y = 0;
    bool engine_active[2]{};
    bool bg0_3d = false;
    bool cap = false;
    uint32_t capw = 0;
    uint8_t direct_class = 0;
    uint8_t direct_extra_bg_mask = 0;
};

// The set of BG layers this engine decodes on this line. Single source of
// truth for the decoders and for the affine-accumulator advance (F4).
uint8_t bg_active_mask(const Unit& u, bool bg0_3d) {
    uint8_t mask = 0;
    for (int bg = 0; bg < 4; ++bg) {
        if (!(u.dispcnt & (0x100u << bg))) continue;
        if (bg == 0 && bg0_3d) continue;   // BG0's slot is the 3D layer
        if (bg_kind(u, bg) != BgKind::None) mask |= static_cast<uint8_t>(1u << bg);
    }
    return mask;
}

// Composite one scanline into the internal 6-bit format (channels at bits
// 0-5/8-13/16-21) including the 3D layer when BG0 is redirected to it.
// This is the general-path twin of the dst-direct fast paths in
// render_engine_line, which carry the same layer/priority/blend rules but
// skip the 3D layer and the 6-bit intermediate.
void compose_line6(int engine, int y, const Unit& u, const uint8_t* palette,
                   const uint8_t* oam, const NdsVramRendererView& vram,
                   const uint32_t* line3d, bool bg0_3d, uint32_t* out,
                   LineScratch& sc) {
    std::array<Pixel,256>& obj = sc.obj;
    std::array<uint8_t,256>& obj_window = sc.obj_window;
    const auto obj_start = profiling() ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
    render_obj_line(u, engine, y, obj.data(), 256, oam, palette, vram,
                    obj_window.data());
    if (profiling()) {
        sc.obj_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - obj_start).count());
    }
    const uint16_t backdrop15 = view16(palette, 0);
    const Pixel backdrop{rgb6(backdrop15), 0x20u, 0, 4, 5, true};
    std::array<BgLine, 4>& bg_lines = sc.bg_lines;
    size_t bg_count = 0;
    int active_bgs[4];
    const uint8_t active_mask = bg_active_mask(u, bg0_3d);
    for (int bg = 0; bg < 4; ++bg)
        if (active_mask & (1u << bg)) active_bgs[bg_count++] = bg;
    for (size_t i = 0; i < bg_count; ++i)
        decode_bg_line(u, engine, active_bgs[i], y, palette, vram,
                       bg_lines[i]);
    const uint8_t prio3d = static_cast<uint8_t>(u.bgcnt[0] & 3u);
    auto ahead = [](const Pixel& a, const Pixel& b) {
        return a.priority < b.priority ||
               (a.priority == b.priority && a.order < b.order);
    };
    const NdsGpu2dWindowState window_state{u.dispcnt, u.win};
    for (int x = 0; x < 256; ++x) {
        const uint8_t window_mask = nds_gpu2d_window_mask(
            window_state, static_cast<uint8_t>(x), static_cast<uint8_t>(y),
            obj_window[x] != 0u);
        Pixel top = backdrop, below = backdrop;
        auto push = [&](const Pixel& p) {
            if (ahead(p, top)) { below = top; top = p; }
            else if (ahead(p, below)) { below = p; }
        };
        for (size_t i = 0; i < bg_count; ++i) {
            const uint16_t c = bg_lines[i].color[x];
            if (!(c & 0x8000u) || !(window_mask & (1u << active_bgs[i])))
                continue;
            const BgLine& l = bg_lines[i];
            push(Pixel{rgb6(static_cast<uint16_t>(c & 0x7FFFu)),
                       l.target, 0, l.prio, l.order, true});
        }
        if (bg0_3d && line3d && (window_mask & 0x01u)) {
            const uint32_t c3 = line3d[x];
            const uint8_t a3 = static_cast<uint8_t>((c3 >> 24) & 0x1Fu);
            // alpha 0 = fully transparent; the layer competes at BG0's
            // priority and order (melonDS DrawBG_3D).
            if (a3)
                push(Pixel{c3 & 0x003F3F3Fu, 0x01u, 0, prio3d, 1, true, a3});
        }
        if (obj[x].valid && (window_mask & 0x10u)) push(obj[x]);
        out[x] = compose(u, top, below, (window_mask & 0x20u) != 0u);
    }
}

// DISPCAPCNT capture (engine A), mirroring melonDS SoftRenderer::DoCapture:
// writes 15-bit+alpha pixels into the physical destination bank (gated on
// its LCDC mapping), blending source A (composite or 3D-only line, in the
// internal 6-bit format) with source B (LCDC VRAM or the display FIFO).
void do_capture(const Unit& u, int line, uint32_t width,
                const uint32_t* comp6, const uint32_t* line3d) {
    const uint32_t cap = u.capture;
    const uint32_t dstbank = (cap >> 16) & 3u;
    if (!nds_vram_lcdc_mapped(dstbank)) return;
    uint16_t* const dstp =
        reinterpret_cast<uint16_t*>(nds_vram_bank_data(dstbank));
    uint32_t dstaddr = (((cap >> 18) & 3u) << 14) + line * width;

    // Source A: the 3D-only line or the pre-master-brightness composite.
    // A composited pixel always carries the opaque alpha bit (the software
    // compositor never leaves a hole); 3D-only pixels use their own alpha.
    const bool srcA_3d = (cap & 0x01000000u) != 0;
    const uint32_t* const srcA = srcA_3d ? line3d : comp6;

    // Source B: LCDC VRAM bank (selected by DISPCNT's VRAM-block field) or
    // the main-memory display FIFO. The FIFO feed (0x04000068 + DMA mode 4)
    // is not implemented; an unfed melonDS FIFO buffer reads all-zero.
    static constexpr uint16_t kZeroLine[256] = {};
    const uint16_t* srcB = nullptr;
    uint32_t srcBaddr = line * 256u;
    if (cap & 0x02000000u) {
        srcB = kZeroLine;
        srcBaddr = 0;
    } else {
        const uint32_t srcbank = (u.dispcnt >> 18) & 3u;
        if (nds_vram_lcdc_mapped(srcbank))
            srcB = reinterpret_cast<const uint16_t*>(
                nds_vram_bank_data(srcbank));
        if (((u.dispcnt >> 16) & 3u) != 2u)
            srcBaddr += ((cap >> 26) & 3u) << 14;
    }
    dstaddr &= 0xFFFFu;
    srcBaddr &= 0xFFFFu;

    switch ((cap >> 29) & 3u) {
        case 0:  // source A only
            for (uint32_t i = 0; i < width; ++i) {
                const uint32_t val = srcA ? srcA[i] : 0;
                const uint32_t r = (val >> 1) & 0x1Fu;
                const uint32_t g = (val >> 9) & 0x1Fu;
                const uint32_t b = (val >> 17) & 0x1Fu;
                const uint32_t a = (!srcA_3d || (val >> 24)) ? 0x8000u : 0u;
                dstp[dstaddr] = static_cast<uint16_t>(r | (g << 5) |
                                                      (b << 10) | a);
                dstaddr = (dstaddr + 1u) & 0xFFFFu;
            }
            break;
        case 1:  // source B only
            for (uint32_t i = 0; i < width; ++i) {
                dstp[dstaddr] = srcB ? srcB[srcBaddr & 0xFFFFu] : 0;
                srcBaddr = (srcBaddr + 1u) & 0xFFFFu;
                dstaddr = (dstaddr + 1u) & 0xFFFFu;
            }
            break;
        default: {  // A+B blend with the capture EVA/EVB fields
            uint32_t eva = cap & 0x1Fu;
            uint32_t evb = (cap >> 8) & 0x1Fu;
            if (eva > 16u) eva = 16u;
            if (evb > 16u) evb = 16u;
            for (uint32_t i = 0; i < width; ++i) {
                const uint32_t val = srcA ? srcA[i] : 0;
                const uint32_t rA = (val >> 1) & 0x1Fu;
                const uint32_t gA = (val >> 9) & 0x1Fu;
                const uint32_t bA = (val >> 17) & 0x1Fu;
                const uint32_t aA = (!srcA_3d || (val >> 24)) ? 1u : 0u;
                uint32_t rD, gD, bD, aD;
                if (srcB) {
                    const uint16_t vb = srcB[srcBaddr & 0xFFFFu];
                    const uint32_t rB = vb & 0x1Fu;
                    const uint32_t gB = (vb >> 5) & 0x1Fu;
                    const uint32_t bB = (vb >> 10) & 0x1Fu;
                    const uint32_t aB = vb >> 15;
                    rD = ((rA * aA * eva) + (rB * aB * evb) + 8u) >> 4;
                    gD = ((gA * aA * eva) + (gB * aB * evb) + 8u) >> 4;
                    bD = ((bA * aA * eva) + (bB * aB * evb) + 8u) >> 4;
                    aD = (eva > 0 ? aA : 0u) | (evb > 0 ? aB : 0u);
                } else {
                    // Unmapped source-B bank: the B term is absent entirely
                    // (melonDS drops it rather than blending with black).
                    rD = ((rA * aA * eva) + 8u) >> 4;
                    gD = ((gA * aA * eva) + 8u) >> 4;
                    bD = ((bA * aA * eva) + 8u) >> 4;
                    aD = eva > 0 ? aA : 0u;
                }
                if (rD > 0x1Fu) rD = 0x1Fu;
                if (gD > 0x1Fu) gD = 0x1Fu;
                if (bD > 0x1Fu) bD = 0x1Fu;
                dstp[dstaddr] = static_cast<uint16_t>(rD | (gD << 5) |
                                                      (bD << 10) |
                                                      (aD << 15));
                srcBaddr = (srcBaddr + 1u) & 0xFFFFu;
                dstaddr = (dstaddr + 1u) & 0xFFFFu;
            }
            break;
        }
    }
    nds_vram_note_capture_write();
}

// True when this engine's line render takes the general (melonDS
// DrawScanline) path rather than one of the dst-direct fast paths. Shared by
// the render and by the latch, which must decide whether to fetch the 3D line
// under exactly the same condition (F5).
bool line_general_path(const Unit& u, int engine, bool bg0_3d, bool cap) {
    const uint32_t mode = (u.dispcnt >> 16) & (engine ? 1u : 3u);
    return bg0_3d || cap || mode != 1u ||
           (u.dispcnt & NDS_GPU2D_DISPCNT_WINDOW_ENABLE_MASK);
}

bool line_forceblank(const LineJob& job, int engine) {
    return !job.palette[engine] || !job.oam[engine] ||
           (job.unit[engine].dispcnt & 0x80u);
}

void render_engine_line(const LineJob& job, int engine, LineScratch& sc) {
    const Unit& u = job.unit[engine];
    const int y = job.y;
    Frame& fb = g_fb[job.buffer][job.screen[engine]];
    uint32_t* const dst = fb.data() + y * 256;
    const uint8_t* const palette = job.palette[engine];
    const uint8_t* const oam = job.oam[engine];
    if (line_forceblank(job, engine)) {
        std::fill_n(dst, 256, 0xFFFFFFFFu);
        return;
    }
    const NdsVramRendererView& vram = *job.vram[engine];
    const uint32_t mode=(u.dispcnt>>16)&(engine?1u:3u);

    const uint32_t capw = engine == 0 ? job.capw : 0u;
    const bool cap = engine == 0 && job.cap;
    const bool bg0_3d = engine == 0 && job.bg0_3d;

    if (line_general_path(u, engine, bg0_3d, cap)) {
        // General path: mirror melonDS DrawScanline ordering — composite
        // (when the display or capture consumes it), display-mode mux,
        // capture, master brightness on every mode except screen-off.
        const uint32_t* const line3d = (engine == 0) ? job.line3d : nullptr;
        std::array<uint32_t, 256>& comp6 = sc.comp6;
        const bool need_comp =
            mode == 1u || (cap && !(u.capture & 0x01000000u));
        if (need_comp)
            compose_line6(engine, y, u, palette, oam, vram, line3d,
                          bg0_3d, comp6.data(), sc);
        const uint32_t mbmode = u.master_bright >> 14;
        const uint32_t mb = std::min<uint32_t>(16, u.master_bright & 0x1Fu);
        auto bright = [&](uint32_t c6) {
            if (mbmode == 1u) return brighten(c6, mb, 0u);
            if (mbmode == 2u) return darken(c6, mb, 15u);
            return c6;
        };
        if (mode == 0u) {
            std::fill_n(dst, 256, 0xFFFFFFFFu);
        } else if (mode == 1u) {
            for (int x = 0; x < 256; ++x)
                dst[x] = to_rgb32(bright(comp6[x]));
        } else if (mode == 2u) {
            const uint32_t bank=(u.dispcnt>>18)&3u;
            const uint32_t base=0x06800000u+(bank<<17)+(y*256u)*2u;
            for(int x=0;x<256;++x){
                const uint16_t c=static_cast<uint16_t>(
                    nds_video_read(9,base+x*2u,2));
                dst[x]=to_rgb32(bright(rgb6(c)));
            }
        } else {
            // Main-memory display FIFO: the feed (0x04000068 + DMA mode 4)
            // is not implemented; an unfed FIFO displays black.
            std::fill_n(dst, 256, to_rgb32(bright(0u)));
        }
        if (cap) do_capture(u, y, capw, need_comp ? comp6.data() : nullptr,
                            line3d);
        return;
    }

    std::array<Pixel,256>& obj = sc.obj;
    const auto obj_start = profiling() ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
    render_obj_line(u,engine,y,obj.data(),256,oam,palette,vram);
    if (profiling()) {
        sc.obj_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - obj_start).count());
    }
    const uint16_t backdrop15 = view16(palette, 0);
    const Pixel backdrop{rgb6(backdrop15), 0x20u, 0, 4, 5, true};
    const uint32_t* const lut = rgb32_lut();
    // decoded, sorted front-first
    std::array<BgLine, 4>& bg_lines = sc.bg_lines;
    size_t bg_count = 0;
    int active_bgs[4];
    {
        const uint8_t active = bg_active_mask(u, false);
        for (int bg = 0; bg < 4; ++bg)
            if (active & (1u << bg)) active_bgs[bg_count++] = bg;
    }
    // Front-first order: lower BGCNT priority wins, ties break to the lower
    // BG index. Four elements maximum: insertion sort on the index list.
    {
        int order[4];
        uint8_t prio[4];
        for (size_t i = 0; i < bg_count; ++i) {
            order[i] = active_bgs[i];
            prio[i] = static_cast<uint8_t>(u.bgcnt[active_bgs[i]] & 3u);
        }
        for (size_t i = 1; i < bg_count; ++i) {
            const int bg = order[i];
            const uint8_t p = prio[i];
            size_t j = i;
            while (j && (p < prio[j - 1] ||
                         (p == prio[j - 1] && bg < order[j - 1]))) {
                order[j] = order[j - 1];
                prio[j] = prio[j - 1];
                --j;
            }
            order[j] = bg;
            prio[j] = p;
        }
        for (size_t i = 0; i < bg_count; ++i)
            decode_bg_line(u, engine, order[i], y, palette, vram,
                           bg_lines[i]);
    }
    const uint32_t mbmode=u.master_bright>>14;
    const uint32_t mb=std::min<uint32_t>(16,u.master_bright&0x1Fu);
    // Common firmware top-screen mode: OBJ over a backdrop, with no BG or
    // color effect enabled. There is no second layer to sort or blend.
    if (bg_count == 0 && u.bldcnt == 0 && mbmode == 0) {
        if (profiling()) ++sc.text_lines[engine][0];
        for (int x = 0; x < 256; ++x)
            dst[x] = obj[x].valid ? to_rgb32(obj[x].color)
                                  : lut[backdrop15];
        return;
    }
    auto ahead = [](const Pixel& a, const Pixel& b) {
        return a.priority < b.priority ||
               (a.priority == b.priority && a.order < b.order);
    };
    // With no color-effect mode, no second-target layers, and no master
    // brightness, only the front pixel matters. Firmware uses this state for
    // most menu scanlines; avoid maintaining a second candidate and running
    // the general blender for every pixel.
    if ((u.bldcnt & 0x3FC0u) == 0 && mbmode == 0) {
        if (profiling()) {
            ++sc.text_lines[engine][bg_count];
            ++sc.no_effect_lines[engine];
        }
        for (int x = 0; x < 256; ++x) {
            // BG layers are front-first: the first opaque pixel is the top
            // candidate; an OBJ pixel wins any priority tie (order 0).
            uint16_t top15 = backdrop15;
            uint8_t top_prio = 4;
            for (size_t i = 0; i < bg_count; ++i) {
                const uint16_t c = bg_lines[i].color[x];
                if (c & 0x8000u) {
                    top15 = c;
                    top_prio = bg_lines[i].prio;
                    break;
                }
            }
            if (obj[x].valid && obj[x].priority <= top_prio) {
                dst[x] = to_rgb32(obj[x].color);
            } else {
                dst[x] = lut[top15 & 0x7FFFu];
            }
        }
        return;
    }
    if (profiling()) ++sc.text_lines[engine][bg_count];
    for(int x=0;x<256;++x){
        Pixel top=backdrop, below=backdrop;
        bool have_top = false;
        for (size_t i = 0; i < bg_count; ++i) {
            const uint16_t c = bg_lines[i].color[x];
            if (!(c & 0x8000u)) continue;
            const BgLine& l = bg_lines[i];
            const Pixel pixel{rgb6(static_cast<uint16_t>(c & 0x7FFFu)),
                              l.target, 0, l.prio, l.order, true};
            if (!have_top) {
                top = pixel;
                have_top = true;
            } else {
                below = pixel;
                break;
            }
        }
        if (obj[x].valid) {
            if (ahead(obj[x], top)) {
                below=top;
                top=obj[x];
            } else if (ahead(obj[x], below)) {
                below=obj[x];
            }
        }
        uint32_t c=compose(u,top,below);
        if(mbmode==1)c=brighten(c,mb,0u);else if(mbmode==2)c=darken(c,mb,15u);
        dst[x]=to_rgb32(c);
    }
}

bool enabled(int engine){return (nds_powercontrol9()&(engine?0x0200u:0x0002u))!=0;}

uint32_t reg_read(const Unit& u,uint32_t off,uint32_t width){
    if(width==4){if(off==0)return u.dispcnt;if(off==0x64)return u.capture;return reg_read(u,off,2)|(reg_read(u,off+2,2)<<16);}
    if(width==2){
        if(off==0)return u.dispcnt&0xFFFFu;
        if(off==2)return u.dispcnt>>16;
        if(off>=8&&off<16&&!(off&1))return u.bgcnt[(off-8)>>1];
        if(off==0x48)return u.win[8]|(u.win[9]<<8);
        if(off==0x4A)return u.win[10]|(u.win[11]<<8);
        if(off==0x50)return u.bldcnt;
        if(off==0x52)return u.bldalpha;
        if(off==0x64)return u.capture&0xFFFFu;
        if(off==0x66)return u.capture>>16;
        if(off==0x6C)return u.master_bright;
        return 0;
    }
    if(off<4)return(u.dispcnt>>(off*8))&0xFFu;
    if(off>=8&&off<16)return(u.bgcnt[(off-8)>>1]>>((off&1)*8))&0xFFu;
    if(off>=0x48&&off<=0x4B)return u.win[8+(off-0x48)];
    return 0;
}

void reg_write16(Unit&u,int engine,uint32_t off,uint16_t v){
    if(off==0){u.dispcnt=(u.dispcnt&0xFFFF0000u)|v;if(engine)u.dispcnt&=0xC0B1FFF7u;return;}
    if(off==2){u.dispcnt=(u.dispcnt&0xFFFFu)|(uint32_t{v}<<16);if(engine)u.dispcnt&=0xC0B1FFF7u;return;}
    if(off==0x64){u.capture=(u.capture&0xFFFF0000u)|(v&0x1F1Fu);return;}
    if(off==0x66){u.capture=(u.capture&0xFFFFu)|((uint32_t{v}<<16)&0xEF3F0000u);return;}
    if(off==0x6C){u.master_bright=v;return;}
    // Engine A BG0HOFS doubles as the 3D scroll register, forwarded before
    // the power gate (melonDS Write16 case 0x010; the engine-side BGXPos
    // store below still happens when powered).
    if(off==0x10&&engine==0)nds_gpu3d_set_render_xpos(v);
    if(!enabled(engine))return;
    if(off>=8&&off<16){u.bgcnt[(off-8)>>1]=v;return;}
    if(off>=0x10&&off<0x20){uint16_t* p=((off-0x10)&2)?u.bgy:u.bgx;p[(off-0x10)>>2]=v;return;}
    if(off>=0x20&&off<0x40){
        int n=(off>=0x30);
        uint32_t sub=(off-(n?0x30:0x20));
        if(sub==0)u.pa[n]=v;
        else if(sub==2)u.pb[n]=v;
        else if(sub==4)u.pc[n]=v;
        else if(sub==6)u.pd[n]=v;
        else if(sub==8){
            u.refx[n]=(u.refx[n]&0xFFFF0000)|v;
            u.refx_internal[n]=u.refx[n];
        }
        else if(sub==0xA){
            if(v&0x800)v|=0xF000;
            u.refx[n]=(u.refx[n]&0xFFFF)|(uint32_t{v}<<16);
            u.refx_internal[n]=u.refx[n];
        } else if(sub==0xC){
            u.refy[n]=(u.refy[n]&0xFFFF0000)|v;
            u.refy_internal[n]=u.refy[n];
        }
        else if(sub==0xE){
            if(v&0x800)v|=0xF000;
            u.refy[n]=(u.refy[n]&0xFFFF)|(uint32_t{v}<<16);
            u.refy_internal[n]=u.refy[n];
        }
        return;
    }
    if(off>=0x40&&off<=0x4A){u.win[off-0x40]=v&0xFF;u.win[off-0x3F]=v>>8;return;}
    if(off==0x4C){u.bg_mosaic_x=v&0xF;u.bg_mosaic_y=(v>>4)&0xF;u.obj_mosaic_x=(v>>8)&0xF;u.obj_mosaic_y=v>>12;return;}
    if(off==0x50){u.bldcnt=v&0x3FFF;return;}if(off==0x52){u.bldalpha=v&0x1F1F;u.eva=std::min<uint8_t>(16,v&0x1F);u.evb=std::min<uint8_t>(16,(v>>8)&0x1F);return;}
    if(off==0x54){u.evy=std::min<uint8_t>(16,v&0x1F);return;}
}

void reg_write8(Unit& u, int engine, uint32_t off, uint8_t v) {
    if (off < 4) {
        const uint32_t shift = off * 8u;
        u.dispcnt = (u.dispcnt & ~(0xFFu << shift)) | (uint32_t{v} << shift);
        if (engine) u.dispcnt &= 0xC0B1FFF7u;
        return;
    }
    if (off == 0x10 && engine == 0)
        nds_gpu3d_set_render_xpos(static_cast<uint16_t>(
            (nds_gpu3d_render_xpos() & 0xFF00u) | v));
    else if (off == 0x11 && engine == 0)
        nds_gpu3d_set_render_xpos(static_cast<uint16_t>(
            (nds_gpu3d_render_xpos() & 0x00FFu) | (uint16_t{v} << 8)));
    if (!enabled(engine)) return;
    if (off >= 8 && off < 16) {
        uint16_t& reg = u.bgcnt[(off - 8) >> 1];
        const uint32_t shift = (off & 1u) * 8u;
        reg = static_cast<uint16_t>((reg & ~(0xFFu << shift)) |
                                    (uint16_t{v} << shift));
        return;
    }
    if (off >= 0x10 && off < 0x20) {
        uint16_t* reg = ((off - 0x10) & 2u) ? u.bgy : u.bgx;
        uint16_t& value = reg[(off - 0x10) >> 2];
        const uint32_t shift = (off & 1u) * 8u;
        value = static_cast<uint16_t>((value & ~(0xFFu << shift)) |
                                      (uint16_t{v} << shift));
        return;
    }
    if (off >= 0x40 && off <= 0x4B) {
        u.win[off - 0x40] = v;
        return;
    }
    if (off == 0x4C) {
        u.bg_mosaic_x = v & 0xFu;
        u.bg_mosaic_y = v >> 4;
        return;
    }
    if (off == 0x4D) {
        u.obj_mosaic_x = v & 0xFu;
        u.obj_mosaic_y = v >> 4;
        return;
    }
    if (off == 0x50) {
        u.bldcnt = (u.bldcnt & 0x3F00u) | v;
        return;
    }
    if (off == 0x51) {
        u.bldcnt = (u.bldcnt & 0x00FFu) | (uint16_t{v} << 8);
        return;
    }
    if (off == 0x52) {
        u.bldalpha = (u.bldalpha & 0x1F00u) | (v & 0x1Fu);
        u.eva = std::min<uint8_t>(16, v & 0x1Fu);
        return;
    }
    if (off == 0x53) {
        u.bldalpha = static_cast<uint16_t>((u.bldalpha & 0x001Fu) |
                                           ((uint16_t{v} & 0x1Fu) << 8));
        u.evb = std::min<uint8_t>(16, v & 0x1Fu);
        return;
    }
    if (off == 0x54) u.evy = std::min<uint8_t>(16, v & 0x1Fu);
}


// ---- Latch / execute -----------------------------------------------------

uint64_t ns_since(std::chrono::steady_clock::time_point a,
                  std::chrono::steady_clock::time_point b) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

// Scheduler-thread latch. Everything a line render reads that a later guest
// write could change is captured here, and the two pieces of renderer-owned
// mutable state -- the BG2/BG3 affine accumulators and the DISPCAPCNT enable
// latch -- are advanced here and nowhere else.
void latch_line(int y, LineJob& job) {
    const bool engine_a_on_top = (nds_powercontrol9() & 0x8000u) != 0;
    job.y = y;
    job.buffer = static_cast<uint32_t>(g_front ^ 1);
    job.line3d = nullptr;
    job.direct_class = static_cast<uint8_t>(g_direct_frame_class);
    job.direct_extra_bg_mask = g_direct_extra_bg_mask;
    for (int e = 0; e < 2; ++e) {
        job.unit[e] = g_unit[e];
        job.screen[e] = static_cast<uint32_t>(engine_a_on_top ? e : (e ^ 1));
        job.palette[e] = nds_vram_renderer_palette(e);
        job.oam[e] = nds_vram_renderer_oam(e);
        job.vram[e] = nds_vram_renderer_view(e);
    }
    job.engine_active[0] = !g_direct_frame_active;
    job.engine_active[1] = true;

    // DISPCAPCNT latches at the top of the frame; setting the enable bit
    // mid-frame does not capture until the next frame. melonDS's arm skips
    // only its own force-blank cases (VCount>192, engine-B power-off),
    // neither of which applies to engine A here. This ran inside the engine-A
    // render before the force-blank early-out, and it is guest-readable
    // through DISPCAPCNT, so it belongs on this thread.
    if (y == 0 && (g_unit[0].capture & 0x80000000u))
        g_unit[0].capture_latch = true;
    job.unit[0].capture_latch = g_unit[0].capture_latch;

    job.cap = false;
    job.capw = 0;
    if (job.unit[0].capture_latch) {
        static constexpr uint16_t kCapW[4] = {128, 256, 256, 256};
        static constexpr uint8_t kCapH[4] = {128, 64, 128, 192};
        const uint32_t size = (job.unit[0].capture >> 20) & 3u;
        job.capw = kCapW[size];
        job.cap = y < kCapH[size];
    }
    job.bg0_3d = (job.unit[0].dispcnt & 0x8u) != 0 &&
                 (job.unit[0].dispcnt & 0x100u) != 0;

    // The 3D line and the wide-3D snapshot are pulled here, in scanline order,
    // under exactly the condition the inline render used: GPU3D::GetLine writes
    // a single shared scratch buffer and SoftRenderer::GetLine consumes
    // scanline-semaphore tokens, so no other thread may call it.
    const bool engine_a_renders =
        job.engine_active[0] && !line_forceblank(job, 0);
    if (engine_a_renders &&
        line_general_path(job.unit[0], 0, job.bg0_3d, job.cap)) {
        const uint32_t* const line3d = nds_gpu3d_line(y);
        if (line3d) {
            std::copy_n(line3d, 256, job.line3d_storage.data());
            job.line3d = job.line3d_storage.data();
        }
        if (job.bg0_3d) {
            const uint16_t wide_width = nds_gpu3d_output_width();
            if (wide_width > 256u &&
                wide_width <= static_cast<uint16_t>(kMaxAdaptiveWidth)) {
                const size_t offset = static_cast<size_t>(y) * wide_width;
                std::copy_n(nds_gpu3d_wide_line(y), wide_width,
                            g_wide_3d_frame[job.buffer].data() + offset);
                if (g_adaptive_skybox_fill) {
                    const uint32_t* const attr = nds_gpu3d_wide_attr_line(y);
                    if (attr)
                        std::copy_n(attr, wide_width,
                                    g_wide_3d_attr_frame[job.buffer].data() +
                                        offset);
                }
                g_wide_3d_width[job.buffer] = wide_width;
            }
        }
    }

    // Affine reference accumulators advance once per decoded line, per layer.
    for (int e = 0; e < 2; ++e) {
        if (!job.engine_active[e] || line_forceblank(job, e)) continue;
        const Unit& snapshot = job.unit[e];
        const bool bg0_3d_e = (e == 0) && job.bg0_3d;
        const bool cap_e = (e == 0) && job.cap;
        if (line_general_path(snapshot, e, bg0_3d_e, cap_e)) {
            const uint32_t mode = (snapshot.dispcnt >> 16) & (e ? 1u : 3u);
            const bool need_comp =
                mode == 1u || (cap_e && !(snapshot.capture & 0x01000000u));
            if (!need_comp) continue;   // nothing decoded, nothing advances
        }
        const uint8_t active = bg_active_mask(snapshot, bg0_3d_e);
        for (int bg = 2; bg < 4; ++bg)
            if (active & (1u << bg)) advance_affine_line(g_unit[e], bg);
    }
}

void render_line_job(const LineJob& job, LineScratch& sc) {
    if (!profiling()) {
        if (job.engine_active[0]) render_engine_line(job, 0, sc);
        if (job.engine_active[1]) render_engine_line(job, 1, sc);
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    if (job.engine_active[0]) render_engine_line(job, 0, sc);
    const auto middle = std::chrono::steady_clock::now();
    if (job.engine_active[1]) render_engine_line(job, 1, sc);
    const auto finish = std::chrono::steady_clock::now();
    sc.render_ns += ns_since(start, finish);
    const uint64_t engine_a_ns = ns_since(start, middle);
    sc.engine_ns[0] += engine_a_ns;
    sc.direct_class_engine_a_ns[job.direct_class] += engine_a_ns;
    if (job.direct_class == NDS_GPU2D_DIRECT_EXTRA_BG)
        sc.direct_extra_bg_mask_engine_a_ns[job.direct_extra_bg_mask] +=
            engine_a_ns;
    sc.engine_ns[1] += ns_since(middle, finish);
    ++sc.scanlines;
}

// Fold a scratch's profiling accumulators into the module counters. Called on
// the scheduler thread at a drain, so the counters never tear.
void merge_scratch(LineScratch& sc) {
    if (!sc.scanlines && !sc.render_ns && !sc.obj_ns) return;
    g_render_ns += sc.render_ns;
    g_obj_ns += sc.obj_ns;
    g_render_scanlines += sc.scanlines;
    for (int e = 0; e < 2; ++e) {
        g_engine_ns[e] += sc.engine_ns[e];
        g_no_effect_lines[e] += sc.no_effect_lines[e];
        for (int i = 0; i < 5; ++i)
            g_text_lines[e][i] += sc.text_lines[e][i];
    }
    for (uint32_t i = 0; i < NDS_GPU2D_DIRECT_CLASS_COUNT; ++i)
        g_direct_class_engine_a_ns[i] += sc.direct_class_engine_a_ns[i];
    for (uint32_t i = 0; i < NDS_GPU2D_DIRECT_BG_MASK_COUNT; ++i)
        g_direct_extra_bg_mask_engine_a_ns[i] +=
            sc.direct_extra_bg_mask_engine_a_ns[i];
    sc.render_ns = 0;
    sc.obj_ns = 0;
    sc.scanlines = 0;
    std::memset(sc.engine_ns, 0, sizeof(sc.engine_ns));
    std::memset(sc.text_lines, 0, sizeof(sc.text_lines));
    std::memset(sc.no_effect_lines, 0, sizeof(sc.no_effect_lines));
    std::memset(sc.direct_class_engine_a_ns, 0,
                sizeof(sc.direct_class_engine_a_ns));
    std::memset(sc.direct_extra_bg_mask_engine_a_ns, 0,
                sizeof(sc.direct_extra_bg_mask_engine_a_ns));
}

LineJob g_latch_job{};
LineScratch g_inline_scratch{};


// ---- Worker pool ---------------------------------------------------------
// Topology mirrors the in-tree GPU3D soft-renderer contract: an atomic
// progress counter plus a condition-variable wait, with the scheduler thread
// helping rather than idling at a drain.

constexpr uint32_t kJobSlots = 256;   // > 192 lines per frame, power of two

struct Pool {
    std::array<LineJob, kJobSlots> jobs{};
    // head: slots published by the scheduler thread.
    // claim: slots handed out to a renderer (worker or a helping drain).
    // done: slots whose render has returned.
    std::atomic<uint64_t> head{0};
    std::atomic<uint64_t> claim{0};
    std::atomic<uint64_t> done{0};
    std::atomic<bool> stop{false};
    std::mutex m;
    std::condition_variable work_cv;
    std::condition_variable done_cv;
    std::vector<std::thread> threads;
    std::vector<std::unique_ptr<LineScratch>> scratch;
};

std::unique_ptr<Pool> g_pool;
bool g_threaded_requested = false;
unsigned g_worker_count = 1;

uint64_t g_threaded_lines = 0;
uint64_t g_inline_lines = 0;
uint64_t g_fence_drains[NDS_GPU2D_FENCE_CAUSE_COUNT] = {};
uint64_t g_fenced_lines[NDS_GPU2D_FENCE_CAUSE_COUNT] = {};
uint64_t g_fence_wait_ns = 0;
uint64_t g_fence_helped_lines = 0;

void worker_main(Pool* pool, LineScratch* sc) {
    for (;;) {
        uint64_t index = 0;
        bool have = false;
        for (;;) {
            if (pool->stop.load(std::memory_order_acquire)) return;
            uint64_t c = pool->claim.load(std::memory_order_acquire);
            const uint64_t h = pool->head.load(std::memory_order_acquire);
            if (c < h) {
                if (pool->claim.compare_exchange_weak(
                        c, c + 1, std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    index = c;
                    have = true;
                    break;
                }
                continue;
            }
            std::unique_lock<std::mutex> lock(pool->m);
            if (pool->stop.load(std::memory_order_acquire)) return;
            if (pool->claim.load(std::memory_order_acquire) >=
                pool->head.load(std::memory_order_acquire))
                pool->work_cv.wait(lock);
        }
        if (!have) continue;
        render_line_job(pool->jobs[index % kJobSlots], *sc);
        pool->done.fetch_add(1, std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> lock(pool->m);
        }
        pool->done_cv.notify_all();
    }
}

void stop_pool() {
    if (!g_pool) return;
    Pool* const pool = g_pool.get();
    {
        std::lock_guard<std::mutex> lock(pool->m);
        pool->stop.store(true, std::memory_order_release);
    }
    pool->work_cv.notify_all();
    for (auto& t : pool->threads)
        if (t.joinable()) t.join();
    for (auto& sc : pool->scratch) merge_scratch(*sc);
    g_pool.reset();
}

void start_pool() {
    if (g_pool || !g_threaded_requested) return;
    g_pool = std::make_unique<Pool>();
    Pool* const pool = g_pool.get();
    const unsigned n = g_worker_count ? g_worker_count : 1u;
    pool->scratch.reserve(n);
    pool->threads.reserve(n);
    for (unsigned i = 0; i < n; ++i)
        pool->scratch.push_back(std::make_unique<LineScratch>());
    for (unsigned i = 0; i < n; ++i)
        pool->threads.emplace_back(worker_main, pool, pool->scratch[i].get());
}

// Render every published-but-unrendered job, helping rather than idling, then
// wait for any job a worker is still inside.
void drain_pool(uint32_t cause) {
    Pool* const pool = g_pool.get();
    if (!pool) return;
    const uint64_t target = pool->head.load(std::memory_order_acquire);
    const uint64_t already = pool->done.load(std::memory_order_acquire);
    if (already >= target) {
        nds_gpu2d_jobs_outstanding.store(0u, std::memory_order_relaxed);
        return;
    }
    if (cause < NDS_GPU2D_FENCE_CAUSE_COUNT) {
        ++g_fence_drains[cause];
        g_fenced_lines[cause] += target - already;
    }
    const auto wait_start = std::chrono::steady_clock::now();
    for (;;) {
        uint64_t c = pool->claim.load(std::memory_order_acquire);
        if (c >= target) break;
        if (!pool->claim.compare_exchange_weak(c, c + 1,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire))
            continue;
        render_line_job(pool->jobs[c % kJobSlots], g_inline_scratch);
        ++g_fence_helped_lines;
        pool->done.fetch_add(1, std::memory_order_acq_rel);
    }
    while (pool->done.load(std::memory_order_acquire) < target) {
        std::unique_lock<std::mutex> lock(pool->m);
        if (pool->done.load(std::memory_order_acquire) >= target) break;
        pool->done_cv.wait_for(lock, std::chrono::microseconds(200));
    }
    g_fence_wait_ns += ns_since(wait_start, std::chrono::steady_clock::now());
    nds_gpu2d_jobs_outstanding.store(0u, std::memory_order_relaxed);
    for (auto& sc : pool->scratch) merge_scratch(*sc);
    merge_scratch(g_inline_scratch);
}

// A capture-enabled frame writes back into guest VRAM (DoCapture) and bumps
// the texture generation the 3D engine reads, so it renders inline.
bool line_must_be_inline(const LineJob& job) {
    return job.cap || job.unit[0].capture_latch ||
           (job.unit[0].capture & 0x80000000u) != 0u;
}

// g_pool holds joinable std::threads; destroying it with the workers still
// running would call std::terminate. This guard is declared after g_pool so it
// is destroyed first, joining them at process exit.
struct PoolShutdownGuard {
    ~PoolShutdownGuard() { stop_pool(); }
};
PoolShutdownGuard g_pool_shutdown_guard{};

void submit_line(int y) {
    if (!g_pool) {
        latch_line(y, g_latch_job);
        render_line_job(g_latch_job, g_inline_scratch);
        merge_scratch(g_inline_scratch);
        ++g_inline_lines;
        return;
    }
    Pool* const pool = g_pool.get();
    // Reserve the slot before latching into it.
    const uint64_t head = pool->head.load(std::memory_order_relaxed);
    if (head - pool->done.load(std::memory_order_acquire) >= kJobSlots)
        drain_pool(NDS_GPU2D_FENCE_SLOTS);
    LineJob& job = pool->jobs[head % kJobSlots];
    latch_line(y, job);
    if (line_must_be_inline(job)) {
        // Everything already published must land before a capture line, so
        // the capture sees the same VRAM the inline path would have.
        drain_pool(NDS_GPU2D_FENCE_FRAME);
        render_line_job(job, g_inline_scratch);
        merge_scratch(g_inline_scratch);
        ++g_inline_lines;
        return;
    }
    pool->head.store(head + 1, std::memory_order_release);
    nds_gpu2d_jobs_outstanding.store(1u, std::memory_order_relaxed);
    ++g_threaded_lines;
    pool->work_cv.notify_one();
}

} // namespace

void nds_gpu2d_reset(){
    drain_pool(NDS_GPU2D_FENCE_FRAME);
    g_unit={};
    g_front = 0;
    g_direct_frame_active = false;
    g_direct_present_frame_active = false;
    g_frame_capture_active = false;
    g_present_capture_active = false;
    g_direct_current_frame = {};
    g_direct_present_frame = {};
    g_direct_object_write = 0;
    g_direct_force_cpu_frames = 0;
    g_render_ns = 0;
    g_obj_ns = 0;
    g_engine_ns[0] = g_engine_ns[1] = 0;
    std::memset(g_text_lines, 0, sizeof(g_text_lines));
    g_no_effect_lines[0] = g_no_effect_lines[1] = 0;
    g_render_scanlines = 0;
    g_direct_frames = 0;
    g_direct_overlay_ns = 0;
    std::fill_n(g_direct_class_frames,
                NDS_GPU2D_DIRECT_CLASS_COUNT, 0u);
    std::fill_n(g_direct_class_engine_a_ns,
                NDS_GPU2D_DIRECT_CLASS_COUNT, 0u);
    std::fill_n(g_direct_extra_bg_mask_frames,
                NDS_GPU2D_DIRECT_BG_MASK_COUNT, 0u);
    std::fill_n(g_direct_extra_bg_mask_engine_a_ns,
                NDS_GPU2D_DIRECT_BG_MASK_COUNT, 0u);
    std::fill_n(g_direct_extra_bg_mode_frames,
                NDS_GPU2D_DIRECT_BG_MODE_COUNT, 0u);
    std::fill_n(g_direct_extra_effect_frames,
                NDS_GPU2D_DIRECT_EFFECT_MODE_COUNT, 0u);
    std::fill_n(g_direct_extra_master_bright_frames,
                NDS_GPU2D_DIRECT_EFFECT_MODE_COUNT, 0u);
    g_direct_class_transitions = 0;
    g_threaded_lines = 0;
    g_inline_lines = 0;
    g_fence_wait_ns = 0;
    g_fence_helped_lines = 0;
    std::fill_n(g_fence_drains, NDS_GPU2D_FENCE_CAUSE_COUNT, 0u);
    std::fill_n(g_fenced_lines, NDS_GPU2D_FENCE_CAUSE_COUNT, 0u);
    g_direct_frame_class = NDS_GPU2D_DIRECT_DISABLED;
    g_direct_previous_class = NDS_GPU2D_DIRECT_CLASS_COUNT;
    g_direct_extra_bg_mask = 0;
    for (auto& buffers : g_fb)
        for (auto& frame : buffers)
            frame.fill(0xFFFFFFFFu);
    for (auto& frame : g_wide_3d_frame) frame.fill(0u);
    for (auto& frame : g_wide_3d_attr_frame) frame.fill(0u);
    g_wide_3d_width.fill(0u);
    for (auto& frame : g_direct_object_frame) frame.fill(0u);
}
void nds_gpu2d_stop(){
    drain_pool(NDS_GPU2D_FENCE_FRAME);
    for (auto& buffers : g_fb)
        for (auto& frame : buffers)
            frame.fill(0u);
}
uint32_t nds_gpu2d_read(uint32_t addr,uint32_t width){const int e=(addr&0x1000u)?1:0;return reg_read(g_unit[e],addr&0xFFFu,width);}
void nds_gpu2d_write(uint32_t addr,uint32_t value,uint32_t width){
    const int e=(addr&0x1000u)?1:0;Unit&u=g_unit[e];const uint32_t off=addr&0xFFFu;
    if(width==4){if(off==0){u.dispcnt=value;if(e)u.dispcnt&=0xC0B1FFF7u;return;}if(off==0x64){u.capture=value&0xEF3F1F1Fu;return;}reg_write16(u,e,off,value);reg_write16(u,e,off+2,value>>16);return;}
    if(width==2){reg_write16(u,e,off,value);return;}
    reg_write8(u, e, off, static_cast<uint8_t>(value));
}
std::atomic<uint32_t> nds_gpu2d_jobs_outstanding{0};

const char* nds_gpu2d_fence_cause_name(uint32_t index) {
    static const char* const kNames[NDS_GPU2D_FENCE_CAUSE_COUNT] = {
        "vram", "vramcnt", "palette", "oam", "frame", "present", "slots"};
    return index < NDS_GPU2D_FENCE_CAUSE_COUNT ? kNames[index] : "?";
}

void nds_gpu2d_drain(uint32_t cause) { drain_pool(cause); }

void nds_gpu2d_set_threaded(bool enabled, unsigned workers) {
    if (g_threaded_requested == enabled &&
        g_worker_count == (workers ? workers : 1u))
        return;
    stop_pool();
    g_threaded_requested = enabled;
    g_worker_count = workers ? workers : 1u;
    start_pool();
}

bool nds_gpu2d_threaded() { return g_pool != nullptr; }

void nds_gpu2d_shutdown_workers() { stop_pool(); }

void nds_gpu2d_render_scanline(int line) {
    if (line < 0 || line >= 192) return;
    submit_line(line);
}
void nds_gpu2d_render_frame(){
    for (int line = 0; line < 192; ++line)
        nds_gpu2d_render_scanline(line);
}
void prepare_direct_frame() {
    g_direct_current_frame = {};
    if (!g_direct_frame_active) return;
    Unit& u = g_unit[0];
    const uint8_t* const palette = nds_vram_renderer_palette(0);
    const uint8_t* const oam = nds_vram_renderer_oam(0);
    const NdsVramRendererView* const vram = nds_vram_renderer_view(0);
    const int output_width = nds_gpu3d_output_width();
    if (!palette || !oam || !vram || output_width <= 256 ||
        output_width > kMaxAdaptiveWidth) {
        g_direct_frame_active = false;
        return;
    }

    const auto start = profiling() ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
    g_direct_object_write ^= 1u;
    AdaptiveFrame& object_frame =
        g_direct_object_frame[g_direct_object_write];
    std::array<Pixel, kMaxAdaptiveWidth> obj{};
    for (int y = 0; y < 192; ++y) {
        render_obj_line(u, 0, y, obj.data(), output_width,
                        oam, palette, *vram);
        uint32_t* const dst = object_frame.data() +
            static_cast<size_t>(y) * output_width;
        for (int x = 0; x < output_width; ++x) {
            const Pixel& pixel = obj[x];
            if (!pixel.valid) {
                dst[x] = 0u;
                continue;
            }
            const uint32_t r = pixel.color & 0x3Fu;
            const uint32_t g = (pixel.color >> 8) & 0x3Fu;
            const uint32_t b = ((pixel.color >> 16) & 0x3Fu) |
                (static_cast<uint32_t>(pixel.priority & 3u) << 6);
            const uint32_t alpha_code =
                pixel.alpha == 0u ? 17u :
                pixel.alpha == 0xFFu ? 0xFFu :
                std::min<uint32_t>(16u, pixel.alpha);
            dst[x] = r | (g << 8) | (b << 16) |
                     (alpha_code << 24);
        }
    }

    g_direct_current_frame.object_pixels = object_frame.data();
    g_direct_current_frame.width = static_cast<uint16_t>(output_width);
    g_direct_current_frame.backdrop_color = rgb6(view16(palette, 0));
    g_direct_current_frame.bldcnt = u.bldcnt;
    g_direct_current_frame.master_bright = u.master_bright;
    g_direct_current_frame.eva = u.eva;
    g_direct_current_frame.evb = u.evb;
    g_direct_current_frame.evy = u.evy;
    g_direct_current_frame.priority_3d =
        static_cast<uint8_t>(u.bgcnt[0] & 3u);
    g_direct_current_frame.render_xpos = nds_gpu3d_render_xpos();
    if (profiling()) {
        g_direct_overlay_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());
    }
}

void nds_gpu2d_start_frame(){
    drain_pool(NDS_GPU2D_FENCE_FRAME);
    // The frontend presents g_front after this boundary, while rasterization
    // has already advanced to the next back buffer. Preserve the completed
    // frame's host-only presentation decision and descriptor before updating
    // the current frame. Without this one-frame latch, capture/logo
    // transitions alternate between an unrendered CPU buffer and the wrong
    // direct surface.
    g_direct_present_frame_active = g_direct_frame_active;
    g_direct_present_frame = g_direct_current_frame;
    g_present_capture_active = g_frame_capture_active;
    const bool force_cpu = g_direct_force_cpu_frames != 0u;
    if (force_cpu) --g_direct_force_cpu_frames;
    g_direct_frame_class =
        force_cpu ? NDS_GPU2D_DIRECT_FORCE_CPU : direct_scene_class();
    g_direct_extra_bg_mask = static_cast<uint8_t>(
        (g_unit[0].dispcnt >> 9u) & 7u);
    g_direct_frame_active =
        g_direct_frame_class == NDS_GPU2D_DIRECT_SUPPORTED;
    g_frame_capture_active =
        (g_unit[0].capture & 0x80000000u) != 0u ||
        g_unit[0].capture_latch;
    prepare_direct_frame();
    if (g_direct_frame_active) ++g_direct_frames;
    if (profiling()) {
        ++g_direct_class_frames[g_direct_frame_class];
        if (g_direct_previous_class != NDS_GPU2D_DIRECT_CLASS_COUNT &&
            g_direct_previous_class != g_direct_frame_class)
            ++g_direct_class_transitions;
        g_direct_previous_class = g_direct_frame_class;
        if (g_direct_frame_class == NDS_GPU2D_DIRECT_EXTRA_BG) {
            ++g_direct_extra_bg_mask_frames[g_direct_extra_bg_mask];
            ++g_direct_extra_bg_mode_frames[g_unit[0].dispcnt & 7u];
            ++g_direct_extra_effect_frames[
                (g_unit[0].bldcnt >> 6u) & 3u];
            ++g_direct_extra_master_bright_frames[
                (g_unit[0].master_bright >> 14u) & 3u];
        }
    }
    for (auto& u : g_unit) {
        for (int affine = 0; affine < 2; ++affine) {
            u.refx_internal[affine] = u.refx[affine];
            u.refy_internal[affine] = u.refy[affine];
        }
    }
}
void nds_gpu2d_finish_frame(){
    drain_pool(NDS_GPU2D_FENCE_FRAME);
    g_front ^= 1;
}
void nds_gpu2d_vblank(){
    drain_pool(NDS_GPU2D_FENCE_FRAME);
    // melonDS Unit::VBlank: the capture enable bit auto-clears at line 192
    // only if it latched at line 0 this frame.
    for (auto& u : g_unit) {
        if (u.capture_latch) {
            u.capture &= ~0x80000000u;
            u.capture_latch = false;
        }
    }
}
const uint32_t* nds_gpu2d_framebuffer(int screen){
    drain_pool(NDS_GPU2D_FENCE_PRESENT);
    return g_fb[g_front][screen & 1].data();
}
void nds_gpu2d_set_adaptive_skybox_fill(bool enabled) {
    g_adaptive_skybox_fill = enabled;
}

void nds_gpu2d_set_direct_present(bool enabled) {
    g_direct_present_enabled = enabled;
    if (!enabled) {
        g_direct_frame_active = false;
        g_direct_present_frame_active = false;
        g_direct_current_frame = {};
        g_direct_present_frame = {};
    }
}

bool nds_gpu2d_direct_frame_active() {
    return g_direct_frame_active;
}

bool nds_gpu2d_direct_present_frame_active() {
    return g_direct_present_frame_active;
}

bool nds_gpu2d_requires_3d_readback() {
    return !g_direct_frame_active;
}

void nds_gpu2d_force_cpu_frames(uint32_t frames) {
    g_direct_force_cpu_frames =
        std::max(g_direct_force_cpu_frames, frames);
}

bool nds_gpu2d_direct_frame(NdsGpu2dDirectFrame* out) {
    drain_pool(NDS_GPU2D_FENCE_PRESENT);
    if (!out || !g_direct_present_frame_active ||
        !g_direct_present_frame.object_pixels) return false;
    *out = g_direct_present_frame;
    return true;
}

void nds_gpu2d_set_adaptive_hud_anchor(bool enabled,
                                       uint16_t center_width) {
    g_adaptive_hud_anchor = enabled;
    g_adaptive_hud_center_width =
        std::clamp<int>(center_width, 8, 256);
}

void nds_gpu2d_set_adaptive_center_native(bool enabled) {
    g_adaptive_center_native = enabled;
}

void nds_gpu2d_set_adaptive_center_max_polygons(uint32_t max_polygons) {
    g_adaptive_center_max_polygons = max_polygons;
}

const uint32_t* nds_gpu2d_adaptive_framebuffer(int screen, uint16_t* width) {
    drain_pool(NDS_GPU2D_FENCE_PRESENT);
    const uint32_t* native = nds_gpu2d_framebuffer(screen);
    const bool engine_a_on_top = (nds_powercontrol9() & 0x8000u) != 0;
    const int engine = engine_a_on_top ? screen : (screen ^ 1);
    // Cleared so every early return below (native width, unsupported scene)
    // leaves the presenter on the CPU composite rather than re-reading last
    // frame's surfaces. Guarded on engine A: this function is called once per
    // screen, and an unguarded reset here would let the engine-B call wipe
    // the flag the engine-A call just set, silently disabling HD entirely.
    if (engine == 0) g_hd_frame_valid = false;
    const int output_width = nds_gpu3d_output_width();
    if (output_width <= 256) {
        if (width) *width = 256;
        return native;
    }

    AdaptiveFrame& adaptive = g_adaptive_frame[screen & 1];
    if (width) *width = static_cast<uint16_t>(output_width);
    std::fill_n(adaptive.data(),
                static_cast<size_t>(output_width) * 192u,
                0xFF000000u);
    const int extra = (output_width - 256) / 2;
    if (engine != 0) {
        for (int y = 0; y < 192; ++y)
            std::copy_n(native + y * 256, 256,
                        adaptive.data() + y * output_width + extra);
        return adaptive.data();
    }
    if (g_adaptive_center_native) {
        for (int y = 0; y < 192; ++y)
            std::copy_n(native + y * 256, 256,
                        adaptive.data() + y * output_width + extra);
        return adaptive.data();
    }
    if (g_adaptive_center_max_polygons != 0u &&
        nds_gpu3d_render_polygon_count() <= g_adaptive_center_max_polygons) {
        for (int y = 0; y < 192; ++y)
            std::copy_n(native + y * 256, 256,
                        adaptive.data() + y * output_width + extra);
        return adaptive.data();
    }

    Unit& u = g_unit[0];
    const uint8_t* const palette = nds_vram_renderer_palette(0);
    const uint8_t* const oam = nds_vram_renderer_oam(0);
    const NdsVramRendererView* const vram = nds_vram_renderer_view(0);
    const uint32_t mode = (u.dispcnt >> 16) & 3u;
    const bool bg0_3d = (u.dispcnt & 0x8u) != 0 &&
                        (u.dispcnt & 0x100u) != 0;
    // Known-safe adaptive scenes use main-engine 3D plus OBJ. Text BG planes
    // can either be title-anchored into left/center/right bands or kept as a
    // native-width centered overlay. Affine/bitmap backgrounds still retain a
    // centered native image; windowed text scenes use the same native stack
    // filtering as the regular compositor below.
    bool supported_hud_bgs = true;
    for (int bg = 1; bg < 4; ++bg) {
        if ((u.dispcnt & (0x100u << bg)) &&
            bg_kind(u, bg) != BgKind::Text) {
            supported_hud_bgs = false;
            break;
        }
    }
    const NdsGpu2dWindowState window_state{u.dispcnt, u.win};
    const bool windows_supported = nds_gpu2d_windows_support_hd(window_state);
    const bool supported_scene =
        palette && oam && vram && !(u.dispcnt & 0x80u) &&
        mode == 1u && bg0_3d && supported_hud_bgs && windows_supported &&
        nds_gpu3d_render_xpos() == 0u && !g_present_capture_active &&
        !nds_title_patches_mph_adaptive_centered_native();
    if (!supported_scene) {
        for (int y = 0; y < 192; ++y)
            std::copy_n(native + y * 256, 256,
                        adaptive.data() + y * output_width + extra);
        return adaptive.data();
    }

    const uint16_t backdrop15 = view16(palette, 0);
    const Pixel backdrop{
        rgb6(backdrop15), 0x20u, 0, 4, 5, true};
    const uint8_t prio3d = static_cast<uint8_t>(u.bgcnt[0] & 3u);
    const uint32_t mbmode = u.master_bright >> 14;
    const uint32_t mb =
        std::min<uint32_t>(16, u.master_bright & 0x1Fu);
    std::array<Pixel, kMaxAdaptiveWidth> obj{};
    std::array<uint8_t, kMaxAdaptiveWidth> obj_window{};
    std::array<int16_t, kMaxAdaptiveWidth> sky_left{};
    std::array<int16_t, kMaxAdaptiveWidth> sky_indices{};
    std::array<int16_t, kMaxAdaptiveWidth> sky_rank{};
    std::array<int16_t, kMaxAdaptiveWidth> black_run{};
    std::array<uint32_t, kMaxAdaptiveWidth> repaired_3d{};
    std::array<BgLine, 3> hud_bg_lines{};
    int hud_bgs[3]{};
    size_t hud_bg_count = 0;
    for (int bg = 1; bg < 4; ++bg) {
        if (u.dispcnt & (0x100u << bg))
            hud_bgs[hud_bg_count++] = bg;
    }
    const bool snapshot_matches =
        g_wide_3d_width[g_front] == output_width;
    // The skybox repair rewrites the native 3D line before compositing. The
    // presenter samples the raw hi-res surface and cannot see that repair, so
    // rather than silently dropping it, HD stands down for those frames and
    // the CPU composite is presented instead.
    // screen == 0 as well as engine == 0: the direct presenter owns the top
    // window only, so when POWCNT routes engine A to the bottom screen these
    // surfaces would be applied to the wrong window. The direct path rejects
    // that case as SCREEN_ROUTE for the same reason.
    const bool emit_hd =
        g_hd_emit_enabled && !g_adaptive_skybox_fill && screen == 0;
    if (emit_hd) {
        const size_t needed =
            static_cast<size_t>(output_width) * 192u * 2u;
        if (g_hd_top_pixels.size() != needed) {
            g_hd_top_pixels.resize(needed);
            g_hd_below_pixels.resize(needed);
        }
    }
    for (int y = 0; y < 192; ++y) {
        render_obj_line(u, 0, y, obj.data(), output_width,
                        oam, palette, *vram, obj_window.data());
        for (size_t i = 0; i < hud_bg_count; ++i)
            decode_bg_line(u, 0, hud_bgs[i], y, palette, *vram,
                           hud_bg_lines[i]);
        const uint32_t* const line3d = snapshot_matches
            ? g_wide_3d_frame[g_front].data() +
                  static_cast<size_t>(y) * output_width
            : nds_gpu3d_wide_line(y);
        const uint32_t* const attr3d =
            !g_adaptive_skybox_fill ? nullptr
            : snapshot_matches
                ? g_wide_3d_attr_frame[g_front].data() +
                      static_cast<size_t>(y) * output_width
                : nds_gpu3d_wide_attr_line(y);
        const uint32_t* composited_3d = line3d;
        if (attr3d) {
            int last_sky = -1;
            int sky_count = 0;
            int previous = -1;
            std::fill_n(sky_rank.data(), output_width,
                        static_cast<int16_t>(-1));
            for (int x = 0; x < output_width; ++x) {
                const uint32_t rgb = line3d[x] & 0x003F3F3Fu;
                const bool sky =
                    ((line3d[x] >> 24) & 0x1Fu) != 0u &&
                    rgb != 0u &&
                    (attr3d[x] & 0x3F000000u) == 0x02000000u;
                if (sky) {
                    last_sky = x;
                    previous = x;
                    sky_indices[sky_count] = static_cast<int16_t>(x);
                    sky_rank[x] = static_cast<int16_t>(sky_count);
                    ++sky_count;
                }
                sky_left[x] = static_cast<int16_t>(previous);
            }
            if (sky_count > 1) {
                std::copy_n(line3d, output_width, repaired_3d.data());
                for (int start = 0; start < output_width;) {
                    if ((line3d[start] & 0x003F3F3Fu) != 0u) {
                        black_run[start++] = 0;
                        continue;
                    }
                    int end = start + 1;
                    while (end < output_width &&
                           (line3d[end] & 0x003F3F3Fu) == 0u)
                        ++end;
                    const int16_t length =
                        static_cast<int16_t>(end - start);
                    std::fill(black_run.begin() + start,
                              black_run.begin() + end, length);
                    start = end;
                }
                for (int x = 0; x < output_width; ++x) {
                    const uint32_t alpha =
                        (line3d[x] >> 24) & 0x1Fu;
                    const uint32_t rgb =
                        line3d[x] & 0x003F3F3Fu;
                    const bool skybox_black =
                        alpha != 0u && rgb == 0u &&
                        black_run[x] >= 8;
                    if (alpha != 0u && !skybox_black) continue;
                    int left = sky_left[x];
                    if (left < 0) left = last_sky - output_width;
                    const int left_index =
                        (left + output_width) % output_width;
                    int rank = sky_rank[left_index];
                    if (rank < 0) rank = sky_count - 1;
                    rank = (rank + (x - left)) % sky_count;
                    repaired_3d[x] =
                        line3d[sky_indices[rank]];
                }
                composited_3d = repaired_3d.data();
            }
        }
        uint32_t* const dst =
            adaptive.data() + y * output_width;
        auto ahead = [](const Pixel& a, const Pixel& b) {
            return a.priority < b.priority ||
                   (a.priority == b.priority && a.order < b.order);
        };
        const int hud_center_left =
            (256 - g_adaptive_hud_center_width) / 2;
        const int hud_center_right =
            hud_center_left + g_adaptive_hud_center_width;
        for (int x = 0; x < output_width; ++x) {
            Pixel top = backdrop;
            Pixel below = backdrop;
            auto push = [&](const Pixel& pixel) {
                if (ahead(pixel, top)) {
                    below = top;
                    top = pixel;
                } else if (ahead(pixel, below)) {
                    below = pixel;
                }
            };
            // Resolve the stack WITHOUT the 3D layer first. Because
            // (priority, order) is a total order, pushing 3D afterwards
            // reproduces exactly the pair a single interleaved pass would,
            // so the CPU result below is unchanged by this reordering.
            int hud_x = -1;
            if (g_adaptive_hud_anchor) {
                if (x < hud_center_left) {
                    hud_x = x;
                } else if (x >= extra + hud_center_left &&
                           x < extra + hud_center_right) {
                    hud_x = x - extra;
                } else if (x >= output_width -
                                    (256 - hud_center_right)) {
                    hud_x = x - (output_width - 256);
                }
            } else if (x >= extra && x < extra + 256) {
                hud_x = x - extra;
            }
            // Rectangular windows use the same native coordinate as the HUD
            // source. In the 3D-only widened margins there is no native 2D
            // source; selection still supplies the correct OBJ-window/outside
            // mask, and HD eligibility guarantees BG0/effects remain enabled.
            const uint8_t window_x = static_cast<uint8_t>(
                hud_x >= 0 ? hud_x : x);
            const uint8_t window_mask = nds_gpu2d_window_mask(
                window_state, window_x, static_cast<uint8_t>(y),
                obj_window[x] != 0u);
            if (hud_x >= 0) {
                for (size_t i = 0; i < hud_bg_count; ++i) {
                    const uint16_t c = hud_bg_lines[i].color[hud_x];
                    if (!(c & 0x8000u) ||
                        !(window_mask & (1u << hud_bgs[i])))
                        continue;
                    const BgLine& line = hud_bg_lines[i];
                    push(Pixel{
                        rgb6(static_cast<uint16_t>(c & 0x7FFFu)),
                        line.target, 0, line.prio, line.order, true});
                }
            }
            if (obj[x].valid && (window_mask & 0x10u)) push(obj[x]);
            if (emit_hd) {
                const size_t o = (static_cast<size_t>(y) * output_width + x)
                                 * 2u;
                const bool effects_enabled = (window_mask & 0x20u) != 0u;
                g_hd_top_pixels[o] = top.color;
                g_hd_top_pixels[o + 1] = hd_meta(top, effects_enabled);
                g_hd_below_pixels[o] = below.color;
                g_hd_below_pixels[o + 1] = hd_meta(below, effects_enabled);
            }
            const uint32_t c3 = composited_3d[x];
            const uint8_t a3 =
                static_cast<uint8_t>((c3 >> 24) & 0x1Fu);
            if (a3 && (window_mask & 0x01u)) {
                push(Pixel{c3 & 0x003F3F3Fu, 0x01u, 0,
                           prio3d, 1, true, a3});
            }
            uint32_t color = compose(u, top, below,
                                     (window_mask & 0x20u) != 0u);
            if (mbmode == 1u)
                color = brighten(color, mb, 0u);
            else if (mbmode == 2u)
                color = darken(color, mb, 15u);
            dst[x] = to_rgb32(color);
        }
    }
    if (emit_hd) {
        g_hd_frame.top_pixels = g_hd_top_pixels.data();
        g_hd_frame.below_pixels = g_hd_below_pixels.data();
        g_hd_frame.width = static_cast<uint16_t>(output_width);
        g_hd_frame.priority_3d = prio3d;
        g_hd_frame.order_3d = 1u;  // the 3D layer occupies BG0's slot
        g_hd_frame.bldcnt = u.bldcnt;
        g_hd_frame.master_bright = u.master_bright;
        g_hd_frame.eva = static_cast<uint8_t>(u.eva);
        g_hd_frame.evb = static_cast<uint8_t>(u.evb);
        g_hd_frame.evy = static_cast<uint8_t>(u.evy);
        g_hd_frame.render_xpos = nds_gpu3d_render_xpos();
        g_hd_frame_valid = true;
        ++g_hd_frames;
    }
    return adaptive.data();
}

void nds_gpu2d_invalidate_hd_frame() {
    g_hd_frame_valid = false;
}

void nds_gpu2d_set_hd_emit(bool enabled) {
    g_hd_emit_enabled = enabled;
    if (!enabled) g_hd_frame_valid = false;
}

bool nds_gpu2d_hd_frame(NdsGpu2dHdFrame* out) {
    drain_pool(NDS_GPU2D_FENCE_PRESENT);
    if (!out || !g_hd_frame_valid || !g_hd_frame.top_pixels) return false;
    *out = g_hd_frame;
    // Counted on consumption, not emission: emitting surfaces nobody reads
    // looks identical to working HD from an emit-side counter.
    ++g_hd_presented;
    return true;
}
void nds_gpu2d_profile(NdsGpu2dProfile* out) {
    drain_pool(NDS_GPU2D_FENCE_PRESENT);
    if (!out) return;
    out->render_ns = g_render_ns;
    out->obj_ns = g_obj_ns;
    out->engine_ns[0] = g_engine_ns[0];
    out->engine_ns[1] = g_engine_ns[1];
    std::memcpy(out->text_lines, g_text_lines, sizeof(g_text_lines));
    out->no_effect_lines[0] = g_no_effect_lines[0];
    out->no_effect_lines[1] = g_no_effect_lines[1];
    out->scanlines = g_render_scanlines;
    out->direct_frames = g_direct_frames;
    out->hd_frames = g_hd_frames;
    out->hd_presented = g_hd_presented;
    out->direct_overlay_ns = g_direct_overlay_ns;
    std::copy_n(g_direct_class_frames,
                NDS_GPU2D_DIRECT_CLASS_COUNT,
                out->direct_class_frames);
    std::copy_n(g_direct_class_engine_a_ns,
                NDS_GPU2D_DIRECT_CLASS_COUNT,
                out->direct_class_engine_a_ns);
    out->direct_class_transitions = g_direct_class_transitions;
    std::copy_n(g_direct_extra_bg_mask_frames,
                NDS_GPU2D_DIRECT_BG_MASK_COUNT,
                out->direct_extra_bg_mask_frames);
    std::copy_n(g_direct_extra_bg_mask_engine_a_ns,
                NDS_GPU2D_DIRECT_BG_MASK_COUNT,
                out->direct_extra_bg_mask_engine_a_ns);
    std::copy_n(g_direct_extra_bg_mode_frames,
                NDS_GPU2D_DIRECT_BG_MODE_COUNT,
                out->direct_extra_bg_mode_frames);
    std::copy_n(g_direct_extra_effect_frames,
                NDS_GPU2D_DIRECT_EFFECT_MODE_COUNT,
                out->direct_extra_effect_frames);
    std::copy_n(g_direct_extra_master_bright_frames,
                NDS_GPU2D_DIRECT_EFFECT_MODE_COUNT,
                out->direct_extra_master_bright_frames);
    out->threaded_lines = g_threaded_lines;
    out->inline_lines = g_inline_lines;
    out->fence_wait_ns = g_fence_wait_ns;
    out->fence_helped_lines = g_fence_helped_lines;
    std::copy_n(g_fence_drains, NDS_GPU2D_FENCE_CAUSE_COUNT,
                out->fence_drains);
    std::copy_n(g_fenced_lines, NDS_GPU2D_FENCE_CAUSE_COUNT,
                out->fenced_lines);
}

const char* nds_gpu2d_direct_class_name(uint32_t index) {
    static constexpr const char* kNames[NDS_GPU2D_DIRECT_CLASS_COUNT] = {
        "supported",
        "disabled",
        "force_cpu",
        "screen_route",
        "engine_off",
        "capture",
        "renderer_view",
        "force_blank",
        "display_mode",
        "no_bg0_3d",
        "windows",
        "extra_bg",
        "width",
        "render_xpos",
        "center_native",
    };
    return index < NDS_GPU2D_DIRECT_CLASS_COUNT ? kNames[index] : "unknown";
}
