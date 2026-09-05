// host_prof_ring.cpp -- see host_prof_ring.h.

#include "host_prof_ring.h"

#include <algorithm>
#include <cstring>
#include <new>

const char* nds_hostprof_role_name(NdsHostProfRole role) {
    switch (role) {
        case NDS_HOSTPROF_ROLE_EMU: return "emu";
        case NDS_HOSTPROF_ROLE_RENDER: return "render";
        case NDS_HOSTPROF_ROLE_AUDIO: return "audio";
        case NDS_HOSTPROF_ROLE_LIVE_COMPILER: return "live_compiler";
        default: return "other";
    }
}

NdsHostProfRole nds_hostprof_role_from_name(const char* name) {
    if (!name || !name[0]) return NDS_HOSTPROF_ROLE_OTHER;
    if (std::strcmp(name, "emu") == 0) return NDS_HOSTPROF_ROLE_EMU;
    if (std::strcmp(name, "render") == 0) return NDS_HOSTPROF_ROLE_RENDER;
    if (std::strcmp(name, "audio") == 0) return NDS_HOSTPROF_ROLE_AUDIO;
    if (std::strcmp(name, "live_compiler") == 0)
        return NDS_HOSTPROF_ROLE_LIVE_COMPILER;
    return NDS_HOSTPROF_ROLE_OTHER;
}

// ── Ring ────────────────────────────────────────────────────────────────
bool NdsHostProfRing::init(uint32_t capacity) {
    shutdown();
    // Power of two only: the index-to-slot map is a mask, which is what keeps
    // push() free of a division on a 1 kHz path that must not be seen in a
    // profile of the profiler.
    if (capacity == 0u || (capacity & (capacity - 1u)) != 0u) return false;
    slots_ = new (std::nothrow) NdsHostProfSample[capacity];
    if (!slots_) return false;
    std::memset(slots_, 0, sizeof(NdsHostProfSample) * capacity);
    capacity_ = capacity;
    mask_ = capacity - 1u;
    written_.store(0u, std::memory_order_release);
    return true;
}

void NdsHostProfRing::shutdown() {
    delete[] slots_;
    slots_ = nullptr;
    capacity_ = 0u;
    mask_ = 0u;
    written_.store(0u, std::memory_order_release);
}

uint64_t NdsHostProfRing::evicted() const {
    const uint64_t w = written();
    return w > capacity_ ? w - capacity_ : 0u;
}

void NdsHostProfRing::push(const NdsHostProfSample& sample) {
    if (!slots_) return;
    const uint64_t index = written_.load(std::memory_order_relaxed);
    slots_[index & mask_] = sample;
    // Release AFTER the slot is filled: a reader that observes the new cursor
    // therefore observes a complete sample, which is what lets copy_slot()
    // treat a cursor comparison as a validity test.
    written_.store(index + 1u, std::memory_order_release);
}

uint64_t NdsHostProfRing::newest_qpc() const {
    const uint64_t w = written();
    if (!slots_ || w == 0u) return 0u;
    NdsHostProfSample s{};
    if (!copy_slot(w - 1u, &s)) return 0u;
    return s.qpc;
}

bool NdsHostProfRing::copy_slot(uint64_t index, NdsHostProfSample* out) const {
    const uint64_t before = written_.load(std::memory_order_acquire);
    if (index >= before) return false;
    if (before > capacity_ && index < before - capacity_) return false;
    *out = slots_[index & mask_];
    // Re-read the cursor: if the writer lapped far enough to reclaim this slot
    // while we were copying it, the bytes in `out` may be a splice of two
    // different stacks. A spliced stack is an INVENTED CALL EDGE, so it is
    // dropped rather than reported.
    const uint64_t after = written_.load(std::memory_order_acquire);
    if (after > capacity_ && index < after - capacity_) return false;
    return true;
}

uint32_t NdsHostProfRing::copy_window(uint64_t qpc_lo, uint64_t qpc_hi,
                                      NdsHostProfSample* out, uint32_t max,
                                      bool* out_torn,
                                      bool* out_window_older) const {
    if (out_torn) *out_torn = false;
    if (out_window_older) *out_window_older = false;
    if (!slots_ || !out || max == 0u) return 0u;
    const uint64_t w = written();
    if (w == 0u) return 0u;
    const uint64_t oldest = w > capacity_ ? w - capacity_ : 0u;

    // Backward scan. qpc is non-decreasing along the ring (one writer, one
    // clock), so the first sample older than qpc_lo ends the walk -- a "last
    // 5 seconds" query costs 5 seconds of ring, not the whole retention.
    uint32_t n = 0;
    bool exhausted = true;
    for (uint64_t idx = w; idx-- > oldest;) {
        NdsHostProfSample s{};
        if (!copy_slot(idx, &s)) {
            if (out_torn) *out_torn = true;
            continue;
        }
        if (s.qpc > qpc_hi) continue;
        if (s.qpc < qpc_lo) {
            exhausted = false;
            break;
        }
        if (n == max) {
            exhausted = false;
            break;
        }
        out[n++] = s;
    }
    // The scan ran off the oldest resident sample without reaching qpc_lo, so
    // the requested window extends into history the ring has already dropped.
    if (out_window_older) *out_window_older = exhausted;
    std::reverse(out, out + n);
    return n;
}

bool NdsHostProfRing::read_index(uint64_t index,
                                 NdsHostProfSample* out) const {
    if (!slots_ || !out) return false;
    return copy_slot(index, out);
}

uint32_t NdsHostProfRing::copy_all(NdsHostProfSample* out, uint32_t max,
                                   bool* out_torn) const {
    return copy_window(0u, ~static_cast<uint64_t>(0), out, max, out_torn,
                       nullptr);
}

// ── Histogram ───────────────────────────────────────────────────────────
bool NdsHostProfHist::init() {
    shutdown();
    slots_ = new (std::nothrow) Slot[kNdsHostProfHistSlots];
    if (!slots_) return false;
    for (uint32_t i = 0; i < kNdsHostProfHistSlots; ++i) {
        slots_[i].key.store(0u, std::memory_order_relaxed);
        slots_[i].count.store(0u, std::memory_order_relaxed);
    }
    total_.store(0u, std::memory_order_relaxed);
    overflow_.store(0u, std::memory_order_relaxed);
    return true;
}

void NdsHostProfHist::shutdown() {
    delete[] slots_;
    slots_ = nullptr;
}

void NdsHostProfHist::insert(uint64_t rip) {
    if (!slots_ || rip == 0u) return;
    total_.fetch_add(1u, std::memory_order_relaxed);
    const uint32_t home = hash(rip);
    for (uint32_t probe = 0; probe < kNdsHostProfHistProbes; ++probe) {
        Slot& slot = slots_[(home + probe) & (kNdsHostProfHistSlots - 1u)];
        const uint64_t key = slot.key.load(std::memory_order_relaxed);
        if (key == 0u) {
            // One writer, so no CAS: nobody else can claim this slot. The
            // count is published before the key so a concurrent reader that
            // sees the key never sees a zero count and mistake the slot for
            // unclaimed.
            slot.count.store(1u, std::memory_order_relaxed);
            slot.key.store(rip, std::memory_order_release);
            return;
        }
        if (key == rip) {
            slot.count.fetch_add(1u, std::memory_order_relaxed);
            return;
        }
    }
    // Eight occupied non-matching slots. Non-zero overflow means the table is
    // crowded and the top list is missing weight -- reported rather than hidden.
    overflow_.fetch_add(1u, std::memory_order_relaxed);
}

uint32_t NdsHostProfHist::top(NdsHostProfHistEntry* out, uint32_t max) const {
    if (!slots_ || !out || max == 0u) return 0u;
    // Insertion into a `max`-long sorted array, one linear pass: max is a few
    // hundred at most against 65536 slots, so this beats sorting the table.
    uint32_t filled = 0;
    for (uint32_t i = 0; i < kNdsHostProfHistSlots; ++i) {
        const uint64_t key = slots_[i].key.load(std::memory_order_acquire);
        if (key == 0u) continue;
        const uint64_t count = slots_[i].count.load(std::memory_order_relaxed);
        if (count == 0u) continue;
        if (filled == max && count <= out[max - 1u].count) continue;
        uint32_t pos = filled < max ? filled : max - 1u;
        while (pos > 0u && out[pos - 1u].count < count) {
            out[pos] = out[pos - 1u];
            --pos;
        }
        out[pos] = NdsHostProfHistEntry{key, count};
        if (filled < max) ++filled;
    }
    return filled;
}
