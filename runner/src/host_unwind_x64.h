// host_unwind_x64.h -- x64 stack unwinder driven by the PE .pdata/.xdata
// tables, run over a COPY of the sampled thread's stack.
//
// WHY THIS EXISTS, and why it is not StackWalk64.
//
// The host sampler (host_profile.h) has to walk another thread's stack. The
// only way to see that thread's registers is SuspendThread + GetThreadContext,
// and the well-known hazard of that pair is that the suspended thread may be
// holding a user-mode lock -- the process heap lock, the loader lock, dbghelp's
// own lock. Anything the sampler calls between suspend and resume that takes
// one of those deadlocks the process, permanently, in a Release build shipped
// to players. StackWalk64 allocates and takes dbghelp's lock, so it is
// disqualified outright; RtlVirtualUnwind allocates nothing but reads the
// target's live stack, which means it can only run inside the suspended window
// and can fault on a torn frame -- and MinGW has no SEH to catch that fault.
//
// So the sampler does the only thing that is both lock-free and fault-free:
// inside the suspended window it copies REGISTERS and a bounded slice of the
// STACK into preallocated buffers and resumes immediately. Every unwind
// decision then happens here, afterwards, against those copies, with every
// single read bounds-checked. A malformed frame, a stack deeper than the copy,
// a module without unwind data -- all of them end the walk with a stop reason,
// and none of them can fault or block.
//
// WHY .pdata IS ENOUGH WITHOUT PDBs. x64 Windows requires every non-leaf
// function to publish its prologue in the image's .pdata/.xdata tables; that is
// how the OS unwinds exceptions. Those tables are in the PE image, not in a
// PDB, so this works on a stripped Release nds_runner.exe and on shard DLLs
// alike -- unlike frame-pointer chasing, which -O3 and -fomit-frame-pointer
// make useless. Symbol NAMES still need the offline step
// (tools/hostprof_symbolize.py against the mingw DWARF); what this file
// produces is the RVA chain, which is all the ring has to store.
//
// KNOWN LIMITS, stated rather than hidden, because a profiler that lies about
// its blind spots is worse than one that has none:
//   * Mid-EPILOGUE samples. RtlVirtualUnwind detects an epilogue by
//     disassembling forward from RIP; this does not. A sample taken between a
//     function's stack teardown and its `ret` therefore attributes the PARENT
//     frame wrongly (the leaf frame -- the self-time attribution, which is the
//     number this whole subsystem exists to produce -- is always exact,
//     because it is just RIP). Epilogues are a few instructions out of a
//     function, so this is a sub-percent effect on the inclusive tables and
//     zero effect on the self-time tables.
//   * Code with no .pdata entry. A true leaf function has none by design and
//     is handled (return address at [RSP]); a TinyCC-built live shard may have
//     none either, and there the walk stops with NO_PDATA. The stop-reason
//     histogram in the dump says how often that happened, so a reader can
//     bound it instead of guessing.
//   * Stacks deeper than the copied slice end with READ_FAILED, again counted.
#pragma once

#include <cstdint>

// Frames captured per sample. 16 is deep enough to reach from a generated guest
// body out through the dispatch loop, the scheduler round and the frontend
// frame into main(), which is the chain every host-attribution question here
// needs; deeper than that and the ring's per-sample cost stops being worth it.
constexpr unsigned kNdsHostUnwindMaxFrames = 16u;

// The x64 integer register file, indexed by the SAME encoding the unwind codes
// use in their operation-info nibble: 0=RAX 1=RCX 2=RDX 3=RBX 4=RSP 5=RBP
// 6=RSI 7=RDI 8..15=R8..R15. Sharing one indexing scheme with the data format
// is what keeps UWOP_PUSH_NONVOL/UWOP_SAVE_NONVOL/UWOP_SET_FPREG to a single
// array store each, with no translation table to get wrong.
struct NdsHostUnwindRegs {
    uint64_t gpr[16];
    uint64_t rip;
};

inline uint64_t& nds_unwind_rsp(NdsHostUnwindRegs& r) { return r.gpr[4]; }
inline uint64_t nds_unwind_rsp(const NdsHostUnwindRegs& r) { return r.gpr[4]; }
inline uint64_t& nds_unwind_rbp(NdsHostUnwindRegs& r) { return r.gpr[5]; }

// Everything the unwinder is allowed to touch. Both callbacks are pure
// lookups; neither may allocate, block, or fault -- the real implementations
// serve stack reads from the sampler's copy and image reads from either the
// never-unloaded main image or a private copy of a DLL's unwind sections, and
// the unit test serves both from plain arrays.
struct NdsHostUnwindEnv {
    // Read exactly `len` bytes of the sampled address space at `addr`. MUST
    // return false (not garbage, not a fault) for any address the caller
    // cannot vouch for.
    bool (*read)(void* ctx, uint64_t addr, void* dst, uint32_t len);
    // Locate the module containing `pc` and its exception directory. `pdata_rva`
    // / `pdata_size` describe the RUNTIME_FUNCTION array; size 0 means the
    // module has no unwind data at all.
    bool (*pdata)(void* ctx, uint64_t pc, uint64_t* image_base,
                  uint32_t* pdata_rva, uint32_t* pdata_size);
    void* ctx;
};

// Why the walk ended. Recorded per sample and aggregated in the dump, because
// "how much of this profile is truncated, and where" is not optional context
// for a stack-based inclusive table.
enum NdsHostUnwindStop : uint8_t {
    NDS_HOST_UNWIND_DEPTH = 0,    // filled `max` frames; deeper frames exist
    NDS_HOST_UNWIND_ROOT,         // return address 0 -- thread entry, complete
    NDS_HOST_UNWIND_NO_MODULE,    // pc outside every module we know about
    NDS_HOST_UNWIND_NO_PDATA,     // module has no entry for a non-leaf pc
    NDS_HOST_UNWIND_READ_FAILED,  // stack slice exhausted or unreadable image
    NDS_HOST_UNWIND_BAD_INFO,     // unwind data we refuse to trust
    NDS_HOST_UNWIND_STOP_COUNT
};

const char* nds_host_unwind_stop_name(NdsHostUnwindStop stop);

// Walk from `regs` (which the caller filled from a CONTEXT) and write the
// return-address chain, innermost first, into out_frames[0..max). out_frames[0]
// is always regs.rip when max >= 1, so the self-time attribution never depends
// on any unwind step succeeding. Returns the number of frames written and, if
// out_stop is non-null, why it stopped.
//
// `regs` is taken BY VALUE: the walk mutates the register file as it unwinds,
// and a sampler that wanted to re-walk the same sample differently would
// otherwise find its context shredded.
unsigned nds_host_unwind(const NdsHostUnwindEnv& env,
                         NdsHostUnwindRegs regs,
                         uint64_t* out_frames, unsigned max,
                         NdsHostUnwindStop* out_stop);
