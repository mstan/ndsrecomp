/*
    ndsrecomp shim replacing melonDS's GPU.h for the vendored GPU3D engine.

    Supplies, by the same names, the slice of melonDS::GPU that the unmodified
    GPU3D.cpp/GPU3D_Soft.cpp translation units consume (surveyed 2026-07-16):
    the owned GPU3D sub-object, the flat texture/texture-palette VRAM views
    with their per-frame coherence calls, and the dirty-tracking types those
    calls are keyed on. Texture data comes from the runner's VRAM model
    (runner/src/vram.cpp); the dirty-tracking shim starts from a clean set and
    MakeVRAMFlat_*Coherent compares the flat view in 512-byte chunks, reports
    each changed chunk, and copies it (runner/src/gpu3d.cpp). The software
    renderer consumes the aggregate result; the optional compute texture
    cache also consumes the precise chunk bits.

    As an interface derived from melonDS this file is distributed under the
    same terms as the vendored sources: GPL-3.0-or-later (see GPU3D.h).
    Copyright 2016-2024 melonDS team; shim adaptation 2026 ndsrecomp.
*/

#ifndef GPU_H
#define GPU_H

#include "types.h"
#include "NonStupidBitfield.h"
#include "GPU3D.h"

namespace melonDS
{

class NDS;
class GPU;

constexpr u32 VRAMDirtyGranularity = 512;

// Dirty-region tracker shim. The real melonDS type diffs bank mappings and
// per-bank write bitmaps; the runner starts clean and its coherence methods
// OR in the exact flat-view chunks whose bytes changed.
template <u32 Size, u32 MappingGranularity>
struct VRAMTrackingSet
{
    NonStupidBitField<Size / VRAMDirtyGranularity>
    DeriveState(const u32*, GPU&) const { return {}; }
};

class GPU
{
public:
    explicit GPU(melonDS::NDS& nds) noexcept : GPU3D(nds) {}

    melonDS::GPU3D GPU3D;

    // Maintained for interface parity with GPU3D_Soft.cpp's DeriveState
    // calls; the shimmed coherence path reads the runner's live VRAM
    // mapping state directly instead.
    u32 VRAMMap_Texture[4] {};
    u32 VRAMMap_TexPal[8] {};

    VRAMTrackingSet<512*1024, 128*1024> VRAMDirty_Texture {};
    VRAMTrackingSet<128*1024, 16*1024> VRAMDirty_TexPal {};

    alignas(u64) u8 VRAMFlat_Texture[512*1024] {};
    alignas(u64) u8 VRAMFlat_TexPal[128*1024] {};

    template<typename T>
    T ReadVRAMFlat_Texture(u32 addr) const
    {
        return *(T*)&VRAMFlat_Texture[addr & 0x7FFFF];
    }
    template<typename T>
    T ReadVRAMFlat_TexPal(u32 addr) const
    {
        return *(T*)&VRAMFlat_TexPal[addr & 0x1FFFF];
    }

    bool MakeVRAMFlat_TextureCoherent(
        NonStupidBitField<512*1024/VRAMDirtyGranularity>& dirty) noexcept;
    bool MakeVRAMFlat_TexPalCoherent(
        NonStupidBitField<128*1024/VRAMDirtyGranularity>& dirty) noexcept;
};

}

#endif // GPU_H
