// host_prof_ring.h -- storage for the always-on host CPU sampler: a fixed
// sample ring plus a running self-time histogram keyed by host RIP.
//
// WHY THIS EXISTS. Every perf surface in this runner attributes time to GUEST
// addresses -- emu_profile.h partitions emu time by guest-side machinery,
// pc_profile.h names guest PCs, dispatch_timing.h prices dispatch classes. None
// of them can answer the question the Kanden measurement raised: the runner
// burns ~36 host cycles per guest cycle, and the host-side breakdown between
// dispatch lookup/validation, bus decode, MMIO handlers, scheduler/CPU sync,
// gpu3d, gpu2d, audio and the generated bodies themselves is invisible. Getting
// it used to mean reproducing the session under an external profiler, which is
// exactly the step field diagnostics exist to delete.
//
// TWO POPULATIONS, and the reason there are two rather than one:
//
//   * The RING holds whole stacks with timestamps. It is what makes INCLUSIVE
//     attribution possible ("how much time is under the dispatch loop", which
//     no leaf histogram can answer) and what makes a [t0,t1] query possible
//     ("the 4 seconds where the fight dipped", which no whole-run total can).
//     It costs 144 bytes a sample, so a 1<<18 ring is 36 MB and, at 1 kHz,
//     just over four minutes of continuous history.
//   * The HISTOGRAM holds leaf RIP counts for the WHOLE RUN. It exists so the
//     common query -- rank host functions by self time -- is one pass over a
//     1 MB table instead of a pass over a 36 MB ring, and so that the answer
//     survives a session longer than the ring's retention. It is monotonic and
//     never reset, which makes an interval measurement the same
//     snapshot-twice-and-subtract every other surface here uses.
//
// Ring-buffer philosophy (DEBUG.md): always on, Release included, never armed,
// no start/stop, no reset. A probe joins late and reads backward. Never
// arm-then-run-then-dump -- by the time an LLM or a human has armed a trace the
// interesting frame is already gone, and worse, "no samples" then reads as "no
// events" when it really means "not recording yet".
//
// THREADING. Exactly one writer -- the sampler thread -- and readers on the emu
// thread (the debug-server pump and the diagnostics shutdown writer). The write
// cursor is released after the slot is filled and re-read after a reader copies
// a slot, so a reader can tell that a sample it copied was overwritten
// mid-copy rather than silently returning a torn stack. At 1 kHz against a
// 1<<18 ring a full wrap takes 262 s and a query takes milliseconds, so this
// path is defensive rather than routine -- but a profiler that can hand back a
// spliced stack is a profiler that invents call edges, which is the one failure
// mode that would make the whole subsystem untrustworthy.
#pragma once

#include <atomic>
#include <cstdint>

#include "host_unwind_x64.h"

// Which registered thread a sample came from. The emu thread is the one the
// perf question is about; the others are registered so that time the emu thread
// is BLOCKED ON them (the 2D worker pool, the 3D thread, the audio callback,
// the live compiler) is attributable rather than showing up as an unexplained
// gap.
enum NdsHostProfRole : uint8_t {
    NDS_HOSTPROF_ROLE_EMU = 0,
    NDS_HOSTPROF_ROLE_RENDER,
    NDS_HOSTPROF_ROLE_AUDIO,
    NDS_HOSTPROF_ROLE_LIVE_COMPILER,
    NDS_HOSTPROF_ROLE_OTHER,
    NDS_HOSTPROF_ROLE_COUNT
};

const char* nds_hostprof_role_name(NdsHostProfRole role);
NdsHostProfRole nds_hostprof_role_from_name(const char* name);

// 144 bytes: 16 bytes of header and 16 frames. Deliberately POD and
// fixed-width -- the binary dump writes these structs verbatim, so the offline
// symbolizer reads one struct format forever and a version field in the dump
// header covers any change.
struct NdsHostProfSample {
    uint64_t qpc;       // QueryPerformanceCounter at capture
    uint32_t tid;       // OS thread id
    uint8_t depth;      // valid entries in frames[]
    uint8_t role;       // NdsHostProfRole
    uint8_t stop;       // NdsHostUnwindStop: why the walk ended
    uint8_t reserved;
    uint64_t frames[kNdsHostUnwindMaxFrames];   // innermost first
};

static_assert(sizeof(NdsHostProfSample) == 16u + 8u * kNdsHostUnwindMaxFrames,
              "the dump format is the struct layout; keep it packed");

// ── Sample ring ─────────────────────────────────────────────────────────
class NdsHostProfRing {
public:
    // `capacity` must be a power of two. Allocates once, up front: an always-on
    // ring that allocates on first use has an unobserved window at exactly the
    // moment a startup regression would be visible, and an allocation inside
    // the sampler would be an allocation the sampler thread can be holding when
    // it suspends the emu thread.
    bool init(uint32_t capacity);
    void shutdown();
    bool ready() const { return slots_ != nullptr; }
    uint32_t capacity() const { return capacity_; }

    // Total samples ever pushed, which can far exceed capacity(). The delta of
    // two reads is how many samples a window covers even when the ring wrapped.
    uint64_t written() const {
        return written_.load(std::memory_order_acquire);
    }
    // Samples pushed but already evicted -- written() - capacity(), floored.
    uint64_t evicted() const;

    // Writer side. Sampler thread only.
    void push(const NdsHostProfSample& sample);

    // Copy the resident samples whose qpc lies in [qpc_lo, qpc_hi] into `out`,
    // OLDEST FIRST, at most `max` of them (keeping the NEWEST `max` when the
    // window holds more). Returns how many were written.
    //
    // Because qpc is non-decreasing along the ring, the scan walks backward
    // from the newest sample and stops at the first sample older than qpc_lo,
    // so a "last 5 seconds" query touches 5 seconds of ring and not 4 minutes
    // of it. *out_torn is set when a slot was overwritten while being copied
    // (that sample is dropped, never returned half-spliced); *out_window_older
    // is set when the requested window starts before the oldest resident
    // sample, i.e. the answer is truncated by eviction.
    uint32_t copy_window(uint64_t qpc_lo, uint64_t qpc_hi,
                         NdsHostProfSample* out, uint32_t max,
                         bool* out_torn, bool* out_window_older) const;

    // Copy every resident sample, oldest first, up to `max`. Used by the
    // shutdown bundle writer, which wants the whole ring.
    uint32_t copy_all(NdsHostProfSample* out, uint32_t max,
                      bool* out_torn) const;

    // Copy the sample at ABSOLUTE index `index` (as counted by written()), if
    // it is still resident and was not reclaimed mid-copy. This is what lets
    // the dump writer stream oldest-to-newest with a 256-sample scratch buffer
    // instead of materialising a 36 MB ring on the emu thread.
    bool read_index(uint64_t index, NdsHostProfSample* out) const;

    // Newest sample's qpc, or 0 when the ring is empty. Lets a "last N seconds"
    // query anchor on the sampler's own clock rather than on the querying
    // thread's, which matters when the emu thread has been stalled.
    uint64_t newest_qpc() const;

private:
    bool copy_slot(uint64_t index, NdsHostProfSample* out) const;

    NdsHostProfSample* slots_ = nullptr;
    uint32_t capacity_ = 0;
    uint32_t mask_ = 0;
    std::atomic<uint64_t> written_{0};
};

// ── Running self-time histogram ──────────────────────────────────────────
//
// Open-addressed, no eviction, no rehash, keyed by leaf RIP -- the same shape
// as pc_profile.h's guest-PC tables, for the same reason: a claimed slot keeps
// its key for the life of the process, so a per-interval delta is a per-slot
// subtraction against an earlier snapshot and entries never move between them.
constexpr uint32_t kNdsHostProfHistSlots = 1u << 16;   // 1 MB
constexpr uint32_t kNdsHostProfHistProbes = 8u;

struct NdsHostProfHistEntry {
    uint64_t rip;
    uint64_t count;
};

class NdsHostProfHist {
public:
    bool init();
    void shutdown();
    bool ready() const { return slots_ != nullptr; }

    void insert(uint64_t rip);          // sampler thread only
    uint64_t total() const { return total_.load(std::memory_order_relaxed); }
    uint64_t overflow() const {
        return overflow_.load(std::memory_order_relaxed);
    }
    // Top `max` entries by count, count-descending. Returns how many written.
    uint32_t top(NdsHostProfHistEntry* out, uint32_t max) const;

private:
    struct Slot {
        std::atomic<uint64_t> key;
        std::atomic<uint64_t> count;
    };
    // Knuth multiplicative hash over the whole 64-bit RIP, taking the high bits
    // so that the low-order structure of an instruction address (x64 code is
    // byte-aligned but hot call targets cluster on 16-byte boundaries) cannot
    // collapse the index.
    static uint32_t hash(uint64_t rip) {
        return static_cast<uint32_t>((rip * 0x9E3779B97F4A7C15ull) >> 48) &
               (kNdsHostProfHistSlots - 1u);
    }

    Slot* slots_ = nullptr;
    std::atomic<uint64_t> total_{0};
    std::atomic<uint64_t> overflow_{0};
};
