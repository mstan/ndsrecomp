// wifi.cpp -- stateful Nintendo DS Wi-Fi register/RAM core.
//
// This models the local device state used by the real ARM7 firmware.  Network
// transport is a separate concern: reads and writes here are architectural
// MMIO operations and must remain deterministic even when no access point is
// attached.

#include "wifi.h"

#include "runtime_arm.h"
#include "state.h"
#include "io.h"

#include <array>
#include <cstddef>
#include <cstring>

namespace {

constexpr uint32_t kWifiBase = 0x04800000u;
constexpr uint32_t kWifiEnd  = 0x04810000u;

// Register offsets used by reset/read semantics.  Names follow melonDS Wifi.h
// and GBATEK so later active-device behavior can grow without changing the
// memory representation.
constexpr uint32_t W_ID            = 0x000u;
constexpr uint32_t W_ModeReset     = 0x004u;
constexpr uint32_t W_ModeWEP       = 0x006u;
constexpr uint32_t W_TXStatCnt     = 0x008u;
constexpr uint32_t W_IF            = 0x010u;
constexpr uint32_t W_IE            = 0x012u;
constexpr uint32_t W_AIDLow        = 0x028u;
constexpr uint32_t W_AIDFull       = 0x02Au;
constexpr uint32_t W_TXRetryLimit  = 0x02Cu;
constexpr uint32_t W_TRXPower      = 0x034u;
constexpr uint32_t W_Preamble      = 0x0BCu;
constexpr uint32_t W_Random        = 0x044u;
constexpr uint32_t W_BBCnt         = 0x158u;
constexpr uint32_t W_BBRead        = 0x15Cu;
constexpr uint32_t W_BBBusy        = 0x15Eu;
constexpr uint32_t W_RFBusy        = 0x180u;
constexpr uint32_t W_TXBusy        = 0x0B6u;
constexpr uint32_t W_PowerUS       = 0x036u;
constexpr uint32_t W_PowerTX       = 0x038u;
constexpr uint32_t W_PowerState    = 0x03Cu;
constexpr uint32_t W_PowerForce    = 0x040u;
constexpr uint32_t W_PowerDownCtrl = 0x048u;
constexpr uint32_t W_RXBufReadAddr = 0x058u;
constexpr uint32_t W_RXBufCount    = 0x05Cu;
constexpr uint32_t W_RXBufDataRead = 0x060u;
constexpr uint32_t W_RXBufGapAddr  = 0x062u;
constexpr uint32_t W_RXBufGapSize  = 0x064u;
constexpr uint32_t W_TXBufWriteAddr = 0x068u;
constexpr uint32_t W_TXBufCount    = 0x06Cu;
constexpr uint32_t W_TXBufDataWrite = 0x070u;
constexpr uint32_t W_TXBufGapAddr  = 0x074u;
constexpr uint32_t W_TXBufGapSize  = 0x076u;
constexpr uint32_t W_TXSlotBeacon  = 0x080u;
constexpr uint32_t W_TXSlotCmd     = 0x090u;
constexpr uint32_t W_TXSlotReply1  = 0x094u;
constexpr uint32_t W_TXSlotReply2  = 0x098u;
constexpr uint32_t W_TXSlotLoc1    = 0x0A0u;
constexpr uint32_t W_TXSlotLoc2    = 0x0A4u;
constexpr uint32_t W_TXSlotLoc3    = 0x0A8u;
constexpr uint32_t W_TXReqReset    = 0x0ACu;
constexpr uint32_t W_TXReqSet      = 0x0AEu;
constexpr uint32_t W_TXSlotReset   = 0x0B4u;
constexpr uint32_t W_TXStat        = 0x0B8u;
constexpr uint32_t W_CMDStat0      = 0x1D0u;
constexpr uint32_t W_CMDStat7      = 0x1DEu;
constexpr uint32_t W_IFSet         = 0x21Cu;
constexpr uint32_t W_USCountCnt    = 0x0E8u;
constexpr uint32_t W_USCompareCnt  = 0x0EAu;
constexpr uint32_t W_CmdCountCnt   = 0x0EEu;
constexpr uint32_t W_USCompare0    = 0x0F0u;
constexpr uint32_t W_USCompare3    = 0x0F6u;
constexpr uint32_t W_USCount0      = 0x0F8u;
constexpr uint32_t W_USCount3      = 0x0FEu;
constexpr uint32_t W_ContentFree   = 0x10Cu;
constexpr uint32_t W_PreBeacon     = 0x110u;
constexpr uint32_t W_CmdCount      = 0x118u;
constexpr uint32_t W_BeaconCount1  = 0x11Cu;
constexpr uint32_t W_BeaconCount2  = 0x134u;
constexpr uint32_t W_ListenCount   = 0x088u;
constexpr uint32_t W_BeaconInterval = 0x08Cu;
constexpr uint32_t W_ListenInterval = 0x08Eu;
constexpr uint32_t W_TXReqRead     = 0x0B0u;
constexpr uint32_t W_CmdTotalTime  = 0x1C0u;
constexpr uint32_t W_CmdReplyTime  = 0x1C4u;
constexpr uint32_t W_TXBeaconTIM   = 0x084u;
constexpr uint32_t W_RXFilter      = 0x0D0u;
constexpr uint32_t W_RXFilter2     = 0x0D2u;
constexpr uint32_t W_TXHeaderCnt   = 0x194u;
constexpr uint32_t W_RFPins        = 0x19Cu;
constexpr uint32_t W_TXSeqNo       = 0x210u;
constexpr uint32_t W_RFStatus      = 0x214u;
constexpr uint32_t W_RXTXAddr      = 0x268u;
constexpr uint32_t kTimerInterval  = 8u; // Wi-Fi microseconds per tick

std::array<uint8_t, 0x2000> g_ram{};
std::array<uint16_t, 0x800> g_io{};
std::array<uint8_t, 0x100> g_bb{};
std::array<uint8_t, 0x100> g_bb_read_only{};
uint16_t g_random = 1u;
uint16_t g_device_id = 0x1440u;
bool g_enabled = false;
bool g_power_on = false;
bool g_block_beacon_irq14 = false;
int32_t g_timer_error = 0;
uint64_t g_timer_deadline = UINT64_MAX;
uint64_t g_us_timestamp = 0;
uint64_t g_us_counter = 0;
uint64_t g_us_compare = 0;
uint32_t g_cmd_counter = 0;
int32_t g_us_until_power_on = 0;

struct TxSlot {
    bool valid = false;
    uint16_t addr = 0;
    uint16_t length = 0;
    uint8_t rate = 0;
    uint8_t phase = 0;
    int32_t phase_time = 0;
    uint32_t halfword_time_mask = 0;
};
std::array<TxSlot, 6> g_tx_slots{};
uint32_t g_com_status = 0; // 0=idle, 2=transmitting (RX transport is separate)
uint32_t g_tx_cur_slot = UINT32_MAX;

uint16_t& io(uint32_t offset) {
    return g_io[(offset & 0x0FFFu) >> 1u];
}

uint16_t ram_read16(uint32_t offset) {
    offset &= 0x1FFEu;
    return static_cast<uint16_t>(g_ram[offset]) |
           static_cast<uint16_t>(g_ram[offset + 1u] << 8u);
}

void ram_write16(uint32_t offset, uint16_t value) {
    offset &= 0x1FFEu;
    g_ram[offset] = static_cast<uint8_t>(value);
    g_ram[offset + 1u] = static_cast<uint8_t>(value >> 8u);
}

void set_fixed_bb(uint32_t index, uint8_t value) {
    g_bb[index] = value;
    g_bb_read_only[index] = 1u;
}

void reset_bb() {
    g_bb.fill(0);
    g_bb_read_only.fill(0);
    set_fixed_bb(0x00u, 0x6Du);
    for (uint32_t i = 0x0Du; i <= 0x12u; ++i) set_fixed_bb(i, 0x00u);
    for (uint32_t i = 0x16u; i <= 0x1Au; ++i) set_fixed_bb(i, 0x00u);
    set_fixed_bb(0x27u, 0x00u);
    set_fixed_bb(0x4Du, 0x00u);
    set_fixed_bb(0x5Du, 0x01u);
    set_fixed_bb(0x5Eu, 0x00u);
    set_fixed_bb(0x5Fu, 0x00u);
    set_fixed_bb(0x60u, 0x00u);
    set_fixed_bb(0x61u, 0x00u);
    set_fixed_bb(0x64u, 0xFFu);
    set_fixed_bb(0x66u, 0x00u);
    for (uint32_t i = 0x69u; i < 0x100u; ++i) set_fixed_bb(i, 0x00u);
}

uint64_t active_system_timestamp() {
    return g_nds_active == NDS_ARM9 ? (g_runtime_cycles >> 1u)
                                    : g_runtime_cycles;
}

void schedule_timer(bool first, uint64_t timestamp) {
    if (first) g_timer_error = 0;
    int32_t cycles = static_cast<int32_t>(33513982u * kTimerInterval);
    cycles -= g_timer_error;
    const int32_t delay = (cycles + 999999) / 1000000;
    g_timer_error = delay * 1000000 - cycles;
    g_timer_deadline = first ? timestamp + static_cast<uint32_t>(delay)
                             : g_timer_deadline + static_cast<uint32_t>(delay);
    if (first) nds_reschedule_slice(g_timer_deadline);
}

void update_power_on(uint64_t timestamp) {
    const bool on = g_enabled && ((io(W_PowerUS) & 1u) == 0u);
    if (on == g_power_on) return;
    g_power_on = on;
    if (on) schedule_timer(true, timestamp);
    else g_timer_deadline = UINT64_MAX;
}

void check_irq(uint16_t old_flags) {
    const uint16_t new_flags = io(W_IF) & io(W_IE);
    if (old_flags == 0u && new_flags != 0u)
        nds_raise_irq(1, 0x01000000u); // ARM7 IF bit 24 = Wi-Fi
}

void set_irq(uint32_t index) {
    const uint16_t old_flags = io(W_IF) & io(W_IE);
    io(W_IF) |= static_cast<uint16_t>(1u << index);
    check_irq(old_flags);
}

void set_status(uint32_t status) {
    // Values driven on the RF status pins for each transceiver state.  The
    // firmware reads both registers while bringing the local radio up/down.
    static constexpr uint16_t kRfPins[10] = {
        0x0004u, 0x0084u, 0x0000u, 0x0046u, 0x0000u,
        0x0084u, 0x0087u, 0x0000u, 0x0046u, 0x0004u,
    };
    if (status >= std::size(kRfPins)) return;
    io(W_RFStatus) = static_cast<uint16_t>(status);
    io(W_RFPins) = kRfPins[status];
}

void update_power_status(int power) {
    // Transceiver power is separate from the Wi-Fi block clock controlled by
    // POWCNT2/W_POWER_US.  This is the firmware-visible state machine used by
    // melonDS: W_POWERSTATE bit 9 means powered down, while bit 8 marks the
    // requested power-up interval.
    int cur_flags = 0;
    if (io(W_TRXPower) == 1u) cur_flags |= 1;
    if ((io(W_PowerState) & 0x0200u) == 0u) cur_flags |= 2;
    int requested_flags = cur_flags;

    if (io(W_PowerForce) & 0x8000u) {
        requested_flags = (io(W_PowerForce) & 1u) ? 0 : 3;
    } else if ((io(W_ModeReset) & 1u) == 0u) {
        requested_flags = 0;
    } else {
        if (power == 0) {
            if ((io(W_PowerState) & 0x0202u) == 0x0202u) power = 1;
            else if ((io(W_PowerState) & 0x0201u) == 0x0001u) power = -1;
        }

        if (power == -1 && (io(W_PowerDownCtrl) & 1u)) power = 0;

        // Partial power states are not supported by melonDS either; values 1
        // and 2 of POWERDOWNCTRL both resolve to the fully-on state here.
        if (power == 1) requested_flags = 3;
        else if (power == -1) requested_flags = io(W_PowerDownCtrl) ? 3 : 0;
        else if (io(W_PowerDownCtrl) & 2u) requested_flags = 3;
    }

    if (requested_flags == cur_flags) return;

    if (requested_flags & 1) {
        if ((cur_flags & 1) == 0) {
            io(W_TRXPower) = 1u;
            set_status(1u);
        }
    } else {
        io(W_TRXPower) = 2u;
        // No network transport is active, so ComStatus is always idle and the
        // transition completes synchronously.
        io(W_TRXPower) = 0u;
        set_status(9u);
    }

    if (requested_flags & 2) {
        io(W_PowerState) |= 0x0100u;
        if ((cur_flags & 2) == 0 && g_us_until_power_on == 0) {
            g_us_until_power_on = -2048;
            set_irq(11u);
        }
    } else {
        io(W_PowerState) &= static_cast<uint16_t>(~0x0001u);
        io(W_PowerState) &= static_cast<uint16_t>(~0x0100u);
        io(W_PowerState) |= 0x0200u;
        g_us_until_power_on = 0;
    }
}

uint32_t preamble_length(uint32_t rate) {
    if (rate == 1u) return 192u;
    if (io(W_Preamble) & 0x0004u) return 96u;
    return 192u;
}

void tx_prepare_sequence(const TxSlot& slot, uint32_t number) {
    uint32_t no_sequence = g_ram[(slot.addr + 4u) & 0x1FFFu] ? 2u : 0u;
    if (number == 1u && (io(W_TXSlotCmd) & 0x4000u)) no_sequence = 1u;
    if (no_sequence != 0u) return;

    if ((io(W_TXHeaderCnt) & 0x0004u) == 0u)
        ram_write16(slot.addr + 0x0Cu + 22u,
                    static_cast<uint16_t>(io(W_TXSeqNo) << 4u));
    io(W_TXSeqNo) = static_cast<uint16_t>((io(W_TXSeqNo) + 1u) & 0x0FFFu);
}

void start_tx_beacon() {
    TxSlot& slot = g_tx_slots[4];
    slot.valid = true;
    slot.addr = static_cast<uint16_t>((io(W_TXSlotBeacon) & 0x0FFFu) << 1u);
    slot.length = ram_read16(slot.addr + 0x0Au) & 0x3FFFu;
    slot.rate = g_ram[(slot.addr + 8u) & 0x1FFFu] == 0x14u ? 2u : 1u;
    slot.phase = 0u;
    slot.phase_time = static_cast<int32_t>(preamble_length(slot.rate));
    slot.halfword_time_mask = UINT32_MAX;
    io(W_TXBusy) |= 0x0010u;
}

bool process_tx(TxSlot& slot, uint32_t number) {
    slot.phase_time -= static_cast<int32_t>(kTimerInterval);
    if (slot.phase_time > 0) {
        if (slot.phase == 1u &&
            (static_cast<uint32_t>(slot.phase_time) &
             slot.halfword_time_mask) == 0u)
            ++io(W_RXTXAddr);
        return false;
    }

    switch (slot.phase) {
        case 0u: {
            set_irq(7u);
            set_status(number == 5u ? 8u : 3u);
            uint32_t length_time = slot.length;
            if (slot.rate == 2u) {
                length_time *= 4u;
                slot.halfword_time_mask = 0x7u & ~7u;
            } else {
                length_time *= 8u;
                slot.halfword_time_mask = 0xFu & ~7u;
            }
            slot.phase = 1u;
            slot.phase_time = static_cast<int32_t>(length_time);
            io(W_RXTXAddr) = slot.addr >> 1u;
            if (number != 5u) tx_prepare_sequence(slot, number);
            return false;
        }
        case 1u:
            if (number != 1u && number != 5u)
                ram_write16(slot.addr, 0x0001u);
            g_ram[(slot.addr + 5u) & 0x1FFFu] = 0u;

            // The first active room path transmits firmware beacons.  Preserve
            // the complete guest-visible completion state even though packets
            // are not handed to a host network backend.
            if (number == 4u) {
                io(W_TXBusy) &= static_cast<uint16_t>(~0x0010u);
                if (io(W_TXStatCnt) & 0x8000u) {
                    io(W_TXStat) = 0x0301u;
                    set_irq(1u);
                }
                set_status(1u);
                return true;
            }
            return true;
        default:
            return true;
    }
}

void set_irq13() {
    set_irq(13u);
    if ((io(W_ModeWEP) & 7u) != 3u && (io(W_PowerTX) & 2u) == 0u)
        update_power_status(-1);
}

void set_irq14(uint32_t source) {
    // source: 0=USCOMPARE, 1=BEACONCOUNT, 2=forced by control write.
    if (source != 2u) io(W_BeaconCount1) = io(W_BeaconInterval);
    if (g_block_beacon_irq14 && source == 1u) return;
    if ((io(W_USCompareCnt) & 1u) == 0u) return;

    set_irq(14u);
    io(W_BeaconCount2) = 0xFFFFu;
    io(W_TXReqRead) &= 0xFFF2u;

    if (io(W_TXSlotBeacon) & 0x8000u) start_tx_beacon();
    if (io(W_ListenCount) == 0u) io(W_ListenCount) = io(W_ListenInterval);
    --io(W_ListenCount);
}

void set_irq15() {
    set_irq(15u);
    if (io(W_PowerTX) & 1u) update_power_status(1);
}

void ms_timer() {
    if (io(W_USCompareCnt) & 1u) {
        if ((g_us_counter & ~0x3FFull) == g_us_compare) {
            g_block_beacon_irq14 = false;
            set_irq14(0u);
        }
    }

    if (io(W_BeaconCount1) != 0u) {
        --io(W_BeaconCount1);
        if (io(W_BeaconCount1) == 0u) set_irq14(1u);
    }
    if (io(W_BeaconCount1) == 0u)
        io(W_BeaconCount1) = io(W_BeaconInterval);

    if (io(W_BeaconCount2) != 0u) {
        --io(W_BeaconCount2);
        if (io(W_BeaconCount2) == 0u) set_irq13();
    }
}

uint16_t read16(uint32_t addr) {
    if (addr >= kWifiEnd) return 0u;
    uint32_t offset = addr & 0x7FFEu;

    // 0x4000-0x5FFF is the 8 KiB packet RAM.  0x2000-0x3FFF is an
    // unpopulated region whose pull-ups read as all ones.  The remaining
    // mirrors select the 4 KiB register file.
    if (offset >= 0x4000u && offset < 0x6000u) return ram_read16(offset);
    if (offset >= 0x2000u && offset < 0x4000u) return 0xFFFFu;

    switch (offset) {
        case W_Random:
            // Same deterministic 11-bit LFSR step used by melonDS.  It is
            // intentionally read-driven; the firmware observes this state.
            g_random = static_cast<uint16_t>(
                (g_random & 1u) ^ (((g_random & 0x03FFu) << 1u) | (g_random >> 10u)));
            return g_random;
        case W_Preamble:
            return io(W_Preamble) & 0x0003u;
        case W_USCount0: return static_cast<uint16_t>(g_us_counter);
        case W_USCount0 + 2u: return static_cast<uint16_t>(g_us_counter >> 16u);
        case W_USCount0 + 4u: return static_cast<uint16_t>(g_us_counter >> 32u);
        case W_USCount3: return static_cast<uint16_t>(g_us_counter >> 48u);
        case W_USCompare0: return static_cast<uint16_t>(g_us_compare);
        case W_USCompare0 + 2u: return static_cast<uint16_t>(g_us_compare >> 16u);
        case W_USCompare0 + 4u: return static_cast<uint16_t>(g_us_compare >> 32u);
        case W_USCompare3: return static_cast<uint16_t>(g_us_compare >> 48u);
        case W_CmdCount:
            return static_cast<uint16_t>((g_cmd_counter + 9u) / 10u);
        case W_RXBufDataRead: {
            uint32_t read_addr = io(W_RXBufReadAddr) & 0x1FFEu;
            const uint16_t value = ram_read16(read_addr);
            read_addr += 2u;
            if (read_addr == (io(0x052u) & 0x1FFEu))
                read_addr = io(0x050u) & 0x1FFEu;
            if (read_addr == io(W_RXBufGapAddr)) {
                read_addr += static_cast<uint32_t>(io(W_RXBufGapSize)) << 1u;
                const uint32_t end = io(0x052u) & 0x1FFEu;
                const uint32_t begin = io(0x050u) & 0x1FFEu;
                if (read_addr >= end) read_addr = read_addr + begin - end;
                if (io(W_ID) == 0xC340u) io(W_RXBufGapSize) = 0u;
            }
            io(W_RXBufReadAddr) = static_cast<uint16_t>(read_addr & 0x1FFEu);
            io(W_RXBufDataRead) = value;
            if (io(W_RXBufCount) != 0u && --io(W_RXBufCount) == 0u) set_irq(9u);
            return value;
        }
        case W_BBRead:
            if ((io(W_BBCnt) & 0xF000u) != 0x6000u) return 0u;
            return g_bb[io(W_BBCnt) & 0x00FFu];
        case W_BBBusy:
        case W_RFBusy:
            return 0u;
        case W_TXBusy:
            return io(W_TXBusy) & 0x001Fu;
        default:
            if (offset >= W_CMDStat0 && offset <= W_CMDStat7 &&
                (offset & 1u) == 0u) {
                const uint16_t value = io(offset);
                io(offset) = 0u;
                return value;
            }
            return io(offset);
    }
}

bool read_only_register(uint32_t offset) {
    switch (offset) {
        case 0x000u: case 0x034u: case 0x044u: case 0x054u:
        case 0x098u: case 0x0B0u: case 0x0B6u: case 0x0B8u:
        case 0x15Cu: case 0x15Eu: case 0x180u: case 0x19Cu:
        case 0x1A8u: case 0x1ACu: case 0x1C4u: case 0x210u:
        case 0x214u: case 0x268u:
            return true;
        default:
            return false;
    }
}

void write16(uint32_t addr, uint16_t value) {
    if (addr >= kWifiEnd) return;
    uint32_t offset = addr & 0x7FFEu;

    if (offset >= 0x4000u && offset < 0x6000u) {
        ram_write16(offset, value);
        return;
    }
    if (offset >= 0x2000u && offset < 0x4000u) return;
    if (read_only_register(offset)) return;

    if (offset == W_IE) {
        const uint16_t old_flags = io(W_IF) & io(W_IE);
        io(W_IE) = value;
        check_irq(old_flags);
        return;
    }
    if (offset == W_IF) {
        io(W_IF) &= static_cast<uint16_t>(~value);
        return;
    }
    if (offset == W_IFSet) {
        const uint16_t old_flags = io(W_IF) & io(W_IE);
        io(W_IF) |= value & 0xFBFFu;
        check_irq(old_flags);
        return;
    }
    if (offset == W_TXReqReset) {
        io(W_TXReqRead) &= static_cast<uint16_t>(~value);
        return;
    }
    if (offset == W_TXReqSet) {
        io(W_TXReqRead) |= value;
        return;
    }

    if (offset == W_TXBufDataWrite) {
        uint32_t write_addr = io(W_TXBufWriteAddr) & 0x1FFEu;
        ram_write16(write_addr, value);
        write_addr += 2u;
        if (write_addr == io(W_TXBufGapAddr))
            write_addr += static_cast<uint32_t>(io(W_TXBufGapSize)) << 1u;
        io(W_TXBufWriteAddr) = static_cast<uint16_t>(write_addr & 0x1FFEu);
        if (io(W_TXBufCount) != 0u && --io(W_TXBufCount) == 0u) set_irq(8u);
        return;
    }
    if (offset == W_RXBufDataRead) {
        if (io(W_RXBufCount) != 0u && --io(W_RXBufCount) == 0u) set_irq(9u);
        return;
    }

    // Baseband writes are latched when BBCNT carries command 5, matching the
    // firmware-visible synchronous transfer behavior.
    if (offset == W_BBCnt) {
        io(W_BBCnt) = value;
        if ((value & 0xF000u) == 0x5000u) {
            const uint32_t index = value & 0x00FFu;
            if (!g_bb_read_only[index]) g_bb[index] = static_cast<uint8_t>(io(0x15Au));
        }
        return;
    }

    // These masks and state transitions are observable even without a network
    // backend.  Keep the local device model exact while transport remains a
    // separate concern.
    switch (offset) {
        case W_ModeReset: {
            const uint16_t old_value = io(W_ModeReset);
            io(W_ModeReset) = value & 0x0001u;

            if ((old_value & 1u) == 0u && (value & 1u) != 0u) {
                io(0x27Cu) = 0x0005u;
                update_power_status(0);
            } else if ((old_value & 1u) != 0u && (value & 1u) == 0u) {
                io(0x27Cu) = 0x000Au;
                update_power_status(0);
            }

            if (value & 0x2000u) {
                io(0x056u) = 0u; // W_RXBufWriteAddr
                io(W_CmdTotalTime) = 0u;
                io(W_CmdReplyTime) = 0u;
                io(0x1A4u) = 0u;
                io(0x278u) = 0x000Fu;
            }
            if (value & 0x4000u) {
                io(W_ModeWEP) = 0u;
                io(W_TXStatCnt) = 0u;
                io(0x00Au) = 0u;
                io(0x018u) = io(0x01Au) = io(0x01Cu) = 0u;
                io(0x020u) = io(0x022u) = io(0x024u) = 0u;
                io(W_AIDLow) = 0u;
                io(W_AIDFull) = 0u;
                io(W_TXRetryLimit) = 0x0707u;
                io(0x02Eu) = 0u;
                io(0x050u) = 0x4000u; // W_RXBufBegin
                io(0x052u) = 0x4800u; // W_RXBufEnd
                io(W_TXBeaconTIM) = 0u;
                io(W_Preamble) = 0x0001u;
                io(W_RXFilter) = 0x0401u;
                io(0x0D4u) = 0x0001u;
                io(W_RXFilter2) = 0x0008u;
                io(0x0ECu) = 0x3F03u;
                io(W_TXHeaderCnt) = 0u;
                io(0x198u) = 0u;
                io(0x1A2u) = 0x0001u;
                io(0x224u) = 0x0003u;
                io(0x230u) = 0x0047u;
            }
            return;
        }
        case W_ModeWEP:
            io(W_ModeWEP) = value & 0x007Fu;
            if (io(W_PowerTX) & 2u) {
                if ((io(W_ModeWEP) & 7u) == 1u) io(W_PowerDownCtrl) |= 2u;
                else if ((io(W_ModeWEP) & 7u) == 2u) io(W_PowerDownCtrl) = 3u;
                if ((io(W_ModeWEP) & 7u) != 3u) io(W_PowerState) &= 0x0300u;
                update_power_status(0);
            }
            return;
        case W_AIDLow: value &= 0x000Fu; break;
        case W_AIDFull: value &= 0x07FFu; break;
        case W_PowerUS:
            io(W_PowerUS) = value & 0x0003u;
            update_power_on(active_system_timestamp());
            return;
        case W_PowerTX:
            io(W_PowerTX) = value & 0x0003u;
            if (value & 2u) {
                if ((io(W_ModeWEP) & 7u) == 1u) io(W_PowerDownCtrl) |= 2u;
                else if ((io(W_ModeWEP) & 7u) == 2u) io(W_PowerDownCtrl) = 3u;
                update_power_status(0);
            }
            return;
        case W_PowerState:
            if ((io(W_ModeWEP) & 7u) != 3u) return;
            value = static_cast<uint16_t>((io(W_PowerState) & 0x0300u) |
                                          (value & 0x0003u));
            if ((value & 0x0300u) == 0x0200u) value &= static_cast<uint16_t>(~1u);
            else value &= static_cast<uint16_t>(~2u);
            if ((value & 0x0200u) == 0u) value &= static_cast<uint16_t>(~0x0100u);
            io(W_PowerState) = value;
            update_power_status(0);
            return;
        case W_PowerForce:
            io(W_PowerForce) = value & 0x8001u;
            update_power_status(0);
            return;
        case W_PowerDownCtrl:
            io(W_PowerDownCtrl) = value & 0x0003u;
            if (io(W_PowerTX) & 2u) {
                if ((io(W_ModeWEP) & 7u) == 1u) io(W_PowerDownCtrl) |= 2u;
                else if ((io(W_ModeWEP) & 7u) == 2u) io(W_PowerDownCtrl) = 3u;
            }
            update_power_status(0);
            return;
        case W_TXSlotReset:
            if (value & 0x0001u) io(W_TXSlotLoc1) &= 0x7FFFu;
            if (value & 0x0002u) io(W_TXSlotCmd) &= 0x7FFFu;
            if (value & 0x0004u) io(W_TXSlotLoc2) &= 0x7FFFu;
            if (value & 0x0008u) io(W_TXSlotLoc3) &= 0x7FFFu;
            if (value & 0x0040u) io(W_TXSlotReply2) &= 0x7FFFu;
            if (value & 0x0080u) io(W_TXSlotReply1) &= 0x7FFFu;
            value = 0u;
            break;
        case W_USCountCnt: value &= 0x0001u; break;
        case W_USCompareCnt:
            if (value & 0x0002u) set_irq14(2u);
            value &= 0x0001u;
            break;
        case 0x184u: value &= 0x413Fu; break; // W_RFCnt
        case 0x058u: case 0x062u:             // RX addresses
        case 0x068u: case 0x074u:             // TX addresses
            value &= 0x1FFEu; break;
        case 0x05Au: case 0x05Cu: case 0x056u: case 0x064u:
        case 0x06Cu: case 0x076u:
            value &= 0x0FFFu; break;
        case W_USCount0:
            g_us_counter = (g_us_counter & 0xFFFFFFFFFFFF0000ull) | value;
            return;
        case W_USCount0 + 2u:
            g_us_counter = (g_us_counter & 0xFFFFFFFF0000FFFFull) |
                           (static_cast<uint64_t>(value) << 16u);
            return;
        case W_USCount0 + 4u:
            g_us_counter = (g_us_counter & 0xFFFF0000FFFFFFFFull) |
                           (static_cast<uint64_t>(value) << 32u);
            return;
        case W_USCount3:
            g_us_counter = (g_us_counter & 0x0000FFFFFFFFFFFFull) |
                           (static_cast<uint64_t>(value) << 48u);
            return;
        case W_USCompare0:
            g_us_compare = (g_us_compare & 0xFFFFFFFFFFFF0000ull) |
                           (value & 0xFC00u);
            if (value & 0x0001u) g_block_beacon_irq14 = true;
            return;
        case W_USCompare0 + 2u:
            g_us_compare = (g_us_compare & 0xFFFFFFFF0000FFFFull) |
                           (static_cast<uint64_t>(value) << 16u);
            return;
        case W_USCompare0 + 4u:
            g_us_compare = (g_us_compare & 0xFFFF0000FFFFFFFFull) |
                           (static_cast<uint64_t>(value) << 32u);
            return;
        case W_USCompare3:
            g_us_compare = (g_us_compare & 0x0000FFFFFFFFFFFFull) |
                           (static_cast<uint64_t>(value) << 48u);
            return;
        case W_CmdCount:
            g_cmd_counter = static_cast<uint32_t>(value) * 10u;
            return;
        default: break;
    }
    io(offset) = value;
}

}  // namespace

void nds_wifi_reset() {
    g_ram.fill(0);
    g_io.fill(0);
    g_random = 1u;
    g_enabled = false;
    g_power_on = false;
    g_block_beacon_irq14 = false;
    g_timer_error = 0;
    g_timer_deadline = UINT64_MAX;
    g_us_timestamp = 0;
    g_us_counter = 0;
    g_us_compare = 0;
    g_cmd_counter = 0;
    g_us_until_power_on = 0;
    g_tx_slots.fill(TxSlot{});
    g_com_status = 0u;
    g_tx_cur_slot = UINT32_MAX;
    reset_bb();

    io(W_ID) = g_device_id;
    for (uint32_t offset = 0x018u; offset < 0x01Eu; offset += 2u) io(offset) = 0xFFFFu;
    for (uint32_t offset = 0x020u; offset < 0x026u; offset += 2u) io(offset) = 0xFFFFu;
    io(W_PowerUS) = 0x0001u;
}

void nds_wifi_load_firmware(const uint8_t* data, uint32_t size) {
    // FirmwareHeader::ConsoleType is byte 0x1D in retail DS firmware.  The
    // original DS radio identifies as 0x1440; Lite-class hardware as 0xC340.
    const uint8_t console = (data && size > 0x1Du) ? data[0x1Du] : 0xFFu;
    g_device_id = (console == 0x20u || console == 0x57u || console == 0x63u)
        ? 0xC340u : 0x1440u;
    io(W_ID) = g_device_id;
}

void nds_wifi_set_power_control(bool enabled, uint64_t timestamp) {
    g_enabled = enabled;
    update_power_on(timestamp);
}

uint64_t nds_wifi_next_event_time() { return g_timer_deadline; }

uint16_t nds_wifi_debug_if() { return io(W_IF); }
uint16_t nds_wifi_debug_ie() { return io(W_IE); }

void nds_wifi_run_events(uint64_t timestamp) {
    while (g_power_on && g_timer_deadline <= timestamp) {
        g_us_timestamp += kTimerInterval;
        if (g_us_until_power_on < 0) {
            g_us_until_power_on += static_cast<int32_t>(kTimerInterval);
            if (g_us_until_power_on >= 0) {
                g_us_until_power_on = 0;
                io(W_PowerState) = 0u;
                set_status(1u);
                update_power_status(0);
            }
        }
        if (io(W_USCountCnt) & 1u) {
            g_us_counter += kTimerInterval;
            const uint32_t us_part = static_cast<uint32_t>(g_us_counter) & 0x3FFu;
            if (io(W_USCompareCnt) & 1u) {
                const uint32_t beacon_us =
                    (static_cast<uint32_t>(io(W_BeaconCount1)) << 10u) |
                    (0x3FFu - us_part);
                if ((beacon_us & ~7u) == (io(W_PreBeacon) & ~7u))
                    set_irq15();
            }
            if ((us_part & ~7u) == 0u) ms_timer();
        }
        if ((io(W_CmdCountCnt) & 1u) && g_cmd_counter != 0u)
            g_cmd_counter = g_cmd_counter < kTimerInterval
                ? 0u : g_cmd_counter - kTimerInterval;
        if (io(W_ContentFree) != 0u)
            io(W_ContentFree) = io(W_ContentFree) < kTimerInterval
                ? 0u : static_cast<uint16_t>(io(W_ContentFree) - kTimerInterval);

        if (g_com_status == 0u) {
            const uint16_t busy = io(W_TXBusy);
            if (busy != 0u) {
                if (io(W_PowerState) & 0x0200u) {
                    g_tx_cur_slot = UINT32_MAX;
                } else {
                    g_com_status = 2u;
                    if      (busy & 0x0080u) g_tx_cur_slot = 5u;
                    else if (busy & 0x0010u) g_tx_cur_slot = 4u;
                    else if (busy & 0x0008u) g_tx_cur_slot = 3u;
                    else if (busy & 0x0004u) g_tx_cur_slot = 2u;
                    else if (busy & 0x0002u) g_tx_cur_slot = 1u;
                    else                    g_tx_cur_slot = 0u;
                }
            }
        }
        if ((g_com_status & 2u) && g_tx_cur_slot < g_tx_slots.size()) {
            const bool finished = process_tx(g_tx_slots[g_tx_cur_slot],
                                             g_tx_cur_slot);
            if (finished) {
                if (io(W_PowerState) & 0x0200u) {
                    io(W_TXBusy) = 0u;
                    io(W_TRXPower) = 0u;
                    set_status(9u);
                }
                const uint16_t busy = io(W_TXBusy);
                if      (busy & 0x0080u) g_tx_cur_slot = 5u;
                else if (busy & 0x0010u) g_tx_cur_slot = 4u;
                else if (busy & 0x0008u) g_tx_cur_slot = 3u;
                else if (busy & 0x0004u) g_tx_cur_slot = 2u;
                else if (busy & 0x0002u) g_tx_cur_slot = 1u;
                else if (busy & 0x0001u) g_tx_cur_slot = 0u;
                else {
                    g_tx_cur_slot = UINT32_MAX;
                    g_com_status = 0u;
                }
            }
        }
        schedule_timer(false, g_timer_deadline);
    }
}

bool nds_wifi_address(int cpu, uint32_t addr) {
    return cpu == 7 && addr >= kWifiBase && addr < kWifiEnd;
}

uint32_t nds_wifi_read(uint32_t addr, uint32_t width, bool powered) {
    if (!powered || addr < kWifiBase || addr >= kWifiEnd) return 0u;
    if (width == 1u) {
        const uint16_t value = read16(addr & ~1u);
        return (value >> ((addr & 1u) * 8u)) & 0xFFu;
    }
    if (width == 2u) return read16(addr);
    if (width == 4u)
        return static_cast<uint32_t>(read16(addr)) |
               (static_cast<uint32_t>(read16(addr + 2u)) << 16u);
    return 0u;
}

void nds_wifi_write(uint32_t addr, uint32_t value, uint32_t width, bool powered) {
    if (!powered || addr < kWifiBase || addr >= kWifiEnd) return;
    if (width == 2u) {
        write16(addr, static_cast<uint16_t>(value));
    } else if (width == 4u) {
        write16(addr, static_cast<uint16_t>(value));
        write16(addr + 2u, static_cast<uint16_t>(value >> 16u));
    }
    // ARM7 byte writes do not route to Wifi::Write in melonDS/NDS.cpp.
}
