// coverage_manifest.cpp — see coverage_manifest.h. beads-yjp.28.

#include "coverage_manifest.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "state.h"
#include "diagnostics.h"
#include "tier3.h"
#include "../../recompiler/support/sha1.h"

uint64_t g_coverage_write_epoch = 0u;
CoverageExecCache g_coverage_exec_cache[2];

constexpr uint32_t kPageSize = 4096u;

// Roots -- the PCs where native code fell into the interpreter -- are dense,
// not sparse. The interpreter re-enters at whatever instruction an IRQ or a DMA
// stall interrupted, so over a session they converge on the set of ALL
// interpreted instructions: on the first real MPH submission 92% of root
// addresses had another root exactly one instruction away and the longest
// unbroken run was 415. Storing one ~96-byte JSON record per address made 94%
// of the manifest's entries roots. A bitmap over the page stores the same
// address set for a fixed 384 bytes.
//
// Two bitmaps, because mode is load-bearing: tools/seed_overlay_from_coverage.py
// feeds roots to the recompiler's interior-entry switch and must know whether a
// landing pad is ARM or Thumb. ARM roots are word aligned (1024 bits), Thumb
// roots halfword aligned (2048 bits).
constexpr uint32_t kRootArmBytes = 128u;    // 4096 / 4 / 8
constexpr uint32_t kRootThumbBytes = 256u;  // 4096 / 2 / 8
// Hit counts per 256-byte block. A single per-page total would have collapsed
// the interpreted-span ranking, which is the whole point of keeping roots; 16
// counters keep cost attribution at 256-byte resolution for 128 bytes.
constexpr uint32_t kRootBlocks = 16u;

// RootBits, StoredEntry and the snapshot types below sit at file scope rather
// than in the anonymous namespace: CoverageLiveSnapshot is named in the header
// and must not have fields whose types have internal linkage.
struct RootBits {
    std::array<uint8_t, kRootArmBytes> arm{};
    std::array<uint8_t, kRootThumbBytes> thumb{};
    std::array<uint64_t, kRootBlocks> hits{};

    void note(uint32_t offset, bool thumb_mode) {
        if (thumb_mode) {
            const uint32_t bit = offset >> 1u;
            thumb[bit >> 3u] |= static_cast<uint8_t>(1u << (bit & 7u));
        } else {
            const uint32_t bit = offset >> 2u;
            arm[bit >> 3u] |= static_cast<uint8_t>(1u << (bit & 7u));
        }
        ++hits[(offset * kRootBlocks) / kPageSize];
    }
    void clear() { arm = {}; thumb = {}; hits = {}; }
};

struct StoredEntry {
    uint64_t hits;
    uint32_t pc;
    uint32_t caller;
    uint8_t thumb;
    uint8_t kind;
};

// One captured code page, detached from the resident store.
struct SnapshotPage {
    uint32_t addr = 0u;
    uint8_t cpu = 0u;
    std::string sha1;
    uint64_t executions = 0u;
    std::vector<StoredEntry> entries;
    RootBits roots;
    std::vector<uint8_t> bytes;
};

// beads-yjp.59: everything a manifest is rendered from, copied out of the
// resident store in one bounded pass. Capture reads emulation-owned state and
// therefore runs on the emulation thread; rendering (base64, the root-map sort,
// the JSON build) and the file write are pure functions of this struct and run
// wherever the caller puts them.
struct CoverageLiveSnapshot {
    std::string rom_sha1;
    std::string rom_name;
    std::string build_id;
    Tier3Stats stats{};
    std::vector<Tier3CoverageEntry> entries;
    std::vector<SnapshotPage> pages;
    std::vector<std::pair<uint64_t, RootBits>> root_map;
    uint64_t dropped = 0u;
    uint64_t replaced = 0u;
    uint64_t revisits = 0u;
};

namespace {

// Ceiling on the page store. 8192 pages is 32 MB of guest code, far above any
// observed session (an MPH adventure capture touches a few hundred), and the
// overflow is counted and reported rather than silently truncating -- a
// manifest that quietly stopped recording would read as "fully covered".
constexpr uint64_t kMaxPages = 8192u;

// Distinct code images retained per page address. Genuine overlay generations
// at one address number in the low single digits; anything beyond that is a
// code page churning because data shares its 4 KiB.
constexpr uint32_t kMaxVersionsPerAddress = 8u;

// Distinct callers retained per (page, entry PC, mode, kind). The caller is
// diagnostic -- it says which site reached an entry -- while the (page, PC,
// mode, kind) tuple is the part an ingest actually needs. Keying the stored
// record on the caller as well made both the resident map and the manifest
// scale with the CALL GRAPH rather than with the code: a real MPH story
// session produced 2,185,955 records for 61,036 distinct tuples, a 36x blowup
// that made the dump 202 MB, of which 196.5 MB was the entry lists. One page
// alone held 1,012,605 records for 274 distinct PCs because 71,610 sites
// branched into it. Measured over that session the fanout is median 1, p99
// 275 and max 51,884, so a small cap loses nothing diagnostically: keep the
// first few callers verbatim, then fold every later one into a single
// kCallerMany record whose hit count stays exact. Capping at 4 keeps 4.35% of
// the records and bounds the resident map, which previously grew without
// limit for the whole session.
constexpr uint32_t kMaxCallersPerEntry = 4u;

// Caller value on the folded record. 0xFFFFFFFF is neither 2- nor 4-byte
// aligned, so it is never a real ARM or Thumb branch source, and an ingest can
// tell "many further sites, not recorded individually" from a real address.
constexpr uint32_t kCallerMany = 0xFFFFFFFFu;

struct StoredPage {
    uint32_t addr;
    uint8_t cpu;
    std::string sha1;
    std::array<uint8_t, 20> digest;
    std::vector<uint8_t> bytes;
    uint64_t executions;  // distinct capture events that resolved to this page
    uint64_t last_seen;
    std::vector<uint32_t> entry_indices;
    // Generation-bound: these roots executed while THIS image was resident, so
    // an overlay seeder can attribute them to one overlay generation.
    RootBits roots;
};

struct PageKey {
    uint32_t addr;
    uint8_t cpu;
    std::array<uint8_t, 20> digest;
    bool operator==(const PageKey& other) const {
        return addr == other.addr && cpu == other.cpu &&
               digest == other.digest;
    }
};

struct PageKeyHash {
    size_t operator()(const PageKey& key) const {
        // The digest is already uniformly distributed; fold its first 8 bytes
        // together with the address.
        uint64_t folded = (uint64_t{key.cpu} << 32u) | key.addr;
        for (int i = 0; i < 8; ++i)
            folded = (folded << 8) ^ key.digest[static_cast<size_t>(i)];
        return static_cast<size_t>(folded);
    }
};

struct PageEntryKey {
    uint32_t page_index;
    uint32_t pc;
    uint32_t caller;
    uint8_t thumb;
    uint8_t kind;
    bool operator==(const PageEntryKey& other) const {
        return page_index == other.page_index && pc == other.pc &&
               caller == other.caller && thumb == other.thumb &&
               kind == other.kind;
    }
};

struct PageEntryKeyHash {
    size_t operator()(const PageEntryKey& key) const {
        uint64_t folded = (uint64_t{key.page_index} << 32u) ^ key.pc;
        folded ^= uint64_t{key.caller} * 0x9E3779B185EBCA87ull;
        folded ^= static_cast<uint64_t>(
            static_cast<uint32_t>(key.thumb) |
            (static_cast<uint32_t>(key.kind) << 1u)) << 56u;
        return static_cast<size_t>(folded ^ (folded >> 32u));
    }
};

std::vector<StoredPage> g_pages;
std::unordered_map<PageKey, uint32_t, PageKeyHash> g_page_index;
// Stored versions per address, so a repeat execution can be dismissed with one
// resolve + memcmp against what we already hold instead of re-reading and
// re-hashing the page. Overlay swaps mean one address legitimately accumulates
// several versions, but only a handful.
std::unordered_multimap<uint64_t, uint32_t> g_pages_by_addr;
std::vector<StoredEntry> g_generation_entries;
std::unordered_map<PageEntryKey, uint32_t, PageEntryKeyHash>
    g_generation_entry_index;
// (page, PC, mode, kind) -> how many distinct callers we have kept, plus the
// folded record once the cap is reached. Separate from the map above because
// that one is keyed WITH the caller; this is what bounds it.
struct PageFanout {
    uint32_t kept;
    uint32_t folded_index;
};
std::unordered_map<PageEntryKey, PageFanout, PageEntryKeyHash>
    g_generation_entry_fanout;
// Session-wide roots, keyed by (cpu, page base) and NEVER evicted. The
// per-page-version bitmaps above are bound to a generation, which is what an
// overlay seeder needs, but they die with their page when the version cap
// evicts one -- and eviction is common: one 9000-vblank MPH scenario reported
// captured=166 with replaced=87263. Before this, the session-wide picture was
// preserved only because tier3's own map kept every root address forever at
// ~96 bytes each. This keeps the same guarantee for 512 bytes per page base.
std::unordered_map<uint64_t, RootBits> g_root_map;
CoveragePageStats g_page_stats{};
uint64_t g_observation_seq = 0u;

std::string g_rom_sha1;
std::string g_rom_name;
std::string g_build_id;

// Automatic-dump destination. Empty disables rotation (the explicit-path
// writer still works, which is what the debug command uses).
std::string g_out_base;
std::string g_run_stamp;
uint32_t g_part_index = 0u;
uint64_t g_rotations = 0u;

std::string run_stamp() {
    if (!g_run_stamp.empty()) return g_run_stamp;
    if (nds_diagnostics_enabled()) {
        g_run_stamp = nds_diagnostics_run_stamp();
        if (!g_run_stamp.empty()) return g_run_stamp;
    }
    const std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm_buf);
    g_run_stamp = buf;
    return g_run_stamp;
}

std::string next_part_path() {
    if (g_out_base.empty()) return {};
    char buf[32];
    std::snprintf(buf, sizeof(buf), "-part%02u", g_part_index);
    return g_out_base + "-coverage-" + run_stamp() + buf + ".json";
}

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

void append_base64(std::string& out, const uint8_t* data, size_t size);

template <size_t N>
void append_base64(std::string& out, const std::array<uint8_t, N>& data) {
    append_base64(out, data.data(), N);
}

void append_base64(std::string& out, const std::vector<uint8_t>& data) {
    append_base64(out, data.data(), data.size());
}

// True when a bitmap holds nothing, so an all-zero map can be omitted from the
// manifest instead of costing its base64 on every page that never ran in that
// mode -- which is most pages, since a page is normally all ARM or all Thumb.
template <size_t N>
bool any_bits(const std::array<uint8_t, N>& data) {
    for (uint8_t byte : data)
        if (byte != 0u) return true;
    return false;
}

// Does this page carry ANY interpreted-span root? The hits array is
// deliberately not consulted: it is a per-block counter that says how often,
// while the two bitmaps are what say whether an address was ever a root at
// all, and a promotable observation is an address, not a count.
bool any_roots(const RootBits& roots) {
    return any_bits(roots.arm) || any_bits(roots.thumb);
}

void append_root_bits(std::string& out, const RootBits& roots) {
    if (any_bits(roots.arm)) {
        out += ", \"root_arm\": \"";
        append_base64(out, roots.arm);
        out += "\"";
    }
    if (any_bits(roots.thumb)) {
        out += ", \"root_thumb\": \"";
        append_base64(out, roots.thumb);
        out += "\"";
    }
    uint64_t total = 0u;
    for (uint64_t block : roots.hits) total += block;
    if (total == 0u) return;
    out += ", \"root_hits\": [";
    for (uint32_t i = 0; i < kRootBlocks; ++i) {
        if (i) out += ",";
        out += std::to_string(roots.hits[i]);
    }
    out += "]";
}

void append_base64(std::string& out, const uint8_t* data, size_t size) {
    size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const uint32_t triple = (uint32_t{data[i]} << 16) |
                                (uint32_t{data[i + 1]} << 8) |
                                uint32_t{data[i + 2]};
        out.push_back(kBase64[(triple >> 18) & 0x3Fu]);
        out.push_back(kBase64[(triple >> 12) & 0x3Fu]);
        out.push_back(kBase64[(triple >> 6) & 0x3Fu]);
        out.push_back(kBase64[triple & 0x3Fu]);
    }
    if (i + 1 == size) {
        const uint32_t triple = uint32_t{data[i]} << 16;
        out.push_back(kBase64[(triple >> 18) & 0x3Fu]);
        out.push_back(kBase64[(triple >> 12) & 0x3Fu]);
        out += "==";
    } else if (i + 2 == size) {
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
    cache.stored_index = UINT32_MAX;
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
    const uint64_t address_key =
        (static_cast<uint64_t>(index) << 32u) | base;
    const auto stored_range = g_pages_by_addr.equal_range(address_key);
    for (auto it = stored_range.first; it != stored_range.second; ++it) {
        StoredPage& candidate = g_pages[it->second];
        if (bus_live_bytes_equal(base, candidate.bytes.data(), kPageSize)) {
            ++candidate.executions;
            candidate.last_seen = ++g_observation_seq;
            ++g_page_stats.revisits;
            cache.stored_index = it->second;
            return;
        }
    }

    // A page address may legitimately hold several distinct code images over a
    // session -- that is exactly what overlay generations are -- but only a
    // handful. Hundreds means the page shares its 4 KiB with churning data, and
    // storing every one of those evicts real coverage: a live multiplayer
    // session reported captured=8192 (the cap) with dropped=26092. Bound the
    // versions per address so churn cannot crowd out other pages.
    uint32_t versions = 0;
    for (auto it = stored_range.first; it != stored_range.second; ++it)
        ++versions;
    const int bus_cpu = (index == 1) ? 7 : 9;
    std::vector<uint8_t> bytes(kPageSize);
    bus_debug_copy(bus_cpu, base, bytes.data(), kPageSize);

    const gba::Sha1Digest digest = gba::sha1(bytes.data(), bytes.size());
    PageKey key{base, static_cast<uint8_t>(index), digest.bytes};
    auto found = g_page_index.find(key);
    if (found != g_page_index.end()) {
        ++g_pages[found->second].executions;
        g_pages[found->second].last_seen = ++g_observation_seq;
        ++g_page_stats.revisits;
        cache.stored_index = found->second;
        return;
    }

    if (versions >= kMaxVersionsPerAddress) {
        // A page can mix stable instructions with frequently changing data.
        // Refusing version nine forever made a later real overlay generation
        // impossible to learn. Keep a rolling recent set instead: exact
        // native validations remain content-addressed, while capture capacity
        // cannot be permanently consumed by stale data-bearing snapshots.
        uint32_t evict_index = UINT32_MAX;
        uint64_t oldest = UINT64_MAX;
        for (auto it = stored_range.first; it != stored_range.second; ++it) {
            const StoredPage& candidate = g_pages[it->second];
            if (candidate.last_seen < oldest) {
                oldest = candidate.last_seen;
                evict_index = it->second;
            }
        }
        if (evict_index == UINT32_MAX) {
            ++g_page_stats.dropped;
            return;
        }
        for (auto it = g_page_index.begin(); it != g_page_index.end();) {
            if (it->second == evict_index) it = g_page_index.erase(it);
            else ++it;
        }
        for (auto it = g_generation_entry_index.begin();
             it != g_generation_entry_index.end();) {
            if (it->first.page_index == evict_index)
                it = g_generation_entry_index.erase(it);
            else
                ++it;
        }
        // The slot is about to hold different code. Its fanout counters must
        // go too, or the new generation inherits a full caller budget and
        // records nothing.
        for (auto it = g_generation_entry_fanout.begin();
             it != g_generation_entry_fanout.end();) {
            if (it->first.page_index == evict_index)
                it = g_generation_entry_fanout.erase(it);
            else
                ++it;
        }
        StoredPage& replacement = g_pages[evict_index];
        replacement.sha1 = digest.hex();
        replacement.digest = digest.bytes;
        replacement.bytes = std::move(bytes);
        replacement.executions = 1u;
        replacement.last_seen = ++g_observation_seq;
        replacement.entry_indices.clear();
        // Generation-bound roots belong to the image being evicted, not to the
        // one replacing it. The session-wide g_root_map keeps them.
        replacement.roots.clear();
        g_page_index.emplace(key, evict_index);
        cache.stored_index = evict_index;
        ++g_page_stats.replaced;
        return;
    }

    // Store full. Rotate to the next part rather than dropping: a long session
    // used to report dropped=254865, i.e. it silently stopped learning after
    // the first 8192 pages. Only drop if there is nowhere to rotate to.
    if (g_page_stats.captured >= kMaxPages) {
        char error[256] = {};
        if (g_out_base.empty() || !coverage_manifest_flush_part(error, sizeof(error))) {
            ++g_page_stats.dropped;
            return;
        }
    }

    g_page_index.emplace(key, static_cast<uint32_t>(g_pages.size()));
    const uint32_t stored_index = static_cast<uint32_t>(g_pages.size());
    g_pages_by_addr.emplace(address_key, stored_index);
    g_pages.push_back(StoredPage{base, static_cast<uint8_t>(index),
                                 digest.hex(), digest.bytes, std::move(bytes),
                                 1u, ++g_observation_seq, {}, {}});
    cache.stored_index = stored_index;
    ++g_page_stats.captured;
    g_page_stats.bytes += kPageSize;
}

void coverage_note_generation_entry(int cpu, uint32_t pc, bool thumb,
                                    uint8_t kind, uint32_t caller) {
    const int index = cpu & 1;
    const uint32_t aligned_pc = pc & (thumb ? ~1u : ~3u);
    const uint32_t base = aligned_pc & ~0xFFFu;
    CoverageExecCache& cache = g_coverage_exec_cache[index];
    const uint32_t generation = bus_exec_page_generation(base);
    if (!cache.valid || cache.base != base ||
        cache.generation != generation ||
        cache.stored_index == UINT32_MAX) {
        coverage_capture_exec_page(index, base, aligned_pc);
    }
    if (!cache.valid || cache.base != base ||
        cache.stored_index == UINT32_MAX ||
        cache.stored_index >= g_pages.size()) {
        return;
    }

    const uint8_t mode = static_cast<uint8_t>(thumb ? 1u : 0u);

    // Roots are recorded as bits, not records. Same address set, same mode,
    // cost attribution kept per 256-byte block; see RootBits above for why the
    // per-address record shape was the wrong encoding for this data.
    if (kind == TIER3_COVERAGE_ROOT) {
        const uint32_t offset = aligned_pc - base;
        g_pages[cache.stored_index].roots.note(offset, thumb);
        g_root_map[(static_cast<uint64_t>(index) << 32u) | base]
            .note(offset, thumb);
        g_pages[cache.stored_index].last_seen = ++g_observation_seq;
        return;
    }

    const PageEntryKey key{cache.stored_index, aligned_pc, caller, mode, kind};
    const auto found = g_generation_entry_index.find(key);
    if (found != g_generation_entry_index.end()) {
        ++g_generation_entries[found->second].hits;
        g_pages[cache.stored_index].last_seen = ++g_observation_seq;
        return;
    }

    // New caller for this entry. Past the cap, fold it into the single
    // kCallerMany record instead of storing another one; the hit count stays
    // exact and neither the map nor the manifest grows with the call graph.
    const PageEntryKey fanout_key{cache.stored_index, aligned_pc,
                                  kCallerMany, mode, kind};
    PageFanout& fanout = g_generation_entry_fanout[fanout_key];
    if (fanout.kept >= kMaxCallersPerEntry) {
        if (fanout.folded_index < g_generation_entries.size()) {
            ++g_generation_entries[fanout.folded_index].hits;
        } else {
            fanout.folded_index =
                static_cast<uint32_t>(g_generation_entries.size());
            g_generation_entries.push_back(
                StoredEntry{1u, aligned_pc, kCallerMany, mode, kind});
            g_pages[cache.stored_index].entry_indices.push_back(
                fanout.folded_index);
        }
        g_pages[cache.stored_index].last_seen = ++g_observation_seq;
        return;
    }

    const uint32_t entry_index =
        static_cast<uint32_t>(g_generation_entries.size());
    g_generation_entry_index.emplace(key, entry_index);
    ++fanout.kept;
    if (fanout.kept == 1u) fanout.folded_index = UINT32_MAX;
    g_generation_entries.push_back(
        StoredEntry{1u, aligned_pc, caller, mode, kind});
    g_pages[cache.stored_index].entry_indices.push_back(entry_index);
    g_pages[cache.stored_index].last_seen = ++g_observation_seq;
}

void coverage_manifest_set_identity(const char* rom_sha1, const char* rom_name,
                                    const char* build_id) {
    if (rom_sha1) g_rom_sha1 = rom_sha1;
    if (rom_name) g_rom_name = rom_name;
    if (build_id) g_build_id = build_id;
}

void coverage_manifest_set_output(const char* base_path) {
    g_out_base = base_path ? base_path : "";
}

bool coverage_manifest_flush_part(char* error, unsigned error_cap) {
    const std::string path = next_part_path();
    if (path.empty()) {
        if (error && error_cap) std::snprintf(error, error_cap, "no output base");
        return false;
    }
    // Nothing new since the last part: don't litter the folder with empties.
    if (g_pages.empty() && g_part_index > 0u) return true;
    if (!coverage_manifest_write(path.c_str(), error, error_cap)) return false;
    std::fprintf(stderr, "[coverage] wrote %s (%llu code pages, %llu bytes)\n",
                 path.c_str(), (unsigned long long)g_page_stats.captured,
                 (unsigned long long)g_page_stats.bytes);
    ++g_part_index;
    ++g_rotations;
    // Start the next part with an empty store. Entry points keep accumulating
    // in tier3's own map, so each part carries the full address list and only
    // the PAGE payload is split -- every part stays independently ingestible.
    g_pages.clear();
    g_page_index.clear();
    g_pages_by_addr.clear();
    g_generation_entries.clear();
    g_generation_entry_index.clear();
    g_generation_entry_fanout.clear();
    // g_root_map is deliberately NOT cleared: it is the session-wide execution
    // map and every part carries the whole of it, exactly as the entry-point
    // list already did, so each part stays independently ingestible.
    g_page_stats.captured = 0u;
    g_page_stats.bytes = 0u;
    for (CoverageExecCache& cache : g_coverage_exec_cache) cache = {};
    return true;
}

CoveragePageStats coverage_page_stats() { return g_page_stats; }

void coverage_pages_reset() {
    g_pages.clear();
    g_page_index.clear();
    g_pages_by_addr.clear();
    g_generation_entries.clear();
    g_generation_entry_index.clear();
    g_generation_entry_fanout.clear();
    g_root_map.clear();
    g_page_stats = {};
    g_observation_seq = 0u;
    for (CoverageExecCache& cache : g_coverage_exec_cache) cache = {};
}

// Emulation-thread half: a bounded copy of the resident store. Nothing here
// formats, sorts by key or touches the filesystem -- the whole point of the
// split is that everything expensive is a pure function of what this returns.
static void capture_manifest_snapshot(uint32_t live_max_pages,
                               CoverageLiveSnapshot& snap) {
    snap.rom_sha1 = g_rom_sha1;
    snap.rom_name = g_rom_name;
    snap.build_id = g_build_id;

    // Pull the Tier-3 address map. The cap matches the debug server's
    // tier3_coverage ceiling so a manifest is never a smaller view than a live
    // probe would have given.
    if (live_max_pages == 0u) {
        snap.entries.resize(262144);
        const uint32_t entry_count = tier3_coverage_copy(
            snap.entries.data(), static_cast<uint32_t>(snap.entries.size()));
        snap.entries.resize(entry_count);
    }
    snap.stats = tier3_stats();
    snap.dropped = g_page_stats.dropped;
    snap.replaced = g_page_stats.replaced;
    snap.revisits = g_page_stats.revisits;

    std::vector<uint32_t> page_indices;
    page_indices.reserve(g_pages.size());
    // beads-yjp.62: a page qualifies for the LIVE snapshot on entry points OR
    // on roots. Entry points alone was the whole starvation: since
    // beads-yjp.53/.55 a fresh install's coverage is ROOT-ONLY -- the deferred
    // filing rule records a call/indirect entry point only when the
    // interpreter resolves a transfer whose target has no live bank, and a
    // cold session spends its time in straight-line interpreted spans that
    // produce root-map bits and nothing else. A tester's own manifest shows
    // it: 27 captured pages, 2 with a non-empty entry_points array. Filtering
    // the other 25 out here starves the provider no matter what the compiler
    // is told, which is why passing --include-roots to it (main.cpp,
    // bundled_tcc_command) is necessary but not sufficient -- the flag can
    // only widen the selection over pages the snapshot actually carries.
    // Recency sort and the max_pages cap below are unchanged; this only
    // decides which pages are eligible to be sorted.
    for (uint32_t i = 0; i < g_pages.size(); ++i) {
        if (live_max_pages == 0u || !g_pages[i].entry_indices.empty() ||
            any_roots(g_pages[i].roots)) {
            page_indices.push_back(i);
        }
    }
    if (live_max_pages != 0u) {
        std::sort(page_indices.begin(), page_indices.end(),
                  [](uint32_t a, uint32_t b) {
            if (g_pages[a].last_seen != g_pages[b].last_seen)
                return g_pages[a].last_seen > g_pages[b].last_seen;
            if (g_pages[a].executions != g_pages[b].executions)
                return g_pages[a].executions > g_pages[b].executions;
            if (g_pages[a].cpu != g_pages[b].cpu)
                return g_pages[a].cpu < g_pages[b].cpu;
            return g_pages[a].addr < g_pages[b].addr;
        });
        if (page_indices.size() > live_max_pages)
            page_indices.resize(live_max_pages);
    }

    snap.pages.reserve(page_indices.size());
    for (const uint32_t page_index : page_indices) {
        const StoredPage& page = g_pages[page_index];
        SnapshotPage out;
        out.addr = page.addr;
        out.cpu = page.cpu;
        out.sha1 = page.sha1;
        out.executions = page.executions;
        out.roots = page.roots;
        out.bytes = page.bytes;
        out.entries.reserve(page.entry_indices.size());
        for (const uint32_t entry_index : page.entry_indices) {
            if (entry_index >= g_generation_entries.size()) continue;
            out.entries.push_back(g_generation_entries[entry_index]);
        }
        snap.pages.push_back(std::move(out));
    }

    snap.root_map.reserve(g_root_map.size());
    for (const auto& item : g_root_map) snap.root_map.push_back(item);
}

// Worker half: format. Byte-for-byte the document the single-pass writer
// produced, which is what keeps every existing ingest working.
static std::string render_manifest(const CoverageLiveSnapshot& snap) {
    std::string out;
    out.reserve(snap.pages.size() * 6000u + snap.entries.size() * 96u + 4096u);
    out += "{\n  \"schema\": 4,\n";
    out += "  \"kind\": \"ndsrecomp-tier3-coverage\",\n";
    out += "  \"rom_sha1\": ";
    append_json_escaped(out, snap.rom_sha1);
    out += ",\n  \"rom_name\": ";
    append_json_escaped(out, snap.rom_name);
    out += ",\n  \"build_id\": ";
    append_json_escaped(out, snap.build_id);

    out += ",\n  \"static_coverage\": {";
    out += "\"tier3_entries9\": " + std::to_string(snap.stats.entries[0]);
    out += ", \"tier3_entries7\": " + std::to_string(snap.stats.entries[1]);
    out += ", \"tier3_insns9\": " + std::to_string(snap.stats.instructions[0]);
    out += ", \"tier3_insns7\": " + std::to_string(snap.stats.instructions[1]);
    out += ", \"clean_ram_rejects9\": " +
           std::to_string(snap.stats.clean_ram_rejects[0]);
    out += ", \"clean_ram_rejects7\": " +
           std::to_string(snap.stats.clean_ram_rejects[1]);
    out += "}";

    // Entry points, split per CPU to match the committed coverage JSON shape
    // the promoters already read.
    for (int cpu = 0; cpu < 2; ++cpu) {
        out += ",\n  \"entry_points_";
        out += (cpu == 0) ? "arm9" : "arm7";
        out += "\": [";
        bool first = true;
        for (const Tier3CoverageEntry& entry : snap.entries) {
            if (entry.cpu != cpu) continue;
            // Roots are carried by root_map / the per-page bitmaps now. They
            // were 94% of this array and every address in it is still present,
            // at 1/40th the bytes. tier3's own map is untouched, so the debug
            // server's tier3_coverage command still reports them per address.
            if (entry.kind == TIER3_COVERAGE_ROOT) continue;
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

    // Session-wide execution map: every page base a root was ever seen in,
    // independent of whether that page's code is still in the store. This is
    // what replaces the per-address root list, and unlike the per-page bitmaps
    // it survives version eviction and part rotation.
    out += ",\n  \"root_map\": [";
    {
        std::vector<uint32_t> order;
        order.reserve(snap.root_map.size());
        for (uint32_t i = 0; i < snap.root_map.size(); ++i) order.push_back(i);
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            return snap.root_map[a].first < snap.root_map[b].first;
        });
        bool first_root = true;
        for (const uint32_t index : order) {
            const uint64_t key = snap.root_map[index].first;
            if (!first_root) out += ",";
            first_root = false;
            out += "\n    {\"addr\": \"" +
                   hex32(static_cast<uint32_t>(key)) + "\"";
            out += ", \"cpu\": ";
            out += ((key >> 32u) == 1u) ? "7" : "9";
            append_root_bits(out, snap.root_map[index].second);
            out += "}";
        }
        out += first_root ? "]" : "\n  ]";
    }

    out += ",\n  \"pages\": {";
    out += "\"captured\": " + std::to_string(snap.pages.size());
    out += ", \"dropped\": " + std::to_string(snap.dropped);
    out += ", \"replaced\": " + std::to_string(snap.replaced);
    out += ", \"bytes\": " + std::to_string(snap.pages.size() * kPageSize);
    out += ", \"revisits\": " + std::to_string(snap.revisits);
    out += ", \"page_size\": " + std::to_string(kPageSize);
    // Declared so an ingest can read a per-page entry whose caller is
    // 0xFFFFFFFF as "this many further sites, folded" rather than as an
    // address. Additive: a reader that ignores it still sees exact hits.
    out += ", \"caller_cap\": " + std::to_string(kMaxCallersPerEntry);
    out += ", \"entries\": [";
    bool first_page = true;
    for (const SnapshotPage& page : snap.pages) {
        if (!first_page) out += ",";
        first_page = false;
        out += "\n    {\"addr\": \"" + hex32(page.addr) + "\"";
        out += ", \"cpu\": ";
        out += (page.cpu == 1) ? "7" : "9";
        out += ", \"sha1\": \"" + page.sha1 + "\"";
        out += ", \"executions\": " + std::to_string(page.executions);
        out += ", \"entry_points\": [";
        bool first_entry = true;
        for (const StoredEntry& entry : page.entries) {
            if (!first_entry) out += ",";
            first_entry = false;
            out += "{\"addr\": \"" + hex32(entry.pc) + "\"";
            out += ", \"mode\": \"";
            out += entry.thumb ? "thumb" : "arm";
            out += "\", \"kind\": \"";
            out += kind_name(entry.kind);
            out += "\", \"hits\": " + std::to_string(entry.hits);
            out += ", \"caller\": \"" + hex32(entry.caller) + "\"}";
        }
        out += "]";
        append_root_bits(out, page.roots);
        out += ", \"data\": \"";
        append_base64(out, page.bytes);
        out += "\"}";
    }
    out += first_page ? "]}" : "\n  ]}";
    out += "\n}\n";
    return out;
}

static bool write_manifest_text(const char* path, const std::string& out,
                         char* error, unsigned error_cap) {
    auto fail = [&](const char* message) {
        if (error && error_cap) {
            std::snprintf(error, error_cap, "%s", message);
        }
        return false;
    };
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

bool coverage_manifest_write(const char* path, char* error,
                             unsigned error_cap) {
    if (!path || !path[0]) {
        if (error && error_cap)
            std::snprintf(error, error_cap, "no manifest path");
        return false;
    }
    CoverageLiveSnapshot snap;
    capture_manifest_snapshot(0u, snap);
    return write_manifest_text(path, render_manifest(snap), error, error_cap);
}

CoverageLiveSnapshot* coverage_manifest_capture_live_snapshot(
    uint32_t max_pages) {
    if (max_pages == 0u) max_pages = 64u;
    auto* snap = new CoverageLiveSnapshot();
    capture_manifest_snapshot(max_pages, *snap);
    return snap;
}

bool coverage_manifest_write_captured_snapshot(
    const CoverageLiveSnapshot* snapshot, const char* path, char* error,
    unsigned error_cap) {
    if (!snapshot || !path || !path[0]) {
        if (error && error_cap)
            std::snprintf(error, error_cap, "no manifest snapshot");
        return false;
    }
    return write_manifest_text(path, render_manifest(*snapshot), error,
                               error_cap);
}

void coverage_manifest_release_snapshot(CoverageLiveSnapshot* snapshot) {
    delete snapshot;
}
