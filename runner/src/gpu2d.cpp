#include "gpu2d.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "gpu3d.h"
#include "io.h"
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
// melonDS draws into the back buffer during the active frame and publishes it
// only in GPU::FinishFrame, after VBlank. Keeping the same lifecycle matters
// for instruction-precise framebuffer queries made while VCount is 192..262.
std::array<std::array<Frame, 2>, 2> g_fb{}; // [buffer][engine], 0xFFRRGGBB
int g_front = 0;
uint64_t g_render_ns = 0;
uint64_t g_obj_ns = 0;
uint64_t g_engine_ns[2] = {};
uint64_t g_text_lines[2][5] = {};
uint64_t g_no_effect_lines[2] = {};
uint64_t g_render_scanlines = 0;

bool profiling() {
    static const bool enabled = std::getenv("NDS_PROFILE_GPU") != nullptr;
    return enabled;
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

TextLine prepare_text_line(int engine, int bg, int y,
                           const uint8_t* palette,
                           const NdsVramRendererView* vram) {
    Unit& u = g_unit[engine];
    TextLine line{};
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
    Unit& u = g_unit[line.engine];
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

void decode_text_line(int engine, int bg, int y, const uint8_t* palette,
                      const NdsVramRendererView& vram, BgLine& out) {
    Unit& u = g_unit[engine];
    TextLine line = prepare_text_line(engine, bg, y, palette, &vram);
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

void put_obj(std::array<Pixel,256>& line, int x, const Pixel& p) {
    if (x < 0 || x >= 256 || !p.valid) return;
    // OAM index is resolved by visiting entries from 127 down to 0: an equal
    // priority pixel written later therefore has the lower (winning) index.
    // A numerically worse priority must not replace an existing front pixel.
    if (!line[x].valid || p.priority <= line[x].priority) line[x]=p;
}

void render_obj_line(int engine, int line_y, std::array<Pixel,256>& out,
                     const uint8_t* oam, const uint8_t* palette,
                     const NdsVramRendererView& vram) {
    out.fill({});
    const Unit& u=g_unit[engine];
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
            if (sx<=-bw || sx>=256) continue;
            const int mode=(a0>>10)&3;
            if (mode==2) continue; // OBJ-window is handled when window modes land.
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
                const int screen_x=sx+dx;if(screen_x<0||screen_x>=256)continue;
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
                Pixel p{rgb6(color), 0x10u, alpha,
                        static_cast<uint8_t>(priority), 0, true};
                if(mode==3)p.alpha=static_cast<uint8_t>(std::min(16u,((a2>>12)&0xFu)+1u));
                put_obj(out,screen_x,p);
            }
    }
}

uint32_t compose(const Unit& u, const Pixel& top, const Pixel& below) {
    uint32_t c=top.color;
    const uint16_t target2=static_cast<uint16_t>(below.target)<<8;
    if (top.alpha5) {
        // 3D layer on top: whenever the pixel behind is a BLDCNT second
        // target, per-pixel 5-bit blending is forced regardless of the
        // selected color effect (melonDS ColorComposite coloreffect=4).
        if (u.bldcnt & target2) return blend5(c, below.color, top.alpha5);
        // Otherwise the 3D layer acts as BG0 (first-target bit 0x01) for
        // brightness effects only; alpha blend never applies here.
        if (u.bldcnt & top.target) {
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
    }else if(u.bldcnt&top.target){
        switch((u.bldcnt>>6)&3u){
            case 1:if(u.bldcnt&target2)c=blend(c,below.color,u.eva,u.evb);break;
            case 2:c=brighten(c,u.evy);break;
            case 3:c=darken(c,u.evy);break;
        }
    }
    return c;
}

// Composite one scanline into the internal 6-bit format (channels at bits
// 0-5/8-13/16-21) including the 3D layer when BG0 is redirected to it.
// This is the general-path twin of the dst-direct fast paths in
// render_engine_line, which carry the same layer/priority/blend rules but
// skip the 3D layer and the 6-bit intermediate.
void compose_line6(int engine, int y, Unit& u, const uint8_t* palette,
                   const uint8_t* oam, const NdsVramRendererView& vram,
                   const uint32_t* line3d, bool bg0_3d, uint32_t* out) {
    std::array<Pixel,256> obj{};
    const auto obj_start = profiling() ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
    render_obj_line(engine, y, obj, oam, palette, vram);
    if (profiling()) {
        g_obj_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - obj_start).count());
    }
    const uint16_t backdrop15 = view16(palette, 0);
    const Pixel backdrop{rgb6(backdrop15), 0x20u, 0, 4, 5, true};
    static std::array<BgLine, 4> text_lines;
    size_t text_count = 0;
    const uint32_t bgmode = u.dispcnt & 7u;
    int text_bgs[4];
    for (int bg = 0; bg < 4; ++bg) {
        if (!(u.dispcnt & (0x100u << bg))) continue;
        if (bg == 0 && bg0_3d) continue;   // BG0's slot is the 3D layer
        const bool text = (bg < 2) || (bgmode == 0) ||
            (bg == 2 && (bgmode == 1 || bgmode == 3)) ||
            (bg == 3 && bgmode == 0);
        if (text) text_bgs[text_count++] = bg;
    }
    for (size_t i = 0; i < text_count; ++i)
        decode_text_line(engine, text_bgs[i], y, palette, vram,
                         text_lines[i]);
    const uint8_t prio3d = static_cast<uint8_t>(u.bgcnt[0] & 3u);
    auto ahead = [](const Pixel& a, const Pixel& b) {
        return a.priority < b.priority ||
               (a.priority == b.priority && a.order < b.order);
    };
    for (int x = 0; x < 256; ++x) {
        Pixel top = backdrop, below = backdrop;
        auto push = [&](const Pixel& p) {
            if (ahead(p, top)) { below = top; top = p; }
            else if (ahead(p, below)) { below = p; }
        };
        for (size_t i = 0; i < text_count; ++i) {
            const uint16_t c = text_lines[i].color[x];
            if (!(c & 0x8000u)) continue;
            const BgLine& l = text_lines[i];
            push(Pixel{rgb6(static_cast<uint16_t>(c & 0x7FFFu)),
                       l.target, 0, l.prio, l.order, true});
        }
        if (bg0_3d && line3d) {
            const uint32_t c3 = line3d[x];
            const uint8_t a3 = static_cast<uint8_t>((c3 >> 24) & 0x1Fu);
            // alpha 0 = fully transparent; the layer competes at BG0's
            // priority and order (melonDS DrawBG_3D).
            if (a3)
                push(Pixel{c3 & 0x003F3F3Fu, 0x01u, 0, prio3d, 1, true, a3});
        }
        if (obj[x].valid) push(obj[x]);
        out[x] = compose(u, top, below);
    }
}

// DISPCAPCNT capture (engine A), mirroring melonDS SoftRenderer::DoCapture:
// writes 15-bit+alpha pixels into the physical destination bank (gated on
// its LCDC mapping), blending source A (composite or 3D-only line, in the
// internal 6-bit format) with source B (LCDC VRAM or the display FIFO).
void do_capture(Unit& u, int line, uint32_t width, const uint32_t* comp6,
                const uint32_t* line3d) {
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

void render_engine_line(int engine, int y) {
    Unit& u=g_unit[engine];
    Frame& fb = g_fb[g_front ^ 1][engine];
    uint32_t* const dst = fb.data() + y * 256;
    const uint8_t* const palette = nds_vram_renderer_palette(engine);
    const uint8_t* const oam = nds_vram_renderer_oam(engine);
    const NdsVramRendererView& vram = *nds_vram_renderer_view(engine);
    const bool forceblank = !palette || !oam || (u.dispcnt & 0x80u);
    // DISPCAPCNT latches at the top of the frame; setting the enable bit
    // mid-frame does not capture until the next frame. melonDS's arm skips
    // only its own force-blank cases (VCount>192, engine-B power-off),
    // neither of which applies to engine A here.
    if (engine == 0 && y == 0 && (u.capture & 0x80000000u))
        u.capture_latch = true;
    if (forceblank) {
        std::fill_n(dst, 256, 0xFFFFFFFFu);
        return;
    }
    const uint32_t mode=(u.dispcnt>>16)&(engine?1u:3u);

    uint32_t capw = 0;
    bool cap = false;
    if (engine == 0 && u.capture_latch) {
        static constexpr uint16_t kCapW[4] = {128, 256, 256, 256};
        static constexpr uint8_t kCapH[4] = {128, 64, 128, 192};
        const uint32_t size = (u.capture >> 20) & 3u;
        capw = kCapW[size];
        cap = y < kCapH[size];
    }
    const bool bg0_3d = engine == 0 && (u.dispcnt & 0x8u) != 0 &&
                        (u.dispcnt & 0x100u) != 0;

    if (bg0_3d || cap || mode != 1u) {
        // General path: mirror melonDS DrawScanline ordering — composite
        // (when the display or capture consumes it), display-mode mux,
        // capture, master brightness on every mode except screen-off.
        const uint32_t* line3d =
            (engine == 0) ? nds_gpu3d_line(y) : nullptr;
        static std::array<uint32_t, 256> comp6;
        const bool need_comp =
            mode == 1u || (cap && !(u.capture & 0x01000000u));
        if (need_comp)
            compose_line6(engine, y, u, palette, oam, vram, line3d,
                          bg0_3d, comp6.data());
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

    std::array<Pixel,256> obj{};
    const auto obj_start = profiling() ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
    render_obj_line(engine,y,obj,oam,palette,vram);
    if (profiling()) {
        g_obj_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - obj_start).count());
    }
    const uint16_t backdrop15 = view16(palette, 0);
    const Pixel backdrop{rgb6(backdrop15), 0x20u, 0, 4, 5, true};
    const uint32_t* const lut = rgb32_lut();
    static std::array<BgLine, 4> text_lines;  // decoded, sorted front-first
    size_t text_count = 0;
    const uint32_t bgmode = u.dispcnt & 7u;
    int text_bgs[4];
    for (int bg = 0; bg < 4; ++bg) {
        if (!(u.dispcnt & (0x100u << bg))) continue;
        const bool text = (bg < 2) || (bgmode == 0) ||
            (bg == 2 && (bgmode == 1 || bgmode == 3)) ||
            (bg == 3 && bgmode == 0);
        if (text) text_bgs[text_count++] = bg;
    }
    // Front-first order: lower BGCNT priority wins, ties break to the lower
    // BG index. Four elements maximum: insertion sort on the index list.
    {
        int order[4];
        uint8_t prio[4];
        for (size_t i = 0; i < text_count; ++i) {
            order[i] = text_bgs[i];
            prio[i] = static_cast<uint8_t>(u.bgcnt[text_bgs[i]] & 3u);
        }
        for (size_t i = 1; i < text_count; ++i) {
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
        for (size_t i = 0; i < text_count; ++i)
            decode_text_line(engine, order[i], y, palette, vram,
                             text_lines[i]);
    }
    const uint32_t mbmode=u.master_bright>>14;
    const uint32_t mb=std::min<uint32_t>(16,u.master_bright&0x1Fu);
    // Common firmware top-screen mode: OBJ over a backdrop, with no BG or
    // color effect enabled. There is no second layer to sort or blend.
    if (text_count == 0 && u.bldcnt == 0 && mbmode == 0) {
        if (profiling()) ++g_text_lines[engine][0];
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
            ++g_text_lines[engine][text_count];
            ++g_no_effect_lines[engine];
        }
        for (int x = 0; x < 256; ++x) {
            // BG layers are front-first: the first opaque pixel is the top
            // candidate; an OBJ pixel wins any priority tie (order 0).
            uint16_t top15 = backdrop15;
            uint8_t top_prio = 4;
            for (size_t i = 0; i < text_count; ++i) {
                const uint16_t c = text_lines[i].color[x];
                if (c & 0x8000u) {
                    top15 = c;
                    top_prio = text_lines[i].prio;
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
    if (profiling()) ++g_text_lines[engine][text_count];
    for(int x=0;x<256;++x){
        Pixel top=backdrop, below=backdrop;
        bool have_top = false;
        for (size_t i = 0; i < text_count; ++i) {
            const uint16_t c = text_lines[i].color[x];
            if (!(c & 0x8000u)) continue;
            const BgLine& l = text_lines[i];
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
        else if(sub==8)u.refx[n]=(u.refx[n]&0xFFFF0000)|v;
        else if(sub==0xA){
            if(v&0x800)v|=0xF000;
            u.refx[n]=(u.refx[n]&0xFFFF)|(uint32_t{v}<<16);
        } else if(sub==0xC)u.refy[n]=(u.refy[n]&0xFFFF0000)|v;
        else if(sub==0xE){
            if(v&0x800)v|=0xF000;
            u.refy[n]=(u.refy[n]&0xFFFF)|(uint32_t{v}<<16);
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

} // namespace

void nds_gpu2d_reset(){
    g_unit={};
    g_front = 0;
    g_render_ns = 0;
    g_obj_ns = 0;
    g_engine_ns[0] = g_engine_ns[1] = 0;
    std::memset(g_text_lines, 0, sizeof(g_text_lines));
    g_no_effect_lines[0] = g_no_effect_lines[1] = 0;
    g_render_scanlines = 0;
    for (auto& buffers : g_fb)
        for (auto& frame : buffers)
            frame.fill(0xFFFFFFFFu);
}
void nds_gpu2d_stop(){
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
void nds_gpu2d_render_scanline(int line) {
    if (line < 0 || line >= 192) return;
    if (!profiling()) {
        render_engine_line(0, line);
        render_engine_line(1, line);
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    render_engine_line(0, line);
    const auto middle = std::chrono::steady_clock::now();
    render_engine_line(1, line);
    const auto finish = std::chrono::steady_clock::now();
    const auto elapsed = finish - start;
    g_render_ns += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    g_engine_ns[0] += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(middle - start).count());
    g_engine_ns[1] += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - middle).count());
    ++g_render_scanlines;
}
void nds_gpu2d_render_frame(){
    for (int line = 0; line < 192; ++line)
        nds_gpu2d_render_scanline(line);
}
void nds_gpu2d_finish_frame(){g_front ^= 1;}
void nds_gpu2d_vblank(){
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
    const bool normal=(nds_powercontrol9()&0x8000u)!=0;
    const int engine=normal?screen:(screen^1);
    return g_fb[g_front][engine&1].data();
}
void nds_gpu2d_profile(NdsGpu2dProfile* out) {
    if (!out) return;
    out->render_ns = g_render_ns;
    out->obj_ns = g_obj_ns;
    out->engine_ns[0] = g_engine_ns[0];
    out->engine_ns[1] = g_engine_ns[1];
    std::memcpy(out->text_lines, g_text_lines, sizeof(g_text_lines));
    out->no_effect_lines[0] = g_no_effect_lines[0];
    out->no_effect_lines[1] = g_no_effect_lines[1];
    out->scanlines = g_render_scanlines;
}
