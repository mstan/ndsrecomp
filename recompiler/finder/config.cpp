// config.cpp — TOML loader for nds_recompile. See config.h /
// docs/TOML_SCHEMA.md.

#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

#include "toml.hpp"
#include "sha1.h"

namespace ndsrecomp {

namespace {

constexpr const char* kAbortHeader =
    "[nds_recompile] CONFIG ERROR ";

bool parse_mode(const std::string& s, CpuMode& out) {
    if (s == "arm")   { out = CpuMode::Arm;   return true; }
    if (s == "thumb") { out = CpuMode::Thumb; return true; }
    return false;
}

bool parse_jt_format(const std::string& s, JumpTableFormat& out) {
    if (s == "abs32")       { out = JumpTableFormat::Abs32;      return true; }
    if (s == "abs16")       { out = JumpTableFormat::Abs16;      return true; }
    if (s == "pcrel_arm")   { out = JumpTableFormat::PcrelArm;   return true; }
    if (s == "pcrel_thumb") { out = JumpTableFormat::PcrelThumb; return true; }
    return false;
}

bool parse_jt_mode(const std::string& s, JumpTableEntriesMode& out) {
    if (s == "arm")   { out = JumpTableEntriesMode::Arm;   return true; }
    if (s == "thumb") { out = JumpTableEntriesMode::Thumb; return true; }
    if (s == "auto")  { out = JumpTableEntriesMode::Auto;  return true; }
    return false;
}

// Hex lowercase, no `0x`. Returns empty string on length mismatch.
std::string hex_lower(const std::string& s) {
    std::string out = s;
    if (out.size() > 2 && out[0] == '0' && (out[1] == 'x' || out[1] == 'X')) {
        out.erase(0, 2);
    }
    for (auto& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool safe_profile_name(const std::string& text) {
    return !text.empty() && text.size() <= 96u && std::all_of(
        text.begin(), text.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-';
        });
}

bool safe_profile_bank(const std::string& text) {
    if (text.empty() || text.size() > 48u ||
        !(std::isalpha(static_cast<unsigned char>(text.front())) != 0 ||
          text.front() == '_'))
        return false;
    return std::all_of(text.begin() + 1, text.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_';
    });
}

uint32_t get_manifest_u32(const toml::table& table, std::string_view key,
                          bool& ok) {
    const toml::node* node = table.get(key);
    if (!node) {
        ok = false;
        return 0u;
    }
    const auto value = node->value<int64_t>();
    if (!value || *value < 0 ||
        static_cast<uint64_t>(*value) > UINT32_MAX) {
        ok = false;
        return 0u;
    }
    return static_cast<uint32_t>(*value);
}

uint32_t get_u32_field(const toml::table& t, std::string_view key,
                       bool required, bool& ok, std::string& err) {
    auto node = t.get(key);
    if (!node) {
        if (required) {
            ok = false;
            err = std::string("missing required key '") +
                  std::string(key) + "'";
        }
        return 0u;
    }
    if (auto v = node->value<int64_t>()) {
        return static_cast<uint32_t>(*v);
    }
    if (auto v = node->value<std::string>()) {
        // Allow `"0xABCD"` strings as a convenience, though
        // TOML's bare 0xABCD is preferred.
        const std::string s = hex_lower(*v);
        char* end = nullptr;
        unsigned long n = std::strtoul(s.c_str(), &end, 16);
        if (end == s.c_str()) {
            ok = false;
            err = std::string("key '") + std::string(key) +
                  "' is not a parseable integer";
            return 0u;
        }
        return static_cast<uint32_t>(n);
    }
    ok = false;
    err = std::string("key '") + std::string(key) +
          "' must be an integer (decimal or hex)";
    return 0u;
}

std::string get_string_field(const toml::table& t, std::string_view key,
                             bool required, bool& ok, std::string& err) {
    auto node = t.get(key);
    if (!node) {
        if (required) {
            ok = false;
            err = std::string("missing required key '") +
                  std::string(key) + "'";
        }
        return std::string();
    }
    if (auto v = node->value<std::string>()) return *v;
    ok = false;
    err = std::string("key '") + std::string(key) + "' must be a string";
    return std::string();
}

// Parse [[entry_point]] / [[extra_func]] entries. Both table names map to
// the same record; `label` is used only for diagnostics.
bool parse_entry_points(const toml::array& arr, const char* label,
                        std::vector<ConfigExtraFunc>& out) {
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto* t = arr[i].as_table();
        if (!t) {
            std::fprintf(stderr,
                "%s%s entry %zu is not a table\n",
                kAbortHeader, label, i);
            return false;
        }
        ConfigExtraFunc e;
        bool ok = true;
        std::string err;
        e.addr = get_u32_field(*t, "addr", true, ok, err);
        if (!ok) {
            std::fprintf(stderr,
                "%s%s entry %zu: %s\n",
                kAbortHeader, label, i, err.c_str());
            return false;
        }
        e.size = get_u32_field(*t, "size", false, ok, err);
        if (!ok) {
            std::fprintf(stderr,
                "%s%s entry %zu: %s\n",
                kAbortHeader, label, i, err.c_str());
            return false;
        }
        std::string mode_s = get_string_field(*t, "mode", true, ok, err);
        if (!ok || !parse_mode(mode_s, e.mode)) {
            std::fprintf(stderr,
                "%s%s entry %zu: mode must be \"arm\" or "
                "\"thumb\" (got %s)\n",
                kAbortHeader, label, i, mode_s.c_str());
            return false;
        }
        const uint32_t alignment = e.mode == CpuMode::Thumb ? 2u : 4u;
        if (e.size != 0u && (e.size < alignment || e.size % alignment != 0u)) {
            std::fprintf(stderr,
                "%s%s entry %zu: size 0x%X is not a non-zero multiple "
                "of the %u-byte instruction width\n",
                kAbortHeader, label, i, e.size, alignment);
            return false;
        }
        e.name = get_string_field(*t, "name", false, ok, err);
        e.kind = get_string_field(*t, "kind", false, ok, err);
        e.note = get_string_field(*t, "note", false, ok, err);
        out.push_back(std::move(e));
    }
    return true;
}

bool parse_data_ranges(const toml::array& arr,
                       std::vector<ConfigDataRange>& out) {
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto* t = arr[i].as_table();
        if (!t) {
            std::fprintf(stderr,
                "%s[[data_range]] entry %zu is not a table\n",
                kAbortHeader, i);
            return false;
        }
        ConfigDataRange r;
        bool ok = true;
        std::string err;
        r.start = get_u32_field(*t, "start", true, ok, err);
        r.end   = get_u32_field(*t, "end",   true, ok, err);
        if (!ok) {
            std::fprintf(stderr,
                "%s[[data_range]] entry %zu: %s\n",
                kAbortHeader, i, err.c_str());
            return false;
        }
        if (r.start >= r.end) {
            std::fprintf(stderr,
                "%s[[data_range]] entry %zu: start (0x%08X) must be "
                "strictly less than end (0x%08X)\n",
                kAbortHeader, i, r.start, r.end);
            return false;
        }
        r.note = get_string_field(*t, "note", false, ok, err);
        out.push_back(std::move(r));
    }
    return true;
}

bool parse_code_copies(const toml::array& arr,
                       std::vector<ConfigCodeCopy>& out) {
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto* t = arr[i].as_table();
        if (!t) {
            std::fprintf(stderr,
                "%s[[code_copy]] entry %zu is not a table\n",
                kAbortHeader, i);
            return false;
        }
        ConfigCodeCopy cc;
        bool ok = true;
        std::string err;
        cc.runtime_start = get_u32_field(*t, "runtime_start", true, ok, err);
        cc.source_start  = get_u32_field(*t, "source_start",  true, ok, err);
        cc.size          = get_u32_field(*t, "size",          true, ok, err);
        if (!ok) {
            std::fprintf(stderr,
                "%s[[code_copy]] entry %zu: %s\n",
                kAbortHeader, i, err.c_str());
            return false;
        }
        if (cc.size == 0) {
            std::fprintf(stderr,
                "%s[[code_copy]] entry %zu: size must be non-zero\n",
                kAbortHeader, i);
            return false;
        }
        cc.name = get_string_field(*t, "name", false, ok, err);
        cc.note = get_string_field(*t, "note", false, ok, err);
        out.push_back(std::move(cc));
    }
    return true;
}

bool parse_jump_tables(const toml::array& arr,
                       std::vector<ConfigJumpTable>& out) {
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto* t = arr[i].as_table();
        if (!t) {
            std::fprintf(stderr,
                "%s[[jump_table]] entry %zu is not a table\n",
                kAbortHeader, i);
            return false;
        }
        ConfigJumpTable jt;
        bool ok = true;
        std::string err;
        jt.addr   = get_u32_field(*t, "addr",   true, ok, err);
        jt.stride = get_u32_field(*t, "stride", true, ok, err);
        jt.count  = get_u32_field(*t, "count",  true, ok, err);
        if (!ok) {
            std::fprintf(stderr,
                "%s[[jump_table]] entry %zu: %s\n",
                kAbortHeader, i, err.c_str());
            return false;
        }
        std::string fmt_s = get_string_field(*t, "format", true, ok, err);
        if (!ok || !parse_jt_format(fmt_s, jt.format)) {
            std::fprintf(stderr,
                "%s[[jump_table]] entry %zu: format must be one of "
                "abs32 / abs16 / pcrel_arm / pcrel_thumb (got %s)\n",
                kAbortHeader, i, fmt_s.c_str());
            return false;
        }
        std::string em_s = get_string_field(*t, "entries_mode", true, ok, err);
        if (!ok || !parse_jt_mode(em_s, jt.entries_mode)) {
            std::fprintf(stderr,
                "%s[[jump_table]] entry %zu: entries_mode must be "
                "arm / thumb / auto (got %s)\n",
                kAbortHeader, i, em_s.c_str());
            return false;
        }
        // Mode/format compatibility (see schema doc).
        if (jt.format == JumpTableFormat::PcrelArm &&
            jt.entries_mode != JumpTableEntriesMode::Arm) {
            std::fprintf(stderr,
                "%s[[jump_table]] entry %zu: pcrel_arm requires "
                "entries_mode = \"arm\"\n", kAbortHeader, i);
            return false;
        }
        if (jt.format == JumpTableFormat::PcrelThumb &&
            jt.entries_mode != JumpTableEntriesMode::Thumb) {
            std::fprintf(stderr,
                "%s[[jump_table]] entry %zu: pcrel_thumb requires "
                "entries_mode = \"thumb\"\n", kAbortHeader, i);
            return false;
        }
        if (jt.format == JumpTableFormat::Abs16 &&
            jt.entries_mode == JumpTableEntriesMode::Auto) {
            std::fprintf(stderr,
                "%s[[jump_table]] entry %zu: abs16 does not support "
                "entries_mode = \"auto\" (16-bit pointers don't span "
                "the interworking address space)\n",
                kAbortHeader, i);
            return false;
        }
        jt.name = get_string_field(*t, "name", false, ok, err);
        jt.note = get_string_field(*t, "note", false, ok, err);
        out.push_back(std::move(jt));
    }
    return true;
}

bool parse_exclude_funcs(const toml::array& arr,
                          std::vector<ConfigExcludeFunc>& out) {
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto* t = arr[i].as_table();
        if (!t) {
            std::fprintf(stderr,
                "%s[[exclude_func]] entry %zu is not a table\n",
                kAbortHeader, i);
            return false;
        }
        ConfigExcludeFunc e;
        bool ok = true;
        std::string err;
        e.addr = get_u32_field(*t, "addr", true, ok, err);
        if (!ok) {
            std::fprintf(stderr,
                "%s[[exclude_func]] entry %zu: %s\n",
                kAbortHeader, i, err.c_str());
            return false;
        }
        e.reason = get_string_field(*t, "reason", true, ok, err);
        if (!ok || e.reason.empty()) {
            std::fprintf(stderr,
                "%s[[exclude_func]] entry %zu: reason is required\n",
                kAbortHeader, i);
            return false;
        }
        out.push_back(std::move(e));
    }
    return true;
}

// Cross-section structural validation. See docs/TOML_SCHEMA.md
// "Precedence and conflict resolution" step 2.
bool validate_cross_section(const Config& cfg) {
    const uint64_t program_start = cfg.program.load_address;
    const uint64_t program_end = program_start + cfg.program.size;
    if (cfg.program.authoritative_entry_points && cfg.extra_funcs.empty()) {
        std::fprintf(stderr,
            "%s[program].authoritative_entry_points requires at least one "
            "[[entry_point]]\n",
            kAbortHeader);
        return false;
    }
    for (const auto& ef : cfg.extra_funcs) {
        if (cfg.program.authoritative_entry_points && ef.size == 0u) {
            std::fprintf(stderr,
                "%s[program].authoritative_entry_points requires an exact "
                "size for [[entry_point]] 0x%08X\n",
                kAbortHeader, ef.addr);
            return false;
        }
        if (ef.size != 0u &&
            (ef.addr < program_start ||
             static_cast<uint64_t>(ef.addr) + ef.size > program_end)) {
            std::fprintf(stderr,
                "%s[[entry_point]] 0x%08X size 0x%X lies outside the "
                "program image [0x%08X, 0x%08llX)\n",
                kAbortHeader, ef.addr, ef.size, cfg.program.load_address,
                static_cast<unsigned long long>(program_end));
            return false;
        }
    }
    // exclude_func + extra_func at the same addr is contradictory.
    for (const auto& ex : cfg.exclude_funcs) {
        for (const auto& ef : cfg.extra_funcs) {
            if (ef.addr == ex.addr) {
                std::fprintf(stderr,
                    "%saddress 0x%08X is declared in BOTH "
                    "[[extra_func]] and [[exclude_func]] — "
                    "contradictory intent\n",
                    kAbortHeader, ex.addr);
                return false;
            }
        }
    }
    // extra_func addr inside a data_range is also contradictory.
    for (const auto& ef : cfg.extra_funcs) {
        for (const auto& dr : cfg.data_ranges) {
            if (ef.addr >= dr.start && ef.addr < dr.end) {
                std::fprintf(stderr,
                    "%s[[extra_func]] at 0x%08X falls inside "
                    "[[data_range]] [0x%08X, 0x%08X)%s%s\n",
                    kAbortHeader, ef.addr, dr.start, dr.end,
                    dr.note.empty() ? "" : " — ",
                    dr.note.c_str());
                return false;
            }
        }
    }
    // jump_table table bytes overlapping a data_range is also wrong
    // (the table bytes are AUTO-excluded, declaring them as data
    // is harmless but suggests confusion — accept silently for now).
    // Per-table entry-range vs other ranges: deferred to task #5
    // (the finder is what reads + walks entries; cross-checks
    // belong there).
    return true;
}

}  // namespace

bool load_hle_profile_manifest(const std::string& path,
                               HleProfileManifest& out) {
    out = HleProfileManifest{};
    out.source_path = path;

    toml::table tbl;
    try {
        tbl = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        std::fprintf(stderr, "%sHLE manifest parse error in %s: %s\n",
                     kAbortHeader, path.c_str(), e.what());
        return false;
    }

    // The schema intentionally has only these three top-level keys. Requiring
    // the exact count after checking their types makes misspellings fail
    // closed without coupling the rest of the config loader to strict mode.
    const auto version = tbl["version"].value<int64_t>();
    const auto* program = tbl["program"].as_table();
    const auto* routines = tbl["routine"].as_array();
    if (!version || !program || !routines || tbl.size() != 3u) {
        std::fprintf(stderr,
            "%sHLE manifest requires exactly version, [program], and "
            "[[routine]] keys\n", kAbortHeader);
        return false;
    }
    if (*version != 1) {
        std::fprintf(stderr,
            "%sHLE manifest version must be 1 (got %lld)\n",
            kAbortHeader, static_cast<long long>(*version));
        return false;
    }
    out.version = 1u;

    bool ok = true;
    std::string err;
    out.bank = get_string_field(*program, "bank", true, ok, err);
    out.program_sha1 = hex_lower(
        get_string_field(*program, "sha1", true, ok, err));
    if (!ok || program->size() != 2u || !safe_profile_bank(out.bank) ||
        out.program_sha1.size() != 40u ||
        !std::all_of(out.program_sha1.begin(), out.program_sha1.end(),
                     [](unsigned char c) { return std::isxdigit(c) != 0; })) {
        std::fprintf(stderr,
            "%sHLE [program] requires only non-empty bank and a 40-digit "
            "SHA-1\n", kAbortHeader);
        return false;
    }

    if (routines->empty()) {
        std::fprintf(stderr,
            "%sHLE manifest must select at least one routine\n",
            kAbortHeader);
        return false;
    }
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> selectors;
    for (std::size_t index = 0; index < routines->size(); ++index) {
        const auto* table = (*routines)[index].as_table();
        if (!table || table->size() != 4u) {
            std::fprintf(stderr,
                "%sHLE [[routine]] %zu requires exactly id, address, "
                "end_address, and mode\n", kAbortHeader, index);
            return false;
        }
        HleProfileRoutine routine;
        ok = true;
        err.clear();
        routine.id = get_string_field(*table, "id", true, ok, err);
        routine.address = get_manifest_u32(*table, "address", ok);
        routine.end_address = get_manifest_u32(*table, "end_address", ok);
        const std::string mode = get_string_field(
            *table, "mode", true, ok, err);
        if (!ok || !safe_profile_name(routine.id) ||
            !parse_mode(mode, routine.mode)) {
            std::fprintf(stderr,
                "%sHLE [[routine]] %zu has an invalid required field%s%s\n",
                kAbortHeader, index, err.empty() ? "" : ": ", err.c_str());
            return false;
        }
        const uint32_t alignment = routine.mode == CpuMode::Thumb ? 2u : 4u;
        if ((routine.address % alignment) != 0u ||
            (routine.end_address % alignment) != 0u ||
            routine.end_address <= routine.address) {
            std::fprintf(stderr,
                "%sHLE [[routine]] %zu has an invalid or unaligned range "
                "[0x%08X,0x%08X)\n", kAbortHeader, index,
                routine.address, routine.end_address);
            return false;
        }
        if (!ids.insert(routine.id).second) {
            std::fprintf(stderr,
                "%sHLE [[routine]] %zu duplicates id '%s'\n",
                kAbortHeader, index, routine.id.c_str());
            return false;
        }
        const std::string selector = std::to_string(routine.address) + ":" +
            std::to_string(routine.end_address) + ":" + mode;
        if (!selectors.insert(selector).second) {
            std::fprintf(stderr,
                "%sHLE [[routine]] %zu duplicates an address/range/mode "
                "selector\n", kAbortHeader, index);
            return false;
        }
        out.routines.push_back(std::move(routine));
    }
    return true;
}

bool load_config(const std::string& path, Config& out) {
    out = Config{};
    out.source_path = path;

    toml::table tbl;
    try {
        tbl = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        std::fprintf(stderr, "%sparse error in %s: %s\n",
                     kAbortHeader, path.c_str(), e.what());
        return false;
    }

    // [program]
    auto prog_node = tbl["program"];
    if (!prog_node.is_table()) {
        std::fprintf(stderr,
            "%s[program] table missing in %s\n",
            kAbortHeader, path.c_str());
        return false;
    }
    {
        const auto& t = *prog_node.as_table();
        bool ok = true;
        std::string err;
        out.program.name         = get_string_field(t, "name",         false, ok, err);
        out.program.id           = get_string_field(t, "id",           false, ok, err);
        out.program.load_address = get_u32_field   (t, "load_address", true,  ok, err);
        out.program.size         = get_u32_field   (t, "size",         true,  ok, err);
        out.program.entry_pc     = get_u32_field   (t, "entry_pc",     true,  ok, err);
        if (!ok) {
            std::fprintf(stderr, "%s[program]: %s\n",
                         kAbortHeader, err.c_str());
            return false;
        }
        if (const auto* node = t.get("authoritative_entry_points")) {
            const auto value = node->value<bool>();
            if (!value) {
                std::fprintf(stderr,
                    "%s[program]: key 'authoritative_entry_points' must be a boolean\n",
                    kAbortHeader);
                return false;
            }
            out.program.authoritative_entry_points = *value;
        }
    }

    // [identity]
    auto id_node = tbl["identity"];
    if (!id_node.is_table()) {
        std::fprintf(stderr,
            "%s[identity] table missing in %s\n",
            kAbortHeader, path.c_str());
        return false;
    }
    {
        const auto& t = *id_node.as_table();
        bool ok = true;
        std::string err;
        out.identity.sha1 = get_string_field(t, "sha1", true,  ok, err);
        out.identity.md5  = get_string_field(t, "md5",  false, ok, err);
        if (!ok) {
            std::fprintf(stderr, "%s[identity]: %s\n",
                         kAbortHeader, err.c_str());
            return false;
        }
        if (out.identity.sha1.empty()) {
            std::fprintf(stderr,
                "%s[identity].sha1 is required\n", kAbortHeader);
            return false;
        }
    }

    // [[entry_point]] (preferred) and legacy [[extra_func]] — both feed the
    // same finder seed list. A future importer should emit [[entry_point]].
    if (auto ep = tbl["entry_point"].as_array()) {
        if (!parse_entry_points(*ep, "[[entry_point]]", out.extra_funcs))
            return false;
    }
    if (auto ef = tbl["extra_func"].as_array()) {
        if (!parse_entry_points(*ef, "[[extra_func]]", out.extra_funcs))
            return false;
    }

    // [[data_range]]
    if (auto dr = tbl["data_range"].as_array()) {
        if (!parse_data_ranges(*dr, out.data_ranges)) return false;
    }

    // [[code_copy]]
    if (auto cc = tbl["code_copy"].as_array()) {
        if (!parse_code_copies(*cc, out.code_copies)) return false;
    }

    // [[jump_table]]
    if (auto jt = tbl["jump_table"].as_array()) {
        if (!parse_jump_tables(*jt, out.jump_tables)) return false;
    }

    // [[exclude_func]]
    if (auto ex = tbl["exclude_func"].as_array()) {
        if (!parse_exclude_funcs(*ex, out.exclude_funcs)) return false;
    }

    if (!validate_cross_section(out)) return false;
    return true;
}

bool verify_identity(const Config& cfg,
                      const uint8_t* binary, std::size_t binary_len) {
    if (cfg.identity.sha1.empty()) {
        std::fprintf(stderr,
            "%s[identity].sha1 missing (should have been caught earlier)\n",
            kAbortHeader);
        return false;
    }
    if (binary_len != cfg.program.size) {
        std::fprintf(stderr,
            "%sbinary size mismatch: file is %zu bytes, "
            "config declares %u bytes\n",
            kAbortHeader, binary_len, cfg.program.size);
        return false;
    }
    auto digest = gba::sha1(binary, binary_len);
    std::string got = digest.hex();
    std::string want = hex_lower(cfg.identity.sha1);
    if (got != want) {
        std::fprintf(stderr,
            "%sSHA-1 mismatch:\n"
            "  config declares: %s\n"
            "  binary hashes:   %s\n"
            "Either the binary is the wrong revision or the TOML\n"
            "was authored against a different binary.\n",
            kAbortHeader, want.c_str(), got.c_str());
        return false;
    }
    return true;
}

void print_config_summary(const Config& cfg) {
    std::printf("[nds_recompile config: %s]\n", cfg.source_path.c_str());
    std::printf("  program:               %s (%s)\n",
                cfg.program.name.c_str(), cfg.program.id.c_str());
    std::printf("  load_address:          0x%08X\n", cfg.program.load_address);
    std::printf("  size:                  0x%08X (%u bytes)\n",
                cfg.program.size, cfg.program.size);
    std::printf("  entry_pc:              0x%08X\n", cfg.program.entry_pc);
    std::printf("  authoritative entries: %s\n",
                cfg.program.authoritative_entry_points ? "yes" : "no");
    std::printf("  identity sha1:         %s (verified)\n",
                hex_lower(cfg.identity.sha1).c_str());
    std::printf("  entry_point entries:   %zu\n", cfg.extra_funcs.size());
    std::printf("  data_range entries:    %zu\n", cfg.data_ranges.size());
    std::printf("  code_copy entries:     %zu\n", cfg.code_copies.size());
    std::printf("  jump_table entries:    %zu\n", cfg.jump_tables.size());
    std::printf("  exclude_func entries:  %zu\n", cfg.exclude_funcs.size());
}

}  // namespace ndsrecomp
