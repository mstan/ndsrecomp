/*
    ndsrecomp shim replacing melonDS's SPI_Firmware.h for the vendored Wifi
    device model.

    Wifi::Reset() (Wifi.cpp:147-186) reads exactly four things out of the
    firmware image's 512-byte header: RFChipType, ConsoleType,
    Type2Config.InitialRF56Values[84], and Type3Config.{RFIndex1, RFIndex2,
    RFData1[14], RFData2[14]}. Real melonDS represents the header as a
    512-byte union-of-struct laid directly over the firmware image bytes
    (src/SPI_Firmware.h FirmwareHeader, static_assert'd to sizeof==512).
    This shim does not reproduce that C++ struct; it is a thin typed VIEW
    over the runner's own already-loaded firmware buffer (io.cpp's g_fw,
    reached here via nds_firmware_bytes()/nds_firmware_size(), declared in
    runner/src/io.h) at the identical byte offsets. The SPI shim
    (runner/vendor/melonds/SPI.h) constructs the Firmware view from those
    two accessors; this header has no dependency on io.h itself so it stays
    includable from a plain vendored-source context.

    The byte offsets below were not guessed: they were derived by compiling
    a field-for-field transcription of melonDS's real FirmwareHeader (tag
    1.0rc) with this project's own mingw g++ and reading offsetof() -- the
    same compiler this file itself builds with, so struct-packing behavior
    can't drift between the derivation and the shim. The result
    (ConsoleType at 0x1D) matches the runner's pre-existing, independently
    GBATEK-sourced comment in runner/src/wifi.cpp
    ("FirmwareHeader::ConsoleType is byte 0x1D in retail DS firmware"),
    and sizeof(FirmwareHeader) came out to exactly 512, matching melonDS's
    own static_assert -- two independent cross-checks, not one guess:

      0x1D        ConsoleType            (1 byte)
      0x40        RFChipType             (1 byte)
      0xF2        Type2Config.InitialRF56Values (84 bytes)
      0x116       Type3Config.RFIndex1   (1 byte)
      0x117       Type3Config.RFData1    (14 bytes)
      0x125       Type3Config.RFIndex2   (1 byte)
      0x126       Type3Config.RFData2    (14 bytes)

    As an interface derived from melonDS this file is distributed under the
    same terms as the vendored sources: GPL-3.0-or-later (see GPU3D.h).
    Copyright 2016-2024 melonDS team; shim adaptation 2026 ndsrecomp.
*/

#ifndef SPI_FIRMWARE_H
#define SPI_FIRMWARE_H

#include <cstring>

#include "types.h"

namespace melonDS
{

class FirmwareHeader
{
public:
    enum class FirmwareConsoleType : u8
    {
        DS = 0xFF,
        DSLite = 0x20,
        DSi = 0x57,
        iQueDS = 0x43,
        iQueDSLite = 0x63,
    };

    struct Type2ConfigT
    {
        u8 InitialRF56Values[84];
    };

    struct Type3ConfigT
    {
        u8 RFIndex1;
        u8 RFData1[14];
        u8 RFIndex2;
        u8 RFData2[14];
    };

    FirmwareConsoleType ConsoleType = FirmwareConsoleType::DS;
    u8 RFChipType = 2;
    Type2ConfigT Type2Config {};
    Type3ConfigT Type3Config {};

    // Builds a view from raw firmware bytes. `size` short of the header (or
    // a null `data`) leaves every field at its zero/DS-default value rather
    // than reading out of bounds -- the runtime hash-verifies firmware.bin
    // before boot (PRINCIPLES.md), so this path is a defensive fallback,
    // not the expected case.
    explicit FirmwareHeader(const u8* data, u32 size) noexcept
    {
        if (!data || size < 0x134u) return;
        ConsoleType = static_cast<FirmwareConsoleType>(data[0x1D]);
        RFChipType = data[0x40];
        std::memcpy(Type2Config.InitialRF56Values, data + 0xF2, 84);
        Type3Config.RFIndex1 = data[0x116];
        std::memcpy(Type3Config.RFData1, data + 0x117, 14);
        Type3Config.RFIndex2 = data[0x125];
        std::memcpy(Type3Config.RFData2, data + 0x126, 14);
    }
};

class Firmware
{
public:
    using FirmwareConsoleType = FirmwareHeader::FirmwareConsoleType;

    Firmware() noexcept = default;
    Firmware(const u8* data, u32 size) noexcept : Data(data), Size(size) {}

    // Returned by value; Wifi.cpp binds it with `const auto&`, which is a
    // standard const-ref-to-prvalue lifetime extension, not a dangling ref.
    [[nodiscard]] FirmwareHeader GetHeader() const noexcept
    {
        return FirmwareHeader(Data, Size);
    }

private:
    const u8* Data = nullptr;
    u32 Size = 0;
};

}

#endif // SPI_FIRMWARE_H
