/* Empty retail-BIOS dispatch tables for builds without user BIOS dumps.
   The runner only registers these when --freebios is off, and loading a
   retail BIOS is hash-verified first, so a no-dump build can never reach
   them with game code running. */
#include "runtime_arm.h"

const NdsDispatchEntry g_dispatch_arm9_bios[1] = {{0u, 0u, 0, 0}};
const unsigned g_dispatch_arm9_bios_len = 0u;
const NdsDispatchEntry g_dispatch_arm7_bios[1] = {{0u, 0u, 0, 0}};
const unsigned g_dispatch_arm7_bios_len = 0u;
