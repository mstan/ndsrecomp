// host_profile.h -- ALWAYS-ON host-side CPU sampler: which HOST function the
// runner is actually executing, for every build and every title, queryable
// after the fact.
//
// WHY THIS EXISTS. Kanden at half clock costs ~36 host cycles per guest cycle
// (~17 ms/frame of emu time), and every diagnostic the runner has attributes
// that time to GUEST addresses: emu_profile.h partitions it by guest-side
// machinery, pc_profile.h names guest PCs, dispatch_timing.h prices dispatch
// classes. None of them can say whether the host cycles are going into dispatch
// lookup and validation, bus decode, MMIO handlers, scheduler/CPU sync, gpu3d,
// gpu2d, audio, or the generated bodies themselves -- and "run it under an
// external profiler" is not an answer available for a player's session, a
// title we are not currently debugging, or a dip that already happened.
//
// SHAPE (DEBUG.md: always-on ring buffers, never arm-then-capture). A sampler
// thread starts at runner init in EVERY build, Release included, and samples
// the registered threads (see COST for the rate) into a fixed ring plus a
// running histogram
// (host_prof_ring.h). Probes QUERY that ring for the window they care about.
// There is nothing to arm, nothing to start, nothing to reset; a probe that
// joins late reads backward. The one thing this subsystem must never become is
// "arm a trace, run the fight, dump it" -- by the time anything has armed, the
// frame in question is gone, and an empty capture then reads as "no cost here".
//
// HOW A SAMPLE IS TAKEN, and why it is safe to do this in a shipped build. The
// suspended window contains exactly three OS calls and one memcpy:
//
//   SuspendThread -> GetThreadContext(RIP/RSP/RBP/GPRs)
//                 -> memcpy 64 KB of stack into a PREALLOCATED buffer
//                 -> ResumeThread
//
// No allocation, no lock, no dbghelp, nothing that can block, between suspend
// and resume. That constraint is not stylistic: the suspended thread may be
// holding the process heap lock or the loader lock, and any sampler that takes
// one of those in the suspended window deadlocks the process permanently. The
// unwind then runs on the COPY, afterwards, with every read bounds-checked
// (host_unwind_x64.h) -- so a torn frame or an over-deep stack ends a walk with
// a counted stop reason instead of faulting, which matters because MinGW has no
// SEH to catch a fault.
//
// KNOBS
//   NDS_HOSTPROF=off|on        default ON. `off` starts no thread and
//                              allocates nothing at all.
//   NDS_HOSTPROF_HZ=<1..10000> default 250 -- see COST, this number is a
//                              measurement result, not a preference.
//   NDS_HOSTPROF_RING=<pow2>   default 262144 samples (36 MB, ~17 min at
//                              250 Hz).
//
// COST, measured rather than asserted. One suspend/context/resume cycle costs
// ~33 us median (p90 52 us, min 12 us) and the stack copy inside it costs
// NOTHING measurable -- 0 bytes, 16 KB, 64 KB and 128 KB all benchmark at the
// same median -- so the whole bill is the three syscalls and no code here can
// make it cheaper. That fixes
// the arithmetic: the sampled thread loses hz * 33 us of wall time per second,
// so 1 kHz is 3.3 percent, 500 Hz is 1.7 percent, and only 250 Hz (0.8 percent)
// fits the budget an always-on Release feature inside the thread we are
// optimizing is allowed. 250 Hz is still 7500 samples in a 30 s window and
// stretches the ring to ~17 minutes of retention, which makes the
// after-the-fact query MORE likely to still hold the window someone asks
// about. NDS_HOSTPROF_HZ=1000 is a deliberate opt-in, never a default.
//
// The sampler also accumulates its own suspended-window and unwind times and
// reports them in hostprof_top / hostprof_status / the dump header, so the
// observer's cost travels with every observation instead of being taken on
// faith. Secondary threads (render/audio/live compiler) are sampled at a
// quarter of the emu rate, because the cost is entirely per-suspend.
#pragma once

#include <cstdint>
#include <string>

#include "host_prof_ring.h"

// Start the sampler. Reads the knobs above; safe to call twice (the second call
// is a no-op). Call from the emu thread at init, BEFORE the first frame -- the
// point of an always-on ring is that the boot is inside it too.
void nds_hostprof_start();
void nds_hostprof_stop();
bool nds_hostprof_running();

// Register the CALLING thread as a sampling target. The thread registers itself
// rather than being registered by someone else so that the sampler can take the
// thread's stack top from GetCurrentThreadStackLimits() -- that bound is what
// makes the suspended-window memcpy provably in-bounds without a VirtualQuery.
//
// The emu thread is registered by nds_hostprof_start(). Render/audio/live
// compiler threads are optional: register them and time the emu thread spends
// BLOCKED on them becomes attributable instead of an unexplained gap.
void nds_hostprof_register_current_thread(NdsHostProfRole role);
void nds_hostprof_unregister_current_thread();

// RAII form, and the one worker threads should use. A thread that registers and
// then exits without retiring its slot leaves a dead thread handle in the
// target table: the sampler then pays a failing SuspendThread every tick for
// the rest of the session and the slot is never reusable. Scoping it makes that
// impossible to forget on an early return.
struct NdsHostProfThreadScope {
    explicit NdsHostProfThreadScope(NdsHostProfRole role) {
        nds_hostprof_register_current_thread(role);
    }
    ~NdsHostProfThreadScope() { nds_hostprof_unregister_current_thread(); }
    NdsHostProfThreadScope(const NdsHostProfThreadScope&) = delete;
    NdsHostProfThreadScope& operator=(const NdsHostProfThreadScope&) = delete;
};

// ── Query surface (debug_server) ─────────────────────────────────────────

// Top-K host RIPs by SELF time with module + RVA.
//
// `window_sec` <= 0 means the whole run, answered from the running histogram in
// one pass over a 1 MB table. A positive window is answered from the ring,
// scanning back only as far as the window reaches. Both are the same
// no-arm/no-reset contract as every other surface here; for an interval that
// straddles nothing in particular, snapshot twice and subtract.
std::string nds_hostprof_top_json(double window_sec, unsigned top);

// Whether the sampler is on, at what rate, how much history it holds, and what
// it costs -- the questions a probe asks before trusting a table.
std::string nds_hostprof_status_json();

// Write the ring for a window to `path`, plus the module map (base/size/path of
// the exe and every loaded module, shard DLLs included) needed to turn the raw
// RIPs back into symbols offline.
//
// Format: one UTF-8 JSON metadata line (terminated by '\n') carrying the module
// map and the run identity, followed by NdsHostProfSample records written
// verbatim, back to back. The sample count is derivable from the file size, and
// is also patched into the header line so a truncated file is detectable.
// tools/hostprof_symbolize.py reads it.
//
// `window_sec` <= 0 dumps the whole resident ring. `end_sec_ago` shifts the
// window's END backwards, so [t0,t1] is expressible as
// window_sec = t1 - t0, end_sec_ago = now - t1.
//
// Written via a temporary and renamed, so a half-written dump is never handed
// to anyone. Returns false and fills `error` on failure.
bool nds_hostprof_dump(const char* path, double window_sec, double end_sec_ago,
                       char* error, unsigned error_cap,
                       std::string* out_summary_json);

// Shutdown bundle hook (diagnostics.cpp). Dumps the WHOLE resident ring to
// `path`. Separate from nds_hostprof_dump only so the intent is legible at the
// call site; it is the same writer with the same format.
bool nds_hostprof_write_bundle(const char* path, char* error,
                               unsigned error_cap,
                               std::string* out_summary_json);

// ── Test hook ────────────────────────────────────────────────────────────
// Capture the CALLING thread's stack through the identical copy-then-unwind
// path a sample uses (minus the suspend, which a thread cannot apply to
// itself). Exists so the unit test can compare this unwinder against the OS's
// own RtlCaptureStackBackTrace on real compiler-generated .pdata -- the only
// gate that proves the unwinder against real prologues rather than against a
// table the test wrote itself. Returns the frame count.
unsigned nds_hostprof_self_stack(uint64_t* out_frames, unsigned max,
                                 uint8_t* out_stop);
// Force a module-map refresh (normally done by the sampler thread every 2 s).
// The self-stack hook needs a map, and a test has no sampler thread.
bool nds_hostprof_refresh_modules();
