// config.h — TOML config loader for nds_recompile.
//
// One TOML file per binary, loaded via --config <path>. See
// gbarecomp/docs/TOML_SCHEMA.md for the format.
//
// The config supplements (never replaces) the function-finder's
// automated walk. Manual entries deduplicate against discovered
// ones; data ranges hard-exclude bytes; jump tables auto-expand
// into per-target extra_func equivalents.
//
// Power-user contract: entries are not validated for correctness.
// Only structural contradictions abort.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "function_finder.h"

namespace ndsrecomp {

enum class JumpTableFormat : uint8_t {
    Abs32 = 0,
    Abs16 = 1,
    PcrelArm = 2,
    PcrelThumb = 3,
};

enum class JumpTableEntriesMode : uint8_t {
    Arm = 0,
    Thumb = 1,
    Auto = 2,    // bit 0 of entry encodes mode (interworking)
};

struct ConfigProgram {
    std::string name;
    std::string id;
    uint32_t    load_address = 0;
    uint32_t    size = 0;
    uint32_t    entry_pc = 0;
};

struct ConfigIdentity {
    std::string sha1;       // required
    std::string md5;        // optional, empty if not declared
};

struct ConfigExtraFunc {
    uint32_t    addr = 0;
    CpuMode     mode = CpuMode::Arm;
    std::string name;       // optional; finder generates one if empty
    std::string note;       // documentation only
};

struct ConfigDataRange {
    uint32_t    start = 0;
    uint32_t    end = 0;    // [start, end)
    std::string note;
};

struct ConfigCodeCopy {
    uint32_t    runtime_start = 0;
    uint32_t    source_start = 0;
    uint32_t    size = 0;
    std::string name;
    std::string note;
};

struct ConfigJumpTable {
    uint32_t            addr = 0;
    uint32_t            stride = 0;
    uint32_t            count = 0;
    JumpTableFormat     format = JumpTableFormat::Abs32;
    JumpTableEntriesMode entries_mode = JumpTableEntriesMode::Arm;
    std::string         name;
    std::string         note;
};

struct ConfigExcludeFunc {
    uint32_t    addr = 0;
    std::string reason;     // required
};

struct Config {
    std::string             source_path;  // path the config was loaded from
    ConfigProgram           program;
    ConfigIdentity          identity;
    std::vector<ConfigExtraFunc>    extra_funcs;
    std::vector<ConfigDataRange>    data_ranges;
    std::vector<ConfigCodeCopy>     code_copies;
    std::vector<ConfigJumpTable>    jump_tables;
    std::vector<ConfigExcludeFunc>  exclude_funcs;
};

// Load a TOML config from `path`. On success returns true and
// populates `out`. On parse/structural error returns false and
// writes a human-readable diagnostic to stderr.
//
// Identity hash verification is NOT performed here — call
// verify_identity() against the actual binary bytes after loading.
bool load_config(const std::string& path, Config& out);

// Verify the config's identity hashes against the binary bytes.
// Returns true on match; false and prints a diagnostic on
// mismatch. If `identity.sha1` is empty the check fails (sha1
// is required by the schema).
bool verify_identity(const Config& cfg,
                      const uint8_t* binary, std::size_t binary_len);

// Print a short summary of the loaded config to stdout. Called
// after load + verify so the operator sees what's in effect.
void print_config_summary(const Config& cfg);

}  // namespace ndsrecomp
