#pragma once

#include <cstdint>
#include <vector>

// Generated firmware (beads-yjp.15 increment 2): a synthesized 128 KB
// firmware DATA image — header with Wi-Fi calibration, three access-point
// slots, two user-settings copies, valid checksums — byte-for-byte
// mirroring melonDS Firmware::Firmware(0) (SPI_Firmware.cpp), which is the
// oracle this image is diffed against. It contains NO boot code (the
// 'MELN' identifier marks it non-bootable), so a generated image is only
// usable with --boot direct; the caller enforces that pairing.
//
// The MAC is the caller's per-install identity — never a shared constant.
// melonDS ships DEFAULT_MAC 00:09:BF:11:22:33 for every single-instance
// user; we generate and persist a per-install MAC instead (main.cpp), and
// only force melonDS's value in oracle-diff harnesses.

constexpr uint32_t kNdsGeneratedFirmwareSize = 0x20000;

std::vector<uint8_t> nds_generate_firmware(const uint8_t mac[6]);
