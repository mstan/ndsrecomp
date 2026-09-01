#pragma once

#include <cstdint>
#include <string>

struct NdsDispatchEntry;
struct NdsStaticValidation;
struct NdsLiveBankInfo;

// Upper bound on the reject-cause table in live_overlay.cpp. The summary
// carries the counters as a flat array plus their names, so a consumer never
// has to be rebuilt when a cause is appended; only this cap does, and a
// static_assert in live_overlay.cpp fails the build if the table outgrows it.
constexpr uint32_t kNdsLiveRejectReasonCap = 64u;

void live_overlay_configure(bool enabled, bool auto_trigger,
                            uint32_t activation_delay_ms,
                            uint32_t auto_start_delay_ms,
                            uint32_t auto_cooldown_ms,
                            const char* command, const char* cache_dir,
                            const char* rom_sha1);
void live_overlay_shutdown();
void live_overlay_runtime_reset();
void live_overlay_register_cached_banks();
// Detailed transfer records are diagnostic-only and expensive at the generated
// branch rate. They are opt-in and sampled; ordinary live sharding keeps the
// compact Tier-3/root coverage path without touching the diagnostic ring.
void live_overlay_set_transfer_trace(bool enabled);
void live_overlay_note_tier3(int cpu, uint32_t pc);
// Tier-3 ENTRY hook -- once per tier3_run(), NOT once per interpreted
// instruction (beads-yjp.62). Entering the interpreter at `pc` is the proof
// that the guest code a dormant candidate was compiled from is resident right
// now, which is the one observation the compile-finish and cache-rescan
// preflight moments cannot make; the entry re-queues that candidate for
// another preflight. Returns whether `pc` is covered by a dormant candidate,
// so the caller can suppress its coverage filing without a second lookup.
// Costs one relaxed atomic load when the overlay is off or nothing is dormant.
bool live_overlay_note_tier3_entry(int cpu, uint32_t pc);
// Is `pc` inside a candidate whose compiled output we already hold but could
// not activate yet? Filing such an address as Tier-3 coverage commissions the
// live compiler to reproduce a shard that is already sitting in the cache.
bool live_overlay_dormant_covers(int cpu, uint32_t pc);
void live_overlay_note_transfer(int cpu, uint32_t source_pc, uint32_t target,
                                uint32_t lr, uint32_t cpsr, uint32_t type);
void live_overlay_note_lookup(int cpu, uint32_t pc, uint32_t target_pc,
                              uint32_t lr, uint32_t cpsr,
                              const NdsDispatchEntry* selected,
                              const NdsDispatchEntry* inactive,
                              uint32_t candidate_count,
                              const char* outcome);
uint32_t live_overlay_candidate_serial(int cpu, const NdsDispatchEntry* entry);
void live_overlay_note_cached_hit(uint32_t serial);
bool live_overlay_preflight_for_test(const NdsLiveBankInfo* info,
                                     char* error,
                                     uint32_t error_len);
bool live_overlay_info_for_test(const NdsLiveBankInfo* info,
                                const char* expected_rom_sha1,
                                char* error,
                                uint32_t error_len);
// Test-only: publish a resident bank without compiling a shard DLL, so the
// registration lifecycle can be pinned at the unit layer.
void live_overlay_publish_bank_for_test(int cpu,
                                        const char* bank_id,
                                        const char* candidate_id,
                                        const NdsDispatchEntry* dispatch,
                                        unsigned dispatch_len);
void live_overlay_note_write(int cpu, uint32_t pc, uint32_t addr,
                             uint32_t width, uint32_t old_value,
                             uint32_t new_value);
// The scheduler's per-round hook: a cheap countdown gate that runs the real
// poll body every 1024th call. The body is not round-granular work (it
// services a background compiler whose jobs take seconds) and running it every
// round cost 0.2-0.4 ms/frame in the NDS_EMU_OVERLAY bucket.
void live_overlay_poll();
// The poll body itself, unconditionally. For callers that mean "service the
// overlay NOW": an explicit trigger, the debug server, and any test that
// asserts on what one poll does.
void live_overlay_poll_now();
bool live_overlay_trigger_now();
std::string live_overlay_status_json();
std::string live_overlay_diagnostics_json(uint32_t max_entries);

// Flat snapshot of the live-bank state for the diagnostics record. The full
// status JSON is a nested document built through an ostringstream and carries
// a per-bank array; a periodic perf sample wants a handful of scalars, and
// the fields below (notably backend_tier, which the status JSON does not emit
// at all) are the ones that distinguish "player is running native gcc-built
// banks" from "player is running the interpreter" in a field bundle.
struct NdsLiveOverlaySummary {
    bool enabled;
    bool active;
    uint64_t banks_loaded;
    uint64_t banks_rejected;
    uint64_t native_hits;       // summed over every loaded bank
    uint64_t bank_rejects;      // per-bank guard rejects, summed
    uint32_t registered_banks;  // loaded AND registered, not superseded
    // Best backend that actually has a registered bank: 0 = untiered/none,
    // 1 = tcc (embedded fallback compiler), 2 = gcc (shipped shard cache).
    uint32_t backend_tier;
    uint64_t tier3[2];
    uint64_t mismatch_rejects[2];
    uint64_t futile_runs;
    bool auto_suppressed;
    // Queue policy, as it stands right now. pending_candidates is the backlog
    // the compiler last reported (or the count reloaded from the persisted
    // queue at startup, before any run of this session has finished);
    // batch_cap and cooldown_ms are the values the NEXT run will actually use,
    // not the configured base, so a field bundle shows whether the adaptive
    // path engaged rather than only what was configured.
    uint64_t pending_candidates;
    uint32_t batch_cap;
    uint32_t cooldown_ms;
    bool persisted_backlog;  // a backlog was reloaded at launch
    // Whether a compiler child is running RIGHT NOW. Emitted per diagnostics
    // interval so "did background compiling cost frame time" is answerable by
    // splitting a session's own intervals on this flag, instead of inferring
    // the compile windows from log file timestamps after the fact.
    bool busy;
    uint64_t runs_started;
    // beads-yjp.53: per-cause breakdown of every outcome that costs a shard
    // (or a resident bank's rows) its place in the dispatch index. Always on,
    // Release included. reason_count is the live table size; reason_names[i]
    // names reason_counts[i], so an ingest reads the pairs and never needs to
    // carry a copy of the table.
    //
    // The three families and the aggregate each belongs to:
    //   load_*   -> summed into banks_rejected (the shard never became a bank)
    //   drop_*   -> NOT in banks_rejected: the shard loaded and then lost its
    //              rows to another candidate for the same generation. This
    //              family is why a field bundle could show 58 loaded, 0
    //              rejected and only 33 registered with nothing to explain it.
    //   guard_*  -> summed into bank_rejects (a resident row refused at
    //              dispatch time), split by which half of the guard failed.
    uint32_t reason_count;
    const char* const* reason_names;
    uint64_t reason_counts[kNdsLiveRejectReasonCap];
    // Dispatch rows unregistered by the supersede path, and rows that the
    // replacements brought. A negative delta is coverage LOST mid-session,
    // which is invisible in any bank count.
    uint64_t rows_superseded;
    uint64_t rows_superseding;
    // beads-yjp.62 dormant-shard activation. A shard whose target guest code
    // is not resident at preflight time is held rather than discarded, so a
    // fresh install no longer burns compile runs reproducing shards it
    // already has. dormant_candidates is the count held RIGHT NOW;
    // dormant_activations is how many a Tier-3 entry proof later woke;
    // dormant_parked is how many gave up after kDormantMaxAttempts, dropping
    // both the record and its queued-paths key so the page goes back to the
    // compiler AND a republished shard can be prepared afresh; dormant_requeues
    // counts the entry proofs themselves.
    uint64_t dormant_candidates;
    uint64_t dormant_activations;
    uint64_t dormant_parked;
    uint64_t dormant_requeues;
};
void live_overlay_summary(NdsLiveOverlaySummary* out);

// Test-only: drive the queue policy without a compiler child. Reports the
// pending backlog the same way a finished run's log would, so the adaptive
// cadence and batch ramp can be pinned at the unit layer.
void live_overlay_note_backlog_for_test(uint64_t pending,
                                        uint64_t run_duration_ms);
uint32_t live_overlay_batch_cap_for_test();
uint32_t live_overlay_cooldown_for_test();
void live_overlay_poll_drain_counts_for_test(
    uint64_t* prepare_result_drains,
    uint64_t* maintenance_result_drains);

// Test-only: latch the futility suppression the way a proven-futile run does,
// so the queue-policy paths can be shown NOT to bypass it.
void live_overlay_suppress_for_test(const char* reason);
bool live_overlay_suppressed_for_test();
uint64_t live_overlay_runs_started_for_test();
// Test-only: put a prepared bank through the REAL adoption path, including
// the same-generation backend tie-break, without compiling a shard DLL.
// Returns whether the bank was accepted into the resident set.
bool live_overlay_commit_bank_for_test(int cpu, const char* bank_id,
                                       const char* candidate_id,
                                       const char* generation_id,
                                       unsigned backend_tier,
                                       const NdsDispatchEntry* dispatch,
                                       unsigned dispatch_len);
bool live_overlay_generation_registered_for_test(const char* generation_id,
                                                 unsigned* backend_tier_out);
// Test-only: put a prepared bank through the REAL admission gate -- the
// beads-yjp.62 guard-bytes preflight and, on failure, the dormant registry --
// instead of straight into the adoption path. `path_key` stands in for the
// canonical DLL path that identifies the candidate across attempts. Returns
// whether the bank became resident; false means it went dormant (or parked).
// Test-only: run a canonical path key through the REAL queueing path.
// Returns whether it was newly queued; false means the queued-paths set
// already held it and the prepare worker will never see it again. That set is
// the seam parking has to release, so this is how the release is observed.
bool live_overlay_enqueue_path_for_test(const char* path_key);
bool live_overlay_admit_bank_for_test(int cpu, const char* bank_id,
                                      const char* candidate_id,
                                      const char* generation_id,
                                      unsigned backend_tier,
                                      const char* path_key,
                                      const NdsDispatchEntry* dispatch,
                                      unsigned dispatch_len);
bool live_overlay_admit_bank_with_codegen_for_test(
    int cpu, const char* bank_id, const char* candidate_id,
    const char* generation_id, unsigned backend_tier,
    unsigned producer_codegen_version, const char* path_key,
    const NdsDispatchEntry* dispatch, unsigned dispatch_len);
