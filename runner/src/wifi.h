// wifi.h -- Nintendo DS ARM7 Wi-Fi MMIO core (0x04800000-0x0480FFFF).
//
// The firmware owns this device directly.  The public interface deliberately
// keeps bus-width and POWCNT2 behavior visible so bus.cpp can preserve the DS
// memory-map rules instead of treating Wi-Fi as generic 0x04xxxxxx I/O.

#pragma once

#include <cstdint>

void nds_wifi_reset();
void nds_wifi_load_firmware(const uint8_t* data, uint32_t size);
void nds_wifi_set_power_control(bool enabled, uint64_t timestamp);
uint64_t nds_wifi_next_event_time();
void nds_wifi_run_events(uint64_t timestamp);
uint16_t nds_wifi_debug_if();
uint16_t nds_wifi_debug_ie();

// True only for the ARM7-visible Wi-Fi aperture.  `cpu` is 7 or 9.
bool nds_wifi_address(int cpu, uint32_t addr);

// Reads and writes reproduce NDS.cpp's external access semantics: the device
// itself is 16-bit, 32-bit accesses split into two halfwords, byte reads select
// a byte from an aligned halfword, and byte writes are ignored.  A disabled
// POWCNT2 Wi-Fi bit makes the aperture read as zero and drops writes.
uint32_t nds_wifi_read(uint32_t addr, uint32_t width, bool powered);
void nds_wifi_write(uint32_t addr, uint32_t value, uint32_t width, bool powered);
