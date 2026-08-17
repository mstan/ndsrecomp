// coverage_manifest.cpp — see coverage_manifest.h. beads-yjp.28.

#include "coverage_manifest.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "state.h"
#include "tier3.h"
#include "../../recompiler/support/sha1.h"

uint64_t g_coverage_write_epoch = 0u;
CoverageExecCache g_coverage_exec_cache[2];

namespace {

constexpr uint32_t kPageSize = 4096u;

// Ceiling on the page store. 8192 pages is 32 MB of guest code, far above any
// observed session (an MPH adventure capture touches a few hundred), and the
// overflow is counted and reported rather than silently truncating -- a
// manifest that quietly stopped recording would read as "fully covered".
constexpr uint64_t kMaxPages = 8192u;

struct StoredPage {
    uint32_t addr;
    uint8_t cpu;
    std::string sha1;
    std::vector<uint8_t> bytes;
    uint64_t executions;  // distinct capture events that resolved to this page
};

struct PageKey {
    uint32_t addr;
    std::array<uint8_t, 20> digest;
    bool operator==(const PageKey& other) const {
        return addr == other.addr && digest == other.digest;
    }
};

struct PageKeyHash {
    size_t operator()(const PageKey& key) const {
        // The digest is already uniformly distributed; fold its first 8 bytes
        // together with the address.
        uint64_t folded = key.addr;
        for (int i = 0; i < 8; ++i)
            folded = (folded << 8) ^ key.digest[static_cast<size_t>(i)];
        return static_cast<size_t>(folded);
    }
};

std::vector<StoredPage> g_pages;
std::unordered_map<PageKey, uint32_t, PageKeyHash> g_page_index;
// Stored versions per address, so a repeat execution can be dismissed with one
// resolve + memcmp against what we already hold instead of re-reading and
// re-hashing the page. Overlay swaps mean one address legitimately accumulates
// several versions, but only a handful.
std::unordered_multimap<uint32_t, uint32_t> g_pages_by_addr;
CoveragePageStats g_page_stats{};

std::string g_rom_sha1;
std::string g_rom_name;
std::string g_build_id;

void append_json_escaped(std::string& out, const std::string& value) {
    out.push_back('"');
    for (const char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20u) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X",
                                  static_cast<unsigned>(
                                      static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

std::string hex32(uint32_t value) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", value);
    return buf;
}

const char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void append_base64(std::string& out, const std::vector<uint8_t>& data) {
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const uint32_t triple = (uint32_t{data[i]} << 16) |
                                (uint32_t{data[i + 1]} << 8) |
                                uint32_t{data[i + 2]};
        out.push_back(kBase64[(triple >> 18) & 0x3Fu]);
        out.push_back(kBase64[(triple >> 12) & 0x3Fu]);
        out.push_back(kBase64[(triple >> 6) & 0x3Fu]);
        out.push_back(kBase64[triple & 0x3Fu]);
    }
    if (i + 1 == data.size()) {
        const uint32_t triple = uint32_t{data[i]} << 16;
        out.push_back(kBase64[(triple >> 18) & 0x3Fu]);
        out.push_back(kBase64[(triple >> 12) & 0x3Fu]);
        out += "==";
    } else if (i + 2 == data.size()) {
        const uint32_t triple =
            (uint32_t{data[i]} << 16) | (uint32_t{data[i + 1]} << 8);
        out.push_back(kBase64[(triple >> 18) & 0x3Fu]);
        out.push_back(kBase64[(triple >> 12) & 0x3Fu]);
        out.push_back(kBase64[(triple >> 6) & 0x3Fu]);
        out.push_back('=');
    }
}

const char* kind_name(uint8_t kind) {
    switch (kind) {
        case TIER3_COVERAGE_ROOT: return "root";
        case TIER3_COVERAGE_CALL: return "call";
        case TIER3_COVERAGE_INDIRECT: return "indirect";
        default: return "unknown";
    }
}

}  // namespace

void coverage_capture_exec_page(int cpu, uint32_t base, uint32_t pc) {
    const int index = cpu & 1;
    CoverageExecCache& cache = g_coverage_exec_cache[index];
    const uint32_t generation = bus_exec_page_generation(base);

    // Same page, same generation: the write that invalidated our epoch landed
    // somewhere else. Re-trust the cache without touching the contents.
    if (cache.valid && cache.base == base && cache.generation == generation) {
        cache.epoch = g_coverage_write_epoch;
        ++g_page_stats.revisits;
        return;
    }

    cache.base = base;
    cache.generation = generation;
    cache.epoch = g_coverage_write_epoch;
    cache.valid = true;

    // Only RAM-resident code is interesting. Immutable ROM-derived addresses
    // are promotable from the address alone (the ROM SHA-1 pins the bytes), so
    // storing them would bloat the manifest for nothing.
    if (!bus_addr_is_writable_ram(base)) return;

    // A page's generation is bumped by ANY write to it, and code pages sit
    // next to hot data constantly, so a changed generation is a weak signal
    // that the instructions changed. Ask the cheap question first: does any
    // version we already hold still match the live bytes? bus_live_bytes_equal
    // is one resolve plus one memcmp for an aligned page. Doing the 4 KB read
    // and SHA-1 before this check cost 48s on a 200M-cycle MPH boot -- 3.3M
    // repeat executions each re-reading and re-hashing a page we already had.
    //
    // Verify the WHOLE page, not just a window around the executing PC. A
    // 64-byte window was measured at 3.30s against this full compare's 3.41s
    // -- and it captured 66 pages where the full compare captures 68. Trading
    // two real code pages for 0.11s is not a trade worth making: the manifest
    // exists to raise static coverage, so a page it fails to carry is the
    // exact thing this feature is for.
    const auto stored_range = g_pages_by_addr.equal_range(base);
    for (auto it = stored_range.first; it != stored_range.second; ++it) {
        StoredPage& candidate = g_pages[it->second];
        if (bus_live_bytes_equal(base, candidate.bytes.data(), kPageSize)) {
            ++candidate.executions;
            ++g_page_stats.revisits;
            return;
        }
    }

    const int bus_cpu = (index == 1) ? 7 : 9;
    std::vector<uint8_t> bytes(kPageSize);
    for (uint32_t offset = 0; offset < kPageSize; ++offset)
        bytes[offset] = bus_debug_read8(bus_cpu, base + offset);

    const gba::Sha1Digest digest = gba::sha1(bytes.data(), bytes.size());
    PageKey key{base, digest.bytes};
    auto found = g_page_index.find(key);
    if (found != g_page_index.end()) {
        ++g_pages[found->second].executions;
        ++g_page_stats.revisits;
        return;
    }

    if (g_page_stats.captured >= kMaxPages) {
        ++g_page_stats.dropped;
        return;
    }

    g_page_index.emplace(key, static_cast<uint32_t>(g_pages.size()));
    g_pages_by_addr.emplace(base, static_cast<uint32_t>(g_pages.size()));
    g_pages.push_back(StoredPage{base, static_cast<uint8_t>(index),
                                 digest.hex(), std::move(bytes), 1u});
    ++g_page_stats.captured;
    g_page_stats.bytes += kPageSize;
}

void coverage_manifest_set_identity(const char* rom_sha1, const char* rom_name,
                                    const char* build_id) {
    if (rom_sha1) g_rom_sha1 = rom_sha1;
    if (rom_name) g_rom_name = rom_name;
    if (build_id) g_build_id = build_id;
}

CoveragePageStats coverage_page_stats() { return g_page_stats; }

void coverage_pages_reset() {
    g_pages.clear();
    g_page_index.clear();
    g_pages_by_addr.clear();
    g_page_stats = {};
    for (CoverageExecCache& cache : g_coverage_exec_cache) cache = {};
}

bool coverage_manifest_write(const char* path, char* error,
                             unsigned error_cap) {
    auto fail = [&](const char* message) {
        if (error && error_cap) {
            std::snprintf(error, error_cap, "%s", message);
        }
        return false;
    };
    if (!path || !path[0]) return fail("no manifest path");

    // Pull the Tier-3 address map. The cap matches the debug server's
    // tier3_coverage ceiling so a manifest is never a smaller view than a live
    // probe would have given.
    std::vector<Tier3CoverageEntry> entries(262144);
    const uint32_t entry_count =
        tier3_coverage_copy(entries.data(),
                            static_cast<uint32_t>(entries.size()));
    entries.resize(entry_count);
    const Tier3Stats stats = tier3_stats();

    std::string out;
    out.reserve(g_page_stats.bytes * 2u + entry_count * 96u + 4096u);
    out += "{\n  \"schema\": 2,\n";
    out += "  \"kind\": \"ndsrecomp-tier3-coverage\",\n";
    out += "  \"rom_sha1\": ";
    append_json_escaped(out, g_rom_sha1);
    out += ",\n  \"rom_name\": ";
    append_json_escaped(out, g_rom_name);
    out += ",\n  \"build_id\": ";
    append_json_escaped(out, g_build_id);

    out += ",\n  \"static_coverage\": {";
    out += "\"tier3_entries9\": " + std::to_string(stats.entries[0]);
    out += ", \"tier3_entries7\": " + std::to_string(stats.entries[1]);
    out += ", \"tier3_insns9\": " + std::to_string(stats.instructions[0]);
    out += ", \"tier3_insns7\": " + std::to_string(stats.instructions[1]);
    out += ", \"clean_ram_rejects9\": " +
           std::to_string(stats.clean_ram_rejects[0]);
    out += ", \"clean_ram_rejects7\": " +
           std::to_string(stats.clean_ram_rejects[1]);
    out += "}";

    // Entry points, split per CPU to match the committed coverage JSON shape
    // the promoters already read.
    for (int cpu = 0; cpu < 2; ++cpu) {
        out += ",\n  \"entry_points_";
        out += (cpu == 0) ? "arm9" : "arm7";
        out += "\": [";
        bool first = true;
        for (const Tier3CoverageEntry& entry : entries) {
            if (entry.cpu != cpu) continue;
            if (!first) out += ",";
            first = false;
            out += "\n    {\"addr\": \"" + hex32(entry.pc) + "\"";
            out += ", \"mode\": \"";
            out += entry.thumb ? "thumb" : "arm";
            out += "\", \"kind\": \"";
            out += kind_name(entry.kind);
            out += "\", \"hits\": " + std::to_string(entry.hits);
            out += ", \"caller\": \"" + hex32(entry.caller) + "\"}";
        }
        out += first ? "]" : "\n  ]";
    }

    out += ",\n  \"pages\": {";
    out += "\"captured\": " + std::to_string(g_page_stats.captured);
    out += ", \"dropped\": " + std::to_string(g_page_stats.dropped);
    out += ", \"bytes\": " + std::to_string(g_page_stats.bytes);
    out += ", \"revisits\": " + std::to_string(g_page_stats.revisits);
    out += ", \"page_size\": " + std::to_string(kPageSize);
    out += ", \"entries\": [";
    bool first_page = true;
    for (const StoredPage& page : g_pages) {
        if (!first_page) out += ",";
        first_page = false;
        out += "\n    {\"addr\": \"" + hex32(page.addr) + "\"";
        out += ", \"cpu\": ";
        out += (page.cpu == 1) ? "7" : "9";
        out += ", \"sha1\": \"" + page.sha1 + "\"";
        out += ", \"executions\": " + std::to_string(page.executions);
        out += ", \"data\": \"";
        append_base64(out, page.bytes);
        out += "\"}";
    }
    out += first_page ? "]}" : "\n  ]}";
    out += "\n}\n";

    // Write through a temporary so an interrupted dump never leaves a
    // half-written manifest that looks handable.
    const std::string temporary = std::string(path) + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) return fail("could not open the manifest for writing");
    const size_t written =
        std::fwrite(out.data(), 1, out.size(), file);
    const bool flushed = (std::fclose(file) == 0);
    if (written != out.size() || !flushed) {
        std::remove(temporary.c_str());
        return fail("could not write the manifest");
    }
    std::remove(path);
    if (std::rename(temporary.c_str(), path) != 0) {
        std::remove(temporary.c_str());
        return fail("could not finish the manifest");
    }
    return true;
}
