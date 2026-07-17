// Host-context owner for the opt-in melonDS compute renderer.
// GPL-3.0-or-later; see THIRD_PARTY_ATTRIBUTION.md.
#pragma once

bool nds_compute_host_start();
bool nds_compute_host_make_current();
void nds_compute_host_stop();
bool nds_compute_host_active();
