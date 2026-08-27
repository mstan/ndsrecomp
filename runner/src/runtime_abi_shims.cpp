// runtime_abi_shims.cpp — out-of-line definitions of the three per-instruction
// helpers that became `static inline` in recompiler/armv4t/runtime_arm.h when
// the deadline-bounded machinery landed (beads-yjp.42 phase 1).
//
// WHY THIS FILE EXISTS
// --------------------
// Live overlay shards are DLLs that IMPORT their runtime from the runner EXE
// through runner/src/runtime_exports.def. A shard compiled BEFORE this change
// has `runtime_tick`, `runtime_should_yield` and `runtime_unwinding` in its
// import table by name, and shards already published on disk are not
// recompiled when the runtime headers change (their DLLs stay loadable —
// NDS_LIVE_BANK_ABI_VERSION is unchanged because no struct layout moved). If
// the exported symbols simply disappeared, those shards would fail to load.
//
// So the symbols stay exported, and this is the ONLY translation unit that
// defines them. It defines NDS_RUNTIME_ABI_SHIMS before including the header,
// which makes the header emit prototypes instead of the inline bodies, and
// then reproduces those bodies EXACTLY. There is one behavioural contract and
// two spellings of it, never two contracts:
//
//   * a shard that predates the change calls these and gets the deadline;
//   * a shard recompiled after it inlines the same code from the header
//     (shards see the real header on the include path — they are not
//     header-flattened — and they charge cycles per instruction exactly like
//     static banks, so they must NOT be excluded from the deadline).
//
// The deadline itself is published only by the runner (publish_fast_limit in
// runtime_arm.cpp). `g_nds_fast_limit` and `g_nds_unwinding` are exported as
// DATA purely so a recompiled shard's inline copy reads the runner's storage;
// no shard ever writes either one.

#define NDS_RUNTIME_ABI_SHIMS 1
#include "runtime_arm.h"

extern "C" void runtime_tick(uint32_t cycles) {
    const unsigned long long next = g_runtime_cycles + cycles;
    if (next < g_nds_fast_limit) { g_runtime_cycles = next; return; }
    runtime_tick_slow(cycles);
}

extern "C" bool runtime_should_yield(void) {
    if (g_runtime_cycles < g_nds_fast_limit) return false;
    return runtime_should_yield_slow();
}

extern "C" bool runtime_unwinding(void) { return g_nds_unwinding != 0u; }
