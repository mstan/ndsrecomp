#include "spu.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "state.h"

namespace {

constexpr int8_t kAdpcmIndex[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
constexpr uint16_t kAdpcmStep[89] = {
    0x0007,0x0008,0x0009,0x000A,0x000B,0x000C,0x000D,0x000E,
    0x0010,0x0011,0x0013,0x0015,0x0017,0x0019,0x001C,0x001F,
    0x0022,0x0025,0x0029,0x002D,0x0032,0x0037,0x003C,0x0042,
    0x0049,0x0050,0x0058,0x0061,0x006B,0x0076,0x0082,0x008F,
    0x009D,0x00AD,0x00BE,0x00D1,0x00E6,0x00FD,0x0117,0x0133,
    0x0151,0x0173,0x0198,0x01C1,0x01EE,0x0220,0x0256,0x0292,
    0x02D4,0x031C,0x036C,0x03C3,0x0424,0x048E,0x0502,0x0583,
    0x0610,0x06AB,0x0756,0x0812,0x08E0,0x09C3,0x0ABD,0x0BD0,
    0x0CFF,0x0E4C,0x0FBA,0x114C,0x1307,0x14EE,0x1706,0x1954,
    0x1BDC,0x1EA5,0x21B6,0x2515,0x28CA,0x2CDF,0x315B,0x364B,
    0x3BB9,0x41B2,0x4844,0x4F7E,0x5771,0x602F,0x69CE,0x7462,
    0x7FFF
};
constexpr int16_t kPsg[8][8] = {
    {-0x7FFF,-0x7FFF,-0x7FFF,-0x7FFF,-0x7FFF,-0x7FFF,-0x7FFF, 0x7FFF},
    {-0x7FFF,-0x7FFF,-0x7FFF,-0x7FFF,-0x7FFF,-0x7FFF, 0x7FFF, 0x7FFF},
    {-0x7FFF,-0x7FFF,-0x7FFF,-0x7FFF,-0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF},
    {-0x7FFF,-0x7FFF,-0x7FFF,-0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF},
    {-0x7FFF,-0x7FFF,-0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF},
    {-0x7FFF,-0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF},
    {-0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF},
    {-0x7FFF,-0x7FFF,-0x7FFF,-0x7FFF,-0x7FFF,-0x7FFF,-0x7FFF,-0x7FFF}
};

struct Channel {
    uint32_t num = 0;
    uint32_t cnt = 0;
    uint32_t src = 0;
    uint16_t reload = 0;
    uint32_t loop = 0;
    uint32_t length = 0;
    uint8_t volume = 0;
    uint8_t volume_shift = 0;
    uint8_t pan = 0;
    bool key_on = false;
    uint32_t timer = 0;
    int32_t pos = 0;
    int16_t sample = 0;
    uint16_t noise = 0;
    int32_t adpcm_value = 0;
    int32_t adpcm_index = 0;
    int32_t adpcm_loop_value = 0;
    int32_t adpcm_loop_index = 0;
    uint8_t adpcm_byte = 0;
    std::array<uint8_t, 32> fifo{};
    uint32_t fifo_read = 0;
    uint32_t fifo_write = 0;
    uint32_t source_offset = 0;
    uint32_t fifo_level = 0;

    void set_cnt(uint32_t value) {
        const uint32_t old = cnt;
        cnt = value & 0xFF7F837Fu;
        volume = cnt & 0x7Fu;
        if (volume == 127) ++volume;
        constexpr uint8_t shifts[4] = {4, 3, 2, 0};
        volume_shift = shifts[(cnt >> 8) & 3u];
        pan = (cnt >> 16) & 0x7Fu;
        if (pan == 127) ++pan;
        if ((value & 0x80000000u) && !(old & 0x80000000u)) key_on = true;
    }

    void buffer_data() {
        const uint32_t total = loop + length;
        if (source_offset >= total) {
            const uint32_t repeat = (cnt >> 27) & 3u;
            if (repeat & 1u) source_offset = loop;
            else if (repeat & 2u) return;
        }
        uint32_t burst = 16;
        if (source_offset + burst > total) burst = total - source_offset;
        for (uint32_t i = 0; i < burst; i += 4) {
            uint32_t value = 0;
            if (src + source_offset >= 0x00004000u)
                value = bus_device_read32(7, src + source_offset);
            std::memcpy(&fifo[fifo_write], &value, 4);
            source_offset += 4;
            fifo_write = (fifo_write + 4) & 0x1Fu;
        }
        fifo_level += burst;
    }

    template<typename T> T fifo_read_value() {
        T value{};
        std::memcpy(&value, &fifo[fifo_read], sizeof(T));
        fifo_read = (fifo_read + sizeof(T)) & 0x1Fu;
        fifo_level -= sizeof(T);
        if (fifo_level <= 16) buffer_data();
        return value;
    }

    void start() {
        timer = reload;
        pos = (((cnt >> 29) & 3u) == 3u) ? -1 : -3;
        noise = 0x7FFF;
        sample = 0;
        fifo.fill(0);
        fifo_read = fifo_write = source_offset = fifo_level = 0;
        if (((cnt >> 29) & 3u) != 3u) {
            buffer_data();
            buffer_data();
        }
    }

    void next_pcm8() {
        ++pos;
        if (pos < 0) return;
        if (static_cast<uint32_t>(pos) >= loop + length) {
            const uint32_t repeat = (cnt >> 27) & 3u;
            if (repeat & 1u) pos = static_cast<int32_t>(loop);
            else if (repeat & 2u) { sample = 0; cnt &= ~0x80000000u; return; }
        }
        sample = static_cast<int16_t>(fifo_read_value<int8_t>() << 8);
    }

    void next_pcm16() {
        ++pos;
        if (pos < 0) return;
        if ((static_cast<uint32_t>(pos) << 1) >= loop + length) {
            const uint32_t repeat = (cnt >> 27) & 3u;
            if (repeat & 1u) pos = static_cast<int32_t>(loop >> 1);
            else if (repeat & 2u) { sample = 0; cnt &= ~0x80000000u; return; }
        }
        sample = fifo_read_value<int16_t>();
    }

    void next_adpcm() {
        ++pos;
        if (pos < 8) {
            if (pos == 0) {
                const uint32_t header = fifo_read_value<uint32_t>();
                adpcm_value = static_cast<int16_t>(header & 0xFFFFu);
                adpcm_index = (header >> 16) & 0x7Fu;
                if (adpcm_index > 88) adpcm_index = 88;
                adpcm_loop_value = adpcm_value;
                adpcm_loop_index = adpcm_index;
            }
            return;
        }
        if ((static_cast<uint32_t>(pos) >> 1) >= loop + length) {
            const uint32_t repeat = (cnt >> 27) & 3u;
            if (repeat & 1u) {
                pos = static_cast<int32_t>(loop << 1);
                adpcm_value = adpcm_loop_value;
                adpcm_index = adpcm_loop_index;
                adpcm_byte = fifo_read_value<uint8_t>();
            } else if (repeat & 2u) {
                sample = 0; cnt &= ~0x80000000u; return;
            }
        } else {
            if (!(pos & 1)) adpcm_byte = fifo_read_value<uint8_t>();
            else adpcm_byte >>= 4;
            const uint16_t step = kAdpcmStep[adpcm_index];
            uint16_t diff = step >> 3;
            if (adpcm_byte & 1u) diff += step >> 2;
            if (adpcm_byte & 2u) diff += step >> 1;
            if (adpcm_byte & 4u) diff += step;
            if (adpcm_byte & 8u)
                adpcm_value = std::max(-0x7FFF, adpcm_value - static_cast<int32_t>(diff));
            else
                adpcm_value = std::min(0x7FFF, adpcm_value + static_cast<int32_t>(diff));
            adpcm_index += kAdpcmIndex[adpcm_byte & 7u];
            adpcm_index = std::clamp(adpcm_index, 0, 88);
            if (pos == static_cast<int32_t>(loop << 1)) {
                adpcm_loop_value = adpcm_value;
                adpcm_loop_index = adpcm_index;
            }
        }
        sample = static_cast<int16_t>(adpcm_value);
    }

    void next_psg() { ++pos; sample = kPsg[(cnt >> 24) & 7u][pos & 7]; }
    void next_noise() {
        if (noise & 1u) { noise = (noise >> 1) ^ 0x6000u; sample = -0x7FFF; }
        else { noise >>= 1; sample = 0x7FFF; }
    }

    int32_t run_type(uint32_t type) {
        if (!(cnt & 0x80000000u)) return 0;
        if (type < 3 && length + loop < 16) return 0;
        if (key_on) { start(); key_on = false; }
        timer += 512;
        while (timer >> 16) {
            timer = reload + (timer - 0x10000u);
            switch (type) {
                case 0: next_pcm8(); break;
                case 1: next_pcm16(); break;
                case 2: next_adpcm(); break;
                case 3: next_psg(); break;
                case 4: next_noise(); break;
            }
        }
        int32_t value = static_cast<int32_t>(sample) << volume_shift;
        return value * volume;
    }

    int32_t run() {
        const uint32_t format = (cnt >> 29) & 3u;
        if (format < 3) return run_type(format);
        if (num >= 14) return run_type(4);
        if (num >= 8) return run_type(3);
        return 0;
    }

    void pan_output(int32_t input, int32_t& left, int32_t& right) const {
        left += static_cast<int32_t>((static_cast<int64_t>(input) * (128 - pan)) >> 10);
        right += static_cast<int32_t>((static_cast<int64_t>(input) * pan) >> 10);
    }
};

struct Capture {
    uint8_t cnt = 0;
    uint32_t dst = 0;
    uint16_t reload = 0;
    uint32_t length = 0;
    uint32_t timer = 0;
    int32_t pos = 0;
    std::array<uint8_t, 16> fifo{};
    uint32_t fifo_read = 0;
    uint32_t fifo_write = 0;
    uint32_t write_offset = 0;
    uint32_t fifo_level = 0;

    void start() {
        timer = reload; pos = 0; fifo.fill(0);
        fifo_read = fifo_write = write_offset = fifo_level = 0;
    }
    void set_cnt(uint8_t value) {
        if ((value & 0x80u) && !(cnt & 0x80u)) start();
        value &= 0x8Fu;
        if (!(value & 0x80u)) value &= ~1u;
        cnt = value;
    }
    void set_length(uint32_t value) { length = value << 2; if (!length) length = 4; }
    void flush() {
        for (uint32_t i = 0; i < 4; ++i) {
            uint32_t value = 0;
            std::memcpy(&value, &fifo[fifo_read], 4);
            bus_device_write32(7, dst + write_offset, value);
            fifo_read = (fifo_read + 4) & 0xFu;
            fifo_level -= 4;
            write_offset += 4;
            if (write_offset >= length) { write_offset = 0; break; }
        }
    }
    template<typename T> void write_value(T value) {
        std::memcpy(&fifo[fifo_write], &value, sizeof(T));
        fifo_write = (fifo_write + sizeof(T)) & 0xFu;
        fifo_level += sizeof(T);
        if (fifo_level >= 16) flush();
    }
    void run(int32_t sample_value) {
        timer += 512;
        if (cnt & 8u) {
            while (timer >> 16) {
                timer = reload + (timer - 0x10000u);
                write_value<int8_t>(static_cast<int8_t>(sample_value >> 8));
                ++pos;
                if (static_cast<uint32_t>(pos) >= length) {
                    if (fifo_level >= 4) flush();
                    if (cnt & 4u) { cnt &= 0x7Fu; return; }
                    pos = 0;
                }
            }
        } else {
            while (timer >> 16) {
                timer = reload + (timer - 0x10000u);
                write_value<int16_t>(static_cast<int16_t>(sample_value));
                pos += 2;
                if (static_cast<uint32_t>(pos) >= length) {
                    if (fifo_level >= 4) flush();
                    if (cnt & 4u) { cnt &= 0x7Fu; return; }
                    pos = 0;
                }
            }
        }
    }
};

std::array<Channel, 16> g_channel{};
std::array<Capture, 2> g_capture{};
uint16_t g_cnt = 0;
uint8_t g_master_volume = 0;
uint16_t g_bias = 0;
uint64_t g_mix_count = 0;

constexpr uint32_t kOutputFrames = 8192;
std::array<int16_t, kOutputFrames * 2> g_output{};
uint32_t g_output_read = 0;
uint32_t g_output_write = 0;
uint32_t g_output_count = 0;

void output_push(int16_t left, int16_t right) {
    g_output[g_output_write * 2] = left;
    g_output[g_output_write * 2 + 1] = right;
    g_output_write = (g_output_write + 1) % kOutputFrames;
    if (g_output_count == kOutputFrames) g_output_read = (g_output_read + 1) % kOutputFrames;
    else ++g_output_count;
}

void mix() {
    int32_t left = 0, right = 0;
    int32_t left_out = 0, right_out = 0;
    if (g_cnt & 0x8000u) {
        const int32_t ch0 = g_channel[0].run();
        const int32_t ch1 = g_channel[1].run();
        const int32_t ch2 = g_channel[2].run();
        const int32_t ch3 = g_channel[3].run();
        g_channel[0].pan_output(ch0, left, right);
        g_channel[2].pan_output(ch2, left, right);
        if (!(g_cnt & 0x1000u)) g_channel[1].pan_output(ch1, left, right);
        if (!(g_cnt & 0x2000u)) g_channel[3].pan_output(ch3, left, right);
        for (uint32_t i = 4; i < 16; ++i) {
            const int32_t sample = g_channel[i].run();
            g_channel[i].pan_output(sample, left, right);
        }
        if (g_capture[0].cnt & 0x80u)
            g_capture[0].run(std::clamp(left >> 8, -0x8000, 0x7FFF));
        if (g_capture[1].cnt & 0x80u)
            g_capture[1].run(std::clamp(right >> 8, -0x8000, 0x7FFF));

        switch (g_cnt & 0x0300u) {
            case 0x0000: left_out = left; break;
            case 0x0100: left_out = static_cast<int32_t>((static_cast<int64_t>(ch1) * (128-g_channel[1].pan)) >> 10); break;
            case 0x0200: left_out = static_cast<int32_t>((static_cast<int64_t>(ch3) * (128-g_channel[3].pan)) >> 10); break;
            default: left_out = static_cast<int32_t>((static_cast<int64_t>(ch1) * (128-g_channel[1].pan)) >> 10) + static_cast<int32_t>((static_cast<int64_t>(ch3) * (128-g_channel[3].pan)) >> 10); break;
        }
        switch (g_cnt & 0x0C00u) {
            case 0x0000: right_out = right; break;
            case 0x0400: right_out = static_cast<int32_t>((static_cast<int64_t>(ch1) * g_channel[1].pan) >> 10); break;
            case 0x0800: right_out = static_cast<int32_t>((static_cast<int64_t>(ch3) * g_channel[3].pan) >> 10); break;
            default: right_out = static_cast<int32_t>((static_cast<int64_t>(ch1) * g_channel[1].pan) >> 10) + static_cast<int32_t>((static_cast<int64_t>(ch3) * g_channel[3].pan) >> 10); break;
        }
    }
    left_out = static_cast<int32_t>((static_cast<int64_t>(left_out) * g_master_volume) >> 7);
    right_out = static_cast<int32_t>((static_cast<int64_t>(right_out) * g_master_volume) >> 7);
    left_out = (left_out >> 8) + (static_cast<int32_t>(g_bias) << 6) - 0x8000;
    right_out = (right_out >> 8) + (static_cast<int32_t>(g_bias) << 6) - 0x8000;
    left_out = std::clamp(left_out, -0x8000, 0x7FFF) & ~0x3F;
    right_out = std::clamp(right_out, -0x8000, 0x7FFF) & ~0x3F;
    output_push(static_cast<int16_t>(left_out >> 1), static_cast<int16_t>(right_out >> 1));
}

uint32_t read8(uint32_t addr) {
    if (addr < 0x04000500u) {
        const Channel& ch = g_channel[(addr >> 4) & 0xFu];
        if ((addr & 0xFu) < 4u) return (ch.cnt >> ((addr & 3u) * 8u)) & 0xFFu;
    } else {
        if (addr == 0x04000500u) return g_cnt & 0x7Fu;
        if (addr == 0x04000501u) return g_cnt >> 8;
        if (addr == 0x04000508u) return g_capture[0].cnt;
        if (addr == 0x04000509u) return g_capture[1].cnt;
    }
    return 0;
}
uint32_t read16(uint32_t addr) {
    if (addr < 0x04000500u) {
        const Channel& ch = g_channel[(addr >> 4) & 0xFu];
        if ((addr & 0xFu) == 0) return ch.cnt & 0xFFFFu;
        if ((addr & 0xFu) == 2) return ch.cnt >> 16;
    } else {
        if (addr == 0x04000500u) return g_cnt;
        if (addr == 0x04000504u) return g_bias;
        if (addr == 0x04000508u) return g_capture[0].cnt | (g_capture[1].cnt << 8);
    }
    return 0;
}
uint32_t read32(uint32_t addr) {
    if (addr < 0x04000500u) {
        const Channel& ch = g_channel[(addr >> 4) & 0xFu];
        if ((addr & 0xFu) == 0) return ch.cnt;
    } else {
        if (addr == 0x04000500u) return g_cnt;
        if (addr == 0x04000504u) return g_bias;
        if (addr == 0x04000508u) return g_capture[0].cnt | (g_capture[1].cnt << 8);
        if (addr == 0x04000510u) return g_capture[0].dst;
        if (addr == 0x04000518u) return g_capture[1].dst;
    }
    return 0;
}

void set_global(uint32_t value) {
    g_cnt = value & 0xBF7Fu;
    g_master_volume = g_cnt & 0x7Fu;
    if (g_master_volume == 127) ++g_master_volume;
}

void write8(uint32_t addr, uint8_t value) {
    if (addr < 0x04000500u) {
        Channel& ch = g_channel[(addr >> 4) & 0xFu];
        const uint32_t shift = (addr & 3u) * 8u;
        if ((addr & 0xFu) < 4u) ch.set_cnt((ch.cnt & ~(0xFFu << shift)) | (uint32_t{value} << shift));
        return;
    }
    if (addr == 0x04000500u) { set_global((g_cnt & 0xBF00u) | (value & 0x7Fu)); return; }
    if (addr == 0x04000501u) { set_global((g_cnt & 0x007Fu) | ((value & 0xBFu) << 8)); return; }
    if (addr == 0x04000508u) { g_capture[0].set_cnt(value); return; }
    if (addr == 0x04000509u) { g_capture[1].set_cnt(value); return; }
}

void write16(uint32_t addr, uint16_t value) {
    if (addr < 0x04000500u) {
        Channel& ch = g_channel[(addr >> 4) & 0xFu];
        switch (addr & 0xFu) {
            case 0: ch.set_cnt((ch.cnt & 0xFFFF0000u) | value); return;
            case 2: ch.set_cnt((ch.cnt & 0x0000FFFFu) | (uint32_t{value} << 16)); return;
            case 8:
                ch.reload = value;
                if ((addr & 0xF0u) == 0x10u) g_capture[0].reload = value;
                else if ((addr & 0xF0u) == 0x30u) g_capture[1].reload = value;
                return;
            case 0xA: ch.loop = uint32_t{value} << 2; return;
            case 0xC: ch.length = (((ch.length >> 2) & 0xFFFF0000u) | value) << 2; ch.length &= 0x007FFFFCu; return;
            case 0xE: ch.length = (((ch.length >> 2) & 0x0000FFFFu) | (uint32_t{value} << 16)) << 2; ch.length &= 0x007FFFFCu; return;
        }
        return;
    }
    if (addr == 0x04000500u) { set_global(value); return; }
    if (addr == 0x04000504u) { g_bias = value & 0x3FFu; return; }
    if (addr == 0x04000508u) { g_capture[0].set_cnt(value); g_capture[1].set_cnt(value >> 8); return; }
    if (addr == 0x04000514u) { g_capture[0].set_length(value); return; }
    if (addr == 0x0400051Cu) { g_capture[1].set_length(value); return; }
}

void write32(uint32_t addr, uint32_t value) {
    if (addr < 0x04000500u) {
        Channel& ch = g_channel[(addr >> 4) & 0xFu];
        switch (addr & 0xFu) {
            case 0: ch.set_cnt(value); return;
            case 4: ch.src = value & 0x07FFFFFCu; return;
            case 8:
                ch.reload = value & 0xFFFFu;
                ch.loop = (value >> 16) << 2;
                if ((addr & 0xF0u) == 0x10u) g_capture[0].reload = value;
                else if ((addr & 0xF0u) == 0x30u) g_capture[1].reload = value;
                return;
            case 0xC: ch.length = (value & 0x001FFFFFu) << 2; return;
        }
        return;
    }
    if (addr == 0x04000500u) { set_global(value); return; }
    if (addr == 0x04000504u) { g_bias = value & 0x3FFu; return; }
    if (addr == 0x04000508u) { g_capture[0].set_cnt(value); g_capture[1].set_cnt(value >> 8); return; }
    if (addr == 0x04000510u) { g_capture[0].dst = value & 0x07FFFFFCu; return; }
    if (addr == 0x04000514u) { g_capture[0].set_length(value & 0xFFFFu); return; }
    if (addr == 0x04000518u) { g_capture[1].dst = value & 0x07FFFFFCu; return; }
    if (addr == 0x0400051Cu) { g_capture[1].set_length(value & 0xFFFFu); return; }
}

} // namespace

void nds_spu_reset() {
    for (uint32_t i = 0; i < g_channel.size(); ++i) {
        g_channel[i] = Channel{};
        g_channel[i].num = i;
        g_channel[i].set_cnt(0);
    }
    g_capture = {};
    g_cnt = 0; g_master_volume = 0; g_bias = 0; g_mix_count = 0;
    g_output.fill(0); g_output_read = g_output_write = g_output_count = 0;
}

void nds_spu_stop() {
    g_output.fill(0);
    g_output_read = g_output_write = g_output_count = 0;
}

uint32_t nds_spu_read(uint32_t addr, uint32_t width) {
    if (width == 1) return read8(addr);
    if (width == 2) return read16(addr);
    return read32(addr);
}

void nds_spu_write(uint32_t addr, uint32_t value, uint32_t width) {
    if (width == 1) write8(addr, static_cast<uint8_t>(value));
    else if (width == 2) write16(addr, static_cast<uint16_t>(value));
    else write32(addr, value);
}

void nds_tick_spu(uint64_t system_cycles) {
    const uint64_t due = system_cycles / 1024u;
    while (g_mix_count < due) { mix(); ++g_mix_count; }
}

uint32_t nds_spu_read_output(int16_t* stereo, uint32_t frames) {
    if (!stereo) return 0;
    const uint32_t take = std::min(frames, g_output_count);
    for (uint32_t i = 0; i < take; ++i) {
        stereo[i*2] = g_output[g_output_read*2];
        stereo[i*2+1] = g_output[g_output_read*2+1];
        g_output_read = (g_output_read + 1) % kOutputFrames;
    }
    g_output_count -= take;
    return take;
}
