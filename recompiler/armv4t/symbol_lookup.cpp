// symbol_lookup.cpp — see symbol_lookup.h.
#include "symbol_lookup.h"

namespace {

const GbaSymbol* g_cart = nullptr;
unsigned         g_cart_n = 0u;
const GbaSymbol* g_bios = nullptr;
unsigned         g_bios_n = 0u;

// Largest entry with addr <= pc (upper_bound - 1). Table is sorted ascending.
const char* search(const GbaSymbol* tab, unsigned n, uint32_t pc,
                   uint32_t* off) {
    if (!tab || n == 0u) return nullptr;
    unsigned lo = 0u, hi = n;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2u;
        if (tab[mid].addr <= pc) lo = mid + 1u;
        else hi = mid;
    }
    if (lo == 0u) return nullptr;  // pc precedes the first entry
    const GbaSymbol& e = tab[lo - 1u];
    if (off) *off = pc - e.addr;
    return e.name;
}

}  // namespace

extern "C" void gba_symbol_register_cart(const GbaSymbol* tab, unsigned count) {
    g_cart = tab;
    g_cart_n = count;
}

extern "C" void gba_symbol_register_bios(const GbaSymbol* tab, unsigned count) {
    g_bios = tab;
    g_bios_n = count;
}

extern "C" const char* gba_symbol_lookup(uint32_t pc, uint32_t* out_offset) {
    if (out_offset) *out_offset = 0u;
    // BIOS region (PC < 0x4000) resolves against the BIOS map first.
    if (pc < 0x00004000u) {
        const char* nm = search(g_bios, g_bios_n, pc, out_offset);
        if (nm) return nm;
    }
    return search(g_cart, g_cart_n, pc, out_offset);
}
