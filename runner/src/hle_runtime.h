#pragma once

#include <string>

// Parse the strict opt-in HLE policy. Invalid values, or requesting HLE from
// a runner built without NDS_ENABLE_HLE, return false and print a diagnostic.
bool nds_hle_configure_from_environment();
void nds_hle_print_policy();

// Power-on reset clears observations, but not the process-wide policy.
void nds_hle_reset_diagnostics();

// Passive status query for the debug server. It includes cumulative counters
// and the bounded decision ring; querying never changes execution policy.
std::string nds_hle_status_json();
