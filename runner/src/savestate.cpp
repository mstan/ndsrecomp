#include "savestate.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "state.h"

#if defined(_WIN32)
#include <windows.h>
#else
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
constexpr uint32_t kSectionVersion = 1u;
constexpr uint32_t kRequiredSections = 5u;

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

void set_error(std::string* error, const std::string& text) {
    if (error) *error = text;
}

bool valid_sha1_or_empty(const std::string& text) {
    if (text.empty()) return true;
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

bool read_bytes(const std::vector<uint8_t>& in, size_t& pos, void* dst,
                size_t size) {
    if (in.size() - pos < size) return false;
    std::memcpy(dst, in.data() + pos, size);
    pos += size;
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
        put_bytes(out, &state.cpu[cpu], sizeof(state.cpu[cpu]));
        put_u32(out, state.crs_depth[cpu]);
        put_u32(out, state.deferred_cycles[cpu]);
        put_u64(out, state.cycles[cpu]);
        put_u8(out, state.started[cpu]);
        put_u8(out, state.terminal_halted[cpu]);
        put_bytes(out, state.crs[cpu], sizeof(state.crs[cpu]));
    }
    put_u64(out, state.system_timestamp);
    return out;
}

bool decode_scheduler(const std::vector<uint8_t>& payload,
                      NdsSchedulerSaveState* state) {
    size_t pos = 0;
    *state = NdsSchedulerSaveState{};
    for (int cpu = 0; cpu < 2; ++cpu) {
        if (!read_bytes(payload, pos, &state->cpu[cpu], sizeof(state->cpu[cpu])) ||
            !read_u32(payload, pos, &state->crs_depth[cpu]) ||
            !read_u32(payload, pos, &state->deferred_cycles[cpu]) ||
            !read_u64(payload, pos, &state->cycles[cpu]) ||
            !read_u8(payload, pos, &state->started[cpu]) ||
            !read_u8(payload, pos, &state->terminal_halted[cpu]) ||
            !read_bytes(payload, pos, state->crs[cpu], sizeof(state->crs[cpu])))
            return false;
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
    return pos == payload.size() && reserved == 0;
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
        tag == kSectionRtim;
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

}  // namespace

bool nds_savestate_save_core(const std::string& path,
                             const NdsSavestateIdentity& identity,
                             std::string* error) {
    if (identity.build_id.empty()) {
        set_error(error, "savestate identity requires an exact build id");
        return false;
    }
    if (!valid_sha1_or_empty(identity.rom_sha1)) {
        set_error(error, "savestate identity has invalid ROM SHA-1");
        return false;
    }

    NdsSchedulerSaveState scheduler{};
    NdsBusMemorySnapshot memory{};
    NdsCp15SaveState cp15{};
    NdsRuntimeSaveState runtime{};
    if (!scheduler_savestate_export(&scheduler) ||
        !bus_savestate_export(&memory)) {
        set_error(error, "failed to export core savestate sections");
        return false;
    }
    cp15_savestate_export(&cp15);
    runtime_savestate_export(&runtime);

    std::vector<Section> sections;
    if (!append_section(sections, kSectionIden, encode_identity(identity), error) ||
        !append_section(sections, kSectionSchd, encode_scheduler(scheduler), error) ||
        !append_section(sections, kSectionMemr, encode_memory(memory), error) ||
        !append_section(sections, kSectionCp15, encode_cp15(cp15), error) ||
        !append_section(sections, kSectionRtim, encode_runtime_state(runtime), error))
        return false;

    return atomic_write_file(path, build_file(sections), error);
}

bool nds_savestate_load_core(const std::string& path,
                             const NdsSavestateIdentity& expected_identity,
                             std::string* error) {
    if (expected_identity.build_id.empty() ||
        !valid_sha1_or_empty(expected_identity.rom_sha1)) {
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

    NdsSchedulerSaveState scheduler{};
    NdsBusMemorySnapshot memory{};
    NdsCp15SaveState cp15{};
    NdsRuntimeSaveState runtime{};
    if (!find_section(sections, kSectionSchd, &payload) ||
        !decode_scheduler(payload, &scheduler)) {
        set_error(error, "scheduler section is missing or corrupt");
        return false;
    }
    if (!find_section(sections, kSectionMemr, &payload) ||
        !decode_memory(payload, &memory)) {
        set_error(error, "memory section is missing or corrupt");
        return false;
    }
    if (!find_section(sections, kSectionCp15, &payload) ||
        !decode_cp15(payload, &cp15)) {
        set_error(error, "CP15 section is missing or corrupt");
        return false;
    }
    if (!find_section(sections, kSectionRtim, &payload) ||
        !decode_runtime_state(payload, &runtime)) {
        set_error(error, "runtime section is missing or corrupt");
        return false;
    }

    if (!bus_savestate_import(memory, error) ||
        !cp15_savestate_import(cp15, error) ||
        !runtime_savestate_import(runtime, error) ||
        !scheduler_savestate_import(scheduler, error))
        return false;
    bus_fast_refresh();
    runtime_savestate_invalidate_host_caches();
    return true;
}
