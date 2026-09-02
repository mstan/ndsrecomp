// host_prof_probe_dll.c -- a scratch DLL for the beads-yjp.70 regression test.
//
// It exists only to be loaded, walked, and then FreeLibrary'd out from under
// the host sampler's module map. The one property the test needs from it is
// REAL unwind data: a .pdata/.xdata pair produced by the real toolchain for a
// real prologue, so the map's snapshot-time parse and the unwinder's
// UNWIND_INFO reads both have something to chew on. Hence the deliberately
// non-trivial frames below -- saved non-volatiles and a nested call, rather
// than a leaf that the compiler could emit with no unwind entry at all.
//
// Nothing here is allowed to depend on the host executable: unlike the
// live-overlay shard tests, this module must be unloadable with no residual
// references, or the test it serves would prove nothing.

#include <stdint.h>

__declspec(dllexport) uint64_t nds_probe_leaf(uint64_t a, uint64_t b) {
    return a * 6364136223846793005ull + b;
}

__declspec(dllexport) uint64_t nds_probe_frame(uint64_t seed) {
    // Enough live values across a call to force the prologue to save
    // non-volatile registers and allocate a frame.
    volatile uint64_t scratch[24];
    uint64_t acc = seed;
    for (unsigned i = 0; i < 24u; ++i) {
        scratch[i] = acc;
        acc = nds_probe_leaf(acc, i);
    }
    for (unsigned i = 0; i < 24u; ++i) acc ^= scratch[i];
    return acc;
}
