// diagnostics.h -- player-sendable diagnostic artifacts for release builds.

#pragma once

#include <cstdint>
#include <string>

struct NdsFrontendLiveStats;
struct NdsFrontendOptions;

void nds_diagnostics_configure(bool enabled, const char* directory,
                               uint32_t interval_ms);
void nds_diagnostics_enable_profile_environment();
bool nds_diagnostics_enabled();
std::string nds_diagnostics_directory();
std::string nds_diagnostics_make_path(const char* filename);
std::string nds_diagnostics_run_base(const char* stem);
std::string nds_diagnostics_dispatch_miss_log_path();
void nds_diagnostics_set_identity(const char* rom_sha1, const char* rom_name,
                                  const char* build_id);

void nds_diagnostics_start_performance_log(
    const NdsFrontendOptions& options);
void nds_diagnostics_maybe_write_performance_sample(
    const NdsFrontendLiveStats& stats);
void nds_diagnostics_stop_performance_log();
