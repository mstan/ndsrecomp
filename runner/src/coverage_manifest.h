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
// What the manifest records is shaped by how each kind of observation is
// actually distributed, because the first real player submission was 202 MB
// carrying 1.27 MB of code:
//
//   * ROOTS -- where native code fell into the interpreter -- are DENSE. The
//     interpreter also re-enters wherever an IRQ or DMA stall interrupted it,
//     so over a session roots converge on every interpreted instruction: 92% of
//     the addresses in that submission had another root one instruction away,
//     in runs up to 415 long. They are stored as per-page bitmaps (ARM at word
//     stride, Thumb at halfword stride) plus per-256-byte-block hit counters,
//     both per code generation and in a never-evicted session-wide map. Every
//     address and mode is preserved; only per-address hit counts become a block
//     share, and no consumer used those.
//   * CALL and INDIRECT targets are SPARSE and individually meaningful, so they
//     stay one record each -- there are only a few thousand.
//   * The CALLER on a record is diagnostic, not identifying, and keying storage
//     on it made both the manifest and the resident map scale with the call
//     graph instead of the code (2,185,955 records for 61,036 tuples). Only a
//     few distinct callers per entry are kept; the rest fold into one record.
//
// Together those took the same session from 202 MB to ~2.6 MB with nothing
// dropped. Deliberately NOT done: omitting page bytes that are verbatim ROM
// content. 93.5% of captured pages are, which makes it the tempting cut, but
// all the page bytes together are under 1% of the original file, and they are
// the only part that cannot be reconstructed and the only thing that lets an
// ingest verify a stranger's manifest against the ROM.
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
    uint32_t stored_index = UINT32_MAX;
    uint64_t epoch = 0u;
    bool valid = false;
};
extern CoverageExecCache g_coverage_exec_cache[2];

// Slow path: confirm the generation and, when the contents are new, store the
// page. Never call directly from the step loop -- go through the inline below.
void coverage_capture_exec_page(int cpu, uint32_t base, uint32_t pc);

// Associate a dispatch/resume observation with the exact RAM page generation
// that was resident when the target was observed. Session-wide entry maps are
// insufficient for overlays because several generations reuse the same PC.
void coverage_note_generation_entry(int cpu, uint32_t pc, bool thumb,
                                    uint8_t kind, uint32_t caller);

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

// A bounded, recency-ordered snapshot for the live compiler, split in two so
// the emulation thread pays only for the copy (beads-yjp.59). CAPTURE reads the
// resident page store, the guest page payloads and the Tier-3 counters, so it
// must run on the emulation thread; WRITE is base64, a sort, a JSON build and
// an atomic file replace over that frozen copy, and runs on a worker. The
// document is byte-identical either way. Release the snapshot exactly once.
struct CoverageLiveSnapshot;
CoverageLiveSnapshot* coverage_manifest_capture_live_snapshot(
    uint32_t max_pages);
bool coverage_manifest_write_captured_snapshot(
    const CoverageLiveSnapshot* snapshot, const char* path, char* error,
    unsigned error_cap);
void coverage_manifest_release_snapshot(CoverageLiveSnapshot* snapshot);

struct CoveragePageStats {
    uint64_t captured;      // distinct (address, contents) pages stored
    uint64_t bytes;         // stored page bytes
    uint64_t dropped;       // pages refused because the store hit its cap
    uint64_t revisits;      // slow-path entries that found nothing new
    uint64_t replaced;      // stale per-address versions evicted for newer code
};
CoveragePageStats coverage_page_stats();
void coverage_pages_reset();
