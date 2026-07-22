#pragma once

#include <string>

// Profiling-build-only flat guest-function sampler. Reset is tied to the
// runtime lifecycle; the debug query is passive and cumulative.
void nds_function_heat_reset();
std::string nds_function_heat_json();
