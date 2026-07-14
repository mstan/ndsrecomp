// io.cpp — see io.h. Register map per GBATEK ("DS I/O Maps"); clean-room.

#include "io.h"
#include "wifi.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "state.h"
#include "scheduler.h"
#include "gpu2d.h"
#include "spu.h"
#include "vram.h"

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
uint16_t g_exmemcnt[2] = {0x4000u, 0x4000u}; // 0x204, shared ownership bits
uint16_t g_powercontrol7 = 0x0001u;    // 0x304 POWCNT2 (sound on, Wi-Fi off)
uint32_t g_keyinput = 0x007F03FFu;
uint16_t g_keycnt[2] = {};
uint16_t g_rcnt = 0;
uint8_t g_wramcnt = 0;
uint16_t g_wifiwaitcnt = 0;            // 0x206, visible only while Wi-Fi is on

// Gamecard (no cartridge inserted → data reads back 0xFFFFFFFF). The boot
// probes the slot; we let the transfer "complete" immediately so the
// poll (ROMCTRL bit 23 data-ready / bit 31 busy) clears and the BIOS sees
// an empty slot. ROMCTRL = 0x040001A4; data port = 0x04100010.
uint32_t g_romctrl = 0;
uint32_t g_card_transfer_pos = 0;   // bytes prepared so far
uint32_t g_card_transfer_len = 0;   // bytes in this block
uint64_t g_card_deadline = UINT64_MAX;
bool     g_card_end_event = false;
int      g_card_irq_cpu = 0;

// ARM9 integer math coprocessor. DIV/SQRT are asynchronous hardware units:
// every control/operand write restarts a completion event, BUSY remains set
// until that event, and result registers retain their previous value meanwhile.
uint16_t g_divcnt = 0;
uint32_t g_div_numer[2] = {};
uint32_t g_div_denom[2] = {};
uint32_t g_div_quot[2] = {};
uint32_t g_div_rem[2] = {};
uint64_t g_div_deadline = UINT64_MAX;
uint16_t g_sqrtcnt = 0;
uint32_t g_sqrt_val[2] = {};
uint32_t g_sqrt_res = 0;
uint64_t g_sqrt_deadline = UINT64_MAX;

uint32_t card_block_words(uint32_t romctrl) {
    uint32_t n = (romctrl >> 24) & 7u;       // block-size field
    if (n == 0) return 0;                     // no transfer
    if (n == 7) return 1;                     // 4 bytes
    return (0x100u << n) / 4u;                // 0x200..0x4000 bytes
}

uint64_t active_system_timestamp() {
    return (g_nds_active == NDS_ARM9) ? (g_runtime_cycles >> 1u)
                                       : g_runtime_cycles;
}

uint64_t words_u64(const uint32_t words[2]) {
    return uint64_t{words[0]} | (uint64_t{words[1]} << 32u);
}

int32_t as_s32(uint32_t v) {
    int32_t out;
    std::memcpy(&out, &v, sizeof(out));
    return out;
}

int64_t as_s64(const uint32_t words[2]) {
    const uint64_t bits = words_u64(words);
    int64_t out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

void store_s64(uint32_t words[2], int64_t value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    words[0] = static_cast<uint32_t>(bits);
    words[1] = static_cast<uint32_t>(bits >> 32u);
}

void start_div() {
    g_divcnt |= 0x8000u;
    g_div_deadline = active_system_timestamp() +
                     ((g_divcnt & 0x3u) == 0u ? 18u : 34u);
    nds_reschedule_slice(g_div_deadline);
}

void finish_div() {
    g_divcnt &= ~0xC000u;
    switch (g_divcnt & 0x3u) {
        case 0: {
            const int32_t num = as_s32(g_div_numer[0]);
            const int32_t den = as_s32(g_div_denom[0]);
            if (den == 0) {
                g_div_quot[0] = num < 0 ? 1u : 0xFFFFFFFFu;
                g_div_quot[1] = num < 0 ? 0xFFFFFFFFu : 0u;
                store_s64(g_div_rem, static_cast<int64_t>(num));
            } else if (num == INT32_MIN && den == -1) {
                store_s64(g_div_quot, static_cast<int64_t>(0x80000000u));
            } else {
                store_s64(g_div_quot, static_cast<int64_t>(num / den));
                store_s64(g_div_rem, static_cast<int64_t>(num % den));
            }
            break;
        }
        case 1:
        case 3: {
            const int64_t num = as_s64(g_div_numer);
            const int32_t den = as_s32(g_div_denom[0]);
            if (den == 0) {
                store_s64(g_div_quot, num < 0 ? 1 : -1);
                store_s64(g_div_rem, num);
            } else if (num == INT64_MIN && den == -1) {
                store_s64(g_div_quot, INT64_MIN);
                store_s64(g_div_rem, 0);
            } else {
                store_s64(g_div_quot, num / den);
                store_s64(g_div_rem, num % den);
            }
            break;
        }
        case 2: {
            const int64_t num = as_s64(g_div_numer);
            const int64_t den = as_s64(g_div_denom);
            if (den == 0) {
                store_s64(g_div_quot, num < 0 ? 1 : -1);
                store_s64(g_div_rem, num);
            } else if (num == INT64_MIN && den == -1) {
                store_s64(g_div_quot, INT64_MIN);
                store_s64(g_div_rem, 0);
            } else {
                store_s64(g_div_quot, num / den);
                store_s64(g_div_rem, num % den);
            }
            break;
        }
    }
    if ((g_div_denom[0] | g_div_denom[1]) == 0u)
        g_divcnt |= 0x4000u;
}

void start_sqrt() {
    g_sqrtcnt |= 0x8000u;
    g_sqrt_deadline = active_system_timestamp() + 13u;
    nds_reschedule_slice(g_sqrt_deadline);
}

void finish_sqrt() {
    g_sqrtcnt &= ~0x8000u;
    uint64_t val = (g_sqrtcnt & 1u) ? words_u64(g_sqrt_val)
                                     : uint64_t{g_sqrt_val[0]};
    const uint32_t nbits = (g_sqrtcnt & 1u) ? 32u : 16u;
    const uint32_t topshift = (g_sqrtcnt & 1u) ? 62u : 30u;
    uint32_t res = 0;
    uint64_t rem = 0;
    for (uint32_t i = 0; i < nbits; ++i) {
        rem = (rem << 2u) + ((val >> topshift) & 3u);
        val <<= 2u;
        res <<= 1u;
        const uint32_t prod = (res << 1u) + 1u;
        if (rem >= prod) { rem -= prod; ++res; }
    }
    g_sqrt_res = res;
}

uint32_t math_reg_read32(uint32_t addr) {
    switch (addr) {
        case 0x04000280u: return g_divcnt;
        case 0x04000290u: return g_div_numer[0];
        case 0x04000294u: return g_div_numer[1];
        case 0x04000298u: return g_div_denom[0];
        case 0x0400029Cu: return g_div_denom[1];
        case 0x040002A0u: return g_div_quot[0];
        case 0x040002A4u: return g_div_quot[1];
        case 0x040002A8u: return g_div_rem[0];
        case 0x040002ACu: return g_div_rem[1];
        case 0x040002B0u: return g_sqrtcnt;
        case 0x040002B4u: return g_sqrt_res;
        case 0x040002B8u: return g_sqrt_val[0];
        case 0x040002BCu: return g_sqrt_val[1];
        default: return 0;
    }
}

bool math_addr(uint32_t addr) {
    return (addr >= 0x04000280u && addr < 0x04000284u) ||
           (addr >= 0x04000290u && addr < 0x040002C0u);
}

void math_reg_write(uint32_t addr, uint32_t value, uint32_t width) {
    const uint32_t base = addr & ~3u;
    const uint32_t shift = (addr & 3u) * 8u;
    const uint32_t raw_mask = width >= 4u ? 0xFFFFFFFFu
                                          : ((1u << (width * 8u)) - 1u);
    const uint32_t bits = raw_mask << shift;
    auto merge = [&](uint32_t old) { return (old & ~bits) | ((value << shift) & bits); };
    switch (base) {
        case 0x04000280u: g_divcnt = static_cast<uint16_t>(merge(g_divcnt)); start_div(); break;
        case 0x04000290u: g_div_numer[0] = merge(g_div_numer[0]); start_div(); break;
        case 0x04000294u: g_div_numer[1] = merge(g_div_numer[1]); start_div(); break;
        case 0x04000298u: g_div_denom[0] = merge(g_div_denom[0]); start_div(); break;
        case 0x0400029Cu: g_div_denom[1] = merge(g_div_denom[1]); start_div(); break;
        case 0x040002B0u: g_sqrtcnt = static_cast<uint16_t>(merge(g_sqrtcnt)); start_sqrt(); break;
        case 0x040002B8u: g_sqrt_val[0] = merge(g_sqrt_val[0]); start_sqrt(); break;
        case 0x040002BCu: g_sqrt_val[1] = merge(g_sqrt_val[1]); start_sqrt(); break;
        default: break; // result registers are read-only
    }
}

void schedule_card_event(uint64_t delay, bool end_event) {
    g_card_deadline = active_system_timestamp() + delay;
    g_card_end_event = end_event;
    nds_reschedule_slice(g_card_deadline);
}

uint8_t  g_haltcnt[2] = {0, 0};       // 0x04000301 (ARM7: bit7 = HALT)
bool     g_cpu_halted[2] = {false, false};
uint64_t g_halt_entry_cycle[2] = {0, 0};
NdsEventCounts g_counts = {};
constexpr uint32_t kSpiTraceSize = 2048;
NdsSpiTraceEntry g_spi_trace[kSpiTraceSize] = {};
constexpr uint32_t kIrqTraceSize = 256;
NdsIrqTraceEntry g_irq_trace[2][kIrqTraceSize] = {};
// A firmware frame can retire well over 100K instructions on ARM7. Keep a
// complete frame so an event-aligned failure can always be traced back to its
// first producing instruction without arm-then-capture or repeated stepping.
constexpr uint32_t kInsnTraceSize = 262144;
NdsInsnTraceEntry g_insn_trace[2][kInsnTraceSize] = {};
constexpr uint32_t kFifoTraceSize = 64;
NdsFifoTraceEntry g_fifo_trace[2][kFifoTraceSize] = {};

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
    const uint64_t count = c == 0 ? g_counts.fifo9to7 : g_counts.fifo7to9;
    g_fifo_trace[c][(count - 1) % kFifoTraceSize] = {
        count,
        scheduler_system_timestamp(),
        c == 0 ? g_runtime_cycles : scheduler_cpu_cycles(0),
        c == 1 ? g_runtime_cycles : scheduler_cpu_cycles(1),
        g_counts.insn9,
        g_counts.insn7,
        v,
    };
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
    // IRQ enables are edge-sensitive on the enable bit as well as on FIFO
    // transitions. melonDS/NDS hardware immediately requests the condition
    // when software enables send-empty for an already-empty local queue, or
    // recv-not-empty while the peer queue already contains data.
    const uint16_t old = g_fifocnt[c];
    if ((v & 0x0004u) && !(old & 0x0004u) && g_fifo_cnt[c] == 0)
        nds_raise_irq(c, 0x00020000u);                 // IF bit 17
    if ((v & 0x0400u) && !(old & 0x0400u) && g_fifo_cnt[c ^ 1] != 0)
        nds_raise_irq(c, 0x00040000u);                 // IF bit 18
    uint16_t keep = g_fifocnt[c] & 0x4000u;            // error bit, sticky
    if (v & 0x4000u) keep = 0;                          // write-1 acks error
    g_fifocnt[c] = keep | (v & 0x8404u);               // store enable + IRQ enables
}

// DMA0..3 (0x040000B0..0x040000DF), one bank per CPU.  Transfers run on the
// system bus and stall their owning CPU; they are not CPU loads/stores and so
// use the raw DMA timing table (no ARM9 +3 non-sequential CPU penalty).
struct DmaChannel {
    uint32_t src = 0, dst = 0, cnt = 0;
    uint32_t cur_src = 0, cur_dst = 0, remaining = 0;
    int32_t src_inc = 1, dst_inc = 1;
    uint8_t start_mode = 0;
    uint16_t burst_index = 0;
    bool running = false;
    bool in_progress = false;
    bool burst_start = false;
};
DmaChannel g_dma[2][4] = {};
uint64_t g_dma_entry_cycle[2] = {};

enum class DmaRegion : uint8_t {
    Void, Main, Wram, Io, Palette, Vram, Oam, GbaRom, GbaRam, Wifi
};
struct DmaTiming { uint32_t n, s; DmaRegion region; uint8_t bus_width; };

DmaTiming dma_timing(int cpu, uint32_t addr, uint32_t width) {
    DmaTiming t{1u, 1u, DmaRegion::Void, 32u};
    if (addr >= 0x02000000u && addr < 0x03000000u)
        t = {8u, 1u, DmaRegion::Main, 16u};
    else if (addr >= 0x03000000u && addr < 0x04000000u)
        t = {1u, 1u, DmaRegion::Wram, 32u};
    else if (cpu == 1 && addr >= 0x04800000u && addr < 0x04810000u &&
             (g_powercontrol7 & 0x0002u)) {
        // Wi-Fi lives inside the broad 0x04xxxxxx I/O aperture but has its
        // own 16-bit wait-state bus.  Classify it before generic I/O or DMA
        // silently collapses every radio transfer to N=S=1.
        static constexpr uint8_t n[4] = {10u, 8u, 6u, 18u};
        if (addr < 0x04808000u)
            t = {n[g_wifiwaitcnt & 3u], (g_wifiwaitcnt & 0x4u) ? 4u : 6u,
                 DmaRegion::Wifi, 16u};
        else
            t = {n[(g_wifiwaitcnt >> 3u) & 3u],
                 (g_wifiwaitcnt & 0x20u) ? 4u : 10u,
                 DmaRegion::Wifi, 16u};
    } else if (addr >= 0x04000000u && addr < 0x05000000u)
        t = {1u, 1u, DmaRegion::Io, 32u};
    else if (addr >= 0x05000000u && addr < 0x06000000u)
        t = {1u, 1u, DmaRegion::Palette, 16u};
    else if (addr >= 0x06000000u && addr < 0x07000000u)
        t = {1u, 1u, DmaRegion::Vram, 16u};
    else if (addr >= 0x07000000u && addr < 0x08000000u)
        t = {1u, 1u, DmaRegion::Oam, 32u};
    else if (addr >= 0x08000000u && addr < 0x0A000000u) {
        const bool owner = ((g_exmemcnt[0] & 0x0080u) != 0u) == (cpu == 1);
        if (owner) {
            static constexpr uint8_t n[4] = {10u, 8u, 6u, 18u};
            const uint16_t ex = g_exmemcnt[cpu];
            t = {n[(ex >> 2u) & 3u], (ex & 0x10u) ? 4u : 6u,
                 DmaRegion::GbaRom, 16u};
        }
    } else if (addr >= 0x0A000000u && addr < 0x0B000000u) {
        const bool owner = ((g_exmemcnt[0] & 0x0080u) != 0u) == (cpu == 1);
        if (owner) {
            static constexpr uint8_t n[4] = {10u, 8u, 6u, 18u};
            const uint32_t wait = n[g_exmemcnt[cpu] & 3u];
            // Effective melonDS DMA table: buswidth=8 takes its non-16-bit
            // path, so N32=N16 and S32=S16.
            t = {wait, wait, DmaRegion::GbaRam, 32u};
        }
    }
    if (width >= 4u && t.bus_width == 16u) {
        t.n += t.s;
        t.s += t.s;
    }
    return t;
}

bool dma_same_bank(DmaRegion a, DmaRegion b) {
    return a == b && a != DmaRegion::Void;
}

uint32_t mram_read16(uint32_t table, uint32_t index) {
    if (table == 0u) {
        const uint32_t p = index % 240u;
        if (p == 0u || p == 119u || p == 238u) return 7u;
        if (p == 1u || p == 120u || p == 239u) return 3u;
        return 2u;
    }
    if (table == 1u) {
        const uint32_t p = index % 96u;
        return p == 0u ? 8u : p == 1u ? 6u : 5u;
    }
    const uint32_t p = index % 240u;
    if (p == 0u) return 10u;
    if (p % 34u == 0u) return 12u;
    if (p == 1u || (p > 1u && (p - 1u) % 34u == 0u)) return 8u;
    return 7u;
}

uint32_t mram_read32(uint32_t table, uint32_t index) {
    if (table == 0u) {
        const uint32_t p = index % 80u;
        return p == 0u || p == 79u ? 9u : p == 1u ? 4u : 3u;
    }
    if (table == 1u) {
        const uint32_t p = index % 118u;
        return p == 0u ? 9u : p == 1u ? 3u : 2u;
    }
    if (table == 2u) {
        const uint32_t p = index % 136u;
        if (p == 0u) return 14u;
        if (p % 27u == 0u) return 13u;
        if (p == 1u || (p > 1u && (p - 1u) % 27u == 0u)) return 10u;
        return 9u;
    }
    const uint32_t p = index % 96u;
    if (p == 0u) return 18u;
    if (p % 19u == 0u) return 17u;
    if (p == 1u || (p > 1u && (p - 1u) % 19u == 0u)) return 14u;
    return 13u;
}

uint32_t mram_write16(uint32_t table, uint32_t index) {
    static constexpr uint16_t len[3] = {120u, 48u, 35u};
    static constexpr uint8_t first[3] = {8u, 10u, 9u};
    static constexpr uint8_t steady[3] = {2u, 5u, 7u};
    const uint32_t p = index % len[table];
    return p == 0u ? first[table] : steady[table];
}

uint32_t mram_write32(uint32_t table, uint32_t index) {
    static constexpr uint16_t len[4] = {60u, 80u, 24u, 18u};
    static constexpr uint8_t first[4] = {9u, 9u, 15u, 16u};
    static constexpr uint8_t steady[4] = {4u, 3u, 10u, 14u};
    const uint32_t p = index % len[table];
    return p == 0u ? first[table] : steady[table];
}

uint32_t dma_unit_cycles(int cpu, DmaChannel& d, uint32_t width) {
    const DmaTiming src = dma_timing(cpu, d.cur_src, width);
    const DmaTiming dst = dma_timing(cpu, d.cur_dst, width);
    const bool first = d.burst_start;
    const uint32_t i = d.burst_index++;

    if (src.region == DmaRegion::Main && dst.region == DmaRegion::Main)
        return width >= 4u ? 18u : 16u;

    // Main-RAM burst behavior is asymmetric and periodically restarts. These
    // compact formulas are exactly equivalent to melonDS's measured burst
    // tables, including the back-to-back penalties at some wrap boundaries.
    if (d.dst_inc > 0 && dst.region == DmaRegion::Main) {
        if (width >= 4u) {
            const uint32_t table =
                (src.region == DmaRegion::GbaRom || src.region == DmaRegion::Wifi)
                    ? (src.s == 8u ? 2u : 3u)
                    : (src.n == 2u ? 0u : 1u);
            return mram_write32(table, i);
        }
        const uint32_t table =
            (src.region == DmaRegion::GbaRom || src.region == DmaRegion::Wifi)
                ? (src.s == 4u ? 1u : 2u) : 0u;
        return mram_write16(table, i);
    }
    if (d.src_inc > 0 && src.region == DmaRegion::Main) {
        if (width >= 4u) {
            const uint32_t table =
                (dst.region == DmaRegion::GbaRom || dst.region == DmaRegion::Wifi)
                    ? (dst.s == 8u ? 2u : 3u)
                    : (dst.n == 2u ? 0u : 1u);
            return mram_read32(table, i);
        }
        const uint32_t table =
            (dst.region == DmaRegion::GbaRom || dst.region == DmaRegion::Wifi)
                ? (dst.s == 4u ? 1u : 2u) : 0u;
        return mram_read16(table, i);
    }

    if (src.region == DmaRegion::Main)
        return (width >= 4u
            ? (((d.cur_src & 0x1Fu) == 0x1Cu) ? (dst.n == 2u ? 7u : 8u) : 9u)
            : (((d.cur_src & 0x1Fu) == 0x1Eu) ? 7u : 8u))
            + (first ? dst.n : dst.s);
    if (dst.region == DmaRegion::Main)
        return (first ? src.n : src.s) + (width >= 4u ? 8u : 7u);

    if (dma_same_bank(src.region, dst.region)) return src.n + dst.n + 1u;
    return first ? src.n + dst.n : src.s + dst.s;
}

bool dma_any_running(int cpu) {
    for (const auto& d : g_dma[cpu]) if (d.running) return true;
    return false;
}

void dma_start(int cpu, int ch) {
    DmaChannel& d = g_dma[cpu][ch];
    if (d.running || !(d.cnt & 0x80000000u)) return;
    const bool was_running = dma_any_running(cpu);
    if (!d.in_progress) {
        const uint32_t mask = cpu == 0 ? 0x001FFFFFu
                              : ch == 3 ? 0x0000FFFFu : 0x00003FFFu;
        d.remaining = d.cnt & mask;
        if (!d.remaining) d.remaining = mask + 1u;
    }
    if ((d.cnt & 0x01800000u) == 0x01800000u) d.cur_src = d.src;
    if ((d.cnt & 0x00600000u) == 0x00600000u) d.cur_dst = d.dst;
    d.burst_index = 0;
    d.burst_start = true;
    d.running = true;
    d.in_progress = true;
    if (!was_running) g_dma_entry_cycle[cpu] = g_runtime_cycles;
}

void dma_write_count(int cpu, int ch, uint32_t value) {
    DmaChannel& d = g_dma[cpu][ch];
    const uint32_t old = d.cnt;
    d.cnt = value;
    if (!(old & 0x80000000u) && (value & 0x80000000u)) {
        d.cur_src = d.src;
        d.cur_dst = d.dst;
        switch (value & 0x00600000u) {
            case 0x00000000u: d.dst_inc = 1; break;
            case 0x00200000u: d.dst_inc = -1; break;
            case 0x00400000u: d.dst_inc = 0; break;
            default:          d.dst_inc = 1; break;
        }
        switch (value & 0x01800000u) {
            case 0x00000000u: d.src_inc = 1; break;
            case 0x00800000u: d.src_inc = -1; break;
            case 0x01000000u: d.src_inc = 0; break;
            default:          d.src_inc = 1; break;
        }
        d.start_mode = cpu == 0 ? uint8_t((value >> 27u) & 7u)
                                : uint8_t(((value >> 28u) & 3u) | 0x10u);
        if ((d.start_mode & 7u) == 0u) dma_start(cpu, ch);
    }
}

bool dma_reg_addr(uint32_t addr) {
    return addr >= 0x040000B0u && addr < 0x040000E0u;
}

uint32_t dma_reg_read(int cpu, uint32_t addr, uint32_t width) {
    const uint32_t rel = addr - 0x040000B0u;
    const int ch = int(rel / 12u);
    const uint32_t off = rel % 12u;
    const uint32_t regoff = off & ~3u;
    const uint32_t shift = (off & 3u) * 8u;
    const DmaChannel& d = g_dma[cpu][ch];
    const uint32_t reg = regoff == 0u ? d.src : regoff == 4u ? d.dst : d.cnt;
    const uint32_t mask = width >= 4u ? 0xFFFFFFFFu : (1u << (width * 8u)) - 1u;
    return (reg >> shift) & mask;
}

void dma_reg_write(int cpu, uint32_t addr, uint32_t value, uint32_t width) {
    const uint32_t rel = addr - 0x040000B0u;
    const int ch = int(rel / 12u);
    const uint32_t off = rel % 12u;
    const uint32_t regoff = off & ~3u;
    const uint32_t shift = (off & 3u) * 8u;
    const uint32_t field = width >= 4u ? 0xFFFFFFFFu
                                      : ((1u << (width * 8u)) - 1u) << shift;
    DmaChannel& d = g_dma[cpu][ch];
    uint32_t* reg = regoff == 0u ? &d.src : regoff == 4u ? &d.dst : &d.cnt;
    const uint32_t merged = (*reg & ~field) | ((value << shift) & field);
    if (regoff == 8u) dma_write_count(cpu, ch, merged);
    else *reg = merged;
}

// SPI bus (ARM7). Three chip-selectable devices share SPIDATA (0x040001C2);
// SPICNT bits 9..8 select (0 power-management, 1 firmware FLASH, 2 touchscreen)
// and bit 11 holds chip-select across a multi-byte transfer. Each device keeps
// its own transaction phase; deasserting CS (a non-hold byte, or clearing the
// SPI enable) resets that phase. Modeled after melonDS's SPI device chips —
// the ARM7 firmware boot reads battery/backlight (power-man) and the RTC and
// branches on them, so an all-zero stub silently diverges the entire boot.
std::vector<uint8_t> g_fw;
uint16_t g_spicnt = 0;       // 0x040001C0
uint8_t  g_spi_resp = 0;     // byte clocked back on the next SPIDATA read
uint64_t g_spi_deadline = UINT64_MAX;

// Firmware FLASH (device 1).  Match melonDS FirmwareMem byte-for-byte: the
// first byte after CS assertion selects the command, subsequent bytes either
// build a 24-bit address or transfer data, and releasing CS only terminates
// the transaction (it does not clear the write-enable latch).
bool     g_fw_hold = false;
uint8_t  g_fw_cmd = 0;
uint8_t  g_fw_status = 0;
uint32_t g_fw_addr = 0;
uint32_t g_fw_data_pos = 0;

uint32_t fw_mask() {
    return g_fw.empty() ? 0u : static_cast<uint32_t>(g_fw.size() - 1u);
}

uint8_t fw_write(uint8_t v) {
    if (!g_fw_hold) {
        g_fw_cmd = v;
        g_fw_hold = true;
        g_fw_data_pos = 1;
        g_fw_addr = 0;
        if (g_fw_cmd == 0x04u) g_fw_status &= ~(1u << 1); // WRDI
        if (g_fw_cmd == 0x06u) g_fw_status |=  (1u << 1); // WREN
        return 0;
    }

    switch (g_fw_cmd) {
        case 0x03: // READ
            if (g_fw_data_pos < 4u) {
                g_fw_addr = (g_fw_addr << 8u) | v;
                ++g_fw_data_pos;
                return 0;
            }
            if (g_fw.empty()) return 0xFF;
            {
                const uint8_t result = g_fw[g_fw_addr & fw_mask()];
                ++g_fw_addr;
                ++g_fw_data_pos;
                return result;
            }

        case 0x05: // RDSR
            return g_fw_status;

        case 0x0A: // WRITE (the command used by this melonDS firmware model)
            if (g_fw_data_pos < 4u) {
                g_fw_addr = (g_fw_addr << 8u) | v;
                ++g_fw_data_pos;
                return 0;
            }
            if (!g_fw.empty()) g_fw[g_fw_addr & fw_mask()] = v;
            ++g_fw_addr;
            ++g_fw_data_pos;
            return v;

        case 0x9F: { // JEDEC ID: ST M25PE40-compatible response
            uint8_t result = 0;
            if (g_fw_data_pos == 1u) result = 0x20;
            else if (g_fw_data_pos == 2u) result = 0x40;
            else if (g_fw_data_pos == 3u) result = 0x12;
            ++g_fw_data_pos;
            return result;
        }

        default:
            return 0xFF;
    }
}
void fw_release() {
    g_fw_hold = false;
    g_fw_cmd = 0;
}

// Power management (device 0): index byte (bit7 = read), then a data byte.
// reg1 = battery (0 = OK), reg4 = backlight (0x40). RegMasks gate writes.
uint8_t  g_pm_index = 0; uint8_t g_pm_regs[8] = {}; uint8_t g_pm_masks[8] = {};
bool     g_pm_hold = false;
bool     g_powered_off = false;
uint8_t pm_write(uint8_t v) {
    if (!g_pm_hold) { g_pm_index = v; g_pm_hold = true; return 0; }  // index byte
    uint32_t regid = g_pm_index & 7u;
    if (g_pm_index & 0x80u) return g_pm_regs[regid];                 // read
    g_pm_regs[regid] = (g_pm_regs[regid] & ~g_pm_masks[regid])       // write
                     | (v & g_pm_masks[regid]);
    if (regid == 0u && (v & 0x40u)) {
        g_powered_off = true;
        nds_gpu2d_stop();
        nds_spu_stop();
    }
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

void check_key_irq(int cpu, uint32_t oldkey, uint32_t newkey) {
    const uint16_t cnt = g_keycnt[cpu & 1];
    if (!(cnt & 0x4000u)) return;
    const uint32_t mask = cnt & 0x03FFu;
    oldkey &= mask;
    newkey &= mask;
    const bool oldmatch = (cnt & 0x8000u) ? oldkey == 0 : oldkey != mask;
    const bool newmatch = (cnt & 0x8000u) ? newkey == 0 : newkey != mask;
    if (!oldmatch && newmatch) nds_raise_irq(cpu, 1u << 12);
}

void release_device(int dev) {
    switch (dev) {
        case 0: pm_release();  break;
        case 1: fw_release();  break;
        case 2: tsc_release(); break;
    }
}
void spi_transfer(uint8_t v) {
    if (g_spicnt & 0x0080u) return;
    int dev = (g_spicnt >> 8) & 3;
    switch (dev) {
        case 0: g_spi_resp = pm_write(v);  break;
        case 1: g_spi_resp = fw_write(v);  break;
        case 2: g_spi_resp = tsc_write(v); break;
        default: g_spi_resp = 0;           break;
    }
    if (!(g_spicnt & 0x0800u)) release_device(dev);   // no hold → CS deassert
    g_spicnt |= 0x0080u;
    const uint64_t delay = 8u * (8u << (g_spicnt & 3u));
    g_spi_deadline = active_system_timestamp() + delay;
    nds_reschedule_slice(g_spi_deadline);
}

uint8_t spi_read_data() {
    // melonDS SPIHost::ReadData: the shift register is not externally visible
    // while disabled or while the byte is still transferring.
    if (!(g_spicnt & 0x8000u) || (g_spicnt & 0x0080u)) return 0;
    return g_spi_resp;
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
uint8_t  g_rtc_alarm1[3] = {};
uint8_t  g_rtc_alarm2[3] = {};
uint8_t  g_rtc_clock_adjust = 0;
uint8_t  g_rtc_free = 0;
uint8_t  g_rtc_irq_flag = 0;
uint32_t g_rtc_clock_count = 0;
uint64_t g_rtc_processed_ticks = 0;

uint8_t rtc_bcd_increment(uint8_t v) {
    ++v;
    if ((v & 0x0Fu) >= 0x0Au) v = static_cast<uint8_t>(v + 0x06u);
    if ((v & 0xF0u) >= 0xA0u) v = static_cast<uint8_t>(v + 0x60u);
    return v;
}

uint8_t rtc_bcd_sanitize(uint8_t v, uint8_t lo, uint8_t hi) {
    if (v < lo || v > hi || (v & 0x0Fu) >= 0x0Au || (v & 0xF0u) >= 0xA0u)
        return lo;
    return v;
}

uint8_t rtc_days_in_month() {
    switch (g_rtc_datetime[1]) {
        case 0x01: case 0x03: case 0x05: case 0x07:
        case 0x08: case 0x10: case 0x12: return 0x31;
        case 0x04: case 0x06: case 0x09: case 0x11: return 0x30;
        case 0x02: {
            const unsigned year = (g_rtc_datetime[0] & 0x0Fu) +
                                  10u * (g_rtc_datetime[0] >> 4u);
            return (year & 3u) ? 0x28 : 0x29;
        }
        default: return 0;
    }
}

void rtc_count_year() { g_rtc_datetime[0] = rtc_bcd_increment(g_rtc_datetime[0]); }
void rtc_count_month() {
    g_rtc_datetime[1] = rtc_bcd_increment(g_rtc_datetime[1]);
    if (g_rtc_datetime[1] > 0x12) {
        g_rtc_datetime[1] = 1;
        rtc_count_year();
    }
}
void rtc_check_end_of_month() {
    if (g_rtc_datetime[2] > rtc_days_in_month()) {
        g_rtc_datetime[2] = 1;
        rtc_count_month();
    }
}
void rtc_count_day() {
    if (++g_rtc_datetime[3] >= 7) g_rtc_datetime[3] = 0;
    g_rtc_datetime[2] = rtc_bcd_increment(g_rtc_datetime[2]);
    rtc_check_end_of_month();
}
void rtc_count_hour() {
    uint8_t hour = rtc_bcd_increment(g_rtc_datetime[4] & 0x3Fu);
    uint8_t pm = g_rtc_datetime[4] & 0x40u;
    if (g_rtc_status1 & 0x02u) {
        if (hour >= 0x24u) { hour = 0; rtc_count_day(); }
        pm = hour >= 0x12u ? 0x40u : 0u;
    } else if (hour >= 0x12u) {
        hour = 0;
        if (pm) rtc_count_day();
        pm ^= 0x40u;
    }
    g_rtc_datetime[4] = hour | pm;
}

void rtc_set_irq(uint8_t irq) {
    const uint8_t old = g_rtc_irq_flag;
    g_rtc_irq_flag |= irq;
    g_rtc_status1 |= irq;
    if (!(old & 0x30u) && (g_rtc_irq_flag & 0x30u) &&
        (g_rcnt & 0xC100u) == 0x8100u) {
        nds_raise_irq(1, 1u << 7); // ARM7 RTC interrupt
    }
}

void rtc_clear_irq(uint8_t irq) { g_rtc_irq_flag &= ~irq; }

// type: 0 = minute carry, 1 = 32768 Hz periodic tick,
//       2 = status-register write.
void rtc_process_irq(int type) {
    switch (g_rtc_status2 & 0x0Fu) {
        case 0x0: // none
            if (type == 2) rtc_clear_irq(0x10u);
            break;

        case 0x1:
        case 0x5: // selected-frequency steady interrupt
            if ((type == 1 && !(g_rtc_clock_count & 0x3FFu)) || type == 2) {
                uint32_t mask = 0;
                if (g_rtc_alarm1[2] & (1u << 0)) mask |= 0x4000u;
                if (g_rtc_alarm1[2] & (1u << 1)) mask |= 0x2000u;
                if (g_rtc_alarm1[2] & (1u << 2)) mask |= 0x1000u;
                if (g_rtc_alarm1[2] & (1u << 3)) mask |= 0x0800u;
                if (g_rtc_alarm1[2] & (1u << 4)) mask |= 0x0400u;
                if (mask && ((g_rtc_clock_count & mask) != mask))
                    rtc_set_irq(0x10u);
                else
                    rtc_clear_irq(0x10u);
            }
            break;

        case 0x2:
        case 0x6: // per-minute edge interrupt
            if (type == 0 || (type == 2 && (g_rtc_irq_flag & 0x01u)))
                rtc_set_irq(0x10u);
            break;

        case 0x3: // per-minute steady interrupt, 30-second duty
            if (type == 0 || (type == 2 && (g_rtc_irq_flag & 0x01u)))
                rtc_set_irq(0x10u);
            else if (type == 1 && g_rtc_datetime[6] == 0x30u &&
                     !(g_rtc_clock_count & 0x7FFFu))
                rtc_clear_irq(0x10u);
            break;

        case 0x7: // per-minute steady interrupt, 256-cycle duty
            if (type == 0 || (type == 2 && (g_rtc_irq_flag & 0x01u)))
                rtc_set_irq(0x10u);
            else if (type == 1 && g_rtc_datetime[6] == 0x00u &&
                     (g_rtc_clock_count & 0x7FFFu) == 256u)
                rtc_clear_irq(0x10u);
            break;

        case 0x4: // alarm 1
            if (type == 0) {
                bool match = true;
                if (g_rtc_alarm1[0] & 0x80u)
                    match &= (g_rtc_alarm1[0] & 0x07u) == g_rtc_datetime[3];
                if (g_rtc_alarm1[1] & 0x80u)
                    match &= (g_rtc_alarm1[1] & 0x7Fu) == g_rtc_datetime[4];
                if (g_rtc_alarm1[2] & 0x80u)
                    match &= (g_rtc_alarm1[2] & 0x7Fu) == g_rtc_datetime[5];
                if (match) rtc_set_irq(0x10u);
                else rtc_clear_irq(0x10u);
            }
            break;

        default: // 32 KHz output
            if (type == 1) {
                rtc_set_irq(0x10u);
                rtc_clear_irq(0x10u);
            }
            break;
    }

    if (g_rtc_status2 & 0x40u) { // alarm 2
        if (type == 0) {
            bool match = true;
            if (g_rtc_alarm2[0] & 0x80u)
                match &= (g_rtc_alarm2[0] & 0x07u) == g_rtc_datetime[3];
            if (g_rtc_alarm2[1] & 0x80u)
                match &= (g_rtc_alarm2[1] & 0x7Fu) == g_rtc_datetime[4];
            if (g_rtc_alarm2[2] & 0x80u)
                match &= (g_rtc_alarm2[2] & 0x7Fu) == g_rtc_datetime[5];
            if (match) rtc_set_irq(0x20u);
            else rtc_clear_irq(0x20u);
        }
    } else if (type == 2) {
        rtc_clear_irq(0x20u);
    }
}

void rtc_count_minute() {
    g_rtc_datetime[5] = rtc_bcd_increment(g_rtc_datetime[5]);
    if (g_rtc_datetime[5] >= 0x60u) {
        g_rtc_datetime[5] = 0;
        rtc_count_hour();
    }
    g_rtc_irq_flag |= 0x01u;
    rtc_process_irq(0);
}
void rtc_count_second() {
    g_rtc_datetime[6] = rtc_bcd_increment(g_rtc_datetime[6]);
    if (g_rtc_datetime[6] >= 0x60u) {
        g_rtc_datetime[6] = 0;
        rtc_count_minute();
    }
}

void rtc_reset_state() {
    g_rtc_status1 = g_rtc_status2 = 0;
    std::memset(g_rtc_datetime, 0, sizeof(g_rtc_datetime));
    g_rtc_datetime[1] = g_rtc_datetime[2] = 1;
    std::memset(g_rtc_alarm1, 0, sizeof(g_rtc_alarm1));
    std::memset(g_rtc_alarm2, 0, sizeof(g_rtc_alarm2));
    g_rtc_clock_adjust = g_rtc_free = g_rtc_irq_flag = 0;
}

void rtc_write_datetime(unsigned num, uint8_t v) {
    switch (num) {
        case 1: g_rtc_datetime[0] = rtc_bcd_sanitize(v, 0x00, 0x99); break;
        case 2: g_rtc_datetime[1] = rtc_bcd_sanitize(v & 0x1Fu, 0x01, 0x12); break;
        case 3:
            g_rtc_datetime[2] = rtc_bcd_sanitize(v & 0x3Fu, 0x01, 0x31);
            rtc_check_end_of_month();
            break;
        case 4: g_rtc_datetime[3] = rtc_bcd_sanitize(v & 0x07u, 0x00, 0x06); break;
        case 5: {
            const uint8_t maxhour = (g_rtc_status1 & 0x02u) ? 0x23 : 0x11;
            g_rtc_datetime[4] = rtc_bcd_sanitize(v & 0x3Fu, 0x00, maxhour) |
                                (v & 0x40u);
            break;
        }
        case 6: g_rtc_datetime[5] = rtc_bcd_sanitize(v & 0x7Fu, 0x00, 0x59); break;
        case 7: g_rtc_datetime[6] = rtc_bcd_sanitize(v & 0x7Fu, 0x00, 0x59); break;
    }
}

void rtc_cmd_read() {
    if ((g_rtc_cmd & 0x0Fu) != 0x06u) return;
    switch (g_rtc_cmd & 0x70u) {
        case 0x00: g_rtc_output[0] = g_rtc_status1; g_rtc_status1 &= 0x0Fu; break;
        case 0x40: g_rtc_output[0] = g_rtc_status2; break;
        case 0x20: std::memcpy(g_rtc_output, &g_rtc_datetime[0], 7); break;
        case 0x60: std::memcpy(g_rtc_output, &g_rtc_datetime[4], 3); break;
        case 0x10:
            if (g_rtc_status2 & 0x04u) std::memcpy(g_rtc_output, g_rtc_alarm1, 3);
            else g_rtc_output[0] = g_rtc_alarm1[2];
            break;
        case 0x50: std::memcpy(g_rtc_output, g_rtc_alarm2, 3); break;
        case 0x30: g_rtc_output[0] = g_rtc_clock_adjust; break;
        case 0x70: g_rtc_output[0] = g_rtc_free; break;
        default:   g_rtc_output[0] = 0; break;
    }
}

void rtc_cmd_write(uint8_t v) {
    if ((g_rtc_cmd & 0x0Fu) != 0x06u) return;

    switch (g_rtc_cmd & 0x70u) {
        case 0x00: // status register 1
            if (g_rtc_inpos == 1) {
                const uint8_t old = g_rtc_status1;
                if (v & 0x01u) rtc_reset_state();
                g_rtc_status1 = (g_rtc_status1 & 0xF0u) | (v & 0x0Eu);

                if ((g_rtc_status1 ^ old) & 0x02u) {
                    uint8_t hour = g_rtc_datetime[4] & 0x3Fu;
                    uint8_t pm = g_rtc_datetime[4] & 0x40u;
                    if (g_rtc_status1 & 0x02u) {
                        if (pm) {
                            hour = static_cast<uint8_t>(hour + 0x12u);
                            if ((hour & 0x0Fu) >= 0x0Au)
                                hour = static_cast<uint8_t>(hour + 0x06u);
                        }
                        hour = rtc_bcd_sanitize(hour, 0x00, 0x23);
                    } else {
                        if (hour >= 0x12u) {
                            pm = 0x40u;
                            hour = static_cast<uint8_t>(hour - 0x12u);
                            if ((hour & 0x0Fu) >= 0x0Au)
                                hour = static_cast<uint8_t>(hour - 0x06u);
                        } else {
                            pm = 0;
                        }
                        hour = rtc_bcd_sanitize(hour, 0x00, 0x11);
                    }
                    g_rtc_datetime[4] = hour | pm;
                }
            }
            break;

        case 0x40: // status register 2
            if (g_rtc_inpos == 1) {
                g_rtc_status2 = v;
                rtc_process_irq(2);
            }
            break;

        case 0x20: // date and time
            if (g_rtc_inpos <= 7) rtc_write_datetime(g_rtc_inpos, v);
            break;

        case 0x60: // time only
            if (g_rtc_inpos <= 3) rtc_write_datetime(g_rtc_inpos + 4, v);
            break;

        case 0x10: // alarm 1 / selected frequency
            if (g_rtc_status2 & 0x04u) {
                if (g_rtc_inpos <= 3) g_rtc_alarm1[g_rtc_inpos - 1] = v;
            } else if (g_rtc_inpos == 1) {
                g_rtc_alarm1[2] = v;
            }
            break;

        case 0x50: // alarm 2
            if (g_rtc_inpos <= 3) g_rtc_alarm2[g_rtc_inpos - 1] = v;
            break;

        case 0x30:
            if (g_rtc_inpos == 1) g_rtc_clock_adjust = v;
            break;

        case 0x70:
            if (g_rtc_inpos == 1) g_rtc_free = v;
            break;
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
    rtc_cmd_write(v);
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
unsigned long long g_timer_last[2] = {0, 0};
unsigned long long g_display_last = 0;
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
bool gpu2d_reg_addr(uint32_t addr) {
    // DISPSTAT/VCOUNT (A+0x004..0x007) are global per-CPU registers, not
    // engine-A registers. 0x060..0x063 belongs to the 3D engine.
    if ((addr >= 0x04000000u && addr < 0x04000004u) ||
        (addr >= 0x04000008u && addr < 0x04000060u) ||
        (addr >= 0x04000064u && addr < 0x04000070u))
        return true;
    return (addr >= 0x04001000u && addr < 0x04001060u) ||
           (addr >= 0x0400106Cu && addr < 0x0400106Eu);
}
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
        g_cpu_halted[i] = false;
        g_halt_entry_cycle[i] = 0;
        g_dispstat[i] = 0;
        g_keycnt[i] = 0;
    }
    g_vcount = 0; g_in_vblank = false;
    g_counts = {};
    std::memset(g_dma, 0, sizeof(g_dma));
    g_dma_entry_cycle[0] = g_dma_entry_cycle[1] = 0;
    std::memset(g_spi_trace, 0, sizeof(g_spi_trace));
    std::memset(g_irq_trace, 0, sizeof(g_irq_trace));
    std::memset(g_insn_trace, 0, sizeof(g_insn_trace));
    std::memset(g_fifo_trace, 0, sizeof(g_fifo_trace));
    g_romctrl = 0; g_card_transfer_pos = 0; g_card_transfer_len = 0;
    g_card_deadline = UINT64_MAX; g_card_end_event = false; g_card_irq_cpu = 0;
    g_divcnt = 0; g_div_deadline = UINT64_MAX;
    std::memset(g_div_numer, 0, sizeof(g_div_numer));
    std::memset(g_div_denom, 0, sizeof(g_div_denom));
    std::memset(g_div_quot, 0, sizeof(g_div_quot));
    std::memset(g_div_rem, 0, sizeof(g_div_rem));
    g_sqrtcnt = 0; g_sqrt_res = 0; g_sqrt_deadline = UINT64_MAX;
    std::memset(g_sqrt_val, 0, sizeof(g_sqrt_val));
    for (auto& cpu : g_timer) for (auto& t : cpu) t = Timer{};
    g_timer_last[0] = g_timer_last[1] = 0;
    g_display_last = 0;
    g_spicnt = 0; g_spi_resp = 0; g_spi_deadline = UINT64_MAX;
    g_fw_hold = false; g_fw_cmd = 0; g_fw_status = 0;
    g_fw_addr = 0; g_fw_data_pos = 0;
    g_pm_index = 0; g_pm_hold = false; g_powered_off = false;
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
    std::memset(g_rtc_alarm1, 0, sizeof(g_rtc_alarm1));
    std::memset(g_rtc_alarm2, 0, sizeof(g_rtc_alarm2));
    g_rtc_clock_adjust = g_rtc_free = g_rtc_irq_flag = 0;
    g_rtc_clock_count = 0;
    g_rtc_processed_ticks = 0;
    {
        static const uint8_t dt[7] = {0x24, 0x01, 0x01, 0x01, 0x52, 0x00, 0x00};
        std::memcpy(g_rtc_datetime, dt, 7);
    }
    g_rtc_status1 = 0x02; g_rtc_status2 = 0x00;
    for (int i = 0; i < 2; ++i) {
        g_fifo_cnt[i] = 0; g_fifo_head[i] = 0; g_fifocnt[i] = 0; g_fifo_lastrx[i] = 0;
    }
    for (auto& b : g_io_mem) b = 0;
    g_io_mem[0x304] = 0x01; // ARM9 POWCNT1 reset value
    g_exmemcnt[0] = g_exmemcnt[1] = 0x4000u;
    g_powercontrol7 = 0x0001u;
    g_keyinput = 0x007F03FFu;
    g_rcnt = 0;
    g_wramcnt = 0;
    g_wifiwaitcnt = 0;
    g_warned = 0;
    nds_wifi_reset();
    nds_spu_reset();
    nds_vram_reset();
    nds_gpu2d_reset();
}

void nds_io_load_firmware(const uint8_t* p, uint32_t n) {
    g_fw.assign(p, p + n);
    nds_wifi_load_firmware(p, n);
}

void nds_set_touch(uint16_t x, uint16_t y, bool down) {
    if (!down) {
        g_tsc_x = 0x000u;
        g_tsc_y = 0x0FFFu;
        g_keyinput |= 1u << 22; // EXTKEYIN bit 6: pen up
        return;
    }
    // melonDS TSC::SetTouchCoords accepts screen pixels and converts them to
    // the 12-bit ADC domain with the DS firmware's simple 16x scale.
    g_tsc_x = static_cast<uint16_t>(x << 4);
    g_tsc_y = static_cast<uint16_t>(y << 4);
    g_keyinput &= ~(1u << 22);
}

void nds_set_key_mask(uint32_t mask) {
    const uint32_t key_lo = mask & 0x03FFu;
    const uint32_t key_hi = (mask >> 10) & 0x3u;
    const uint32_t oldkey = g_keyinput;
    g_keyinput &= 0xFFFCFC00u;
    g_keyinput |= key_lo | (key_hi << 16);
    check_key_irq(0, oldkey, g_keyinput);
    check_key_irq(1, oldkey, g_keyinput);
}

const NdsEventCounts& nds_event_counts() { return g_counts; }

bool nds_spi_trace_get(uint64_t count, NdsSpiTraceEntry* out) {
    if (!out || count == 0) return false;
    const NdsSpiTraceEntry& e = g_spi_trace[(count - 1) % kSpiTraceSize];
    if (e.count != count) return false;
    *out = e;
    return true;
}

void nds_note_irq_accept(int cpu, uint32_t return_address) {
    cpu &= 1;
    uint64_t& count = cpu == 0 ? g_counts.irq9 : g_counts.irq7;
    ++count;
    g_irq_trace[cpu][(count - 1) % kIrqTraceSize] = {
        count,
        scheduler_system_timestamp(),
        cpu == 0 ? g_runtime_cycles : scheduler_cpu_cycles(0),
        cpu == 1 ? g_runtime_cycles : scheduler_cpu_cycles(1),
        cpu == 0 ? g_counts.insn9 : g_counts.insn7,
        return_address,
        g_ie[cpu] & g_if[cpu],
        cpu == 1 ? nds_wifi_debug_if() : 0u,
        cpu == 1 ? nds_wifi_debug_ie() : 0u,
    };
}

bool nds_irq_trace_get(int cpu, uint64_t count, NdsIrqTraceEntry* out) {
    if (!out || count == 0) return false;
    cpu &= 1;
    const NdsIrqTraceEntry& e = g_irq_trace[cpu][(count - 1) % kIrqTraceSize];
    if (e.count != count) return false;
    *out = e;
    return true;
}

bool nds_insn_trace_get(int cpu, uint64_t count, NdsInsnTraceEntry* out) {
    if (!out || count == 0) return false;
    cpu &= 1;
    const NdsInsnTraceEntry& e = g_insn_trace[cpu][(count - 1) % kInsnTraceSize];
    if (e.count != count) return false;
    *out = e;
    return true;
}

bool nds_fifo_trace_get(int source_cpu, uint64_t count, NdsFifoTraceEntry* out) {
    if (!out || count == 0) return false;
    source_cpu &= 1;
    const NdsFifoTraceEntry& e =
        g_fifo_trace[source_cpu][(count - 1) % kFifoTraceSize];
    if (e.count != count) return false;
    *out = e;
    return true;
}

uint64_t nds_next_system_event_time() {
    return std::min(std::min(g_card_deadline, g_spi_deadline),
                    std::min(g_div_deadline, g_sqrt_deadline));
}
uint64_t nds_debug_spi_deadline() { return g_spi_deadline; }
uint64_t nds_debug_card_deadline() { return g_card_deadline; }

void nds_run_system_events(uint64_t timestamp) {
    // Both events are one-shot. A handler may schedule its successor only
    // after a guest data-port read, matching melonDS's card-ready handshake.
    if (g_spi_deadline <= timestamp) {
        g_spi_deadline = UINT64_MAX;
        g_spicnt &= ~0x0080u;
    }
    if (g_card_deadline <= timestamp) {
        const bool end_event = g_card_end_event;
        g_card_deadline = UINT64_MAX;
        if (end_event) {
            g_romctrl &= ~0x80000000u;
            if (io_mem_read(0x040001A0u, 2) & 0x4000u)
                nds_raise_irq(g_card_irq_cpu, 0x00080000u);
        } else {
            g_card_transfer_pos += 4u;
            g_romctrl |= 0x00800000u;
        }
    }
    if (g_div_deadline <= timestamp) {
        g_div_deadline = UINT64_MAX;
        finish_div();
    }
    if (g_sqrt_deadline <= timestamp) {
        g_sqrt_deadline = UINT64_MAX;
        finish_sqrt();
    }
}

void nds_note_insn_retired(int cpu) {
    cpu &= 1;
    uint64_t& count = cpu ? g_counts.insn7 : g_counts.insn9;
    ++count;
    NdsInsnTraceEntry& e =
        g_insn_trace[cpu][(count - 1) % kInsnTraceSize];
    e = {
        count,
        scheduler_system_timestamp(),
        g_runtime_cycles,
        g_cpu.R[15],
        g_cpu.cpsr,
        runtime_deferred_cycles(),
    };
    std::memcpy(e.r, g_cpu.R, sizeof(e.r));
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
    if (std::strcmp(name, "spi_w") == 0) return g_counts.spi_w;
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
    if (std::strcmp(name, "spi_w") == 0) return &g_counts.spi_w;
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
    const unsigned long long old_cycles = g_runtime_cycles;
    const int slot = (cpu == 7) ? 1 : 0;
    g_nds_active = slot ? NDS_ARM7 : NDS_ARM9;
    // Timer reads are intentionally live: melonDS advances the selected CPU's
    // timer bank to that CPU's timestamp.  Reusing the previously-active
    // CPU's g_runtime_cycles can advance ARM7 timers at ARM9's 2x timestamp
    // and even fabricate billions of overflow events during observation.
    g_runtime_cycles = scheduler_cpu_cycles(slot);
    // TCP expresses widths in bits; the runtime bus API uses byte counts.
    // Passing 16/32 straight through made a diagnostic read span unrelated
    // adjacent registers and could itself advance live devices incorrectly.
    const uint32_t bytes = width == 8 ? 1u : width == 16 ? 2u : 4u;
    uint32_t v = nds_io_read(addr, bytes);
    g_runtime_cycles = old_cycles;
    g_nds_active = old;
    return v;
}

uint16_t nds_exmemcnt(int cpu) { return g_exmemcnt[cpu & 1]; }
uint16_t nds_powercontrol7() { return g_powercontrol7; }
uint16_t nds_powercontrol9() {
    return static_cast<uint16_t>(io_mem_read(0x04000304u, 2));
}
uint16_t nds_wifiwaitcnt() { return g_wifiwaitcnt; }
uint8_t nds_wramcnt() { return g_wramcnt; }
bool nds_powered_off() { return g_powered_off; }

void nds_rtc_debug_state(NdsRtcDebugState* out) {
    if (!out) return;
    out->io = g_rtc_io;
    out->status1 = g_rtc_status1;
    out->status2 = g_rtc_status2;
    std::memcpy(out->datetime, g_rtc_datetime, sizeof(out->datetime));
}

void nds_tick_rtc(unsigned long long system_cycles) {
    // melonDS schedules event N at floor(N*33513982/32768), starting at N=1.
    // Deriving the due ordinal preserves its fractional phase exactly.
    constexpr uint64_t numerator = 33513982u;
    constexpr uint64_t denominator = 32768u;
    const uint64_t due = (((uint64_t{system_cycles} + 1u) * denominator) - 1u)
                         / numerator;
    while (g_rtc_processed_ticks < due) {
        ++g_rtc_processed_ticks;
        ++g_rtc_clock_count;
        if (!(g_rtc_clock_count & 0x7FFFu)) rtc_count_second();
        else if ((g_rtc_clock_count & 0x7FFFu) == 4u) g_rtc_irq_flag &= ~0x01u;
        rtc_process_irq(1);
    }
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

bool nds_cpu_halted(int cpu) { return g_cpu_halted[cpu & 1]; }

void nds_cpu_enter_halt(int cpu) {
    cpu &= 1;
    if (g_cpu_halted[cpu]) return;
    // CP15 WFI is invoked from inside the enabling MCR, before its runtime
    // tick. Save the true instruction-start timestamp; the scheduler carries
    // the completed MCR cost as ARM::Cycles debt while sleep snaps to targets.
    g_halt_entry_cycle[cpu] = g_runtime_cycles;
    g_cpu_halted[cpu] = true;
}

bool nds_halt_wake_pending(int cpu) {
    cpu &= 1;
    const bool source = (g_ie[cpu] & g_if[cpu]) != 0;
    // melonDS NDS::HaltInterrupted: ARM9 additionally gates wake with IME;
    // ARM7 HALTCNT does not. IRQ vectoring remains gated by nds_irq_pending.
    return source && (cpu == 1 || (g_ime[cpu] & 1u));
}

void nds_cpu_wake(int cpu) { g_cpu_halted[cpu & 1] = false; }

unsigned long long nds_halt_entry_cycle(int cpu) {
    return g_halt_entry_cycle[cpu & 1];
}

bool nds_dma_cpu_stalled(int cpu) { return dma_any_running(cpu & 1); }

unsigned long long nds_dma_entry_cycle(int cpu) {
    return g_dma_entry_cycle[cpu & 1];
}

void nds_dma_trigger(int cpu, uint32_t start_mode) {
    cpu &= 1;
    for (int ch = 0; ch < 4; ++ch) {
        DmaChannel& d = g_dma[cpu][ch];
        if ((d.cnt & 0x80000000u) && d.start_mode == start_mode)
            dma_start(cpu, ch);
    }
}

void nds_dma_run(int cpu, unsigned long long target_cycles) {
    cpu &= 1;
    for (int ch = 0; ch < 4; ++ch) {
        DmaChannel& d = g_dma[cpu][ch];
        if (!d.running || g_runtime_cycles >= target_cycles) continue;
        const uint32_t width = (d.cnt & 0x04000000u) ? 4u : 2u;
        while (d.remaining && g_runtime_cycles < target_cycles) {
            const uint32_t unit = dma_unit_cycles(cpu, d, width);
            g_runtime_cycles += uint64_t{unit} << (cpu == 0 ? 1u : 0u);
            if (width == 4u) {
                const uint32_t v = bus_read_u32(d.cur_src & ~3u);
                bus_write_u32(d.cur_dst & ~3u, v);
            } else {
                const uint16_t v = bus_read_u16(d.cur_src & ~1u);
                bus_write_u16(d.cur_dst & ~1u, v);
            }
            d.cur_src = static_cast<uint32_t>(
                int64_t{d.cur_src} + int64_t{d.src_inc} * width);
            d.cur_dst = static_cast<uint32_t>(
                int64_t{d.cur_dst} + int64_t{d.dst_inc} * width);
            --d.remaining;
            d.burst_start = false;
        }
        if (d.remaining) continue;

        if (!(d.cnt & 0x02000000u)) d.cnt &= ~0x80000000u;
        if (d.cnt & 0x40000000u) nds_raise_irq(cpu, 1u << (8 + ch));
        d.running = false;
        d.in_progress = false;
        ++g_counts.dma_done;
        brk_check();
    }
}

// ── Display + timer clocks (driven from the scheduler) ──────────────────
// DS scanline timing in system-cycle units; the precise
// numbers land with the melonDS oracle. VBlank (IF bit 0) fires once per
// frame. Both cores see it (a display event).
void nds_tick_display(unsigned long long cyc) {
    // Display VBlank (IF bit 0): exactly once per frame at scanline 192, both
    // cores. The DS display runs on the 33.51 MHz
    // system clock (2130 cycles/scanline, 263 lines = 560190 cyc/frame).
    // VBlank begins at line 192. Count by absolute vblank index so a delta that
    // spans the line-192 boundary fires once (and a delta > 1 frame fires per
    // frame) — the old code double-fired (frame-wrap AND line-192) and used the
    // system-clock period against ARM9 cycles, making VBlank 4x too fast.
    static const unsigned long long SCAN = 2130, LINES = 263;
    static const unsigned long long FRAME = SCAN * LINES;
    static const unsigned long long HBLANK_START = 1584;
    static const unsigned long long VB_START = 192ull * SCAN;
    auto vb_index = [](unsigned long long c) -> unsigned long long {
        return c < VB_START ? 0ull : (c - VB_START) / FRAME + 1ull;
    };
    // Current scanline, for DISPSTAT/VCOUNT reads (round-granular; the precise
    // sub-scanline position lands with finer display timing if a poll needs it).
    g_vcount = static_cast<uint16_t>((cyc / SCAN) % LINES);
    g_in_vblank = (g_vcount >= 192);
    auto hblank_index = [](unsigned long long c) -> unsigned long long {
        return c < HBLANK_START ? 0ull
                                : (c - HBLANK_START) / SCAN + 1ull;
    };
    const unsigned long long previous_hblank = hblank_index(g_display_last);
    const unsigned long long current_hblank = hblank_index(cyc);
    for (unsigned long long i = previous_hblank; i < current_hblank; ++i) {
        const int line = static_cast<int>(i % LINES);
        if (line < 192) nds_gpu2d_render_scanline(line);
    }

    unsigned long long pv = vb_index(g_display_last), cv = vb_index(cyc);
    const unsigned long long previous_frame = g_display_last / FRAME;
    const unsigned long long current_frame = cyc / FRAME;
    g_display_last = cyc;
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

    // melonDS publishes the completed back buffer in GPU::FinishFrame at the
    // 263->0 wrap, not at VBlank start. The scheduler normally advances by
    // much less than one frame, but retain one swap per crossed boundary so a
    // long halted interval cannot lose buffer parity.
    for (unsigned long long i = previous_frame; i < current_frame; ++i)
        nds_gpu2d_finish_frame();

}

void nds_tick_timers(int cpu, unsigned long long cpu_cycles) {
    cpu &= 1;
    // ARM9 timestamps are in the 2x CPU clock; timers use system cycles.
    const unsigned long long cyc = cpu == 0 ? (cpu_cycles >> 1u) : cpu_cycles;
    // Timers. Advance each enabled, non-cascade timer by the elapsed cycles
    // and raise its overflow IRQ (IF bit 3+N). Cascade (count-up) timers
    // are driven by the preceding timer's overflow.
    unsigned long long delta = cyc - g_timer_last[cpu];
    g_timer_last[cpu] = cyc;
    if (delta == 0) return;
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

uint32_t nds_io_read(uint32_t addr, uint32_t width) {
    const int cpu = active();
    const uint32_t m = mask_for(width);
    if (cpu == 0 && gpu2d_reg_addr(addr))
        return nds_gpu2d_read(addr, width) & m;
    if (addr >= 0x04000130u && addr < 0x04000134u) {
        const uint32_t reg = (g_keyinput & 0xFFFFu) |
                             (uint32_t{g_keycnt[cpu]} << 16);
        return (reg >> ((addr - 0x04000130u) * 8u)) & m;
    }
    if (addr >= 0x04000136u && addr < 0x04000138u)
        return cpu == 1
            ? (((g_keyinput >> 16) >> ((addr - 0x04000136u) * 8u)) & m)
            : 0u;
    if (addr >= 0x04000134u && addr < 0x04000138u) {
        if (cpu != 1) return 0;
        const uint32_t reg = g_rcnt | (g_keyinput & 0xFFFF0000u);
        return (reg >> ((addr - 0x04000134u) * 8u)) & m;
    }
    if (addr >= 0x04000240u && addr < 0x0400024Au) {
        if (cpu == 1) {
            if (width != 1) return 0;
            if (addr == 0x04000240u) return nds_vramstat();
            if (addr == 0x04000241u) return g_wramcnt;
            return 0;
        }
        uint32_t value = 0;
        for (uint32_t i = 0; i < width; ++i) {
            const uint32_t a = addr + i;
            uint8_t byte = 0;
            if (a >= 0x04000240u && a <= 0x04000246u) byte = nds_vramcnt(a - 0x04000240u);
            else if (a == 0x04000247u) byte = g_wramcnt;
            else if (a >= 0x04000248u && a <= 0x04000249u) byte = nds_vramcnt(a - 0x04000241u);
            value |= uint32_t{byte} << (i * 8u);
        }
        return value & m;
    }
    if (addr >= 0x04000400u && addr < 0x04000520u)
        return cpu == 1 ? (nds_spu_read(addr, width) & m) : 0u;
    if (dma_reg_addr(addr)) return dma_reg_read(cpu, addr, width) & m;
    if (cpu == 0 && math_addr(addr)) {
        const uint32_t reg = math_reg_read32(addr & ~3u);
        return (reg >> ((addr & 3u) * 8u)) & m;
    }
    // Timer counter/control (0x04000100..0x0400010F).
    if (addr >= 0x04000100u && addr < 0x04000110u) {
        // melonDS TimerGetCounter/TimerStart first advances the timer bank to
        // the active CPU's exact timestamp.  Scheduler-end ticking alone is
        // too coarse for a timer accessed midway through a slice.
        nds_tick_timers(cpu, g_runtime_cycles);
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
        case 0x04000204: case 0x04000205:
            return (g_exmemcnt[cpu] >> ((addr & 1u) * 8u)) & m;
        case 0x04000206: case 0x04000207:
            if (cpu != 1 || !(g_powercontrol7 & 0x0002u)) return 0;
            return (g_wifiwaitcnt >> ((addr & 1u) * 8u)) & m;
        case 0x04000300:
            return g_postflg[cpu] & m;
        case 0x04000304: case 0x04000305:
            return cpu == 1
                ? ((g_powercontrol7 >> ((addr & 1u) * 8u)) & m)
                : (io_mem_read(addr, width) & m);
        case 0x040001A4: case 0x040001A5:
        case 0x040001A6: case 0x040001A7:
            return (g_romctrl >> ((addr & 3u) * 8)) & m;
        case 0x04000184: case 0x04000185:  // IPCFIFOCNT
            return (fifocnt_read(cpu) >> ((addr & 1u) * 8)) & m;
        case 0x04100000: case 0x04100001:
        case 0x04100002: case 0x04100003:  // IPCFIFORECV
            return (fifo_recv(cpu) >> ((addr & 3u) * 8)) & m;
        case 0x040001C0: case 0x040001C1:  // SPICNT (+ SPIDATA on 32-bit read)
            if (addr == 0x040001C0u && width == 4u)
                return uint32_t{g_spicnt} | (uint32_t{spi_read_data()} << 16u);
            return (g_spicnt >> ((addr & 1u) * 8)) & m;
        case 0x040001C2: case 0x040001C3:  // SPIDATA (last clocked-in byte)
            return spi_read_data() & m;
        case 0x04000138: case 0x04000139:  // RTC register (bit-banged serial)
            return (rtc_read() >> ((addr & 1u) * 8)) & m;
        case 0x04100010:  // gamecard data — empty slot reads all-ones
            if (!(g_romctrl & 0x40000000u) && (g_romctrl & 0x00800000u)) {
                g_romctrl &= ~0x00800000u;
                if (g_card_transfer_pos < g_card_transfer_len) {
                    const uint32_t xfercycle =
                        (g_romctrl & 0x08000000u) ? 8u : 5u;
                    uint32_t delay = 4u;
                    if (!(g_card_transfer_pos & 0x1FFu))
                        delay += (g_romctrl >> 16) & 0x3Fu;
                    schedule_card_event(uint64_t{xfercycle} * delay, false);
                } else {
                    g_romctrl &= ~0x80000000u;
                    if (io_mem_read(0x040001A0u, 2) & 0x4000u)
                        nds_raise_irq(g_card_irq_cpu, 0x00080000u);
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
    if (cpu == 0 && gpu2d_reg_addr(addr)) {
        nds_gpu2d_write(addr, value, width);
        return;
    }
    if ((addr >= 0x04000132u && addr < 0x04000134u) ||
        (addr == 0x04000130u && width == 4u)) {
        const bool combined = addr == 0x04000130u;
        const uint32_t shift = combined ? 0u : (addr - 0x04000132u) * 8u;
        const uint32_t source = combined ? (value >> 16u) : value;
        const uint32_t write_width = combined ? 2u : width;
        const uint32_t wmask = mask_for(write_width) << shift;
        const uint32_t merged = (g_keycnt[cpu] & ~wmask) |
                                ((source << shift) & wmask);
        g_keycnt[cpu] = static_cast<uint16_t>(merged & 0xC3FFu);
        return;
    }
    if (addr >= 0x04000134u && addr < 0x04000136u) {
        if (cpu != 1) return;
        const uint32_t shift = (addr - 0x04000134u) * 8u;
        const uint32_t wmask = mask_for(width) << shift;
        g_rcnt = static_cast<uint16_t>((g_rcnt & ~wmask) |
                                       ((value << shift) & wmask));
        return;
    }
    if (addr >= 0x04000240u && addr < 0x0400024Au) {
        if (cpu != 0) return;
        for (uint32_t i = 0; i < width; ++i) {
            const uint32_t a = addr + i;
            const uint8_t byte = static_cast<uint8_t>(value >> (i * 8u));
            if (a >= 0x04000240u && a <= 0x04000246u) nds_vram_map(a - 0x04000240u, byte);
            else if (a == 0x04000247u) g_wramcnt = byte;
            else if (a >= 0x04000248u && a <= 0x04000249u) nds_vram_map(a - 0x04000241u, byte);
        }
        return;
    }
    if (addr >= 0x04000400u && addr < 0x04000520u) {
        if (cpu != 1) return;
        if (addr == 0x04000504u) {
            const uint32_t v = value & 0x3FFu;
            if (g_counts.soundbias_w == 0) g_counts.soundbias_first = v;
            ++g_counts.soundbias_w;
            g_counts.soundbias_last = v;
            brk_check();
        }
        nds_spu_write(addr, value, width);
        return;
    }
    if (dma_reg_addr(addr)) {
        dma_reg_write(cpu, addr, value, width);
        return;
    }
    if (cpu == 0 && math_addr(addr)) {
        math_reg_write(addr, value, width);
        return;
    }
    // Timer reload/control (0x04000100..0x0400010F). A 16/32-bit write to a
    // timer: low half = reload, high half = control. Enabling (bit7 0→1)
    // reloads the counter.
    if (addr >= 0x04000100u && addr < 0x04000110u) {
        int t = (addr - 0x04000100u) / 4u;
        uint32_t off = (addr - 0x04000100u) % 4u;
        Timer& T = g_timer[cpu][t];
        const bool writes_ctrl = (off == 0 && width >= 4) || off == 2;
        // TimerStart calls RunTimers before changing enable/prescaler state.
        // Without this, enabling midway through a scheduler slice incorrectly
        // counts the pre-enable portion of that slice and advances the phase.
        if (writes_ctrl) nds_tick_timers(cpu, g_runtime_cycles);
        if (off == 0 && width >= 2) T.reload = static_cast<uint16_t>(value);
        if (writes_ctrl) {
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
        case 0x04000204: case 0x04000205: { // EXMEMCNT
            const uint32_t shift = (addr & 1u) * 8u;
            const uint32_t wmask = mask_for(width) << shift;
            const uint16_t merged = static_cast<uint16_t>(
                (g_exmemcnt[cpu] & ~wmask) | ((value << shift) & wmask));
            if (cpu == 0) {
                g_exmemcnt[0] = merged;
                g_exmemcnt[1] = static_cast<uint16_t>(
                    (g_exmemcnt[1] & 0x007Fu) | (merged & 0xFF80u));
            } else {
                g_exmemcnt[1] = static_cast<uint16_t>(
                    (g_exmemcnt[1] & 0xFF80u) | (merged & 0x007Fu));
            }
            return;
        }
        case 0x04000206: case 0x04000207: { // WIFIWAITCNT (ARM7, powered only)
            if (cpu != 1 || !(g_powercontrol7 & 0x0002u)) return;
            const uint32_t shift = (addr & 1u) * 8u;
            const uint32_t wmask = mask_for(width) << shift;
            g_wifiwaitcnt = static_cast<uint16_t>(
                (g_wifiwaitcnt & ~wmask) | ((value << shift) & wmask));
            return;
        }
        case 0x04000300:
            // POSTFLG: bit0 latches set; ARM9 may also set bit1.
            g_postflg[cpu] |= static_cast<uint8_t>(value & (cpu == 0 ? 0x3u : 0x1u));
            return;
        case 0x04000301:  // HALTCNT (ARM7): 0x80 enters HALT
            g_haltcnt[cpu] = static_cast<uint8_t>(value);
            if (cpu == 1 && (value & 0xC0u) == 0x80u) {
                // runtime_insn_fp has already charged this instruction's code
                // fetch, while the generated runtime_tick has not yet charged
                // its data/execute portion. Recover the true pre-instruction
                // timestamp so the scheduler can carry the complete melonDS
                // ARM::Cycles debt (STRB here is BIOS S32 + I/O N16 = 2).
                const uint64_t code = runtime_code_cycles(g_cpu.R[15] & ~1u);
                g_halt_entry_cycle[cpu] =
                    g_runtime_cycles >= code ? g_runtime_cycles - code : 0u;
                g_cpu_halted[cpu] = true;
            }
            return;
        case 0x04000304: case 0x04000305: { // POWCNT2 (ARM7)
            if (cpu != 1) {
                const uint32_t shift = (addr & 1u) * 8u;
                const uint32_t wmask = mask_for(width) << shift;
                const uint16_t old = static_cast<uint16_t>(io_mem_read(0x04000304u, 2));
                const uint16_t merged = static_cast<uint16_t>(
                    (old & ~wmask) | ((value << shift) & wmask));
                io_mem_write(0x04000304u, merged & 0x820Fu, 2);
                return;
            }
            const uint32_t shift = (addr & 1u) * 8u;
            const uint32_t wmask = mask_for(width) << shift;
            const uint16_t merged = static_cast<uint16_t>(
                (g_powercontrol7 & ~wmask) | ((value << shift) & wmask));
            g_powercontrol7 = merged & 0x0003u;
            nds_wifi_set_power_control((g_powercontrol7 & 0x0002u) != 0u,
                                       active_system_timestamp());
            return;
        }
        case 0x04000184:    // IPCFIFOCNT
            fifocnt_write(cpu, static_cast<uint16_t>(value));
            return;
        case 0x04000188:    // IPCFIFOSEND
            fifo_send(cpu, value);
            return;
        case 0x040001C0: {  // SPICNT
            uint16_t old = g_spicnt;
            g_spicnt = static_cast<uint16_t>((old & 0x0080u) |
                                             (value & 0xCF03u));
            if ((old & 0x8000u) && !(g_spicnt & 0x8000u))   // SPI disabled → drop CS
                release_device((old >> 8) & 3);
            return;
        }
        case 0x040001C2:    // SPIDATA — clock one byte through the selected device
            ++g_counts.spi_w;
            g_spi_trace[(g_counts.spi_w - 1) % kSpiTraceSize] = {
                g_counts.spi_w,
                scheduler_system_timestamp(),
                scheduler_cpu_cycles(0),
                g_runtime_cycles,
                g_counts.insn7,
                g_cpu.R[15],
                static_cast<uint32_t>(value & 0xFFu),
            };
            brk_check();
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
        case 0x040001A4: { // ROMCTRL (32-bit write starts a gamecard block)
            const bool start = (value & ~g_romctrl & 0x80000000u) != 0u;
            g_romctrl = (value & 0xFF7F7FFFu) | (g_romctrl & 0x20800000u);
            const uint16_t aux = static_cast<uint16_t>(
                io_mem_read(0x040001A0u, 2));
            if (!start || !(aux & 0x8000u) || (aux & 0x2000u)) return;

            g_card_transfer_pos = 0;
            g_card_transfer_len = card_block_words(g_romctrl) * 4u;
            g_card_irq_cpu = cpu;
            g_romctrl &= ~0x00800000u;
            const uint32_t xfercycle =
                (g_romctrl & 0x08000000u) ? 8u : 5u;
            uint32_t command_delay = 8u;
            if (!(g_romctrl & 0x40000000u)) {
                command_delay += g_romctrl & 0x1FFFu;
                if (g_card_transfer_len)
                    command_delay += (g_romctrl >> 16) & 0x3Fu;
            }
            schedule_card_event(uint64_t{xfercycle} *
                                    (command_delay +
                                     (g_card_transfer_len ? 4u : 0u)),
                                g_card_transfer_len == 0u);
            return;
        }
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
