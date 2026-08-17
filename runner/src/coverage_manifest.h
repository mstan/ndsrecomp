// coverage_manifest.h — always-on Tier-3 (soft-miss) coverage capture and the
// player-dumpable manifest built from it. beads-yjp.28.
//
// An alpha release runs most of its guest code on the Tier-3 interpreter. That
// fallback used to be invisible outside a developer session: the per-address
// map was gated behind --discover-static-misses, was readable only over the
// TCP debug server, and the resident bytes were never captured at all. A player
// could finish the whole game and hand back nothing that raised static
// coverage.
//
// Addresses alone do not fix that. Measured over a 24270-entry MPH adventure
// capture, only 2.3% of Tier-3 coverage sits in immutable ROM-derived ranges
// where a bare address plus the ROM SHA-1 is enough to promote. The rest is
// runtime-materialized -- ITCM code copied at boot, per-area overlays, ARM7
// WRAM code -- where the bytes living at an address change over the run, so the
// address identifies nothing on its own.
//
// So we capture the executed code itself, content-addressed:
//
//   * Capture happens as the interpreter executes a page, not by snapshotting
//     regions at exit. A single end-of-run snapshot would hold only the LAST
//     resident overlay generation and would leave most of the session's
//     coverage unpromotable.
//   * Pages are keyed by (address, SHA-1 of contents) and deduplicated, so
//     overlay generations that share a virtual address separate naturally
//     instead of overwriting each other.
//   * Only pages the guest actually EXECUTED are stored. Data pages holding the
//     console nickname, WFC credential material and save buffers are never
//     touched. These files are meant to be handed between people, so that is a
//     structural property here rather than a scrubbing step applied afterwards.
//
// The hot path is one compare per interpreted instruction. bus.cpp already
// keeps a write generation per 4 KB exec page and every guest write -- slow bus
// path and generated-bank inline fast path alike -- funnels through
// runtime_note_code_write(), which bumps the write epoch below. So the cached
// page stays trusted until something writes anywhere, and only then do we pay a
// generation lookup to find out whether this page in particular changed.

#pragma once

#include <cstdint>

// Bumped by runtime_note_code_write() on every guest RAM write. A change means
// "some page somewhere may have been rewritten", which is the cue to re-check
// the cached page's generation -- not to re-hash it.
extern uint64_t g_coverage_write_epoch;

struct CoverageExecCache {
    uint32_t base = 0u;
    uint32_t generation = 0u;
    uint64_t epoch = 0u;
    bool valid = false;
};
extern CoverageExecCache g_coverage_exec_cache[2];

// Slow path: confirm the generation and, when the contents are new, store the
// page. Never call directly from the step loop -- go through the inline below.
void coverage_capture_exec_page(int cpu, uint32_t base, uint32_t pc);

// Hot path, once per interpreted instruction.
inline void coverage_note_exec(int cpu, uint32_t pc) {
    CoverageExecCache& cache = g_coverage_exec_cache[cpu & 1];
    const uint32_t base = pc & ~0xFFFu;
    if (cache.valid && cache.base == base &&
        cache.epoch == g_coverage_write_epoch) {
        return;
    }
    coverage_capture_exec_page(cpu & 1, base, pc);
}

// Identity the manifest carries so an ingest can tell whose run it is and
// which build produced it. Safe to call with nulls.
void coverage_manifest_set_identity(const char* rom_sha1, const char* rom_name,
                                    const char* build_id);

// Where automatic dumps go. Parts are written as
// <base>-coverage-<runstamp>-partNN.json, so a manifest is NEVER overwritten:
// a second session gets a new runstamp, and a long session that fills the page
// store rotates to the next part instead of dropping pages on the floor. A
// player can hand back the whole set and every part ingests independently.
void coverage_manifest_set_output(const char* base_path);

// Write the current part and start a new one. Called automatically when the
// page store fills; also called on exit to flush whatever is held.
bool coverage_manifest_flush_part(char* error, unsigned error_cap);

// Write the manifest. Returns false and fills `error` on failure. Writes via a
// temporary and renames, so a half-written file is never handed to anyone.
bool coverage_manifest_write(const char* path, char* error, unsigned error_cap);

struct CoveragePageStats {
    uint64_t captured;      // distinct (address, contents) pages stored
    uint64_t bytes;         // stored page bytes
    uint64_t dropped;       // pages refused because the store hit its cap
    uint64_t revisits;      // slow-path entries that found nothing new
};
CoveragePageStats coverage_page_stats();
void coverage_pages_reset();
