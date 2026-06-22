// function_finder.h — Recompiler-side function discovery.
//
// Given the ROM bytes, an entry point, and a set of seed symbols,
// walk the CFG to find every reachable function address and its
// approximate basic-block layout. The output drives codegen.
//
// Design notes:
//   - We track ARM vs THUMB mode per address because the cart freely
//     mixes them (crt0 is ARM; most of the game is THUMB).
//   - "Function" here is "address reachable as a call target (BL,
//     BLX, or B-as-tail-call) or as a control-flow root from the
//     entry / seed list".
//   - Indirect branches (BX reg, LDR PC, ldm with PC, computed
//     calls) are recorded as "indirect dispatch needed" — the
//     runtime resolves them via the dispatch table at execution time.
//   - Literal pools (data embedded in .text via `LDR rX, =const`)
//     are NOT skipped in this first cut — function bodies stop at
//     the first unconditional terminator (B, BX, undefined). The
//     codegen will see the literal-pool bytes as "garbage" Instrs
//     and emit them as `/* literal: 0x...... */` comments.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ndsrecomp {

enum class CpuMode : uint8_t {
    Arm = 0,
    Thumb = 1,
};

// A seed: an address we KNOW is a function entry, with its mode +
// (optional) name. Sources: entry_point from game.toml, imported
// tmc symbols.
struct FunctionSeed {
    uint32_t addr;
    CpuMode  mode;
    std::string name;   // empty → finder generates "func_XXXXXXXX"
    uint32_t source_addr = 0;  // 0 => decode from addr or code_copy map
};

// Output: one entry per discovered function.
struct Function {
    uint32_t addr;
    uint32_t source_addr;
    CpuMode  mode;
    std::string name;
    // Approximate end address (exclusive). Set by the finder via
    // "next function start" once discovery is done; for the last
    // function in the corpus this stays at addr + 4 (placeholder).
    uint32_t end_addr;
    // The set of direct branch targets discovered inside this body
    // (B/BL/conditional B). Useful for codegen labeling and for
    // verifying CFG coverage.
    std::vector<uint32_t> direct_branch_targets;
    // True if at least one indirect control transfer (BX reg,
    // LDR PC, ldm with PC, etc.) was decoded in this function.
    bool has_indirect_transfer = false;
};

// A jump table the finder recognized automatically (no [[jump_table]]
// hint). Recorded for the discovery summary and for validating the
// detector against the manual ground-truth set. See MC-HP-000.
struct AutoJumpTable {
    uint32_t base;          // table base address (guest)
    uint32_t stride;        // bytes per entry (4 = abs32)
    uint32_t count;         // entries emitted
    uint32_t site_pc;       // PC of the indexed load that revealed the table
    CpuMode  site_mode;     // mode of the dispatching code
    bool     interworking;  // BX/veneer (per-entry bit0 mode) vs MOV pc (mode = site_mode)
    bool     bounded;       // count came from an exact CMP bound (vs walk)
};

struct FinderStats {
    std::size_t functions_total = 0;
    std::size_t functions_arm = 0;
    std::size_t functions_thumb = 0;
    std::size_t indirect_transfer_count = 0;
    std::size_t branch_targets_discovered = 0;
    // LR landing pads auto-discovered (Tier 2): an executable-ROM constant
    // live in LR at an UNRESOLVED indirect PC-write — the return-pad idiom
    // (adr lr,X; ldr pc,[handler]). Seeded as a dispatch entry.
    std::size_t landing_pads_discovered = 0;
    std::size_t undefined_instr_count = 0;
    // Automatic jump-table detection (MC-HP-000).
    std::size_t auto_jump_tables = 0;          // tables emitted
    std::size_t auto_jump_table_targets = 0;   // entry targets seeded
    std::size_t jt_confirmations = 0;          // indexed-load+branch confirmed
    std::size_t jt_rejected_unsized = 0;       // confirmed, no bound, walk found <2
    std::size_t jt_rejected_bound_mismatch = 0;// bound said N but an entry wasn't code
    std::size_t jt_overlap_suppressed = 0;     // table already in a data_range (manual hint)
    // Discovery-source breakdown — populated by run() against the
    // set of (addr, mode) keys seen across seeds vs walks.
    std::size_t manual_seeds_total = 0;     // # of seeds the caller added
    std::size_t discovered_by_walk_only = 0;
    std::size_t redundant_manual = 0;       // in both seeds AND walk-discovered
    std::size_t manual_seeds_only = 0;      // would be lost without seeds
    std::size_t excluded_count = 0;         // # removed by exclude_func
};

// A byte range the finder must not decode as code. See
// docs/TOML_SCHEMA.md "[[data_range]]".
struct DataRange {
    uint32_t    start;
    uint32_t    end;        // [start, end)
    std::string note;
};

struct CodeCopyRange {
    uint32_t    runtime_start;
    uint32_t    source_start;
    uint32_t    size;
    std::string note;
};

// Diagnostic for the hard error: control flow entered a data_range.
// The collision names BOTH the offending branch (origin) AND the
// range, so the operator can decide which to fix.
struct DataRangeCollision {
    uint32_t    flow_target_addr;   // address that landed in the range
    uint32_t    flow_origin_addr;   // function containing the offending branch
    std::string flow_origin_name;
    std::string flow_origin_kind;   // "branch", "seed", "jump_table"
    uint32_t    range_start;
    uint32_t    range_end;
    std::string range_note;
};

class FunctionFinder {
public:
    // ROM bytes; expected to be the cartridge image as loaded into
    // memory at `rom_base` (typically 0x08000000). `rom_base` is
    // used to translate guest addresses to ROM-buffer indices.
    FunctionFinder(const uint8_t* rom_bytes, std::size_t rom_size,
                   uint32_t rom_base);

    // Add a seed. Duplicate addresses are tolerated; the first
    // seed's mode + name wins.
    void add_seed(const FunctionSeed& seed);

    // Register a byte range as data — the finder will hard-error
    // if control flow ever enters it. Caller supplies `note` for
    // the diagnostic; `origin_kind` is "data_range" for explicit
    // [[data_range]] entries or "jump_table" for auto-excluded
    // jump-table bytes.
    void add_data_range(uint32_t start, uint32_t end,
                         const std::string& note);

    void add_code_copy(uint32_t runtime_start, uint32_t source_start,
                       uint32_t size, const std::string& note);

    // Register a post-discovery exclusion. After discovery, any
    // function whose address matches is dropped from the output.
    void add_exclude(uint32_t addr, const std::string& reason);

    // Run discovery starting from all seeds. Bounded by
    // `max_functions` to avoid runaways during bring-up.
    void run(std::size_t max_functions = 4096);

    // Sorted-by-address output.
    const std::vector<Function>& functions() const { return functions_; }
    const FinderStats& stats() const { return stats_; }

    // Jump tables recognized automatically during discovery.
    const std::vector<AutoJumpTable>& auto_jump_tables() const {
        return auto_jump_tables_;
    }

    // Non-empty when the recompile must abort. See
    // docs/TOML_SCHEMA.md "[[data_range]]" — control flow into
    // a data_range is a hard error.
    const std::vector<DataRangeCollision>& collisions() const {
        return collisions_;
    }

    // Read a 32-bit little-endian word at `addr` (interpreted as a
    // guest address; translated to rom buffer offset internally).
    // Returns 0 if out of bounds.
    uint32_t read_u32_public(uint32_t addr) const;

private:
    const uint8_t* rom_;
    std::size_t    rom_size_;
    uint32_t       rom_base_;

    std::vector<FunctionSeed> seeds_;
    std::vector<Function>     functions_;
    std::unordered_map<uint64_t, std::size_t> visited_;  // key=(addr<<1)|mode
    std::vector<AutoJumpTable> auto_jump_tables_;
    // Every jump-table base ever handed to the emitter, so the same table
    // reached by multiple seed walks is accounted once (the dispatch
    // confirmation can fire from several overlapping walks). Keeps the
    // emitted / overlap / rejected tallies counting distinct tables.
    std::unordered_set<uint32_t> jt_seen_bases_;
    // CMP-bound carried across an inverted switch guard. For the
    // `CMP Ri,#N; B{ls,cc} <dispatch>; B <default>` idiom the indexed
    // dispatch lives at the BRANCH TARGET, which the finder walks as its
    // own seed with fresh tracker state — so the bound recovered at the
    // compare must be parked here, keyed by the dispatch-target PC, and
    // re-seeded into reg_bound[reg] when that seed is walked.
    struct SeedBound { uint8_t reg; uint32_t max_index; };
    std::unordered_map<uint32_t, SeedBound> branch_target_bounds_;
    FinderStats stats_{};

    std::vector<DataRange>           data_ranges_;
    std::vector<CodeCopyRange>       code_copies_;
    std::vector<DataRangeCollision>  collisions_;
    std::unordered_set<uint32_t>     excluded_;
    std::unordered_map<uint32_t, std::string> exclude_reasons_;

    // Mode-switching seeds discovered DURING a walk (e.g. BX Rm
    // where the constant tracker resolved Rm to a THUMB target
    // while walking ARM code). Drained by run() into the worklist
    // after each function's body is processed.
    std::vector<FunctionSeed>        mode_switch_seeds_;

    bool addr_in_rom(uint32_t addr) const {
        return addr >= rom_base_ &&
               (addr - rom_base_) < rom_size_;
    }
    bool map_addr_to_source(uint32_t addr, uint32_t* source) const;
    bool can_read_at(uint32_t addr, uint32_t len) const;
    bool addr_in_data_range(uint32_t addr) const;
    const DataRange* find_data_range(uint32_t addr) const;
    void record_collision(uint32_t target,
                          uint32_t origin_addr,
                          const std::string& origin_name,
                          const std::string& origin_kind);

    uint32_t read_u32(uint32_t addr) const;
    uint16_t read_u16(uint32_t addr) const;
    void discover_one(uint32_t addr, CpuMode mode,
                      const std::string& name,
                      uint32_t source_addr = 0);
};

}  // namespace ndsrecomp
