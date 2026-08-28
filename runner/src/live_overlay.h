#pragma once

#include <cstdint>
#include <string>

struct NdsDispatchEntry;
struct NdsStaticValidation;
struct NdsLiveBankInfo;

void live_overlay_configure(bool enabled, bool auto_trigger,
                            uint32_t activation_delay_ms,
                            uint32_t auto_start_delay_ms,
                            uint32_t auto_cooldown_ms,
                            const char* command, const char* cache_dir,
                            const char* rom_sha1);
void live_overlay_shutdown();
void live_overlay_runtime_reset();
void live_overlay_register_cached_banks();
void live_overlay_note_tier3(int cpu, uint32_t pc);
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
void live_overlay_poll();
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
};
void live_overlay_summary(NdsLiveOverlaySummary* out);

// Test-only: drive the queue policy without a compiler child. Reports the
// pending backlog the same way a finished run's log would, so the adaptive
// cadence and batch ramp can be pinned at the unit layer.
void live_overlay_note_backlog_for_test(uint64_t pending,
                                        uint64_t run_duration_ms);
uint32_t live_overlay_batch_cap_for_test();
uint32_t live_overlay_cooldown_for_test();

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
