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
void live_overlay_note_write(int cpu, uint32_t pc, uint32_t addr,
                             uint32_t width, uint32_t old_value,
                             uint32_t new_value);
void live_overlay_poll();
bool live_overlay_trigger_now();
std::string live_overlay_status_json();
std::string live_overlay_diagnostics_json(uint32_t max_entries);
