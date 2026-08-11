/*
    ndsrecomp shim replacing melonDS's SPI.h for the vendored Wifi device
    model.

    Wifi.cpp/WifiAP.cpp only ever reach the SPI subsystem through one call,
    `NDS.SPI.GetFirmware()` (Wifi.cpp:147), to read RF calibration bytes at
    Wifi::Reset() time. Real melonDS's `NDS.SPI` is a full `SPIHost` owning
    the firmware/TSC/powerman devices; this shim supplies just the one
    accessor Wifi.cpp calls, backed by the runner's own SPI-firmware-read
    path (io.cpp's g_fw, via nds_firmware_bytes()/nds_firmware_size() --
    declared in runner/src/io.h, included from the bridge translation unit,
    not from this header, to keep this vendor-tree header dependency-free).
    The actual Firmware/FirmwareHeader byte-offset view lives in
    SPI_Firmware.h.

    As an interface derived from melonDS this file is distributed under the
    same terms as the vendored sources: GPL-3.0-or-later (see GPU3D.h).
    Copyright 2016-2024 melonDS team; shim adaptation 2026 ndsrecomp.
*/

#ifndef SPI_H
#define SPI_H

#include "SPI_Firmware.h"
#include "types.h"

namespace melonDS
{

class SPI
{
public:
    // Bound to the runner's own firmware buffer by the bridge
    // (runner/src/wifi_net.cpp) via SetFirmwareSource(); defaults to an
    // empty view so a shim NDS instance is safe to construct before any
    // firmware has been loaded.
    void SetFirmwareSource(const u8* data, u32 size) noexcept
    {
        FirmwareData = data;
        FirmwareSize = size;
    }

    [[nodiscard]] Firmware GetFirmware() const noexcept
    {
        return Firmware(FirmwareData, FirmwareSize);
    }

private:
    const u8* FirmwareData = nullptr;
    u32 FirmwareSize = 0;
};

}

#endif // SPI_H
