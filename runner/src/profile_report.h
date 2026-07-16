// profile_report.h — shared NDS_PROFILE_GPU / NDS_PROFILE_SCHED report.
//
// One printer for every exit path (batch, interactive soak) so a profiled
// run always yields its numbers regardless of run mode. Prints nothing when
// no samples were collected (profiling env vars absent).

#pragma once

#include <cstdio>

void nds_profile_report(std::FILE* out);
