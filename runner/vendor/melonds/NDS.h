/*
    ndsrecomp shim replacing melonDS's NDS.h for the vendored GPU3D engine.

    The vendored GPU3D.cpp/GPU3D_Soft.cpp translation units are unmodified
    melonDS 1.0rc sources; this header supplies, by the same names, exactly
    the slice of the melonDS::NDS interface those units consume (surveyed
    2026-07-16: GPU member, ARM9Timestamp/ARM9ClockShift, SetIRQ/ClearIRQ,
    CheckDMAs, GXFIFOStall/GXFIFOUnstall, IRQ_GXFIFO). The method bodies live
    in runner/src/gpu3d.cpp and forward to the runner's own device models.

    As an interface derived from melonDS this file is distributed under the
    same terms as the vendored sources: GPL-3.0-or-later (see GPU3D.h).
    Copyright 2016-2024 melonDS team; shim adaptation 2026 ndsrecomp.
*/

#ifndef NDS_H
#define NDS_H

#include "types.h"
#include "GPU.h"

namespace melonDS
{

// Bit indices into IE/IF, matching the melonDS NDS.h enum (GPU3D only ever
// names IRQ_GXFIFO; the value must stay 21 = GBATEK "Geometry Command FIFO").
enum
{
    IRQ_GXFIFO = 21,
};

class NDS
{
public:
    NDS() noexcept : GPU(*this) {}

    melonDS::GPU GPU;

    // ARM9 time in the 2x CPU clock domain; the bridge synchronizes this with
    // the runner's ARM9 timestamp before letting the geometry engine run.
    u64 ARM9Timestamp = 0;
    u32 ARM9ClockShift = 1;

    void SetIRQ(u32 cpu, u32 irq);
    void ClearIRQ(u32 cpu, u32 irq);
    void CheckDMAs(u32 cpu, u32 mode);
    void GXFIFOStall();
    void GXFIFOUnstall();
};

}

#endif // NDS_H
