// io.cpp — see io.h. Register map per GBATEK ("DS I/O Maps"); clean-room.

#include "io.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "state.h"

namespace {

// ── IPCSYNC (0x04000180) — the cross-core handshake ─────────────────────
// A 16-bit register per CPU, but the cores are wired together: CPU X's
// output data (bits 11..8) appears as CPU Y's input data (bits 3..0), and
// vice-versa. Modeling that cross-wire is what lets the ARM9 reset wait
// and the ARM7 boot handshake see each other.
uint16_t g_ipcsync_out[2] = {0, 0};   // [cpu] = last value written (bits 8..14)

// POSTFLG (0x04000300) — per-CPU boot flag (bit0 latches set).
uint8_t  g_postflg[2] = {0, 0};

// Interrupt registers, per CPU.
uint32_t g_ime[2] = {0, 0};           // 0x208 master enable (bit0)
uint32_t g_ie[2]  = {0, 0};           // 0x210 enable mask
uint32_t g_if[2]  = {0, 0};           // 0x214 request flags (write-1-to-clear)

// Gamecard (no cartridge inserted → data reads back 0xFFFFFFFF). The boot
// probes the slot; we let the transfer "complete" immediately so the
// poll (ROMCTRL bit 23 data-ready / bit 31 busy) clears and the BIOS sees
// an empty slot. ROMCTRL = 0x040001A4; data port = 0x04100010.
uint32_t g_romctrl = 0;
uint32_t g_card_words_left = 0;

uint32_t card_block_words(uint32_t romctrl) {
    uint32_t n = (romctrl >> 24) & 7u;       // block-size field
    if (n == 0) return 0;                     // no transfer
    if (n == 7) return 1;                     // 4 bytes
    return (0x100u << n) / 4u;                // 0x200..0x4000 bytes
}

uint8_t  g_haltcnt[2] = {0, 0};       // 0x04000301 (ARM7: bit7 = HALT)
NdsEventCounts g_counts = {};

// IPC FIFO (0x04000184 CNT / 0x04000188 SEND / 0x04100000 RECV). Two 16-word
// hardware queues, one per direction. g_fifo[c] holds words CPU c has SENT
// (the other core RECVs them). The firmware boot uses this to hand the ARM9
// its menu-code entry. IRQ bits: 17 send-empty, 18 recv-not-empty.
uint32_t g_fifo[2][16] = {};
int      g_fifo_cnt[2] = {0, 0};
int      g_fifo_head[2] = {0, 0};
uint16_t g_fifocnt[2] = {0, 0};       // persistent bits: 2,10,14,15
uint32_t g_fifo_lastrx[2] = {0, 0};

void fifo_send(int c, uint32_t v) {
    if (!(g_fifocnt[c] & 0x8000u)) return;            // FIFOs disabled
    if (g_fifo_cnt[c] >= 16) { g_fifocnt[c] |= 0x4000u; return; }  // full → error
    if (c == 0) ++g_counts.fifo9to7;
    else ++g_counts.fifo7to9;
    bool was_empty = (g_fifo_cnt[c] == 0);
    g_fifo[c][(g_fifo_head[c] + g_fifo_cnt[c]) % 16] = v;
    ++g_fifo_cnt[c];
    int rcv = c ^ 1;
    if (was_empty && (g_fifocnt[rcv] & 0x0400u))      // receiver's recv-not-empty IRQ
        nds_raise_irq(rcv, 0x00040000u);              // IF bit 18
}
uint32_t fifo_recv(int c) {
    int snd = c ^ 1;                                   // read the other core's send FIFO
    if (g_fifo_cnt[snd] == 0) { g_fifocnt[c] |= 0x4000u; return g_fifo_lastrx[c]; }
    uint32_t v = g_fifo[snd][g_fifo_head[snd]];
    g_fifo_head[snd] = (g_fifo_head[snd] + 1) % 16;
    --g_fifo_cnt[snd];
    g_fifo_lastrx[c] = v;
    if (g_fifo_cnt[snd] == 0 && (g_fifocnt[snd] & 0x0004u))  // sender's send-empty IRQ
        nds_raise_irq(snd, 0x00020000u);               // IF bit 17
    return v;
}
uint16_t fifocnt_read(int c) {
    int snd = c, rcv = c ^ 1;
    uint16_t v = g_fifocnt[c] & 0xC404u;               // enable/IRQ/error bits
    if (g_fifo_cnt[snd] == 0)  v |= 0x0001u;           // send empty
    if (g_fifo_cnt[snd] >= 16) v |= 0x0002u;           // send full
    if (g_fifo_cnt[rcv] == 0)  v |= 0x0100u;           // recv empty
    if (g_fifo_cnt[rcv] >= 16) v |= 0x0200u;           // recv full
    return v;
}
void fifocnt_write(int c, uint16_t v) {
    if (v & 0x0008u) { g_fifo_cnt[c] = 0; g_fifo_head[c] = 0; }  // send-fifo clear
    uint16_t keep = g_fifocnt[c] & 0x4000u;            // error bit, sticky
    if (v & 0x4000u) keep = 0;                          // write-1 acks error
    g_fifocnt[c] = keep | (v & 0x8404u);               // store enable + IRQ enables
}

// SPI bus (ARM7). The boot path needs the firmware FLASH device (device
// select = 1): the ARM7 BIOS issues READ (0x03 + 24-bit address) and RDSR
// (0x05) commands and clocks the firmware bytes out of SPIDATA. Powerman
// (0) and touchscreen (2) are stubbed to 0 for now.
const uint8_t* g_fw = nullptr;
uint32_t       g_fw_size = 0;
uint16_t g_spicnt = 0;       // 0x040001C0
uint8_t  g_spi_cmd = 0, g_spi_resp = 0;
uint32_t g_spi_addr = 0;
int      g_spi_phase = 0;    // 0 cmd, 1-3 addr, 4 read-data, 10 status

void spi_byte(uint8_t v) {
    if (((g_spicnt >> 8) & 3) != 1) { g_spi_resp = 0; return; }  // not firmware
    switch (g_spi_phase) {
        case 0:                                   // command byte
            g_spi_cmd = v; g_spi_addr = 0; g_spi_resp = 0;
            if (v == 0x03) g_spi_phase = 1;        // READ → 3 address bytes
            else if (v == 0x05) g_spi_phase = 10;  // RDSR → status
            else g_spi_phase = 0;
            break;
        case 1: g_spi_addr = (g_spi_addr << 8) | v; g_spi_phase = 2; break;
        case 2: g_spi_addr = (g_spi_addr << 8) | v; g_spi_phase = 3; break;
        case 3: g_spi_addr = (g_spi_addr << 8) | v; g_spi_phase = 4; break;
        case 4:                                   // stream firmware bytes
            g_spi_resp = (g_fw && g_spi_addr < g_fw_size) ? g_fw[g_spi_addr] : 0xFF;
            ++g_spi_addr;
            break;
        case 10: g_spi_resp = 0x00; break;        // status: ready, not busy
        default: g_spi_resp = 0; break;
    }
}

// Timers: 4 per CPU at 0x04000100 + N*4 (counter/reload @+0, control @+2).
// Control: bits 0-1 prescaler (1/64/256/1024), bit 2 count-up (cascade),
// bit 6 IRQ-on-overflow, bit 7 enable. Driven from the global clock.
struct Timer { uint16_t reload, counter, ctrl; unsigned long long accum; };
Timer g_timer[2][4] = {};
unsigned long long g_timer_last = 0;
const uint32_t kPrescaler[4] = {1, 64, 256, 1024};

int g_warned = 0;

// Backing store for I/O registers we don't model with custom behavior. Many
// DS config registers (SOUNDBIAS, POWCNT, DISPCNT, WRAMCNT, sound channels…)
// are plain read/write latches: code writes a value and later reads it back —
// e.g. the ARM7 BIOS ramps SOUNDBIAS 0→0x200 by repeatedly read/inc/write,
// and spins forever if reads don't reflect writes. Covers 0x04000000..0x04002000
// (both 2D engines + sound + SPI-area config); special-cased registers above
// override. The address is the same on both CPUs, so a single array suffices
// (ARM9-only and ARM7-only register blocks don't overlap).
uint8_t  g_io_mem[0x2000] = {};
bool io_backed(uint32_t addr) { return addr >= 0x04000000u && addr < 0x04002000u; }
uint32_t io_mem_read(uint32_t addr, uint32_t width) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < width; ++i)
        v |= uint32_t(g_io_mem[(addr - 0x04000000u + i) & 0x1FFFu]) << (i * 8);
    return v;
}
void io_mem_write(uint32_t addr, uint32_t value, uint32_t width) {
    for (uint32_t i = 0; i < width; ++i)
        g_io_mem[(addr - 0x04000000u + i) & 0x1FFFu] = uint8_t(value >> (i * 8));
}

int active() { return (g_nds_active == NDS_ARM9) ? 0 : 1; }

uint16_t ipcsync_read(int cpu) {
    int other = cpu ^ 1;
    uint16_t v = g_ipcsync_out[cpu] & 0x4F00u;          // own out-data + ctrl, readable
    v |= (g_ipcsync_out[other] >> 8) & 0x000Fu;          // in-data = other core's out-data
    return v;
}

void ipcsync_write(int cpu, uint16_t val) {
    // Writable: bits 11..8 (out data), bit 14 (enable IRQ from other core).
    // Bit 13 is a send-IRQ pulse to the other core — wired when IRQ
    // delivery is modeled (the current handshake is polled).
    ++g_counts.ipcsync_w;
    g_ipcsync_out[cpu] = (g_ipcsync_out[cpu] & ~0x4F00u) | (val & 0x4F00u);
}

// Width-mask helpers for sub-word access to a 32-bit register value.
uint32_t mask_for(uint32_t width) {
    return width >= 4 ? 0xFFFFFFFFu : ((1u << (width * 8)) - 1u);
}

}  // namespace

void nds_io_reset() {
    for (int i = 0; i < 2; ++i) {
        g_ipcsync_out[i] = 0; g_postflg[i] = 0;
        g_ime[i] = 0; g_ie[i] = 0; g_if[i] = 0; g_haltcnt[i] = 0;
    }
    g_counts = {};
    g_romctrl = 0; g_card_words_left = 0;
    for (auto& cpu : g_timer) for (auto& t : cpu) t = Timer{};
    g_timer_last = 0;
    g_spicnt = 0; g_spi_cmd = 0; g_spi_resp = 0; g_spi_addr = 0; g_spi_phase = 0;
    for (int i = 0; i < 2; ++i) {
        g_fifo_cnt[i] = 0; g_fifo_head[i] = 0; g_fifocnt[i] = 0; g_fifo_lastrx[i] = 0;
    }
    for (auto& b : g_io_mem) b = 0;
    g_warned = 0;
}

void nds_io_load_firmware(const uint8_t* p, uint32_t n) { g_fw = p; g_fw_size = n; }

const NdsEventCounts& nds_event_counts() { return g_counts; }

uint64_t nds_event_value(const char* name) {
    if (!name) return UINT64_MAX;
    if (std::strcmp(name, "vblank9") == 0) return g_counts.vblank9;
    if (std::strcmp(name, "vblank7") == 0) return g_counts.vblank7;
    if (std::strcmp(name, "ipcsync_w") == 0) return g_counts.ipcsync_w;
    if (std::strcmp(name, "fifo9to7") == 0) return g_counts.fifo9to7;
    if (std::strcmp(name, "fifo7to9") == 0) return g_counts.fifo7to9;
    if (std::strcmp(name, "dma_done") == 0) return g_counts.dma_done;
    if (std::strcmp(name, "timer_ovf") == 0) return g_counts.timer_ovf;
    return UINT64_MAX;
}

uint32_t nds_io_debug_read(int cpu, uint32_t addr, uint32_t width) {
    NdsCpu old = g_nds_active;
    g_nds_active = (cpu == 7) ? NDS_ARM7 : NDS_ARM9;
    uint32_t v = nds_io_read(addr, width);
    g_nds_active = old;
    return v;
}

// ── Interrupt controller ────────────────────────────────────────────────
void nds_raise_irq(int cpu, uint32_t bits) { g_if[cpu & 1] |= bits; }

void nds_dump_irq() {
    for (int c = 0; c < 2; ++c)
        std::fprintf(stderr, "  ARM%c IME=%u IE=0x%08X IF=0x%08X IPCSYNC.out=0x%04X\n",
                     c == 0 ? '9' : '7', g_ime[c], g_ie[c], g_if[c], g_ipcsync_out[c]);
}

uint32_t nds_irq_pending(int cpu) {
    cpu &= 1;
    return (g_ime[cpu] & 1u) ? (g_ie[cpu] & g_if[cpu]) : 0u;
}

// ── Display + timer clocks (driven from the scheduler) ──────────────────
// Approximate DS-ish scanline timing in ARM9-cycle units; the precise
// numbers land with the melonDS oracle. VBlank (IF bit 0) fires once per
// frame. Both cores see it (a display event).
void nds_tick_hw(unsigned long long cyc) {
    // Display VBlank (IF bit 0), once per frame, both cores.
    static const unsigned long long SCAN = 2130, LINES = 263;
    static const unsigned long long FRAME = SCAN * LINES;
    static unsigned long long last = 0;
    unsigned long long pf = last / FRAME, pl = (last % FRAME) / SCAN;
    unsigned long long cf = cyc / FRAME,  cl = (cyc % FRAME) / SCAN;
    last = cyc;
    if ((cf > pf) || (pl < 192 && cl >= 192)) {
        ++g_counts.vblank9;
        ++g_counts.vblank7;
        nds_raise_irq(0, 0x1u); nds_raise_irq(1, 0x1u);
    }

    // Timers. Advance each enabled, non-cascade timer by the elapsed cycles
    // and raise its overflow IRQ (IF bit 3+N). Cascade (count-up) timers
    // are driven by the preceding timer's overflow.
    unsigned long long delta = cyc - g_timer_last;
    g_timer_last = cyc;
    if (delta == 0) return;
    for (int cpu = 0; cpu < 2; ++cpu) {
        unsigned long long carry = 0;  // overflows from timer N-1 (cascade)
        for (int t = 0; t < 4; ++t) {
            Timer& T = g_timer[cpu][t];
            if (!(T.ctrl & 0x80u)) { carry = 0; continue; }   // disabled
            unsigned long long ticks;
            if (T.ctrl & 0x4u) {                               // count-up
                ticks = carry;
            } else {
                T.accum += delta;
                uint32_t pre = kPrescaler[T.ctrl & 3u];
                ticks = T.accum / pre;
                T.accum %= pre;
            }
            carry = 0;
            if (ticks == 0) continue;
            unsigned long long span = 0x10000ull - T.counter;
            if (ticks < span) { T.counter = static_cast<uint16_t>(T.counter + ticks); continue; }
            ticks -= span;                                     // first overflow consumed
            uint32_t reload_span = 0x10000u - T.reload;
            if (reload_span == 0) reload_span = 0x10000u;
            unsigned long long extra = ticks / reload_span;
            T.counter = static_cast<uint16_t>(T.reload + (ticks % reload_span));
            carry = 1 + extra;                                 // overflows this round
            if (T.ctrl & 0x40u) {
                g_counts.timer_ovf += carry;
                nds_raise_irq(cpu, 1u << (3 + t));
            }
        }
    }
}

uint32_t nds_io_read(uint32_t addr, uint32_t width) {
    const int cpu = active();
    const uint32_t m = mask_for(width);
    // Timer counter/control (0x04000100..0x0400010F).
    if (addr >= 0x04000100u && addr < 0x04000110u) {
        int t = (addr - 0x04000100u) / 4u;
        uint32_t off = (addr - 0x04000100u) % 4u;
        uint32_t reg = g_timer[cpu][t].counter | (g_timer[cpu][t].ctrl << 16);
        return (reg >> (off * 8)) & m;
    }
    switch (addr) {
        case 0x04000180: case 0x04000181:
            return (ipcsync_read(cpu) >> ((addr & 1u) * 8)) & m;
        case 0x04000208:
            return g_ime[cpu] & m;
        case 0x04000210:
            return g_ie[cpu] & m;
        case 0x04000214:
            return g_if[cpu] & m;
        case 0x04000300:
            return g_postflg[cpu] & m;
        case 0x04000130: case 0x04000131:  // KEYINPUT (buttons; bit=0 → pressed)
            return (0x03FFu >> ((addr & 1u) * 8)) & m;   // nothing held
        case 0x04000136: case 0x04000137:  // EXTKEYIN (X/Y/pen/hinge, ARM7)
            return (0x007Fu >> ((addr & 1u) * 8)) & m;   // released, pen up, lid open
        case 0x040001A4: case 0x040001A5:
        case 0x040001A6: case 0x040001A7:
            return (g_romctrl >> ((addr & 3u) * 8)) & m;
        case 0x04000184: case 0x04000185:  // IPCFIFOCNT
            return (fifocnt_read(cpu) >> ((addr & 1u) * 8)) & m;
        case 0x04100000: case 0x04100001:
        case 0x04100002: case 0x04100003:  // IPCFIFORECV
            return (fifo_recv(cpu) >> ((addr & 3u) * 8)) & m;
        case 0x040001C0: case 0x040001C1:  // SPICNT (busy bit always clear)
            return ((g_spicnt & ~0x0080u) >> ((addr & 1u) * 8)) & m;
        case 0x040001C2: case 0x040001C3:  // SPIDATA (last clocked-in byte)
            return g_spi_resp & m;
        case 0x04100010:  // gamecard data — empty slot reads all-ones
            if (g_card_words_left) {
                if (--g_card_words_left == 0) {
                    g_romctrl &= ~0x80800000u;   // clear data-ready + busy
                    nds_raise_irq(cpu, 0x00080000u);  // bit 19: card xfer done
                }
            }
            return 0xFFFFFFFFu & m;
        default:
            if (io_backed(addr)) return io_mem_read(addr, width) & m;
            if (g_warned < 64) {
                std::fprintf(stderr, "[io] ARM%c read  0x%08X w%u (stub→0)\n",
                             cpu == 0 ? '9' : '7', addr, width);
                ++g_warned;
            }
            return 0;
    }
}

void nds_io_write(uint32_t addr, uint32_t value, uint32_t width) {
    const int cpu = active();
    // Timer reload/control (0x04000100..0x0400010F). A 16/32-bit write to a
    // timer: low half = reload, high half = control. Enabling (bit7 0→1)
    // reloads the counter.
    if (addr >= 0x04000100u && addr < 0x04000110u) {
        int t = (addr - 0x04000100u) / 4u;
        uint32_t off = (addr - 0x04000100u) % 4u;
        Timer& T = g_timer[cpu][t];
        if (off == 0 && width >= 2) T.reload = static_cast<uint16_t>(value);
        if ((off == 0 && width >= 4) || off == 2) {
            uint16_t newctrl = static_cast<uint16_t>(value >> (off == 2 ? 0 : 16));
            bool was_on = (T.ctrl & 0x80u) != 0;
            bool now_on = (newctrl & 0x80u) != 0;
            T.ctrl = newctrl;
            if (now_on && !was_on) { T.counter = T.reload; T.accum = 0; }
        }
        return;
    }
    switch (addr) {
        case 0x04000180:
            ipcsync_write(cpu, static_cast<uint16_t>(value));
            return;
        case 0x04000208:
            g_ime[cpu] = value & 0x1u;
            return;
        case 0x04000210:
            g_ie[cpu] = value;
            return;
        case 0x04000214:
            g_if[cpu] &= ~value;     // write-1-to-clear
            return;
        case 0x04000300:
            // POSTFLG: bit0 latches set; ARM9 may also set bit1.
            g_postflg[cpu] |= static_cast<uint8_t>(value & (cpu == 0 ? 0x3u : 0x1u));
            return;
        case 0x04000301:  // HALTCNT (ARM7): bit7 = HALT (modeled as a spin)
            g_haltcnt[cpu] = static_cast<uint8_t>(value);
            return;
        case 0x04000184:    // IPCFIFOCNT
            fifocnt_write(cpu, static_cast<uint16_t>(value));
            return;
        case 0x04000188:    // IPCFIFOSEND
            fifo_send(cpu, value);
            return;
        case 0x040001C0: {  // SPICNT
            bool was_hold = (g_spicnt & 0x0800u) != 0;
            g_spicnt = static_cast<uint16_t>(value);
            if (was_hold && !(g_spicnt & 0x0800u)) g_spi_phase = 0;  // CS released
            return;
        }
        case 0x040001C2:    // SPIDATA — clock one byte through the device
            if (g_spicnt & 0x8000u) spi_byte(static_cast<uint8_t>(value));
            return;
        case 0x040001A4:  // ROMCTRL (32-bit write starts a gamecard block)
            g_romctrl = value;
            if (value & 0x80000000u) {           // block start
                g_card_words_left = card_block_words(value);
                if (g_card_words_left)
                    g_romctrl |= 0x00800000u;     // data-word ready (bit 23)
                else
                    g_romctrl &= ~0x80000000u;    // nothing to transfer → done
                // Empty slot: the block transfer completes essentially
                // instantly. Raise the transfer-complete IRQ (IF bit 19)
                // for cores that wait on it (e.g. a DMA-driven read where
                // the data port isn't polled). IE gates whether it matters.
                nds_raise_irq(cpu, 0x00080000u);
            }
            return;
        default:
            if (io_backed(addr)) { io_mem_write(addr, value, width); return; }
            if (g_warned < 64) {
                std::fprintf(stderr, "[io] ARM%c write 0x%08X = 0x%X w%u (stub)\n",
                             cpu == 0 ? '9' : '7', addr, value, width);
                ++g_warned;
            }
            return;
    }
}
