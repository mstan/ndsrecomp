// io.cpp — see io.h. Register map per GBATEK ("DS I/O Maps"); clean-room.

#include "io.h"
#include "wifi.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "state.h"
#include "scheduler.h"
#include "gpu2d.h"
#include "gpu3d.h"
#include "spu.h"
#include "vram.h"

// Runner-only rare-condition hint owned by runtime_arm.cpp. Generated banks
// keep calling the unchanged runtime_should_yield ABI.
extern "C" void runtime_request_yield_poll(void);

uint32_t g_nds_irq_pending_cache[2] = {0, 0};

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
uint16_t g_next_vcount = 0;        // guest write, latched at next scanline
bool     g_next_vcount_valid = false;
bool     g_vcount_match[2] = {false, false};
bool     g_in_vblank = false;      // DISPSTAT VBlank flag latch

// Interrupt registers, per CPU.
uint32_t g_ime[2] = {0, 0};           // 0x208 master enable (bit0)
uint32_t g_ie[2]  = {0, 0};           // 0x210 enable mask
uint32_t g_if[2]  = {0, 0};           // 0x214 request flags (write-1-to-clear)
inline void irq_recompute(int cpu) {
    cpu &= 1;
    g_nds_irq_pending_cache[cpu] =
        (g_ime[cpu] & 1u) ? (g_ie[cpu] & g_if[cpu]) : 0u;
}
uint16_t g_exmemcnt[2] = {0x4000u, 0x4000u}; // 0x204, shared ownership bits
uint16_t g_powercontrol7 = 0x0001u;    // 0x304 POWCNT2 (sound on, Wi-Fi off)
uint32_t g_keyinput = 0x007F03FFu;
uint16_t g_keycnt[2] = {};
uint16_t g_rcnt = 0;
uint8_t g_wramcnt = 0;
uint16_t g_wifiwaitcnt = 0;            // 0x206, visible only while Wi-Fi is on

// Gamecard (no cartridge inserted → data reads back 0xFFFFFFFF). The boot
// sees empty-slot all-ones behavior when no image is installed; an installed
// image follows the raw/KEY1/normal command phases. ROMCTRL is
// 0x040001A4 and the data port is 0x04100010.
uint32_t g_romctrl = 0;
uint32_t g_card_transfer_pos = 0;   // bytes prepared so far
uint32_t g_card_transfer_len = 0;   // bytes in this block
uint64_t g_card_deadline = UINT64_MAX;
bool     g_card_end_event = false;
int      g_card_irq_cpu = 0;
std::vector<uint8_t> g_card_rom;
std::vector<uint8_t> g_card_response;
uint8_t  g_card_command[8] = {};
uint32_t g_card_chip_id = 0;
uint32_t g_key1_base[0x412] = {};
uint32_t g_key1_schedule[0x412] = {};
bool     g_key1_available = false;

enum class CardCommandMode : uint8_t { Raw, Key1, Normal };
CardCommandMode g_card_mode = CardCommandMode::Raw;
uint32_t g_card_data_mode = 0;

// ── AUXSPI cartridge backup device (0x040001A0 CNT / 0x040001A2 DATA) ────
// Mirrors melonDS NDSCartSlot::WriteSPICnt/WriteSPIData/ReadSPIData and
// CartRetail's regular-EEPROM chip. SM64DS (ASMP) is melonDS ROMList
// SaveMemType 2 = 8 KiB EEPROM, which is also melonDS's not-in-list default;
// the ROMList lookup for other chip types lands with the next game target.
// A byte transfer holds AUXSPICNT bit7 busy for 8*(8<<speed) system cycles,
// cleared by a scheduled system event; the guest save code spin-waits on
// that bit, so instant completion shifts the whole ARM7 timeline.
uint16_t g_auxspicnt = 0;
uint8_t  g_auxspi_data = 0;
bool     g_auxspi_hold = false;
uint32_t g_auxspi_pos = 0;
uint64_t g_auxspi_deadline = UINT64_MAX;   // busy-clear, system cycles
uint8_t  g_sram_cmd = 0;
uint8_t  g_sram_status = 0;
uint32_t g_sram_addr = 0;
std::vector<uint8_t> g_cart_sram;

uint32_t load_le32(const uint8_t* p) {
    return uint32_t{p[0]} | (uint32_t{p[1]} << 8u) |
           (uint32_t{p[2]} << 16u) | (uint32_t{p[3]} << 24u);
}

uint32_t load_be32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24u) | (uint32_t{p[1]} << 16u) |
           (uint32_t{p[2]} << 8u) | uint32_t{p[3]};
}

void store_le32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8u);
    p[2] = static_cast<uint8_t>(value >> 16u);
    p[3] = static_cast<uint8_t>(value >> 24u);
}

uint32_t byte_swap32(uint32_t value) {
    return (value >> 24u) | ((value >> 8u) & 0x0000FF00u) |
           ((value << 8u) & 0x00FF0000u) | (value << 24u);
}

uint32_t key1_f(uint32_t value) {
    uint32_t out = g_key1_schedule[0x012u + (value >> 24u)];
    out += g_key1_schedule[0x112u + ((value >> 16u) & 0xFFu)];
    out ^= g_key1_schedule[0x212u + ((value >> 8u) & 0xFFu)];
    out += g_key1_schedule[0x312u + (value & 0xFFu)];
    return out;
}

void key1_encrypt(uint32_t words[2]) {
    uint32_t left = words[0];
    uint32_t right = words[1];
    for (uint32_t round = 0; round < 16u; ++round) {
        const uint32_t mixed = right ^ g_key1_schedule[round];
        right = key1_f(mixed) ^ left;
        left = mixed;
    }
    words[0] = right ^ g_key1_schedule[16];
    words[1] = left ^ g_key1_schedule[17];
}

void key1_decrypt(uint32_t words[2]) {
    uint32_t left = words[0];
    uint32_t right = words[1];
    for (uint32_t round = 17; round >= 2u; --round) {
        const uint32_t mixed = right ^ g_key1_schedule[round];
        right = key1_f(mixed) ^ left;
        left = mixed;
    }
    words[0] = right ^ g_key1_schedule[1];
    words[1] = left ^ g_key1_schedule[0];
}

void key1_apply(uint32_t keycode[3], uint32_t modulus) {
    key1_encrypt(&keycode[1]);
    key1_encrypt(&keycode[0]);
    for (uint32_t i = 0; i < 18u; ++i)
        g_key1_schedule[i] ^= byte_swap32(keycode[i % modulus]);
    uint32_t block[2] = {};
    for (uint32_t i = 0; i < 0x412u; i += 2u) {
        key1_encrypt(block);
        g_key1_schedule[i] = block[1];
        g_key1_schedule[i + 1u] = block[0];
    }
}

void key1_init(uint32_t game_code, uint32_t level = 2u) {
    std::memcpy(g_key1_schedule, g_key1_base, sizeof(g_key1_schedule));
    uint32_t keycode[3] = {game_code, game_code >> 1u, game_code << 1u};
    if (level >= 1u) key1_apply(keycode, 2u);
    if (level >= 2u) key1_apply(keycode, 2u);
    if (level >= 3u) {
        keycode[1] <<= 1u;
        keycode[2] >>= 1u;
        key1_apply(keycode, 2u);
    }
}

void key1_encrypt_bytes(uint8_t* bytes) {
    uint32_t block[2] = {load_le32(bytes), load_le32(bytes + 4u)};
    key1_encrypt(block);
    store_le32(bytes, block[0]);
    store_le32(bytes + 4u, block[1]);
}

// Most .nds dumps store the secure area already decrypted: its first two
// words are the post-decryption marker while the remainder contains plaintext
// code. A physical card exposes that area in KEY1-encrypted form and the ARM7
// BIOS decrypts it during boot. Normalize decrypted dumps back to the card-side
// representation so LLE firmware neither double-decrypts nor rejects them.
void card_reencrypt_secure_area_if_needed() {
    constexpr uint32_t kMarker = 0xE7FFDEFFu;
    constexpr uint32_t kSecureSize = 0x800u;
    if (!g_key1_available || g_card_rom.size() < 0x24u) return;

    const uint32_t arm9_rom_offset = load_le32(g_card_rom.data() + 0x20u);
    if (arm9_rom_offset < 0x4000u || arm9_rom_offset >= 0x8000u ||
        uint64_t{arm9_rom_offset} + kSecureSize > g_card_rom.size() ||
        load_le32(g_card_rom.data() + arm9_rom_offset) != kMarker ||
        load_le32(g_card_rom.data() + arm9_rom_offset + 0x10u) == kMarker)
        return;

    uint8_t* secure = g_card_rom.data() + arm9_rom_offset;
    static constexpr uint8_t kSecureMagic[8] = {
        'e', 'n', 'c', 'r', 'y', 'O', 'b', 'j'
    };
    std::memcpy(secure, kSecureMagic, sizeof(kSecureMagic));

    const uint32_t game_code = load_le32(g_card_rom.data() + 0x0Cu);
    key1_init(game_code, 3u);
    for (uint32_t pos = 0; pos < kSecureSize; pos += 8u)
        key1_encrypt_bytes(secure + pos);

    key1_init(game_code, 2u);
    key1_encrypt_bytes(secure);
}

void key1_decode_command(const uint8_t encrypted[8], uint8_t decoded[8]) {
    uint32_t block[2] = {load_be32(encrypted + 4), load_be32(encrypted)};
    key1_decrypt(block);
    store_le32(decoded, byte_swap32(block[1]));
    store_le32(decoded + 4, byte_swap32(block[0]));
}

void card_copy_rom(uint32_t address, uint32_t length, uint32_t destination) {
    if (address >= g_card_rom.size() || destination >= g_card_response.size()) return;
    const size_t available = std::min<size_t>(length, g_card_rom.size() - address);
    const size_t room = g_card_response.size() - destination;
    std::memcpy(g_card_response.data() + destination,
                g_card_rom.data() + address, std::min(available, room));
}

void card_fill_chip_id() {
    for (uint32_t pos = 0; pos + 4u <= g_card_response.size(); pos += 4u)
        store_le32(g_card_response.data() + pos, g_card_chip_id);
}

void card_prepare_response() {
    g_card_response.assign(g_card_transfer_len, 0);
    if (g_card_rom.empty()) {
        std::fill(g_card_response.begin(), g_card_response.end(), 0xFFu);
        return;
    }

    if (g_card_mode == CardCommandMode::Raw) {
        switch (g_card_command[0]) {
            case 0x9F:
                std::fill(g_card_response.begin(), g_card_response.end(), 0xFFu);
                break;
            case 0x00:
                for (uint32_t pos = 0; pos < g_card_transfer_len; pos += 0x1000u)
                    card_copy_rom(0, std::min(0x1000u, g_card_transfer_len - pos), pos);
                break;
            case 0x90:
                card_fill_chip_id();
                break;
            case 0x3C:
                if (g_key1_available && g_card_rom.size() >= 0x10u) {
                    key1_init(load_le32(g_card_rom.data() + 0x0Cu), 2u);
                    g_card_mode = CardCommandMode::Key1;
                }
                break;
            default:
                break;
        }
        return;
    }

    if (g_card_mode == CardCommandMode::Key1) {
        uint8_t decoded[8] = {};
        key1_decode_command(g_card_command, decoded);
        switch (decoded[0] & 0xF0u) {
            case 0x40:
                g_card_data_mode = 2;
                break; // KEY2 is transparent at the command/data-port boundary.
            case 0x10:
                card_fill_chip_id();
                break;
            case 0x20:
                card_copy_rom((decoded[2] & 0xF0u) << 8u,
                              std::min(0x1000u, g_card_transfer_len), 0);
                break;
            case 0xA0:
                g_card_mode = CardCommandMode::Normal;
                break;
            default:
                break;
        }
        return;
    }

    if (g_card_command[0] == 0xB8u) {
        card_fill_chip_id();
    } else if (g_card_command[0] == 0xB7u && !g_card_rom.empty()) {
        uint32_t address = load_be32(g_card_command + 1u);
        address &= static_cast<uint32_t>(g_card_rom.size() - 1u);
        if (address < 0x8000u) address = 0x8000u + (address & 0x1FFu);
        card_copy_rom(address, g_card_transfer_len, 0);
    }
}

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

bool card_owned_by(int cpu) {
    return static_cast<int>((g_exmemcnt[0] >> 11u) & 1u) == (cpu & 1);
}

bool card_register_address(uint32_t addr) {
    return (addr >= 0x040001A0u && addr < 0x040001C0u) ||
           (addr >= 0x04100010u && addr < 0x04100014u);
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
// DMA completions can storm (a mis-paced streaming channel completes
// thousands of times per frame); keep enough to span a full frame of them.
constexpr uint32_t kDmaTraceSize = 8192;
NdsDmaTraceEntry g_dma_trace[kDmaTraceSize] = {};
uint64_t g_dma_trace_count = 0;

// Large enough to retain two maximum-size (0x4000-byte) blocks including
// their causal ROMCTRL/command/completion markers.
constexpr uint32_t kCardTraceSize = 8192;
NdsCardTraceEntry g_card_trace[kCardTraceSize] = {};
uint32_t g_card_trace_w = 0;
uint32_t g_card_trace_count = 0;
uint64_t g_card_trace_seq = 0;

uint32_t card_response_word(uint32_t position) {
    uint32_t word = 0;
    for (uint32_t byte = 0; byte < 4u; ++byte) {
        const uint32_t pos = position + byte;
        const uint8_t value = pos < g_card_response.size()
            ? g_card_response[pos] : 0xFFu;
        word |= uint32_t{value} << (byte * 8u);
    }
    return word;
}

void card_trace_push(uint8_t kind, int owner, const uint8_t* command,
                     uint32_t requested_romctrl, uint32_t auxspicnt,
                     uint32_t word, uint32_t command_mode_before,
                     uint32_t command_mode_after, uint32_t data_mode_before,
                     uint32_t data_mode_after, bool start) {
    NdsCardTraceEntry& e = g_card_trace[g_card_trace_w];
    e = {};
    e.seq = ++g_card_trace_seq;
    e.sys = scheduler_system_timestamp();
    e.cyc9 = scheduler_cpu_cycles(0);
    e.cyc7 = scheduler_cpu_cycles(1);
    e.insn9 = g_insn_count[0];
    e.insn7 = g_insn_count[1];
    e.kind = kind;
    e.owner = static_cast<uint8_t>(owner & 1);
    if (command) std::memcpy(e.command, command, sizeof(e.command));
    e.requested_romctrl = requested_romctrl;
    e.romctrl = g_romctrl;
    e.auxspicnt = auxspicnt;
    e.transfer_pos = g_card_transfer_pos;
    e.transfer_len = g_card_transfer_len;
    e.transfer_dir = 0;
    e.word = word;
    e.command_mode_before = command_mode_before;
    e.command_mode_after = command_mode_after;
    e.data_mode_before = data_mode_before;
    e.data_mode_after = data_mode_after;
    e.start = start ? 1u : 0u;
    g_card_trace_w = (g_card_trace_w + 1u) % kCardTraceSize;
    if (g_card_trace_count < kCardTraceSize) ++g_card_trace_count;
}

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
        runtime_request_yield_poll();
    }
}

}  // namespace

// Retired-insn ordinals + per-insn slow-path gate (see io.h). Defined at
// global scope — extern "C" symbols the generated banks bump directly.
// Armed at startup because g_runtime_deep_trace defaults on;
// nds_insn_hook_recompute keeps it consistent from then on.
extern "C" uint64_t g_insn_count[2] = {0, 0};
extern "C" uint32_t g_insn_hook_armed = 1u;

// Keep the generated code's per-insn slow-path gate consistent with its two
// inputs: the deep-trace policy and the event-break arming state. (Inside
// this TU so it can see the file-local g_brk_ptr.)
void nds_insn_hook_recompute() {
    g_insn_hook_armed = (g_runtime_deep_trace || g_brk_ptr) ? 1u : 0u;
}

// Armed per-insn payload for new-emission banks: the deep-trace register
// ring and the event-break check. The counter bump itself is inlined at the
// emission site (runtime_arm.h "Inline retired-instruction counters"), so
// this must NOT bump — unlike runtime_insn_fp, the whole hook for banks
// generated before the inline scheme.
extern "C" void runtime_insn_slow(void) {
    const int cpu = (g_nds_active == NDS_ARM7) ? 1 : 0;
    if (g_runtime_deep_trace) {
        const uint64_t count = g_insn_count[cpu];
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
    }
    brk_check();
}

namespace {

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
        g_insn_count[0],
        g_insn_count[1],
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

void dma_trace_push(int cpu, int ch, const DmaChannel& d) {
    ++g_dma_trace_count;
    g_dma_trace[(g_dma_trace_count - 1) % kDmaTraceSize] = {
        g_dma_trace_count,
        scheduler_system_timestamp(),
        g_runtime_cycles,
        g_insn_count[cpu == 0 ? 0 : 1],
        d.cnt,
        d.src,
        d.dst,
        static_cast<uint8_t>(cpu),
        static_cast<uint8_t>(ch),
        d.start_mode,
    };
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
        // Increment/reload addressing reloads at the beginning of a DMA
        // transaction, not at every hardware request within that transaction.
        // This matters for gamecard DMA, which is requested once per ready
        // word and therefore resumes the same transaction many times.
        if ((d.cnt & 0x01800000u) == 0x01800000u) d.cur_src = d.src;
        if ((d.cnt & 0x00600000u) == 0x00600000u) d.cur_dst = d.dst;
    }
    d.burst_index = 0;
    d.burst_start = true;
    d.running = true;
    d.in_progress = true;
    runtime_request_yield_poll();
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
        // GXFIFO DMA arms through the engine's FIFO-level check rather than
        // starting unconditionally (melonDS DMA::WriteCnt).
        else if (cpu == 0 && d.start_mode == 7u) nds_gpu3d_check_fifo_dma();
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

uint8_t rtc_bcd(unsigned v) {
    return static_cast<uint8_t>(((v / 10u) << 4) | (v % 10u));
}

// Set the date/time registers from the host's local clock, following
// melonDS RTC::SetDateTime exactly: DS range is 2000-2099; the day-of-week
// register is a software counter the DS firmware counts from 0=Sunday, and
// 01/01/2000 was a Saturday; hour carries the PM flag (0x40) and drops 12
// in 12-hour mode (status1 bit 1 clear).
void rtc_sync_host() {
    const std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    int year = tm_buf.tm_year + 1900;
    const int month = tm_buf.tm_mon + 1;
    const int day = tm_buf.tm_mday;
    int hour = tm_buf.tm_hour;
    const int minute = tm_buf.tm_min;
    const int second = tm_buf.tm_sec > 59 ? 59 : tm_buf.tm_sec;  // leap second

    year %= 100;
    if (year < 0) year = 0;

    static const int monthdays[13] =
        {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int numdays = (year * 365) + ((year + 3) / 4);
    for (int m = 1; m < month; ++m) {
        numdays += monthdays[m];
        if (m == 2 && !(year & 3)) ++numdays;
    }
    numdays += day - 1;
    const int dayofweek = (6 + numdays) % 7;   // 01/01/2000 = Saturday

    const int pm = hour >= 12 ? 0x40 : 0;
    if (!(g_rtc_status1 & 0x02u) && pm) hour -= 12;   // 12-hour mode

    g_rtc_datetime[0] = rtc_bcd(static_cast<unsigned>(year));
    g_rtc_datetime[1] = rtc_bcd(static_cast<unsigned>(month));
    g_rtc_datetime[2] = rtc_bcd(static_cast<unsigned>(day));
    g_rtc_datetime[3] = static_cast<uint8_t>(dayofweek);
    g_rtc_datetime[4] = static_cast<uint8_t>(
        rtc_bcd(static_cast<unsigned>(hour)) | pm);
    g_rtc_datetime[5] = rtc_bcd(static_cast<unsigned>(minute));
    g_rtc_datetime[6] = rtc_bcd(static_cast<unsigned>(second));
}

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

// ── AUXSPI backup-chip protocol (melonDS CartRetail::SPIWrite + the
// regular-EEPROM handler; addrsize 2 below 64 KiB) ──────────────────────
uint8_t cart_sram_spi_write(uint8_t val, uint32_t pos, bool last) {
    if (g_cart_sram.empty()) return 0;
    if (pos == 0) {
        switch (val) {
            case 0x04: g_sram_status &= ~0x02u; return 0;    // WRDI
            case 0x06: g_sram_status |= 0x02u; return 0;     // WREN
            default: g_sram_cmd = val; g_sram_addr = 0; break;
        }
        return 0xFF;
    }
    const uint32_t mask = static_cast<uint32_t>(g_cart_sram.size()) - 1u;
    const uint32_t addrsize = g_cart_sram.size() > 65536u ? 3u : 2u;
    switch (g_sram_cmd) {
        case 0x01:  // write status register
            if (pos == 1) g_sram_status = (g_sram_status & 0x01u) | (val & 0x0Cu);
            return 0;
        case 0x05:  // read status register
            return g_sram_status;
        case 0x02:  // write
            if (pos <= addrsize) {
                g_sram_addr = (g_sram_addr << 8) | val;
            } else {
                if (g_sram_status & 0x02u) g_cart_sram[g_sram_addr & mask] = val;
                ++g_sram_addr;
            }
            if (last) g_sram_status &= ~0x02u;
            return 0;
        case 0x03:  // read
            if (pos <= addrsize) {
                g_sram_addr = (g_sram_addr << 8) | val;
                return 0;
            } else {
                const uint8_t ret = g_cart_sram[g_sram_addr & mask];
                ++g_sram_addr;
                return ret;
            }
        case 0x9F:  // read JEDEC ID
            return 0xFF;
        default:
            return 0xFF;
    }
}

void auxspi_write_cnt(uint16_t val) {
    // Deasserting NDS-slot mode mid-hold forcefully releases the chip select.
    if ((g_auxspicnt & 0x2040u) == 0x2040u && !(val & 0x2000u))
        g_auxspi_hold = false;
    g_auxspicnt = (g_auxspicnt & 0x0080u) | (val & 0xE043u);
}

void auxspi_write_data(uint8_t val) {
    if (!(g_auxspicnt & 0x8000u)) return;
    if (!(g_auxspicnt & 0x2000u)) return;
    if (g_auxspicnt & 0x0080u) return;    // busy: the write is dropped
    g_auxspicnt |= 0x0080u;
    const bool hold = (g_auxspicnt & 0x0040u) != 0;
    bool islast = false;
    if (!hold) {
        if (g_auxspi_hold) ++g_auxspi_pos;
        else g_auxspi_pos = 0;
        islast = true;
        g_auxspi_hold = false;
    } else if (!g_auxspi_hold) {
        g_auxspi_hold = true;
        g_auxspi_pos = 0;
    } else {
        ++g_auxspi_pos;
    }
    g_auxspi_data = cart_sram_spi_write(val, g_auxspi_pos, islast);
    // One bit per SPI clock, 8 bits per byte: 8*(8<<speed) system cycles,
    // based at the writing CPU's live timestamp (melonDS ScheduleEvent).
    const uint32_t delay = 8u * (8u << (g_auxspicnt & 3u));
    const uint64_t now = active() == 0 ? (g_runtime_cycles >> 1)
                                       : g_runtime_cycles;
    g_auxspi_deadline = now + delay;
}

uint8_t auxspi_read_data() {
    if (!(g_auxspicnt & 0x8000u)) return 0;
    if (!(g_auxspicnt & 0x2000u)) return 0;
    if (g_auxspicnt & 0x0080u) return 0;
    return g_auxspi_data;
}

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

// --rtc-host (see io.h): main.cpp sets this before the initial boot.
bool g_nds_rtc_host = false;

void nds_io_reset() {
    for (int i = 0; i < 2; ++i) {
        g_ipcsync_out[i] = 0; g_postflg[i] = 0;
        g_ime[i] = 0; g_ie[i] = 0; g_if[i] = 0; g_haltcnt[i] = 0;
        g_nds_irq_pending_cache[i] = 0;
        g_cpu_halted[i] = false;
        g_halt_entry_cycle[i] = 0;
        g_dispstat[i] = 0;
        g_vcount_match[i] = false;
        g_keycnt[i] = 0;
    }
    g_vcount = 0; g_next_vcount = 0; g_next_vcount_valid = false;
    g_in_vblank = false;
    g_counts = {};
    g_insn_count[0] = g_insn_count[1] = 0;
    std::memset(g_dma, 0, sizeof(g_dma));
    g_dma_entry_cycle[0] = g_dma_entry_cycle[1] = 0;
    std::memset(g_spi_trace, 0, sizeof(g_spi_trace));
    std::memset(g_irq_trace, 0, sizeof(g_irq_trace));
    std::memset(g_insn_trace, 0, sizeof(g_insn_trace));
    std::memset(g_fifo_trace, 0, sizeof(g_fifo_trace));
    std::memset(g_card_trace, 0, sizeof(g_card_trace));
    g_card_trace_w = 0; g_card_trace_count = 0; g_card_trace_seq = 0;
    g_romctrl = 0; g_card_transfer_pos = 0; g_card_transfer_len = 0;
    g_card_deadline = UINT64_MAX; g_card_end_event = false; g_card_irq_cpu = 0;
    g_card_response.clear();
    std::memset(g_card_command, 0, sizeof(g_card_command));
    g_card_mode = CardCommandMode::Raw;
    g_card_data_mode = 0;
    // AUXSPI controller/chip protocol state resets; the backup CONTENTS
    // survive (the cartridge stays inserted across a console reset).
    g_auxspicnt = 0; g_auxspi_data = 0;
    g_auxspi_hold = false; g_auxspi_pos = 0;
    g_auxspi_deadline = UINT64_MAX;
    g_sram_cmd = 0; g_sram_status = 0; g_sram_addr = 0;
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
    // Opt-in host clock: every guest boot starts at host local time. The
    // deterministic power-on datetime above stays the default (oracle gates
    // compare RTC state, so parity runs must never set this).
    if (g_nds_rtc_host) rtc_sync_host();
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
    bus_fast_refresh();
    g_wifiwaitcnt = 0;
    g_warned = 0;
    nds_wifi_reset();
    nds_spu_reset();
    nds_vram_reset();
    nds_gpu2d_reset();
    nds_gpu3d_reset();
}

void nds_io_load_firmware(const uint8_t* p, uint32_t n) {
    g_fw.assign(p, p + n);
    nds_wifi_load_firmware(p, n);
}

bool nds_io_load_cartridge(const uint8_t* rom, uint32_t rom_size,
                           const uint8_t* arm7_bios, uint32_t bios_size) {
    constexpr uint32_t kKeyOffset = 0x30u;
    constexpr uint32_t kKeyBytes = 0x412u * 4u;
    if (!rom || rom_size < 0x200u || !arm7_bios ||
        bios_size < kKeyOffset + kKeyBytes) {
        g_card_rom.clear();
        g_key1_available = false;
        return false;
    }

    g_card_rom.assign(rom, rom + rom_size);
    for (uint32_t i = 0; i < 0x412u; ++i)
        g_key1_base[i] = load_le32(arm7_bios + kKeyOffset + i * 4u);
    g_key1_available = true;
    card_reencrypt_secure_area_if_needed();
    g_card_mode = CardCommandMode::Raw;
    g_card_data_mode = 0;
    g_card_response.clear();
    std::memset(g_card_command, 0, sizeof(g_card_command));

    const uint32_t megabytes = std::max(1u, rom_size >> 20u);
    g_card_chip_id = 0x000000C2u | ((megabytes - 1u) << 8u);

    // Backup chip: 8 KiB regular EEPROM, blank = 0xFF (melonDS CartRetail
    // with ROMList SaveMemType 2 — SM64DS's entry and also melonDS's
    // unknown-ROM default). No host persistence yet; matches ndsref, which
    // also runs without a save file.
    g_cart_sram.assign(8192, 0xFF);
    return true;
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

uint64_t nds_dma_trace_count() { return g_dma_trace_count; }

bool nds_dma_trace_get(uint64_t count, NdsDmaTraceEntry* out) {
    if (!out || count == 0) return false;
    const NdsDmaTraceEntry& e = g_dma_trace[(count - 1) % kDmaTraceSize];
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
        g_insn_count[cpu],
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

void nds_card_debug_state(NdsCardDebugState* out) {
    if (!out) return;
    *out = {};
    out->present = g_card_rom.empty() ? 0u : 1u;
    out->owner = static_cast<uint8_t>((g_exmemcnt[0] >> 11u) & 1u);
    for (uint32_t i = 0; i < 8u; ++i)
        out->command[i] = static_cast<uint8_t>(
            io_mem_read(0x040001A8u + i, 1));
    if (g_card_rom.size() >= 0x10u)
        std::memcpy(out->game_code, g_card_rom.data() + 0x0Cu,
                    sizeof(out->game_code));
    out->chip_id = g_card_chip_id;
    out->rom_size = static_cast<uint32_t>(g_card_rom.size());
    out->auxspicnt = g_auxspicnt;
    out->romctrl = g_romctrl;
    out->transfer_pos = g_card_transfer_pos;
    out->transfer_len = g_card_transfer_len;
    out->transfer_dir = 0;
    out->command_mode = static_cast<uint32_t>(g_card_mode);
    out->data_mode = g_card_data_mode;
    out->produced = g_card_trace_seq;
    out->capacity = kCardTraceSize;
    if (g_card_trace_count) {
        const uint32_t first =
            (g_card_trace_w + kCardTraceSize - g_card_trace_count) %
            kCardTraceSize;
        out->oldest = g_card_trace[first].seq;
    } else {
        out->oldest = g_card_trace_seq + 1u;
    }
}

uint32_t nds_card_debug_trace_copy(NdsCardTraceEntry* out,
                                   uint32_t max_entries) {
    if (!out || max_entries == 0u) return 0u;
    const uint32_t count = std::min(g_card_trace_count, max_entries);
    const uint32_t start =
        (g_card_trace_w + kCardTraceSize - count) % kCardTraceSize;
    for (uint32_t i = 0; i < count; ++i)
        out[i] = g_card_trace[(start + i) % kCardTraceSize];
    return count;
}

uint64_t nds_next_system_event_time() {
    return std::min(std::min(g_card_deadline, g_spi_deadline),
                    std::min(g_auxspi_deadline,
                             std::min(g_div_deadline, g_sqrt_deadline)));
}

uint64_t nds_next_timer_overflow_time() {
    // Timers are catch-up-ticked rather than deadline-scheduled, so the idle
    // fast-forward needs their next overflow as an explicit deadline. Only
    // clock-driven timers own a timeline position; a count-up (cascade) timer
    // overflows at a feeding timer's overflow instant, which this already
    // bounds — the catch-up tick then propagates the carry exactly.
    uint64_t best = UINT64_MAX;
    for (int cpu = 0; cpu < 2; ++cpu) {
        for (int t = 0; t < 4; ++t) {
            const Timer& T = g_timer[cpu][t];
            if (!(T.ctrl & 0x80u)) continue;
            if (T.ctrl & 0x4u) continue;
            const unsigned long long pre = kPrescaler[T.ctrl & 3u];
            const unsigned long long span = 0x10000ull - T.counter;
            const uint64_t when = g_timer_last[cpu] + (span * pre - T.accum);
            best = std::min(best, when);
        }
    }
    return best;
}
uint64_t nds_debug_spi_deadline() { return g_spi_deadline; }
uint64_t nds_debug_card_deadline() { return g_card_deadline; }

void nds_run_system_events(uint64_t timestamp) {
    // These events are one-shot. A handler may schedule its successor only
    // after a guest data-port read, matching melonDS's card-ready handshake.
    if (g_auxspi_deadline <= timestamp) {
        // melonDS NDSCartSlot::SPITransferDone: clear busy, no IRQ.
        g_auxspi_deadline = UINT64_MAX;
        g_auxspicnt &= ~0x0080u;
    }
    if (g_spi_deadline <= timestamp) {
        g_spi_deadline = UINT64_MAX;
        g_spicnt &= ~0x0080u;
        if (g_spicnt & 0x4000u)
            nds_raise_irq(1, 0x00800000u); // ARM7 IRQ_SPI
    }
    if (g_card_deadline <= timestamp) {
        const bool end_event = g_card_end_event;
        g_card_deadline = UINT64_MAX;
        if (end_event) {
            g_romctrl &= ~0x80000000u;
            const uint32_t auxspicnt = g_auxspicnt;
            if (auxspicnt & 0x4000u)
                nds_raise_irq(g_card_irq_cpu, 0x00080000u);
            const uint32_t mode = static_cast<uint32_t>(g_card_mode);
            card_trace_push(NDS_CARD_TRACE_COMPLETE, g_card_irq_cpu,
                            g_card_command, g_romctrl, auxspicnt, 0,
                            mode, mode, g_card_data_mode, g_card_data_mode,
                            false);
        } else {
            g_romctrl |= 0x00800000u;
            const uint32_t mode = static_cast<uint32_t>(g_card_mode);
            card_trace_push(NDS_CARD_TRACE_DATA_READY, g_card_irq_cpu,
                            g_card_command, g_romctrl,
                            g_auxspicnt,
                            card_response_word(g_card_transfer_pos),
                            mode, mode, g_card_data_mode, g_card_data_mode,
                            false);
            nds_dma_trigger(g_card_irq_cpu,
                            g_card_irq_cpu == 0 ? 5u : 0x12u);
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
    // Whole hook for pre-inline-emission banks (via runtime_insn_fp) and the
    // Tier-3 interpreter: bump the architectural ordinal, then the same armed
    // payload new-emission banks reach through runtime_insn_slow. Callers
    // always pass the active CPU, so the slow path's g_nds_active read
    // matches `cpu`.
    ++g_insn_count[cpu & 1];
    if (g_insn_hook_armed) runtime_insn_slow();
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
    if (std::strcmp(name, "insn9") == 0) return g_insn_count[0];
    if (std::strcmp(name, "insn7") == 0) return g_insn_count[1];
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
    if (std::strcmp(name, "insn9") == 0) return &g_insn_count[0];
    if (std::strcmp(name, "insn7") == 0) return &g_insn_count[1];
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
    nds_insn_hook_recompute();
}
void nds_event_break_disarm() {
    g_brk_ptr = nullptr; g_brk_hit = false;
    g_brk_is_insn = false; g_nds_insn_stop = false;
    nds_insn_hook_recompute();
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
void nds_raise_irq(int cpu, uint32_t bits) {
    cpu &= 1;
    g_if[cpu] |= bits;
    irq_recompute(cpu);
}

void nds_clear_irq(int cpu, uint32_t bits) {
    cpu &= 1;
    g_if[cpu] &= ~bits;
    irq_recompute(cpu);
}

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
    runtime_request_yield_poll();
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

// GXFIFO ARM9 stall flag (melonDS CPUStop_GXStall). Set/cleared by the
// vendored GPU3D via the NDS shim; scheduler consumption lands with the
// geometry engine's Run() wiring (3D Phase 2).
static bool g_gxfifo_stall = false;
void nds_gxfifo_set_stall(bool stalled) { g_gxfifo_stall = stalled; }
bool nds_gxfifo_stalled() { return g_gxfifo_stall; }

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
        // While the GXFIFO stall is asserted only ARM9 channel 0 keeps its
        // bus grant (melonDS runs DMAs[1..3] only when CPUStop_GXStall is
        // clear); a stalled channel stays running and resumes when the
        // geometry engine drains.
        if (cpu == 0 && ch > 0 && nds_gxfifo_stalled()) continue;
        if (!d.running || g_runtime_cycles >= target_cycles) continue;
        const uint32_t width = (d.cnt & 0x04000000u) ? 4u : 2u;
        const bool gamecard = d.start_mode == (cpu == 0 ? 5u : 0x12u);
        const bool gxfifo = cpu == 0 && d.start_mode == 7u;
        // Gamecard: one word per data-ready request. GXFIFO: at most 112
        // units per FIFO-level request (melonDS DMA::Start IterCount cap).
        uint32_t request_units = gamecard ? 1u : gxfifo ? 112u : UINT32_MAX;
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
            if (--request_units == 0u) break;
            // A write into a full GXFIFO stalls channels 1-3 mid-iteration
            // (melonDS DMA::StallIfRunning); channel 0 keeps streaming into
            // the overflow queue.
            if (ch > 0 && cpu == 0 && nds_gxfifo_stalled()) break;
        }
        if (d.remaining) {
            if (gamecard) {
                // The card asserts a distinct DMA request for each 32-bit word.
                // Suspend this channel without completing or reloading it; the
                // next data-ready event resumes the pending transaction.
                d.running = false;
            } else if (gxfifo && request_units == 0u) {
                // Iteration exhausted: suspend and let the engine's FIFO-level
                // check re-request the next chunk (melonDS DMA::Run9 tail).
                d.running = false;
                nds_gpu3d_check_fifo_dma();
            }
            continue;
        }

        dma_trace_push(cpu, ch, d);
        if (!(d.cnt & 0x02000000u)) d.cnt &= ~0x80000000u;
        if (d.cnt & 0x40000000u) {
            nds_raise_irq(cpu, 1u << (8 + ch));
            // The oracle counts dma_done in its SetIRQ hook, so only
            // IRQ-raising completions are cross-comparable. IRQ-less
            // completions (e.g. SM64DS GXFIFO streaming) must not count,
            // or the native/ndsref counters silently drift apart.
            ++g_counts.dma_done;
        }
        d.running = false;
        d.in_progress = false;
        brk_check();
    }
}

// ── Display + timer clocks (driven from the scheduler) ──────────────────
// DS scanline timing in system-cycle units; the precise
// numbers land with the melonDS oracle. VBlank (IF bit 0) fires once per
// frame. Both cores see it (a display event).
void nds_tick_display(unsigned long long cyc) {
    // The raster advances on the 33.51 MHz system clock
    // (2130 cycles/scanline, 263 physical lines = 560190 cyc/frame). VCOUNT is
    // normally incremented at each scanline start, but is independently
    // writable: hardware/melonDS apply a write at the next physical scanline.
    // Firmware uses this to resynchronise the logical display counter during
    // the handoff to a game, so deriving VCOUNT only from absolute time loses
    // several scanlines and wakes the ARM7 service on the wrong interrupt.
    static const unsigned long long SCAN = 2130, LINES = 263;
    static const unsigned long long FRAME = SCAN * LINES;
    static const unsigned long long HBLANK_START = 1584;
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

    const unsigned long long previous_scanline = g_display_last / SCAN;
    const unsigned long long current_scanline = cyc / SCAN;
    const unsigned long long previous_frame = g_display_last / FRAME;
    const unsigned long long current_frame = cyc / FRAME;
    g_display_last = cyc;

    for (unsigned long long i = previous_scanline + 1u;
         i <= current_scanline; ++i) {
        const uint32_t physical_line = static_cast<uint32_t>(i % LINES);
        if (physical_line == 0u) {
            // melonDS GPU::StartFrame: an aborted 3D frame restarts its
            // renderer before scanline 0 of the new frame.
            nds_gpu3d_start_frame();
            g_vcount = 0;
        } else if (g_next_vcount_valid) {
            g_vcount = g_next_vcount;
        } else {
            ++g_vcount;
        }
        // A pending write is consumed even when physical line zero forces the
        // counter to zero, matching GPU::StartScanline.
        g_next_vcount_valid = false;

        for (int cpu = 0; cpu < 2; ++cpu) {
            const uint16_t match = static_cast<uint16_t>(
                ((g_dispstat[cpu] & 0x0080u) << 1u) |
                (g_dispstat[cpu] >> 8u));
            g_vcount_match[cpu] = g_vcount == match;
            if (g_vcount_match[cpu] && (g_dispstat[cpu] & 0x0020u))
                nds_raise_irq(cpu, 0x00000004u);
        }

        if (g_vcount == 262u) {
            g_in_vblank = false;
        } else if (g_vcount == 192u) {
            g_in_vblank = true;
            if (g_dispstat[0] & 0x0008u) {
                ++g_counts.vblank9;
                nds_raise_irq(0, 0x00000001u);
            }
            if (g_dispstat[1] & 0x0008u) {
                ++g_counts.vblank7;
                nds_raise_irq(1, 0x00000001u);
            }
            // melonDS StartScanline order: 2D units first (capture-enable
            // auto-clear), then the 3D engine's polygon sort, render-state
            // latch and geometry bank flip.
            nds_gpu2d_vblank();
            nds_gpu3d_vblank();
        } else if (g_vcount == 144u) {
            nds_gpu3d_vcount144();   // renderer frame-sync point
        } else if (g_vcount == 215u) {
            nds_gpu3d_vcount215();   // rasterize the latched frame
        }
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
    if (card_register_address(addr) && !card_owned_by(cpu)) return 0;
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
    // On the ARM9 the 0x04000320..0x040006A3 window is the 3D engine
    // (GXFIFO at 0x400..0x43F overlaps the ARM7's SPU address range); the
    // SPU only exists on the ARM7 side.
    if (cpu == 0 && nds_gpu3d_reg_addr(addr))
        return nds_gpu3d_read(addr, width) & m;
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
            if (g_vcount_match[cpu]) v |= 0x0004u;          // latched at line start
            if (addr == 0x04000004u && width == 4u)
                return uint32_t{v} | (uint32_t{g_vcount} << 16u);
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
        case 0x04000216:             // IF bits 31..16 (ARM9-only, as melonDS)
            return cpu == 0 ? ((g_if[0] >> 16) & m) : 0u;
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
        case 0x040001A0: case 0x040001A1:  // AUXSPICNT (+ AUXSPIDATA on w32)
            if (addr == 0x040001A0u && width == 4u)
                return (uint32_t{g_auxspicnt} |
                        (uint32_t{auxspi_read_data()} << 16u)) & m;
            return (g_auxspicnt >> ((addr & 1u) * 8u)) & m;
        case 0x040001A2: case 0x040001A3:  // AUXSPIDATA
            return addr == 0x040001A2u ? (auxspi_read_data() & m) : 0u;
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
        case 0x04100010: { // gamecard data port
            uint32_t word = 0xFFFFFFFFu;
            if (!(g_romctrl & 0x40000000u) && (g_romctrl & 0x00800000u)) {
                word = card_response_word(g_card_transfer_pos);
                g_romctrl &= ~0x00800000u;
                g_card_transfer_pos += 4u;
                if (g_card_transfer_pos < g_card_transfer_len) {
                    const uint32_t xfercycle =
                        (g_romctrl & 0x08000000u) ? 8u : 5u;
                    uint32_t delay = 4u;
                    if (!(g_card_transfer_pos & 0x1FFu))
                        delay += (g_romctrl >> 16) & 0x3Fu;
                    schedule_card_event(uint64_t{xfercycle} * delay, false);
                } else {
                    g_romctrl &= ~0x80000000u;
                    const uint32_t auxspicnt = g_auxspicnt;
                    if (auxspicnt & 0x4000u)
                        nds_raise_irq(g_card_irq_cpu, 0x00080000u);
                    const uint32_t mode = static_cast<uint32_t>(g_card_mode);
                    card_trace_push(NDS_CARD_TRACE_COMPLETE, g_card_irq_cpu,
                                    g_card_command, g_romctrl, auxspicnt, 0,
                                    mode, mode, g_card_data_mode,
                                    g_card_data_mode, false);
                }
            }
            return word & m;
        }
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
    if (card_register_address(addr) && !card_owned_by(cpu)) return;
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
            else if (a == 0x04000247u) { g_wramcnt = byte; bus_fast_refresh(); }
            else if (a >= 0x04000248u && a <= 0x04000249u) nds_vram_map(a - 0x04000241u, byte);
        }
        return;
    }
    // ARM9 3D engine window (see nds_io_read): must win over the ARM7-SPU
    // range check below, which previously swallowed ARM9 GXFIFO writes.
    if (cpu == 0 && nds_gpu3d_reg_addr(addr)) {
        nds_gpu3d_write(addr, value, width);
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
            if (width == 1u) return;
            uint32_t shift = (addr & 1u) * 8;
            uint32_t wmask = mask_for(width) << shift;
            uint32_t merged = (g_dispstat[cpu] & ~wmask) | ((value << shift) & wmask);
            g_dispstat[cpu] = static_cast<uint16_t>(merged & 0xFFB8u);  // writable bits
            if (addr == 0x04000004u && width == 4u) {
                g_next_vcount = static_cast<uint16_t>(value >> 16u);
                g_next_vcount_valid = true;
            }
            return;
        }
        case 0x04000006: { // VCOUNT write takes effect at next scanline
            if (width == 2u) {
                g_next_vcount = static_cast<uint16_t>(value);
                g_next_vcount_valid = true;
            }
            return;
        }
        case 0x04000180:
            // IPCSYNC's writable fields live in the high byte.  An 8-bit
            // write to the low byte is ignored; melonDS/hardware route byte
            // writes through 0x04000181 instead.
            if (width != 1u)
                ipcsync_write(cpu, static_cast<uint16_t>(value));
            return;
        case 0x04000181:
            if (width == 1u)
                ipcsync_write(cpu, static_cast<uint16_t>(value << 8u));
            return;
        case 0x04000208:
            g_ime[cpu] = value & 0x1u;
            irq_recompute(cpu);
            return;
        case 0x04000210:
            g_ie[cpu] = value;
            irq_recompute(cpu);
            return;
        case 0x04000214:
            g_if[cpu] &= ~value;     // write-1-to-clear
            // The GXFIFO IRQ is level-triggered: acknowledging IF re-asserts
            // the bit while the FIFO condition still holds (melonDS
            // ARM9IOWrite16/32 case 0x04000214/216 call GPU3D.CheckFIFOIRQ()).
            if (cpu == 0) nds_gpu3d_check_fifo_irq();
            irq_recompute(cpu);
            return;
        case 0x04000216:             // IF bits 31..16 (16-bit acknowledge)
            // melonDS wires this on the ARM9 only (ARM9IOWrite16); the ARM7
            // path treats it as an unknown register and drops the write.
            if (cpu == 0) {
                g_if[0] &= ~(value << 16);
                nds_gpu3d_check_fifo_irq();
                irq_recompute(0);
            }
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
                runtime_request_yield_poll();
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
                // POWCNT1 bit3/bit2 gate the 3D geometry/rendering engines
                // (melonDS GPU::SetPowerCnt).
                nds_gpu3d_set_power(merged & 0x820Fu);
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
                g_insn_count[1],
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
        case 0x040001A0:   // AUXSPICNT (w32 also clocks AUXSPIDATA, melonDS)
            if (width == 1u) {
                auxspi_write_cnt(static_cast<uint16_t>(
                    (g_auxspicnt & 0xFF00u) | (value & 0xFFu)));
            } else {
                auxspi_write_cnt(static_cast<uint16_t>(value & 0xFFFFu));
                if (width == 4u)
                    auxspi_write_data(static_cast<uint8_t>(value >> 16u));
            }
            return;
        case 0x040001A1:   // AUXSPICNT high byte
            auxspi_write_cnt(static_cast<uint16_t>(
                (g_auxspicnt & 0x00FFu) | ((value & 0xFFu) << 8u)));
            return;
        case 0x040001A2:   // AUXSPIDATA
            auxspi_write_data(static_cast<uint8_t>(value));
            return;
        case 0x040001A3:   // unmapped byte lane (melonDS drops it)
            return;
        case 0x040001A4: { // ROMCTRL (32-bit write starts a gamecard block)
            const bool start = (value & ~g_romctrl & 0x80000000u) != 0u;
            g_romctrl = (value & 0xFF7F7FFFu) | (g_romctrl & 0x20800000u);
            const uint16_t aux = static_cast<uint16_t>(
                g_auxspicnt);
            uint8_t pending_command[8] = {};
            for (uint32_t i = 0; i < 8u; ++i)
                pending_command[i] = static_cast<uint8_t>(
                    io_mem_read(0x040001A8u + i, 1));
            const uint32_t mode_before = static_cast<uint32_t>(g_card_mode);
            const uint32_t data_mode_before = g_card_data_mode;
            card_trace_push(NDS_CARD_TRACE_ROMCTRL, cpu, pending_command,
                            value, aux, 0, mode_before, mode_before,
                            data_mode_before, data_mode_before, start);
            if (!start || !(aux & 0x8000u) || (aux & 0x2000u)) return;

            g_card_transfer_pos = 0;
            g_card_transfer_len = card_block_words(g_romctrl) * 4u;
            g_card_irq_cpu = cpu;
            g_romctrl &= ~0x00800000u;
            std::memcpy(g_card_command, pending_command,
                        sizeof(g_card_command));
            card_prepare_response();
            card_trace_push(NDS_CARD_TRACE_COMMAND, cpu, g_card_command,
                            g_romctrl, aux, 0, mode_before,
                            static_cast<uint32_t>(g_card_mode),
                            data_mode_before, g_card_data_mode, true);
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
