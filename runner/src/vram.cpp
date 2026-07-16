#include "vram.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

#include "io.h"

namespace {

constexpr uint32_t kBankOffset[9] = {
    0x00000, 0x20000, 0x40000, 0x60000, 0x80000,
    0x90000, 0x94000, 0x98000, 0xA0000
};
constexpr uint32_t kBankSize[9] = {
    0x20000, 0x20000, 0x20000, 0x20000, 0x10000,
    0x04000, 0x04000, 0x08000, 0x04000
};
constexpr uint32_t kTotalVram = 0xA4000;

std::array<uint8_t, kTotalVram> g_vram{};
std::array<uint8_t, 0x800> g_palette{};
std::array<uint8_t, 0x800> g_oam{};
std::array<uint8_t, 9> g_cnt{};
uint16_t g_lcdc = 0;
std::array<uint16_t, 32> g_abg{};
std::array<uint16_t, 16> g_aobj{};
std::array<uint16_t, 8> g_bbg{};
std::array<uint16_t, 8> g_bobj{};
std::array<uint16_t, 4> g_abg_ext{};
uint16_t g_aobj_ext = 0;
std::array<uint16_t, 4> g_bbg_ext{};
uint16_t g_bobj_ext = 0;
std::array<uint16_t, 4> g_texture{};
std::array<uint16_t, 8> g_texpal{};
std::array<uint16_t, 2> g_arm7{};
std::array<NdsVramRendererView, 2> g_renderer_view{};
uint64_t g_texture_generation = 1;

const uint8_t* direct_chunk(uint16_t mask, uint32_t virtual_base) {
    if (!mask || (mask & (mask - 1u))) return nullptr;
    const unsigned bank = std::countr_zero(static_cast<unsigned>(mask));
    return g_vram.data() + kBankOffset[bank] +
           (virtual_base & (kBankSize[bank] - 1u));
}

void refresh_renderer_views() {
    g_renderer_view = {};
    for (uint32_t chunk = 0; chunk < 32; ++chunk)
        g_renderer_view[0].bg[chunk] =
            direct_chunk(g_abg[chunk], chunk << 14u);
    for (uint32_t chunk = 0; chunk < 16; ++chunk)
        g_renderer_view[0].obj[chunk] =
            direct_chunk(g_aobj[chunk], chunk << 14u);
    for (uint32_t chunk = 0; chunk < 8; ++chunk) {
        g_renderer_view[1].bg[chunk] =
            direct_chunk(g_bbg[chunk], chunk << 14u);
        g_renderer_view[1].obj[chunk] =
            direct_chunk(g_bobj[chunk], chunk << 14u);
    }
}

template<size_t N>
void range_map(std::array<uint16_t, N>& map, uint32_t base, uint32_t count,
               uint16_t bit, bool add) {
    for (uint32_t i = 0; i < count && base + i < N; ++i) {
        if (add) map[base+i] |= bit;
        else map[base+i] &= ~bit;
    }
}

uint32_t load(const uint8_t* p, uint32_t width) {
    uint32_t value = 0;
    std::memcpy(&value, p, width);
    return value;
}
void store(uint8_t* p, uint32_t value, uint32_t width) {
    std::memcpy(p, &value, width);
}
uint32_t bank_read(unsigned bank, uint32_t addr, uint32_t width) {
    if (bank >= 9) return 0;
    const uint32_t off = addr & (kBankSize[bank] - 1u);
    return load(&g_vram[kBankOffset[bank] + off], width);
}
void bank_write(unsigned bank, uint32_t addr, uint32_t value, uint32_t width) {
    if (bank >= 9) return;
    const uint32_t off = addr & (kBankSize[bank] - 1u);
    store(&g_vram[kBankOffset[bank] + off], value, width);
}
uint32_t mapped_read(uint16_t mask, uint32_t addr, uint32_t width) {
    // The normal DS mapping has one physical bank behind each renderer view.
    // Avoid scanning all nine banks on every tile/pixel fetch; preserve the
    // slow OR path for the legal overlapping-bank case.
    if (mask && !(mask & (mask - 1u)))
        return bank_read(std::countr_zero(static_cast<unsigned>(mask)),
                         addr, width);
    uint32_t value = 0;
    for (unsigned bank = 0; bank < 9; ++bank)
        if (mask & (1u << bank)) value |= bank_read(bank, addr, width);
    return value;
}
void mapped_write(uint16_t mask, uint32_t addr, uint32_t value, uint32_t width) {
    for (unsigned bank = 0; bank < 9; ++bank)
        if (mask & (1u << bank)) bank_write(bank, addr, value, width);
}

void map_ab(unsigned bank, uint8_t cnt, bool add) {
    const uint16_t bit = 1u << bank;
    uint32_t ofs = (cnt >> 3) & 3u;
    if (!(cnt & 0x80u)) return;
    switch (cnt & 3u) {
        case 0: if (add) g_lcdc |= bit; else g_lcdc &= ~bit; break;
        case 1: range_map(g_abg, ofs << 3, 8, bit, add); break;
        case 2: range_map(g_aobj, (ofs & 1u) << 3, 8, bit, add); break;
        case 3: if (add) g_texture[ofs] |= bit; else g_texture[ofs] &= ~bit; break;
    }
}
void map_cd(unsigned bank, uint8_t cnt, bool add) {
    const uint16_t bit = 1u << bank;
    uint32_t ofs = (cnt >> 3) & 7u;
    if (!(cnt & 0x80u)) return;
    switch (cnt & 7u) {
        case 0: if (add) g_lcdc |= bit; else g_lcdc &= ~bit; break;
        case 1: range_map(g_abg, ofs << 3, 8, bit, add); break;
        case 2: if (add) g_arm7[ofs & 1u] |= bit; else g_arm7[ofs & 1u] &= ~bit; break;
        case 3: if (add) g_texture[ofs & 3u] |= bit; else g_texture[ofs & 3u] &= ~bit; break;
        case 4: if (bank == 2) range_map(g_bbg, 0, 8, bit, add);
                else range_map(g_bobj, 0, 8, bit, add); break;
    }
}
void map_e(unsigned bank, uint8_t cnt, bool add) {
    const uint16_t bit = 1u << bank;
    if (!(cnt & 0x80u)) return;
    switch (cnt & 7u) {
        case 0: if (add) g_lcdc |= bit; else g_lcdc &= ~bit; break;
        case 1: range_map(g_abg, 0, 4, bit, add); break;
        case 2: range_map(g_aobj, 0, 4, bit, add); break;
        case 3: range_map(g_texpal, 0, 4, bit, add); break;
        case 4: range_map(g_abg_ext, 0, 4, bit, add); break;
    }
}
void map_fg(unsigned bank, uint8_t cnt, bool add) {
    const uint16_t bit = 1u << bank;
    const uint32_t ofs = (cnt >> 3) & 7u;
    const uint32_t base = (ofs & 1u) + ((ofs & 2u) << 1);
    if (!(cnt & 0x80u)) return;
    auto bitset = [add,bit](uint16_t& x) { if (add) x |= bit; else x &= ~bit; };
    switch (cnt & 7u) {
        case 0: if (add) g_lcdc |= bit; else g_lcdc &= ~bit; break;
        case 1: bitset(g_abg[base]); bitset(g_abg[base+2]); break;
        case 2: bitset(g_aobj[base]); bitset(g_aobj[base+2]); break;
        case 3: bitset(g_texpal[base]); break;
        case 4: bitset(g_abg_ext[(ofs & 1u) << 1]);
                bitset(g_abg_ext[((ofs & 1u) << 1) + 1]); break;
        case 5: bitset(g_aobj_ext); break;
    }
}
void map_h(unsigned bank, uint8_t cnt, bool add) {
    const uint16_t bit = 1u << bank;
    if (!(cnt & 0x80u)) return;
    auto bitset = [add,bit](uint16_t& x) { if (add) x |= bit; else x &= ~bit; };
    switch (cnt & 3u) {
        case 0: if (add) g_lcdc |= bit; else g_lcdc &= ~bit; break;
        case 1: bitset(g_bbg[0]); bitset(g_bbg[1]); bitset(g_bbg[4]); bitset(g_bbg[5]); break;
        case 2: range_map(g_bbg_ext, 0, 4, bit, add); break;
    }
}
void map_i(unsigned bank, uint8_t cnt, bool add) {
    const uint16_t bit = 1u << bank;
    if (!(cnt & 0x80u)) return;
    auto bitset = [add,bit](uint16_t& x) { if (add) x |= bit; else x &= ~bit; };
    switch (cnt & 3u) {
        case 0: if (add) g_lcdc |= bit; else g_lcdc &= ~bit; break;
        case 1: bitset(g_bbg[2]); bitset(g_bbg[3]); bitset(g_bbg[6]); bitset(g_bbg[7]); break;
        case 2: range_map(g_bobj, 0, 8, bit, add); break;
        case 3: bitset(g_bobj_ext); break;
    }
}

uint8_t sanitize(unsigned bank, uint8_t cnt) {
    if (bank < 2) return cnt & 0x9Bu;
    if (bank < 4) return cnt & 0x9Fu;
    if (bank == 4) return cnt & 0x87u;
    if (bank < 7) return cnt & 0x9Fu;
    return cnt & 0x83u;
}
void apply_map(unsigned bank, uint8_t cnt, bool add) {
    if (bank < 2) map_ab(bank, cnt, add);
    else if (bank < 4) map_cd(bank, cnt, add);
    else if (bank == 4) map_e(bank, cnt, add);
    else if (bank < 7) map_fg(bank, cnt, add);
    else if (bank == 7) map_h(bank, cnt, add);
    else map_i(bank, cnt, add);
}

bool lcdc_bank(uint32_t addr, unsigned& bank, uint32_t& offset) {
    if (addr >= 0x06800000u && addr < 0x06880000u) {
        bank = (addr - 0x06800000u) >> 17;
        offset = addr & 0x1FFFFu;
    } else if (addr >= 0x06880000u && addr < 0x06890000u) {
        bank = 4; offset = addr & 0xFFFFu;
    } else if (addr >= 0x06890000u && addr < 0x06894000u) {
        bank = 5; offset = addr & 0x3FFFu;
    } else if (addr >= 0x06894000u && addr < 0x06898000u) {
        bank = 6; offset = addr & 0x3FFFu;
    } else if (addr >= 0x06898000u && addr < 0x068A0000u) {
        bank = 7; offset = addr & 0x7FFFu;
    } else if (addr >= 0x068A0000u && addr < 0x068A4000u) {
        bank = 8; offset = addr & 0x3FFFu;
    } else return false;
    return true;
}

uint32_t arm9_vram_read(uint32_t addr, uint32_t width) {
    switch (addr & 0x00E00000u) {
        case 0x00000000u: return mapped_read(g_abg[(addr >> 14) & 31u], addr, width);
        case 0x00200000u: return mapped_read(g_bbg[(addr >> 14) & 7u], addr, width);
        case 0x00400000u: return mapped_read(g_aobj[(addr >> 14) & 15u], addr, width);
        case 0x00600000u: return mapped_read(g_bobj[(addr >> 14) & 7u], addr, width);
        default: {
            unsigned bank; uint32_t offset;
            if (!lcdc_bank(addr, bank, offset) || !(g_lcdc & (1u << bank))) return 0;
            return bank_read(bank, offset, width);
        }
    }
}
void arm9_vram_write(uint32_t addr, uint32_t value, uint32_t width) {
    switch (addr & 0x00E00000u) {
        case 0x00000000u: mapped_write(g_abg[(addr >> 14) & 31u], addr, value, width); break;
        case 0x00200000u: mapped_write(g_bbg[(addr >> 14) & 7u], addr, value, width); break;
        case 0x00400000u: mapped_write(g_aobj[(addr >> 14) & 15u], addr, value, width); break;
        case 0x00600000u: mapped_write(g_bobj[(addr >> 14) & 7u], addr, value, width); break;
        default: {
            unsigned bank; uint32_t offset;
            if (lcdc_bank(addr, bank, offset) && (g_lcdc & (1u << bank)))
                bank_write(bank, offset, value, width);
            break;
        }
    }
}

// Refresh one flattened 3D slot space (texture image or texture palette)
// from the live bank mapping. Unmapped slots read as zero; the legal
// overlapping-bank case keeps the exact OR-combining fallback.
void copy_slot_space(uint8_t* dst, const uint16_t* masks, unsigned slots,
                     uint32_t slot_size) {
    for (unsigned s = 0; s < slots; ++s) {
        uint8_t* out = dst + s * slot_size;
        const uint32_t base = s * slot_size;
        const uint16_t mask = masks[s];
        if (!mask) { std::memset(out, 0, slot_size); continue; }
        if (const uint8_t* direct = direct_chunk(mask, base)) {
            std::memcpy(out, direct, slot_size);
            continue;
        }
        for (uint32_t off = 0; off < slot_size; off += 4u)
            store(out + off, mapped_read(mask, base + off, 4), 4);
    }
}

} // namespace

void nds_vram_copy_texture(uint8_t* dst) {
    copy_slot_space(dst, g_texture.data(), 4, 0x20000u);
}

void nds_vram_copy_texpal(uint8_t* dst) {
    copy_slot_space(dst, g_texpal.data(), 8, 0x4000u);
}

uint64_t nds_vram_texture_generation() { return g_texture_generation; }

void nds_vram_reset() {
    g_vram.fill(0); g_palette.fill(0); g_oam.fill(0); g_cnt.fill(0);
    g_lcdc = 0; g_abg.fill(0); g_aobj.fill(0); g_bbg.fill(0); g_bobj.fill(0);
    g_abg_ext.fill(0); g_aobj_ext = 0; g_bbg_ext.fill(0); g_bobj_ext = 0;
    g_texture.fill(0); g_texpal.fill(0); g_arm7.fill(0);
    ++g_texture_generation;
    refresh_renderer_views();
}

void nds_vram_map(unsigned bank, uint8_t value) {
    if (bank >= 9) return;
    const uint8_t next = sanitize(bank, value);
    if (next == g_cnt[bank]) return;
    apply_map(bank, g_cnt[bank], false);
    g_cnt[bank] = next;
    apply_map(bank, next, true);
    ++g_texture_generation;
    refresh_renderer_views();
}
uint8_t nds_vramcnt(unsigned bank) { return bank < 9 ? g_cnt[bank] : 0; }
uint8_t nds_vramstat() {
    uint8_t value = 0;
    if ((g_cnt[2] & 0x87u) == 0x82u) value |= 1u;
    if ((g_cnt[3] & 0x87u) == 0x82u) value |= 2u;
    return value;
}

bool nds_video_address(uint32_t addr) {
    return addr >= 0x05000000u && addr < 0x08000000u;
}
uint32_t nds_video_read(int cpu, uint32_t addr, uint32_t width) {
    addr &= ~(width - 1u);
    if (cpu == 7) {
        if ((addr & 0xFF000000u) != 0x06000000u) return 0;
        return mapped_read(g_arm7[(addr >> 17) & 1u], addr, width);
    }
    if ((addr & 0xFF000000u) == 0x05000000u) {
        const uint16_t power = nds_powercontrol9();
        if (!(power & ((addr & 0x400u) ? 0x0200u : 0x0002u))) return 0;
        return load(&g_palette[addr & 0x7FFu], width);
    }
    if ((addr & 0xFF000000u) == 0x06000000u) return arm9_vram_read(addr, width);
    if ((addr & 0xFF000000u) == 0x07000000u) {
        const uint16_t power = nds_powercontrol9();
        if (!(power & ((addr & 0x400u) ? 0x0200u : 0x0002u))) return 0;
        return load(&g_oam[addr & 0x7FFu], width);
    }
    return 0;
}
void nds_video_write(int cpu, uint32_t addr, uint32_t value, uint32_t width) {
    addr &= ~(width - 1u);
    if (cpu == 7) {
        if ((addr & 0xFF000000u) == 0x06000000u)
            mapped_write(g_arm7[(addr >> 17) & 1u], addr, value, width);
        return;
    }
    if (width == 1) return; // ARM9 byte writes to palette/VRAM/OAM are ignored.
    if ((addr & 0xFF000000u) == 0x05000000u) {
        const uint16_t power = nds_powercontrol9();
        if (power & ((addr & 0x400u) ? 0x0200u : 0x0002u))
            store(&g_palette[addr & 0x7FFu], value, width);
    } else if ((addr & 0xFF000000u) == 0x06000000u) {
        arm9_vram_write(addr, value, width);
    } else if ((addr & 0xFF000000u) == 0x07000000u) {
        const uint16_t power = nds_powercontrol9();
        if (power & ((addr & 0x400u) ? 0x0200u : 0x0002u))
            store(&g_oam[addr & 0x7FFu], value, width);
    }
}

bool nds_video_get_region(const char* name, const uint8_t** ptr, uint32_t* len) {
    if (!name || !ptr || !len) return false;
    for (unsigned i = 0; i < 9; ++i) {
        char expected[6] = {'v','r','a','m',static_cast<char>('A'+i),0};
        if (std::strcmp(name, expected) == 0) {
            *ptr = &g_vram[kBankOffset[i]]; *len = kBankSize[i]; return true;
        }
    }
    if (std::strcmp(name, "palA") == 0) { *ptr = g_palette.data(); *len = 0x400; return true; }
    if (std::strcmp(name, "palB") == 0) { *ptr = g_palette.data()+0x400; *len = 0x400; return true; }
    if (std::strcmp(name, "oam") == 0) { *ptr = g_oam.data(); *len = 0x800; return true; }
    return false;
}

uint32_t nds_vram_read_bg(int engine, uint32_t addr, uint32_t width) {
    return engine ? mapped_read(g_bbg[(addr >> 14) & 7u], addr, width)
                  : mapped_read(g_abg[(addr >> 14) & 31u], addr, width);
}
uint32_t nds_vram_read_obj(int engine, uint32_t addr, uint32_t width) {
    return engine ? mapped_read(g_bobj[(addr >> 14) & 7u], addr, width)
                  : mapped_read(g_aobj[(addr >> 14) & 15u], addr, width);
}
uint32_t nds_vram_read_bg_extpal(int engine, uint32_t addr, uint32_t width) {
    return engine ? mapped_read(g_bbg_ext[(addr >> 13) & 3u], addr, width)
                  : mapped_read(g_abg_ext[(addr >> 13) & 3u], addr, width);
}
uint32_t nds_vram_read_obj_extpal(int engine, uint32_t addr, uint32_t width) {
    return mapped_read(engine ? g_bobj_ext : g_aobj_ext, addr, width);
}
const uint8_t* nds_vram_renderer_palette(int engine) {
    engine &= 1;
    const uint16_t bit = engine ? 0x0200u : 0x0002u;
    return (nds_powercontrol9() & bit) ? g_palette.data() + engine * 0x400u
                                     : nullptr;
}
const uint8_t* nds_vram_renderer_oam(int engine) {
    engine &= 1;
    const uint16_t bit = engine ? 0x0200u : 0x0002u;
    return (nds_powercontrol9() & bit) ? g_oam.data() + engine * 0x400u
                                     : nullptr;
}
const NdsVramRendererView* nds_vram_renderer_view(int engine) {
    return &g_renderer_view[engine & 1];
}
