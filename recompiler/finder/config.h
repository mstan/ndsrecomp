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
    bool        authoritative_entry_points = false;
};

struct ConfigIdentity {
    std::string sha1;       // required
    std::string md5;        // optional, empty if not declared
};

// A declared dispatch entry point (a guest address known to be reached as
// code). Parsed from either [[entry_point]] (preferred) or the legacy
// [[extra_func]] table name — both feed the same finder seed path. An
// entry_point need not be a "function" start: it may be a BIOS-ABI landing
// pad (e.g. the IRQ epilogue) reached only by a runtime-computed branch.
struct ConfigExtraFunc {
    uint32_t    addr = 0;
    uint32_t    size = 0;       // optional exact byte size; 0 = discover
    CpuMode     mode = CpuMode::Arm;
    std::string name;       // optional; finder generates one if empty
    std::string kind;       // optional; documentation (e.g. "bios_irq_epilogue")
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

// A deliberately small, title-owned manifest selecting generated functions
// for opt-in heat measurement.  This is separate from the discovery config:
// it may observe only an already discovered, exact whole function and can
// never create a code entry point.
struct HleProfileRoutine {
    std::string id;
    uint32_t    address = 0;
    uint32_t    end_address = 0;
    CpuMode     mode = CpuMode::Arm;
};

struct HleProfileManifest {
    std::string source_path;
    uint32_t    version = 0;
    std::string bank;
    std::string program_sha1;
    std::vector<HleProfileRoutine> routines;
};

// Load a TOML config from `path`. On success returns true and
// populates `out`. On parse/structural error returns false and
// writes a human-readable diagnostic to stderr.
//
// Identity hash verification is NOT performed here — call
// verify_identity() against the actual binary bytes after loading.
bool load_config(const std::string& path, Config& out);

// Load the strict performance-HLE observation manifest. Unknown keys and
// duplicate routine IDs fail closed so a stale or misspelled selector cannot
// silently instrument a different guest routine.
bool load_hle_profile_manifest(const std::string& path,
                               HleProfileManifest& out);

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
