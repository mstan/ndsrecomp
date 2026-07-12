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

// DISPSTAT (0x04000004) is PER-CPU (ARM9 and ARM7 each own one) + VCOUNT
// (0x04000006). The display controller raises the VBlank/HBlank/VCount IRQ
// only when the matching DISPSTAT enable bit is set (GBATEK; melonDS GPU).
// bits: 0 VBlank flag (RO), 1 HBlank flag (RO), 2 VCount-match flag (RO),
// 3 VBlank IRQ enable, 4 HBlank IRQ enable, 5 VCount IRQ enable,
// 7 = VCount-compare MSB, 15..8 = VCount-compare low 8 (LYC).
uint16_t g_dispstat[2] = {0, 0};   // writable bits (3,4,5,7,15..8), per CPU
uint16_t g_vcount = 0;             // current scanline 0..262
bool     g_in_vblank = false;      // current line >= 192

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

// ── sub-event break (debug-server anchoring) ────────────────────────────
// Armed by nds_event_break_arm; brk_check() (called right after every counter
// bump) trips g_brk_hit when the watched counter reaches the target. The
// scheduler polls nds_event_break_hit() between dispatched blocks. Disarmed by
// default (g_brk_ptr == nullptr → check is a no-op), so normal runs are
// unaffected. Symmetric with the oracle shim's g_brk_* mechanism.
const uint64_t* g_brk_ptr = nullptr;
uint64_t        g_brk_target = 0;
bool            g_brk_hit = false;
// True when the armed break is an insn7/insn9 anchor. Those need a PER-
// INSTRUCTION stop: the scheduler's block-boundary break overshoots by a whole
// recompiled function (hundreds of insns). g_nds_insn_stop makes
// runtime_should_yield (checked at every bank instruction) and the Tier-3 loop
// return AT exactly insn==target. Safe because the fp-stream bisector resets
// per K and never resumes from the mid-function stop. g_nds_insn_stop itself is
// defined at global scope (below) — it needs external linkage so
// runtime_should_yield / Tier-3 (other TUs) can read it via io.h.
bool            g_brk_is_insn = false;
inline void brk_check() {
    if (g_brk_ptr && *g_brk_ptr >= g_brk_target) {
        g_brk_hit = true;
        if (g_brk_is_insn) g_nds_insn_stop = true;
    }
}

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
    brk_check();
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

// SPI bus (ARM7). Three chip-selectable devices share SPIDATA (0x040001C2);
// SPICNT bits 9..8 select (0 power-management, 1 firmware FLASH, 2 touchscreen)
// and bit 11 holds chip-select across a multi-byte transfer. Each device keeps
// its own transaction phase; deasserting CS (a non-hold byte, or clearing the
// SPI enable) resets that phase. Modeled after melonDS's SPI device chips —
// the ARM7 firmware boot reads battery/backlight (power-man) and the RTC and
// branches on them, so an all-zero stub silently diverges the entire boot.
const uint8_t* g_fw = nullptr;
uint32_t       g_fw_size = 0;
uint16_t g_spicnt = 0;       // 0x040001C0
uint8_t  g_spi_resp = 0;     // byte clocked back on the next SPIDATA read

// Firmware FLASH (device 1): READ (0x03 + 24-bit addr, auto-increment) + RDSR.
uint8_t  g_fw_cmd = 0; uint32_t g_fw_addr = 0; int g_fw_phase = 0;
uint8_t fw_write(uint8_t v) {
    switch (g_fw_phase) {
        case 0:                                       // command byte
            g_fw_cmd = v; g_fw_addr = 0;
            if (v == 0x03) g_fw_phase = 1;             // READ → 3 address bytes
            else if (v == 0x05) g_fw_phase = 10;       // RDSR → status
            else g_fw_phase = 0;
            return 0;
        case 1: g_fw_addr = (g_fw_addr << 8) | v; g_fw_phase = 2; return 0;
        case 2: g_fw_addr = (g_fw_addr << 8) | v; g_fw_phase = 3; return 0;
        case 3: g_fw_addr = (g_fw_addr << 8) | v; g_fw_phase = 4; return 0;
        case 4: {                                     // stream firmware bytes
            uint8_t r = (g_fw && g_fw_addr < g_fw_size) ? g_fw[g_fw_addr] : 0xFF;
            ++g_fw_addr; return r;
        }
        case 10: return 0x00;                          // status: ready, not busy
        default: return 0;
    }
}
void fw_release() { g_fw_phase = 0; }

// Power management (device 0): index byte (bit7 = read), then a data byte.
// reg1 = battery (0 = OK), reg4 = backlight (0x40). RegMasks gate writes.
uint8_t  g_pm_index = 0; uint8_t g_pm_regs[8] = {}; uint8_t g_pm_masks[8] = {};
bool     g_pm_hold = false;
uint8_t pm_write(uint8_t v) {
    if (!g_pm_hold) { g_pm_index = v; g_pm_hold = true; return 0; }  // index byte
    uint32_t regid = g_pm_index & 7u;
    if (g_pm_index & 0x80u) return g_pm_regs[regid];                 // read
    g_pm_regs[regid] = (g_pm_regs[regid] & ~g_pm_masks[regid])       // write
                     | (v & g_pm_masks[regid]);
    return 0;
}
void pm_release() { g_pm_hold = false; }

// Touchscreen (device 2): control byte (bit7) selects a channel and latches a
// 12-bit conversion; the two following bytes shift it out MSB-first. Pen state
// is reported via EXTKEYIN bit6 (pen-up here); the ADC just needs stable values.
uint8_t  g_tsc_ctrl = 0; uint16_t g_tsc_conv = 0xFFF; int g_tsc_datapos = 0;
uint16_t g_tsc_x = 0, g_tsc_y = 0xFFF;   // pen up
uint8_t tsc_write(uint8_t v) {
    uint8_t out = (g_tsc_datapos == 1) ? ((g_tsc_conv >> 5) & 0xFFu)
                : (g_tsc_datapos == 2) ? ((g_tsc_conv << 3) & 0xFFu) : 0u;
    if (v & 0x80u) {                          // control byte
        g_tsc_ctrl = v; g_tsc_datapos = 1;
        switch (v & 0x70u) {
            case 0x10: g_tsc_conv = g_tsc_y; break;   // TouchY channel
            case 0x50: g_tsc_conv = g_tsc_x; break;   // TouchX channel
            default:   g_tsc_conv = 0xFFF;  break;
        }
        if (v & 0x08u) g_tsc_conv &= 0x0FF0;          // 8-bit conversion mode
    } else {
        ++g_tsc_datapos;
    }
    return out;
}
void tsc_release() { g_tsc_datapos = 0; }

void release_device(int dev) {
    switch (dev) {
        case 0: pm_release();  break;
        case 1: fw_release();  break;
        case 2: tsc_release(); break;
    }
}
void spi_transfer(uint8_t v) {
    int dev = (g_spicnt >> 8) & 3;
    switch (dev) {
        case 0: g_spi_resp = pm_write(v);  break;
        case 1: g_spi_resp = fw_write(v);  break;
        case 2: g_spi_resp = tsc_write(v); break;
        default: g_spi_resp = 0;           break;
    }
    if (!(g_spicnt & 0x0800u)) release_device(dev);   // no hold → CS deassert
}

// ── RTC (0x04000138, ARM7) — bit-banged serial clock chip ───────────────
// The firmware reads status + date/time during boot and branches on them; an
// unmodeled latch returns garbage and diverges the boot. Bit-bang protocol
// (melonDS RTC): bit2 = CS, bit1 = SCK, bit0 = SIO, bit4 = SIO direction
// (1 = host→RTC). CS rising edge starts a transfer; each SCK-low shifts one
// bit. Command byte (bit-reversed when top nibble is 0x6) selects a register;
// bit7 = read. Date/time is fixed to 2024-01-01 12:00:00 to match the oracle.
uint16_t g_rtc_io = 0;
uint8_t  g_rtc_input = 0; int g_rtc_inbit = 0, g_rtc_inpos = 0;
uint8_t  g_rtc_output[8] = {}; int g_rtc_outbit = 0, g_rtc_outpos = 0;
uint8_t  g_rtc_cmd = 0;
uint8_t  g_rtc_datetime[7] = {0x24, 0x01, 0x01, 0x01, 0x52, 0x00, 0x00};
uint8_t  g_rtc_status1 = 0x02;   // 24-hour mode, power-on/reset flag cleared
uint8_t  g_rtc_status2 = 0x00;

void rtc_cmd_read() {
    if ((g_rtc_cmd & 0x0Fu) != 0x06u) return;
    switch (g_rtc_cmd & 0x70u) {
        case 0x00: g_rtc_output[0] = g_rtc_status1; g_rtc_status1 &= 0x0Fu; break;
        case 0x40: g_rtc_output[0] = g_rtc_status2; break;
        case 0x20: std::memcpy(g_rtc_output, &g_rtc_datetime[0], 7); break;
        case 0x60: std::memcpy(g_rtc_output, &g_rtc_datetime[4], 3); break;
        default:   g_rtc_output[0] = 0; break;
    }
}
void rtc_byte_in(uint8_t v) {
    if (g_rtc_inpos == 0) {                       // command byte
        if ((v & 0xF0u) == 0x60u) {
            static const uint8_t rev[16] = {
                0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6,
                0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6};
            g_rtc_cmd = rev[v & 0xFu];
        } else {
            g_rtc_cmd = v;
        }
        if (g_rtc_cmd & 0x80u) rtc_cmd_read();
        return;
    }
    if ((g_rtc_cmd & 0x0Fu) == 0x06u) {           // accept status-register writes
        if ((g_rtc_cmd & 0x70u) == 0x00u && g_rtc_inpos == 1)
            g_rtc_status1 = (g_rtc_status1 & 0xF0u) | (v & 0x0Eu);
        else if ((g_rtc_cmd & 0x70u) == 0x40u && g_rtc_inpos == 1)
            g_rtc_status2 = v;
        // date/time writes ignored — we hold the oracle's fixed time
    }
}
uint16_t rtc_read() { return g_rtc_io; }
void rtc_write(uint16_t val, bool byte) {
    if (byte) val |= (g_rtc_io & 0xFF00u);
    if (val & 0x0004u) {                          // CS asserted
        if (!(g_rtc_io & 0x0004u)) {              // CS rising edge → start xfer
            g_rtc_input = 0; g_rtc_inbit = 0; g_rtc_inpos = 0;
            std::memset(g_rtc_output, 0, sizeof(g_rtc_output));
            g_rtc_outbit = 0; g_rtc_outpos = 0;
        } else if (!(val & 0x0002u)) {            // SCK low → shift one bit
            if (val & 0x0010u) {                  // write (host → RTC)
                if (val & 0x0001u) g_rtc_input |= (1u << g_rtc_inbit);
                if (++g_rtc_inbit >= 8) {
                    g_rtc_inbit = 0; rtc_byte_in(g_rtc_input);
                    g_rtc_input = 0; ++g_rtc_inpos;
                }
            } else {                              // read (RTC → host)
                if (g_rtc_output[g_rtc_outpos] & (1u << g_rtc_outbit))
                    g_rtc_io |= 0x0001u;
                else
                    g_rtc_io &= 0xFFFEu;
                if (++g_rtc_outbit >= 8) {
                    g_rtc_outbit = 0;
                    if (g_rtc_outpos < 7) ++g_rtc_outpos;
                }
            }
        }
    }
    if (val & 0x0010u) g_rtc_io = val;
    else               g_rtc_io = (g_rtc_io & 0x0001u) | (val & 0xFFFEu);
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
    // Writable/stored: bits 11..8 (out data) + bit 14 (enable IRQ from remote).
    ++g_counts.ipcsync_w;
    brk_check();
    g_ipcsync_out[cpu] = (g_ipcsync_out[cpu] & ~0x4F00u) | (val & 0x4F00u);
    // Bit 13 is a send-IRQ STROBE (not stored): pulse an IPCSYNC IRQ (IF bit 16)
    // on the OTHER core iff that core has enabled IPCSYNC IRQs (its bit 14). The
    // firmware boot handshake goes IRQ-driven around ipcsync_w~80 (the data
    // nibbles freeze and the cores signal via this strobe + a HALT/wait); the
    // melonDS oracle delivers it and reaches the menu, whereas leaving it
    // unwired parks the receiver forever — the ipcsync_w=95 deadlock. Verified
    // via the IPCSYNC ping-pong trace: oracle freezes at SYNC9=0x307/SYNC7=0x703
    // past N=80 (pure strobing) while native desynced.
    const int other = cpu ^ 1;
    if ((val & 0x2000u) && (g_ipcsync_out[other] & 0x4000u))
        nds_raise_irq(other, 0x00010000u);   // IF bit 16 = IPCSYNC
}

// Width-mask helpers for sub-word access to a 32-bit register value.
uint32_t mask_for(uint32_t width) {
    return width >= 4 ? 0xFFFFFFFFu : ((1u << (width * 8)) - 1u);
}

}  // namespace

// Global (external linkage): read by runtime_should_yield (runtime_arm.cpp) and
// the Tier-3 loop (tier3.cpp) via the io.h extern. Set by brk_check above.
bool g_nds_insn_stop = false;

void nds_io_reset() {
    for (int i = 0; i < 2; ++i) {
        g_ipcsync_out[i] = 0; g_postflg[i] = 0;
        g_ime[i] = 0; g_ie[i] = 0; g_if[i] = 0; g_haltcnt[i] = 0;
        g_dispstat[i] = 0;
    }
    g_vcount = 0; g_in_vblank = false;
    g_counts = {};
    g_romctrl = 0; g_card_words_left = 0;
    for (auto& cpu : g_timer) for (auto& t : cpu) t = Timer{};
    g_timer_last = 0;
    g_spicnt = 0; g_spi_resp = 0;
    g_fw_cmd = 0; g_fw_addr = 0; g_fw_phase = 0;
    g_pm_index = 0; g_pm_hold = false;
    std::memset(g_pm_regs, 0, sizeof(g_pm_regs));
    std::memset(g_pm_masks, 0, sizeof(g_pm_masks));
    g_pm_regs[4] = 0x40;                                  // backlight on
    g_pm_masks[0] = 0x7F; g_pm_masks[1] = 0x00; g_pm_masks[2] = 0x01;
    g_pm_masks[3] = 0x03; g_pm_masks[4] = 0x0F;
    g_tsc_ctrl = 0; g_tsc_conv = 0xFFF; g_tsc_datapos = 0;
    g_tsc_x = 0; g_tsc_y = 0xFFF;
    g_rtc_io = 0; g_rtc_input = 0; g_rtc_inbit = 0; g_rtc_inpos = 0;
    std::memset(g_rtc_output, 0, sizeof(g_rtc_output));
    g_rtc_outbit = 0; g_rtc_outpos = 0; g_rtc_cmd = 0;
    {
        static const uint8_t dt[7] = {0x24, 0x01, 0x01, 0x01, 0x52, 0x00, 0x00};
        std::memcpy(g_rtc_datetime, dt, 7);
    }
    g_rtc_status1 = 0x02; g_rtc_status2 = 0x00;
    for (int i = 0; i < 2; ++i) {
        g_fifo_cnt[i] = 0; g_fifo_head[i] = 0; g_fifocnt[i] = 0; g_fifo_lastrx[i] = 0;
    }
    for (auto& b : g_io_mem) b = 0;
    g_warned = 0;
}

void nds_io_load_firmware(const uint8_t* p, uint32_t n) { g_fw = p; g_fw_size = n; }

const NdsEventCounts& nds_event_counts() { return g_counts; }

void nds_note_insn_retired(int cpu) {
    if (cpu & 1) ++g_counts.insn7; else ++g_counts.insn9;
    brk_check();
}

uint64_t nds_event_value(const char* name) {
    if (!name) return UINT64_MAX;
    if (std::strcmp(name, "vblank9") == 0) return g_counts.vblank9;
    if (std::strcmp(name, "vblank7") == 0) return g_counts.vblank7;
    if (std::strcmp(name, "ipcsync_w") == 0) return g_counts.ipcsync_w;
    if (std::strcmp(name, "fifo9to7") == 0) return g_counts.fifo9to7;
    if (std::strcmp(name, "fifo7to9") == 0) return g_counts.fifo7to9;
    if (std::strcmp(name, "dma_done") == 0) return g_counts.dma_done;
    if (std::strcmp(name, "timer_ovf") == 0) return g_counts.timer_ovf;
    if (std::strcmp(name, "soundbias_w") == 0) return g_counts.soundbias_w;
    if (std::strcmp(name, "insn9") == 0) return g_counts.insn9;
    if (std::strcmp(name, "insn7") == 0) return g_counts.insn7;
    return UINT64_MAX;
}

namespace {
const uint64_t* event_ptr(const char* name) {
    if (!name) return nullptr;
    if (std::strcmp(name, "vblank9") == 0) return &g_counts.vblank9;
    if (std::strcmp(name, "vblank7") == 0) return &g_counts.vblank7;
    if (std::strcmp(name, "ipcsync_w") == 0) return &g_counts.ipcsync_w;
    if (std::strcmp(name, "fifo9to7") == 0) return &g_counts.fifo9to7;
    if (std::strcmp(name, "fifo7to9") == 0) return &g_counts.fifo7to9;
    if (std::strcmp(name, "dma_done") == 0) return &g_counts.dma_done;
    if (std::strcmp(name, "timer_ovf") == 0) return &g_counts.timer_ovf;
    if (std::strcmp(name, "soundbias_w") == 0) return &g_counts.soundbias_w;
    if (std::strcmp(name, "insn9") == 0) return &g_counts.insn9;
    if (std::strcmp(name, "insn7") == 0) return &g_counts.insn7;
    return nullptr;
}
}  // namespace

void nds_event_break_arm(const char* name, uint64_t target) {
    g_brk_ptr = event_ptr(name);
    g_brk_target = target;
    g_brk_hit = false;
    g_brk_is_insn = name && (std::strcmp(name, "insn7") == 0 ||
                             std::strcmp(name, "insn9") == 0);
    g_nds_insn_stop = false;
}
void nds_event_break_disarm() {
    g_brk_ptr = nullptr; g_brk_hit = false;
    g_brk_is_insn = false; g_nds_insn_stop = false;
}
bool nds_event_break_hit() { return g_brk_hit; }

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
    // Display VBlank (IF bit 0): exactly once per frame at scanline 192, both
    // cores. Timed in ARM9-cycle units. The DS display runs on the 33.51 MHz
    // system clock (2130 cycles/scanline, 263 lines = 560190 cyc/frame =
    // 59.83 Hz); nds_tick_hw is fed ARM9 cycles (67 MHz, 2x the system clock),
    // so each unit doubles: 4260 ARM9 cyc/scanline, 1,120,380 ARM9 cyc/frame.
    // VBlank begins at line 192. Count by absolute vblank index so a delta that
    // spans the line-192 boundary fires once (and a delta > 1 frame fires per
    // frame) — the old code double-fired (frame-wrap AND line-192) and used the
    // system-clock period against ARM9 cycles, making VBlank 4x too fast.
    static const unsigned long long SCAN = 4260, LINES = 263;
    static const unsigned long long FRAME = SCAN * LINES;
    static const unsigned long long VB_START = 192ull * SCAN;
    static unsigned long long last = 0;
    auto vb_index = [](unsigned long long c) -> unsigned long long {
        return c < VB_START ? 0ull : (c - VB_START) / FRAME + 1ull;
    };
    // Current scanline, for DISPSTAT/VCOUNT reads (round-granular; the precise
    // sub-scanline position lands with finer display timing if a poll needs it).
    g_vcount = static_cast<uint16_t>((cyc / SCAN) % LINES);
    g_in_vblank = (g_vcount >= 192);
    unsigned long long pv = vb_index(last), cv = vb_index(cyc);
    last = cyc;
    for (unsigned long long i = pv; i < cv; ++i) {
        // VBlank IRQ fires (and is counted) per CPU ONLY when that CPU enabled
        // it via DISPSTAT bit 3 — matching melonDS/GBATEK. Previously native
        // raised IF bit0 on both cores every frame unconditionally, a spurious
        // pending IRQ vs the oracle. Counting it the same way also makes
        // vblank9/vblank7 mean "delivered VBlank IRQs" (the oracle's semantics),
        // so they become valid cross-impl anchors once the guest enables VBlank.
        if (g_dispstat[0] & 0x0008u) { ++g_counts.vblank9; nds_raise_irq(0, 0x1u); }
        if (g_dispstat[1] & 0x0008u) { ++g_counts.vblank7; nds_raise_irq(1, 0x1u); }
        brk_check();
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
                brk_check();
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
        case 0x04000004: case 0x04000005: {  // DISPSTAT (per CPU)
            uint16_t v = g_dispstat[cpu] & 0xFFB8u;        // enable + LYC bits
            if (g_in_vblank) v |= 0x0001u;                  // VBlank flag
            uint16_t lyc = static_cast<uint16_t>(
                ((g_dispstat[cpu] & 0x0080u) << 1) | (g_dispstat[cpu] >> 8));
            if (lyc == g_vcount) v |= 0x0004u;              // VCount-match flag
            return (v >> ((addr & 1u) * 8)) & m;
        }
        case 0x04000006: case 0x04000007:    // VCOUNT (current scanline)
            return (g_vcount >> ((addr & 1u) * 8)) & m;
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
        case 0x04000138: case 0x04000139:  // RTC register (bit-banged serial)
            return (rtc_read() >> ((addr & 1u) * 8)) & m;
        case 0x04100010:  // gamecard data — empty slot reads all-ones
            if (g_card_words_left) {
                if (--g_card_words_left == 0) {
                    g_romctrl &= ~0x80800000u;   // clear data-ready + busy
                    // Cart transfer-done IRQ (IF bit19) only if AUXSPICNT
                    // (0x040001A0) bit14 enables it (melonDS ROMEndTransfer).
                    if (io_mem_read(0x040001A0u, 2) & 0x4000u)
                        nds_raise_irq(cpu, 0x00080000u);
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
        case 0x04000004: case 0x04000005: {  // DISPSTAT (per CPU)
            uint32_t shift = (addr & 1u) * 8;
            uint32_t wmask = mask_for(width) << shift;
            uint32_t merged = (g_dispstat[cpu] & ~wmask) | ((value << shift) & wmask);
            g_dispstat[cpu] = static_cast<uint16_t>(merged & 0xFFB8u);  // writable bits
            return;
        }
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
            uint16_t old = g_spicnt;
            g_spicnt = static_cast<uint16_t>(value);
            if ((old & 0x8000u) && !(g_spicnt & 0x8000u))   // SPI disabled → drop CS
                release_device((old >> 8) & 3);
            return;
        }
        case 0x040001C2:    // SPIDATA — clock one byte through the selected device
            if (g_spicnt & 0x8000u) spi_transfer(static_cast<uint8_t>(value));
            return;
        case 0x04000138:    // RTC register (bit-banged serial)
            rtc_write(static_cast<uint16_t>(value), width == 1);
            return;
        case 0x04000504: {  // SOUNDBIAS (ARM7) — track the boot ramp invariant
            uint32_t v = value & 0x3FFu;
            if (g_counts.soundbias_w == 0) g_counts.soundbias_first = v;
            ++g_counts.soundbias_w;
            g_counts.soundbias_last = v;
            brk_check();
            io_mem_write(addr, value, width);   // keep the latch (reads see it)
            return;
        }
        case 0x040001A4:  // ROMCTRL (32-bit write starts a gamecard block)
            g_romctrl = value;
            if (value & 0x80000000u) {           // block start
                g_card_words_left = card_block_words(value);
                if (g_card_words_left)
                    g_romctrl |= 0x00800000u;     // data-word ready (bit 23)
                else
                    g_romctrl &= ~0x80000000u;    // nothing to transfer → done
                // Empty slot: the block transfer completes essentially
                // instantly. Raise the transfer-complete IRQ (IF bit 19) only
                // if AUXSPICNT (0x040001A0) bit14 enables it (melonDS
                // ROMEndTransfer) — previously unconditional, a spurious IF
                // bit19 vs the oracle.
                if (io_mem_read(0x040001A0u, 2) & 0x4000u)
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
