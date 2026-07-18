#pragma once

#include <string>

// Passive cumulative query. Consumers take stopped-route snapshots and
// subtract them; there is deliberately no arm/reset command.
#if defined(NDS_PROFILE_MEM_TIMING)
std::string nds_mem_timing_profile_json();
#else
inline std::string nds_mem_timing_profile_json() {
    return "{\"enabled\":false,\"mode\":\"off\",\"cells\":[]}";
}
#endif
