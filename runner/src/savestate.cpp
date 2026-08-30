#include "savestate.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include "state.h"
#include "scheduler.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

constexpr uint8_t kMagic[8] = {'N', 'D', 'S', 'S', 'T', 'A', 'T', 'E'};
constexpr uint32_t kFormatVersion = 1u;
constexpr uint32_t kHeaderSize = 24u;
constexpr uint32_t kDirEntrySize = 32u;
constexpr uint32_t kMaxSections = 16u;
constexpr uint32_t kSectionIden = 0x4E454449u; // IDEN
constexpr uint32_t kSectionSchd = 0x44484353u; // SCHD
constexpr uint32_t kSectionMemr = 0x524D454Du; // MEMR
constexpr uint32_t kSectionCp15 = 0x35315043u; // CP15
constexpr uint32_t kSectionRtim = 0x4D495452u; // RTIM
constexpr uint32_t kSectionIocr = 0x52434F49u; // IOCR
constexpr uint32_t kSectionVersion = 1u;
constexpr uint32_t kRequiredSections = 6u;

struct Section {
    uint32_t tag = 0;
    uint32_t version = kSectionVersion;
    std::vector<uint8_t> payload;
};

struct DirEntry {
    uint32_t tag = 0;
    uint32_t version = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t crc32 = 0;
    uint32_t flags = 0;
};

struct CoreState {
    NdsSchedulerSaveState scheduler{};
    NdsBusMemorySnapshot memory{};
    NdsCp15SaveState cp15{};
    NdsRuntimeSaveState runtime{};
    NdsIoCoreSaveState io{};
};

std::atomic_flag g_transaction_active = ATOMIC_FLAG_INIT;
std::mutex g_eligibility_mutex;
NdsSavestateEligibilityHook g_eligibility_hook = nullptr;
void* g_eligibility_context = nullptr;

struct TransactionGuard {
    bool locked = false;
    TransactionGuard() {
        locked = !g_transaction_active.test_and_set(std::memory_order_acquire);
        if (locked && !scheduler_savestate_begin()) {
            g_transaction_active.clear(std::memory_order_release);
            locked = false;
        }
    }
    ~TransactionGuard() {
        if (locked) {
            scheduler_savestate_end();
            g_transaction_active.clear(std::memory_order_release);
        }
    }
};

void set_error(std::string* error, const std::string& text) {
    if (error) *error = text;
}

bool valid_sha1(const std::string& text) {
    if (text.size() != 40u) return false;
    for (char ch : text) {
        if (!std::isxdigit(static_cast<unsigned char>(ch)) ||
            std::isupper(static_cast<unsigned char>(ch)))
            return false;
    }
    return true;
}

uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1u) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void put_u8(std::vector<uint8_t>& out, uint8_t value) {
    out.push_back(value);
}

void put_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>(value >> (i * 8u)));
}

void put_u64(std::vector<uint8_t>& out, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>(value >> (i * 8u)));
}

void put_bytes(std::vector<uint8_t>& out, const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + size);
}

void put_string(std::vector<uint8_t>& out, const std::string& text) {
    put_u32(out, static_cast<uint32_t>(text.size()));
    put_bytes(out, text.data(), text.size());
}

void put_vec8(std::vector<uint8_t>& out, const std::vector<uint8_t>& data) {
    put_u32(out, static_cast<uint32_t>(data.size()));
    put_bytes(out, data.data(), data.size());
}

void put_vec32(std::vector<uint8_t>& out, const std::vector<uint32_t>& data) {
    put_u32(out, static_cast<uint32_t>(data.size()));
    for (uint32_t value : data) put_u32(out, value);
}

bool read_u8(const std::vector<uint8_t>& in, size_t& pos, uint8_t* value) {
    if (pos >= in.size()) return false;
    *value = in[pos++];
    return true;
}

bool read_u32(const std::vector<uint8_t>& in, size_t& pos, uint32_t* value) {
    if (in.size() - pos < 4u) return false;
    *value = uint32_t{in[pos]} |
        (uint32_t{in[pos + 1u]} << 8u) |
        (uint32_t{in[pos + 2u]} << 16u) |
        (uint32_t{in[pos + 3u]} << 24u);
    pos += 4u;
    return true;
}

bool read_u64(const std::vector<uint8_t>& in, size_t& pos, uint64_t* value) {
    if (in.size() - pos < 8u) return false;
    uint64_t out = 0;
    for (unsigned i = 0; i < 8; ++i)
        out |= uint64_t{in[pos + i]} << (i * 8u);
    pos += 8u;
    *value = out;
    return true;
}

bool read_string(const std::vector<uint8_t>& in, size_t& pos,
                 std::string* out) {
    uint32_t size = 0;
    if (!read_u32(in, pos, &size) || in.size() - pos < size) return false;
    out->assign(reinterpret_cast<const char*>(in.data() + pos), size);
    pos += size;
    return true;
}

bool read_vec8(const std::vector<uint8_t>& in, size_t& pos,
               std::vector<uint8_t>* out) {
    uint32_t size = 0;
    if (!read_u32(in, pos, &size) || in.size() - pos < size) return false;
    out->assign(in.begin() + static_cast<std::ptrdiff_t>(pos),
                in.begin() + static_cast<std::ptrdiff_t>(pos + size));
    pos += size;
    return true;
}

bool read_vec32(const std::vector<uint8_t>& in, size_t& pos,
                std::vector<uint32_t>* out) {
    uint32_t size = 0;
    if (!read_u32(in, pos, &size)) return false;
    if (size > (in.size() - pos) / 4u) return false;
    out->clear();
    out->reserve(size);
    for (uint32_t i = 0; i < size; ++i) {
        uint32_t value = 0;
        if (!read_u32(in, pos, &value)) return false;
        out->push_back(value);
    }
    return true;
}

void put_cpu_state(std::vector<uint8_t>& out, const ArmCpuState& cpu) {
    for (uint32_t value : cpu.R) put_u32(out, value);
    put_u32(out, cpu.cpsr);
    for (uint32_t value : cpu.banked_sp) put_u32(out, value);
    for (uint32_t value : cpu.banked_lr) put_u32(out, value);
    for (uint32_t value : cpu.banked_spsr) put_u32(out, value);
    for (uint32_t value : cpu.r8_12_user) put_u32(out, value);
    for (uint32_t value : cpu.r8_12_fiq) put_u32(out, value);
}

bool read_cpu_state(const std::vector<uint8_t>& in, size_t& pos,
                    ArmCpuState* cpu) {
    *cpu = ArmCpuState{};
    for (uint32_t& value : cpu->R)
        if (!read_u32(in, pos, &value)) return false;
    if (!read_u32(in, pos, &cpu->cpsr)) return false;
    for (uint32_t& value : cpu->banked_sp)
        if (!read_u32(in, pos, &value)) return false;
    for (uint32_t& value : cpu->banked_lr)
        if (!read_u32(in, pos, &value)) return false;
    for (uint32_t& value : cpu->banked_spsr)
        if (!read_u32(in, pos, &value)) return false;
    for (uint32_t& value : cpu->r8_12_user)
        if (!read_u32(in, pos, &value)) return false;
    for (uint32_t& value : cpu->r8_12_fiq)
        if (!read_u32(in, pos, &value)) return false;
    return true;
}

std::vector<uint8_t> encode_identity(const NdsSavestateIdentity& identity) {
    std::vector<uint8_t> out;
    put_string(out, identity.build_id);
    put_string(out, identity.rom_sha1);
    return out;
}

bool decode_identity(const std::vector<uint8_t>& payload,
                     NdsSavestateIdentity* identity) {
    size_t pos = 0;
    return read_string(payload, pos, &identity->build_id) &&
        read_string(payload, pos, &identity->rom_sha1) &&
        pos == payload.size();
}

std::vector<uint8_t> encode_scheduler(const NdsSchedulerSaveState& state) {
    std::vector<uint8_t> out;
    for (int cpu = 0; cpu < 2; ++cpu) {
        put_cpu_state(out, state.cpu[cpu]);
        put_u32(out, state.crs_depth[cpu]);
        put_u32(out, state.deferred_cycles[cpu]);
        put_u64(out, state.cycles[cpu]);
        put_u8(out, state.started[cpu]);
        put_u8(out, state.terminal_halted[cpu]);
        for (uint32_t value : state.crs[cpu]) put_u32(out, value);
    }
    put_u64(out, state.system_timestamp);
    return out;
}

bool decode_scheduler(const std::vector<uint8_t>& payload,
                      NdsSchedulerSaveState* state) {
    size_t pos = 0;
    *state = NdsSchedulerSaveState{};
    for (int cpu = 0; cpu < 2; ++cpu) {
        if (!read_cpu_state(payload, pos, &state->cpu[cpu]) ||
            !read_u32(payload, pos, &state->crs_depth[cpu]) ||
            !read_u32(payload, pos, &state->deferred_cycles[cpu]) ||
            !read_u64(payload, pos, &state->cycles[cpu]) ||
            !read_u8(payload, pos, &state->started[cpu]) ||
            !read_u8(payload, pos, &state->terminal_halted[cpu]))
            return false;
        for (uint32_t& value : state->crs[cpu])
            if (!read_u32(payload, pos, &value)) return false;
    }
    return read_u64(payload, pos, &state->system_timestamp) &&
        pos == payload.size();
}

std::vector<uint8_t> encode_memory(const NdsBusMemorySnapshot& mem) {
    std::vector<uint8_t> out;
    put_vec8(out, mem.main_ram);
    put_vec8(out, mem.itcm);
    put_vec8(out, mem.dtcm);
    put_vec8(out, mem.shared_wram);
    put_vec8(out, mem.arm7_wram);
    put_vec8(out, mem.arm9_bios);
    put_vec8(out, mem.arm7_bios);
    put_vec8(out, mem.main_ram_written);
    put_vec8(out, mem.itcm_written);
    put_vec8(out, mem.dtcm_written);
    put_vec8(out, mem.shared_wram_written);
    put_vec8(out, mem.arm7_wram_written);
    put_vec32(out, mem.main_ram_generation);
    put_vec32(out, mem.itcm_generation);
    put_vec32(out, mem.dtcm_generation);
    put_vec32(out, mem.shared_wram_generation);
    put_vec32(out, mem.arm7_wram_generation);
    return out;
}

bool decode_memory(const std::vector<uint8_t>& payload,
                   NdsBusMemorySnapshot* mem) {
    size_t pos = 0;
    return read_vec8(payload, pos, &mem->main_ram) &&
        read_vec8(payload, pos, &mem->itcm) &&
        read_vec8(payload, pos, &mem->dtcm) &&
        read_vec8(payload, pos, &mem->shared_wram) &&
        read_vec8(payload, pos, &mem->arm7_wram) &&
        read_vec8(payload, pos, &mem->arm9_bios) &&
        read_vec8(payload, pos, &mem->arm7_bios) &&
        read_vec8(payload, pos, &mem->main_ram_written) &&
        read_vec8(payload, pos, &mem->itcm_written) &&
        read_vec8(payload, pos, &mem->dtcm_written) &&
        read_vec8(payload, pos, &mem->shared_wram_written) &&
        read_vec8(payload, pos, &mem->arm7_wram_written) &&
        read_vec32(payload, pos, &mem->main_ram_generation) &&
        read_vec32(payload, pos, &mem->itcm_generation) &&
        read_vec32(payload, pos, &mem->dtcm_generation) &&
        read_vec32(payload, pos, &mem->shared_wram_generation) &&
        read_vec32(payload, pos, &mem->arm7_wram_generation) &&
        pos == payload.size();
}

std::vector<uint8_t> encode_cp15(const NdsCp15SaveState& state) {
    std::vector<uint8_t> out;
    put_u32(out, state.visible.control);
    put_u8(out, state.visible.high_vectors);
    put_u8(out, state.visible.itcm_enable);
    put_u8(out, state.visible.dtcm_enable);
    put_u8(out, 0);
    put_u32(out, state.visible.itcm_size);
    put_u32(out, state.visible.dtcm_base);
    put_u32(out, state.visible.dtcm_size);
    put_u32(out, state.timing_generation);
    for (uint32_t value : state.mpu_region) put_u32(out, value);
    for (uint32_t value : state.cache_cfg) put_u32(out, value);
    for (uint32_t value : state.access_perm) put_u32(out, value);
    return out;
}

bool decode_cp15(const std::vector<uint8_t>& payload,
                 NdsCp15SaveState* state) {
    size_t pos = 0;
    uint8_t high = 0, itcm = 0, dtcm = 0, reserved = 0;
    *state = NdsCp15SaveState{};
    if (!read_u32(payload, pos, &state->visible.control) ||
        !read_u8(payload, pos, &high) ||
        !read_u8(payload, pos, &itcm) ||
        !read_u8(payload, pos, &dtcm) ||
        !read_u8(payload, pos, &reserved) ||
        !read_u32(payload, pos, &state->visible.itcm_size) ||
        !read_u32(payload, pos, &state->visible.dtcm_base) ||
        !read_u32(payload, pos, &state->visible.dtcm_size) ||
        !read_u32(payload, pos, &state->timing_generation))
        return false;
    state->visible.high_vectors = high != 0;
    state->visible.itcm_enable = itcm != 0;
    state->visible.dtcm_enable = dtcm != 0;
    for (uint32_t& value : state->mpu_region)
        if (!read_u32(payload, pos, &value)) return false;
    for (uint32_t& value : state->cache_cfg)
        if (!read_u32(payload, pos, &value)) return false;
    for (uint32_t& value : state->access_perm)
        if (!read_u32(payload, pos, &value)) return false;
    return pos == payload.size() && reserved == 0 && high <= 1u &&
        itcm <= 1u && dtcm <= 1u;
}

std::vector<uint8_t> encode_runtime_state(const NdsRuntimeSaveState& state) {
    std::vector<uint8_t> out;
    put_u64(out, state.insn_count[0]);
    put_u64(out, state.insn_count[1]);
    put_u64(out, state.force_tier3_misses);
    put_u32(out, state.active_cpu);
    put_u32(out, state.force_tier3);
    return out;
}

bool decode_runtime_state(const std::vector<uint8_t>& payload,
                          NdsRuntimeSaveState* state) {
    size_t pos = 0;
    return read_u64(payload, pos, &state->insn_count[0]) &&
        read_u64(payload, pos, &state->insn_count[1]) &&
        read_u64(payload, pos, &state->force_tier3_misses) &&
        read_u32(payload, pos, &state->active_cpu) &&
        read_u32(payload, pos, &state->force_tier3) &&
        pos == payload.size();
}

std::vector<uint8_t> encode_io_core(const NdsIoCoreSaveState& state) {
    std::vector<uint8_t> out;
    auto flags2 = [&](const uint8_t values[2]) {
        put_u8(out, values[0]); put_u8(out, values[1]);
    };
    auto u16s2 = [&](const uint16_t values[2]) {
        put_u32(out, values[0]); put_u32(out, values[1]);
    };
    auto u32s2 = [&](const uint32_t values[2]) {
        put_u32(out, values[0]); put_u32(out, values[1]);
    };
    auto u64s2 = [&](const uint64_t values[2]) {
        put_u64(out, values[0]); put_u64(out, values[1]);
    };

    u16s2(state.ipcsync_out); flags2(state.postflg);
    u16s2(state.dispstat); put_u32(out, state.vcount);
    put_u32(out, state.next_vcount); put_u8(out, state.next_vcount_valid);
    flags2(state.vcount_match); put_u8(out, state.in_vblank);
    put_u64(out, state.display_last);
    u32s2(state.ime); u32s2(state.ie); u32s2(state.irq_flags);
    flags2(state.haltcnt); flags2(state.cpu_halted);
    u64s2(state.halt_entry_cycle);
    for (const auto& cpu : state.fifo)
        for (uint32_t value : cpu) put_u32(out, value);
    flags2(state.fifo_count); flags2(state.fifo_head);
    u16s2(state.fifocnt); u32s2(state.fifo_lastrx);
    for (const auto& cpu : state.dma) for (const auto& dma : cpu) {
        put_u32(out, dma.src); put_u32(out, dma.dst); put_u32(out, dma.cnt);
        put_u32(out, dma.cur_src); put_u32(out, dma.cur_dst);
        put_u32(out, dma.remaining);
        put_u32(out, static_cast<uint32_t>(dma.src_inc));
        put_u32(out, static_cast<uint32_t>(dma.dst_inc));
        put_u32(out, dma.burst_index); put_u8(out, dma.start_mode);
        put_u8(out, dma.running); put_u8(out, dma.in_progress);
        put_u8(out, dma.burst_start);
    }
    u64s2(state.dma_entry_cycle); put_u8(out, state.gxfifo_stall);
    for (const auto& cpu : state.timer) for (const auto& timer : cpu) {
        put_u32(out, timer.reload); put_u32(out, timer.counter);
        put_u32(out, timer.ctrl); put_u64(out, timer.accum);
    }
    u64s2(state.timer_last);
    put_u32(out, state.divcnt); u32s2(state.div_numer);
    u32s2(state.div_denom); u32s2(state.div_quot); u32s2(state.div_rem);
    put_u64(out, state.div_deadline); put_u32(out, state.sqrtcnt);
    u32s2(state.sqrt_value); put_u32(out, state.sqrt_result);
    put_u64(out, state.sqrt_deadline);
    u16s2(state.exmemcnt); put_u32(out, state.powercontrol7);
    put_u32(out, state.keyinput); u16s2(state.keycnt);
    put_u32(out, state.rcnt); put_u8(out, state.wramcnt);
    put_u32(out, state.wifiwaitcnt); put_u32(out, state.biosprot);
    put_u8(out, state.pm_index); put_bytes(out, state.pm_regs, 8u);
    put_bytes(out, state.pm_masks, 8u); put_u8(out, state.pm_hold);
    put_u8(out, state.powered_off); put_u8(out, state.tsc_ctrl);
    put_u32(out, state.tsc_conv);
    put_u32(out, static_cast<uint32_t>(state.tsc_datapos));
    put_u32(out, state.tsc_x); put_u32(out, state.tsc_y);
    put_bytes(out, state.io_mem, sizeof(state.io_mem));
    return out;
}

bool decode_io_core(const std::vector<uint8_t>& payload,
                    NdsIoCoreSaveState* state) {
    size_t pos = 0;
    *state = NdsIoCoreSaveState{};
    auto flags2 = [&](uint8_t values[2]) {
        return read_u8(payload, pos, &values[0]) &&
            read_u8(payload, pos, &values[1]);
    };
    auto u16s2 = [&](uint16_t values[2]) {
        uint32_t a = 0, b = 0;
        if (!read_u32(payload, pos, &a) || !read_u32(payload, pos, &b) ||
            a > UINT16_MAX || b > UINT16_MAX) return false;
        values[0] = static_cast<uint16_t>(a);
        values[1] = static_cast<uint16_t>(b);
        return true;
    };
    auto u32s2 = [&](uint32_t values[2]) {
        return read_u32(payload, pos, &values[0]) &&
            read_u32(payload, pos, &values[1]);
    };
    auto u64s2 = [&](uint64_t values[2]) {
        return read_u64(payload, pos, &values[0]) &&
            read_u64(payload, pos, &values[1]);
    };
    auto u16 = [&](uint16_t* value) {
        uint32_t wide = 0;
        if (!read_u32(payload, pos, &wide) || wide > UINT16_MAX) return false;
        *value = static_cast<uint16_t>(wide);
        return true;
    };
    auto bytes = [&](void* dst, size_t size) {
        if (payload.size() - pos < size) return false;
        std::memcpy(dst, payload.data() + pos, size);
        pos += size;
        return true;
    };

    if (!u16s2(state->ipcsync_out) || !flags2(state->postflg) ||
        !u16s2(state->dispstat) || !u16(&state->vcount) ||
        !u16(&state->next_vcount) ||
        !read_u8(payload, pos, &state->next_vcount_valid) ||
        !flags2(state->vcount_match) ||
        !read_u8(payload, pos, &state->in_vblank) ||
        !read_u64(payload, pos, &state->display_last) ||
        !u32s2(state->ime) || !u32s2(state->ie) ||
        !u32s2(state->irq_flags) || !flags2(state->haltcnt) ||
        !flags2(state->cpu_halted) || !u64s2(state->halt_entry_cycle))
        return false;
    for (auto& cpu : state->fifo)
        for (uint32_t& value : cpu)
            if (!read_u32(payload, pos, &value)) return false;
    if (!flags2(state->fifo_count) || !flags2(state->fifo_head) ||
        !u16s2(state->fifocnt) || !u32s2(state->fifo_lastrx)) return false;
    for (auto& cpu : state->dma) for (auto& dma : cpu) {
        uint32_t src_inc = 0, dst_inc = 0;
        if (!read_u32(payload, pos, &dma.src) ||
            !read_u32(payload, pos, &dma.dst) ||
            !read_u32(payload, pos, &dma.cnt) ||
            !read_u32(payload, pos, &dma.cur_src) ||
            !read_u32(payload, pos, &dma.cur_dst) ||
            !read_u32(payload, pos, &dma.remaining) ||
            !read_u32(payload, pos, &src_inc) ||
            !read_u32(payload, pos, &dst_inc) ||
            !u16(&dma.burst_index) ||
            !read_u8(payload, pos, &dma.start_mode) ||
            !read_u8(payload, pos, &dma.running) ||
            !read_u8(payload, pos, &dma.in_progress) ||
            !read_u8(payload, pos, &dma.burst_start)) return false;
        dma.src_inc = static_cast<int32_t>(src_inc);
        dma.dst_inc = static_cast<int32_t>(dst_inc);
    }
    if (!u64s2(state->dma_entry_cycle) ||
        !read_u8(payload, pos, &state->gxfifo_stall)) return false;
    for (auto& cpu : state->timer) for (auto& timer : cpu) {
        if (!u16(&timer.reload) || !u16(&timer.counter) ||
            !u16(&timer.ctrl) || !read_u64(payload, pos, &timer.accum))
            return false;
    }
    if (!u64s2(state->timer_last) || !u16(&state->divcnt) ||
        !u32s2(state->div_numer) || !u32s2(state->div_denom) ||
        !u32s2(state->div_quot) || !u32s2(state->div_rem) ||
        !read_u64(payload, pos, &state->div_deadline) ||
        !u16(&state->sqrtcnt) || !u32s2(state->sqrt_value) ||
        !read_u32(payload, pos, &state->sqrt_result) ||
        !read_u64(payload, pos, &state->sqrt_deadline) ||
        !u16s2(state->exmemcnt) || !u16(&state->powercontrol7) ||
        !read_u32(payload, pos, &state->keyinput) ||
        !u16s2(state->keycnt) || !u16(&state->rcnt) ||
        !read_u8(payload, pos, &state->wramcnt) ||
        !u16(&state->wifiwaitcnt) ||
        !read_u32(payload, pos, &state->biosprot) ||
        !read_u8(payload, pos, &state->pm_index) ||
        !bytes(state->pm_regs, sizeof(state->pm_regs)) ||
        !bytes(state->pm_masks, sizeof(state->pm_masks)) ||
        !read_u8(payload, pos, &state->pm_hold) ||
        !read_u8(payload, pos, &state->powered_off) ||
        !read_u8(payload, pos, &state->tsc_ctrl) ||
        !u16(&state->tsc_conv)) return false;
    uint32_t datapos = 0;
    if (!read_u32(payload, pos, &datapos) || !u16(&state->tsc_x) ||
        !u16(&state->tsc_y) ||
        !bytes(state->io_mem, sizeof(state->io_mem))) return false;
    state->tsc_datapos = static_cast<int32_t>(datapos);
    return pos == payload.size();
}

bool append_section(std::vector<Section>& sections, uint32_t tag,
                    std::vector<uint8_t> payload, std::string* error) {
    if (payload.empty()) {
        set_error(error, "refusing to write empty savestate section");
        return false;
    }
    if (std::any_of(sections.begin(), sections.end(),
                    [tag](const Section& section) {
                        return section.tag == tag;
                    })) {
        set_error(error, "duplicate savestate section");
        return false;
    }
    sections.push_back({tag, kSectionVersion, std::move(payload)});
    return true;
}

bool known_section_tag(uint32_t tag) {
    return tag == kSectionIden || tag == kSectionSchd ||
        tag == kSectionMemr || tag == kSectionCp15 ||
        tag == kSectionRtim || tag == kSectionIocr;
}

bool atomic_write_file(const std::string& path,
                       const std::vector<uint8_t>& bytes,
                       std::string* error) {
    const std::filesystem::path dst(path);
    const std::filesystem::path tmp =
        dst.string() + ".tmp-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            set_error(error, "failed to open temporary savestate file");
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            set_error(error, "failed to write temporary savestate file");
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    auto flush_file_to_disk = [&](const std::filesystem::path& file) {
#if defined(_WIN32)
        HANDLE h = CreateFileA(file.string().c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ,
                               nullptr,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            set_error(error, "failed to reopen temporary savestate file");
            return false;
        }
        const BOOL ok = FlushFileBuffers(h);
        CloseHandle(h);
        if (!ok) {
            set_error(error, "failed to flush temporary savestate file");
            return false;
        }
        return true;
#else
        const int fd = open(file.c_str(), O_RDONLY);
        if (fd < 0) {
            set_error(error, "failed to reopen temporary savestate file");
            return false;
        }
        const bool ok = fsync(fd) == 0;
        close(fd);
        if (!ok) {
            set_error(error, "failed to flush temporary savestate file");
            return false;
        }
        return true;
#endif
    };
    if (!flush_file_to_disk(tmp)) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
#if defined(_WIN32)
    if (!MoveFileExA(tmp.string().c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        set_error(error, "failed to atomically replace savestate file");
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
#else
    std::error_code ec;
    std::filesystem::rename(tmp, dst, ec);
    if (ec) {
        set_error(error, "failed to atomically replace savestate file");
        std::filesystem::remove(tmp, ec);
        return false;
    }
    const std::filesystem::path parent =
        dst.parent_path().empty() ? std::filesystem::path(".")
                                  : dst.parent_path();
    const int dir_fd = open(parent.c_str(), O_RDONLY);
    if (dir_fd < 0) {
        set_error(error, "failed to open savestate directory for flush");
        return false;
    }
    const bool dir_ok = fsync(dir_fd) == 0;
    close(dir_fd);
    if (!dir_ok) {
        set_error(error, "failed to flush savestate directory");
        return false;
    }
#endif
    return true;
}

std::vector<uint8_t> build_file(const std::vector<Section>& sections) {
    std::vector<DirEntry> dir;
    dir.reserve(sections.size());
    uint64_t offset = kHeaderSize +
        uint64_t{sections.size()} * kDirEntrySize;
    for (const Section& section : sections) {
        dir.push_back({section.tag, section.version, offset,
                       section.payload.size(),
                       crc32(section.payload.data(), section.payload.size()),
                       0});
        offset += section.payload.size();
    }

    std::vector<uint8_t> out;
    put_bytes(out, kMagic, sizeof(kMagic));
    put_u32(out, kFormatVersion);
    put_u32(out, static_cast<uint32_t>(sections.size()));
    put_u32(out, static_cast<uint32_t>(dir.size() * kDirEntrySize));
    put_u32(out, 0);
    for (const DirEntry& entry : dir) {
        put_u32(out, entry.tag);
        put_u32(out, entry.version);
        put_u64(out, entry.offset);
        put_u64(out, entry.size);
        put_u32(out, entry.crc32);
        put_u32(out, entry.flags);
    }
    for (const Section& section : sections)
        put_bytes(out, section.payload.data(), section.payload.size());
    return out;
}

bool find_section(const std::vector<std::pair<DirEntry, std::vector<uint8_t>>>& sections,
                  uint32_t tag, std::vector<uint8_t>* payload) {
    for (const auto& section : sections) {
        if (section.first.tag == tag) {
            *payload = section.second;
            return true;
        }
    }
    return false;
}

bool load_file(const std::string& path,
               std::vector<std::pair<DirEntry, std::vector<uint8_t>>>* sections,
               std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(error, "failed to open savestate file");
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    if (bytes.size() < kHeaderSize) {
        set_error(error, "savestate file is too small");
        return false;
    }
    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
        set_error(error, "savestate magic mismatch");
        return false;
    }
    size_t pos = sizeof(kMagic);
    uint32_t format = 0, count = 0, dir_bytes = 0, reserved = 0;
    if (!read_u32(bytes, pos, &format) ||
        !read_u32(bytes, pos, &count) ||
        !read_u32(bytes, pos, &dir_bytes) ||
        !read_u32(bytes, pos, &reserved) ||
        format != kFormatVersion || reserved != 0 ||
        count != kRequiredSections || count > kMaxSections ||
        dir_bytes != count * kDirEntrySize) {
        set_error(error, "unsupported or corrupt savestate header");
        return false;
    }
    if (bytes.size() < kHeaderSize + dir_bytes) {
        set_error(error, "savestate directory extends past EOF");
        return false;
    }

    std::vector<DirEntry> dir;
    dir.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        DirEntry entry{};
        if (!read_u32(bytes, pos, &entry.tag) ||
            !read_u32(bytes, pos, &entry.version) ||
            !read_u64(bytes, pos, &entry.offset) ||
            !read_u64(bytes, pos, &entry.size) ||
            !read_u32(bytes, pos, &entry.crc32) ||
            !read_u32(bytes, pos, &entry.flags) ||
            entry.version != kSectionVersion || entry.flags != 0 ||
            entry.size == 0 || !known_section_tag(entry.tag)) {
            set_error(error, "unsupported or corrupt savestate directory");
            return false;
        }
        for (const DirEntry& prior : dir) {
            if (prior.tag == entry.tag) {
                set_error(error, "duplicate savestate section");
                return false;
            }
        }
        if (entry.offset < kHeaderSize + dir_bytes ||
            entry.size > bytes.size() ||
            entry.offset > bytes.size() - entry.size) {
            set_error(error, "savestate section extends past EOF");
            return false;
        }
        dir.push_back(entry);
    }

    std::sort(dir.begin(), dir.end(), [](const DirEntry& a,
                                         const DirEntry& b) {
        return a.offset < b.offset;
    });
    uint64_t expected_offset = kHeaderSize + dir_bytes;
    for (const DirEntry& entry : dir) {
        if (entry.offset != expected_offset) {
            set_error(error, "savestate sections are not tightly packed");
            return false;
        }
        expected_offset += entry.size;
    }
    if (expected_offset != bytes.size()) {
        set_error(error, "savestate file has trailing data");
        return false;
    }

    sections->clear();
    sections->reserve(dir.size());
    for (const DirEntry& entry : dir) {
        const size_t begin = static_cast<size_t>(entry.offset);
        const size_t end = begin + static_cast<size_t>(entry.size);
        std::vector<uint8_t> payload(bytes.begin() + begin, bytes.begin() + end);
        if (crc32(payload.data(), payload.size()) != entry.crc32) {
            set_error(error, "savestate section checksum mismatch");
            return false;
        }
        sections->push_back({entry, std::move(payload)});
    }
    return true;
}

bool export_core_state(CoreState* out, std::string* error) {
    if (!out) return false;
    if (!scheduler_savestate_export(&out->scheduler) ||
        !bus_savestate_export(&out->memory)) {
        set_error(error, "failed to export core savestate sections");
        return false;
    }
    cp15_savestate_export(&out->cp15);
    runtime_savestate_export(&out->runtime);
    return io_savestate_export(&out->io);
}

bool import_core_state(const CoreState& state, std::string* error) {
    if (!bus_savestate_import(state.memory, error) ||
        !cp15_savestate_import(state.cp15, error) ||
        !runtime_savestate_import(state.runtime, error) ||
        !io_savestate_import(state.io, error) ||
        !scheduler_savestate_import(state.scheduler, error))
        return false;
    bus_fast_refresh();
    runtime_savestate_invalidate_host_caches();
    return true;
}

bool validate_core_state(const CoreState& state, std::string* error) {
    auto fail = [&](const char* message) {
        set_error(error, message);
        return false;
    };
    const NdsBusMemorySnapshot& mem = state.memory;
    if (mem.main_ram.size() != 4u * 1024u * 1024u ||
        mem.itcm.size() != 32u * 1024u ||
        mem.dtcm.size() != 16u * 1024u ||
        mem.shared_wram.size() != 32u * 1024u ||
        mem.arm7_wram.size() != 64u * 1024u ||
        mem.arm9_bios.size() != 4u * 1024u ||
        mem.arm7_bios.size() != 16u * 1024u)
        return fail("savestate memory backing size mismatch");
    if (mem.main_ram_written.size() != mem.main_ram.size() ||
        mem.itcm_written.size() != mem.itcm.size() ||
        mem.dtcm_written.size() != mem.dtcm.size() ||
        mem.shared_wram_written.size() != mem.shared_wram.size() ||
        mem.arm7_wram_written.size() != mem.arm7_wram.size())
        return fail("savestate memory provenance size mismatch");
    constexpr size_t kExecPageSize = 4096u;
    if (mem.main_ram_generation.size() != mem.main_ram.size() / kExecPageSize ||
        mem.itcm_generation.size() != mem.itcm.size() / kExecPageSize ||
        mem.dtcm_generation.size() != mem.dtcm.size() / kExecPageSize ||
        mem.shared_wram_generation.size() !=
            mem.shared_wram.size() / kExecPageSize ||
        mem.arm7_wram_generation.size() !=
            mem.arm7_wram.size() / kExecPageSize)
        return fail("savestate memory generation size mismatch");
    if (state.runtime.active_cpu > 1u || state.runtime.force_tier3 > 1u)
        return fail("savestate runtime state is invalid");
    for (int cpu = 0; cpu < 2; ++cpu) {
        if (state.scheduler.crs_depth[cpu] >
                NDS_RUNTIME_CALL_STACK_CAPACITY ||
            state.scheduler.started[cpu] > 1u ||
            state.scheduler.terminal_halted[cpu] > 1u)
            return fail("savestate scheduler state is invalid");
    }
    return io_savestate_validate(state.io, error);
}

bool transaction_allowed(std::string* error) {
    NdsSavestateEligibilityHook hook = nullptr;
    void* context = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_eligibility_mutex);
        hook = g_eligibility_hook;
        context = g_eligibility_context;
    }
    if (hook && !hook(context, error)) {
        if (error && error->empty())
            *error = "savestate transaction rejected by eligibility policy";
        return false;
    }
    return true;
}

}  // namespace

bool nds_savestate_save_core(const std::string& path,
                             const NdsSavestateIdentity& identity,
                             std::string* error) {
    TransactionGuard transaction;
    if (!transaction.locked) {
        set_error(error, "savestate requires the scheduler owner between rounds");
        return false;
    }
    if (!transaction_allowed(error)) return false;
    if (identity.build_id.empty()) {
        set_error(error, "savestate identity requires an exact build id");
        return false;
    }
    if (!valid_sha1(identity.rom_sha1)) {
        set_error(error, "savestate identity has invalid ROM SHA-1");
        return false;
    }

    CoreState core{};
    if (!export_core_state(&core, error)) return false;

    std::vector<Section> sections;
    if (!append_section(sections, kSectionIden, encode_identity(identity), error) ||
        !append_section(sections, kSectionSchd,
                        encode_scheduler(core.scheduler), error) ||
        !append_section(sections, kSectionMemr, encode_memory(core.memory), error) ||
        !append_section(sections, kSectionCp15, encode_cp15(core.cp15), error) ||
        !append_section(sections, kSectionRtim,
                        encode_runtime_state(core.runtime), error) ||
        !append_section(sections, kSectionIocr,
                        encode_io_core(core.io), error))
        return false;

    return atomic_write_file(path, build_file(sections), error);
}

bool nds_savestate_load_core(const std::string& path,
                             const NdsSavestateIdentity& expected_identity,
                             std::string* error) {
    TransactionGuard transaction;
    if (!transaction.locked) {
        set_error(error, "savestate requires the scheduler owner between rounds");
        return false;
    }
    if (!transaction_allowed(error)) return false;
    if (expected_identity.build_id.empty() ||
        !valid_sha1(expected_identity.rom_sha1)) {
        set_error(error, "expected savestate identity is invalid");
        return false;
    }
    std::vector<std::pair<DirEntry, std::vector<uint8_t>>> sections;
    if (!load_file(path, &sections, error)) return false;

    std::vector<uint8_t> payload;
    NdsSavestateIdentity identity{};
    if (!find_section(sections, kSectionIden, &payload) ||
        !decode_identity(payload, &identity)) {
        set_error(error, "savestate identity section is missing or corrupt");
        return false;
    }
    if (identity.build_id != expected_identity.build_id) {
        set_error(error, "savestate build id mismatch");
        return false;
    }
    if (identity.rom_sha1 != expected_identity.rom_sha1) {
        set_error(error, "savestate ROM SHA-1 mismatch");
        return false;
    }

    CoreState loaded{};
    if (!find_section(sections, kSectionSchd, &payload) ||
        !decode_scheduler(payload, &loaded.scheduler)) {
        set_error(error, "scheduler section is missing or corrupt");
        return false;
    }
    if (!find_section(sections, kSectionMemr, &payload) ||
        !decode_memory(payload, &loaded.memory)) {
        set_error(error, "memory section is missing or corrupt");
        return false;
    }
    if (!find_section(sections, kSectionCp15, &payload) ||
        !decode_cp15(payload, &loaded.cp15)) {
        set_error(error, "CP15 section is missing or corrupt");
        return false;
    }
    if (!find_section(sections, kSectionRtim, &payload) ||
        !decode_runtime_state(payload, &loaded.runtime)) {
        set_error(error, "runtime section is missing or corrupt");
        return false;
    }
    if (!find_section(sections, kSectionIocr, &payload) ||
        !decode_io_core(payload, &loaded.io)) {
        set_error(error, "IO core section is missing or corrupt");
        return false;
    }
    if (!validate_core_state(loaded, error)) return false;

    CoreState previous{};
    if (!export_core_state(&previous, error)) return false;
    if (!import_core_state(loaded, error)) {
        const std::string primary = error ? *error : std::string{};
        std::string rollback_error;
        if (!import_core_state(previous, &rollback_error) && error) {
            *error = primary + "; rollback failed: " + rollback_error;
        } else if (error) {
            *error = primary;
        }
        return false;
    }
    return true;
}

void nds_savestate_set_eligibility_hook(NdsSavestateEligibilityHook hook,
                                        void* context) {
    std::lock_guard<std::mutex> lock(g_eligibility_mutex);
    g_eligibility_hook = hook;
    g_eligibility_context = hook ? context : nullptr;
}
