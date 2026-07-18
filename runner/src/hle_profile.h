#pragma once

#include <string>

// Passive cumulative query. Consumers take two snapshots and subtract them;
// there is deliberately no arm/reset command on the debug surface.
std::string nds_hle_profile_json();

