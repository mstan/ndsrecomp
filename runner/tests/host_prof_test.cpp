// host_prof_test.cpp -- host CPU sampler coverage: the sample ring's residency
// rules, the leaf histogram, and the x64 .pdata/.xdata unwinder.
//
// The unwinder is gated TWO ways on purpose. A synthetic table (built here,
// byte by byte, with a hand-laid stack) pins the format parsing
// deterministically on every platform and would catch a misread field or a
// mis-sized opcode; but a synthetic table only proves the unwinder agrees with
// the test author's reading of the spec. So on Windows/x64 the same unwinder is
// also run over the REAL stack of a real call chain, compiled by the real
// toolchain, and compared frame for frame against the OS's own
// RtlCaptureStackBackTrace. That second gate is the one that would catch a
// wrong reading of the spec.

#include "host_prof_ring.h"
#include "host_profile.h"
#include "host_unwind_x64.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

int g_failures = 0;

#define CHECK(condition)                                                  \
    do {                                                                  \
        if (!(condition)) {                                               \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n",             \
                         __FILE__, __LINE__, #condition);                 \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                    \
    do {                                                                  \
        const unsigned long long va = (unsigned long long)(a);            \
        const unsigned long long vb = (unsigned long long)(b);            \
        if (va != vb) {                                                   \
            std::fprintf(stderr,                                          \
                         "%s:%d: CHECK_EQ failed: %s (0x%llx) != %s "     \
                         "(0x%llx)\n",                                    \
                         __FILE__, __LINE__, #a, va, #b, vb);             \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

NdsHostProfSample make_sample(uint64_t qpc, uint64_t leaf) {
    NdsHostProfSample s{};
    s.qpc = qpc;
    s.tid = 42u;
    s.depth = 1u;
    s.role = NDS_HOSTPROF_ROLE_EMU;
    s.stop = NDS_HOST_UNWIND_ROOT;
    s.frames[0] = leaf;
    return s;
}

// ── Ring ────────────────────────────────────────────────────────────────
void test_ring_basics() {
    NdsHostProfRing ring;
    CHECK(!ring.init(0u));          // capacity must be a power of two
    CHECK(!ring.init(1000u));
    CHECK(ring.init(8u));
    CHECK(ring.ready());
    CHECK_EQ(ring.capacity(), 8u);
    CHECK_EQ(ring.written(), 0u);
    CHECK_EQ(ring.newest_qpc(), 0u);

    for (uint64_t i = 0; i < 5u; ++i)
        ring.push(make_sample(100u + i * 10u, 0x1000u + i));
    CHECK_EQ(ring.written(), 5u);
    CHECK_EQ(ring.evicted(), 0u);
    CHECK_EQ(ring.newest_qpc(), 140u);

    NdsHostProfSample out[16]{};
    bool torn = false, older = false;
    // Whole resident contents, oldest first.
    uint32_t n = ring.copy_all(out, 16u, &torn);
    CHECK_EQ(n, 5u);
    CHECK(!torn);
    CHECK_EQ(out[0].qpc, 100u);
    CHECK_EQ(out[4].qpc, 140u);
    CHECK_EQ(out[0].frames[0], 0x1000u);
    CHECK_EQ(out[4].frames[0], 0x1004u);

    // A window is inclusive on both ends and still oldest-first.
    n = ring.copy_window(115u, 135u, out, 16u, &torn, &older);
    CHECK_EQ(n, 2u);
    CHECK_EQ(out[0].qpc, 120u);
    CHECK_EQ(out[1].qpc, 130u);
    // The window's start is inside the resident history, so nothing was lost.
    CHECK(!older);

    // A window reaching before the oldest resident sample reports truncation.
    n = ring.copy_window(0u, 1000u, out, 16u, &torn, &older);
    CHECK_EQ(n, 5u);
    CHECK(older);

    // `max` keeps the NEWEST entries, because a profiler asked for "the top of
    // a window" wants the most recent evidence, not the front of the buffer.
    n = ring.copy_window(0u, 1000u, out, 2u, &torn, &older);
    CHECK_EQ(n, 2u);
    CHECK_EQ(out[0].qpc, 130u);
    CHECK_EQ(out[1].qpc, 140u);

    ring.shutdown();
    CHECK(!ring.ready());
}

void test_ring_eviction() {
    NdsHostProfRing ring;
    CHECK(ring.init(4u));
    for (uint64_t i = 0; i < 10u; ++i)
        ring.push(make_sample(1000u + i, 0x2000u + i));
    CHECK_EQ(ring.written(), 10u);
    // Four resident, six dropped -- and the ring says so rather than pretending
    // it holds ten.
    CHECK_EQ(ring.evicted(), 6u);

    NdsHostProfSample out[8]{};
    bool torn = false;
    const uint32_t n = ring.copy_all(out, 8u, &torn);
    CHECK_EQ(n, 4u);
    CHECK_EQ(out[0].qpc, 1006u);
    CHECK_EQ(out[3].qpc, 1009u);

    // read_index() is the dump writer's forward stream: resident indices
    // resolve, evicted ones refuse rather than returning a recycled slot.
    NdsHostProfSample s{};
    CHECK(!ring.read_index(0u, &s));
    CHECK(!ring.read_index(5u, &s));
    CHECK(ring.read_index(6u, &s));
    CHECK_EQ(s.qpc, 1006u);
    CHECK(ring.read_index(9u, &s));
    CHECK_EQ(s.qpc, 1009u);
    CHECK(!ring.read_index(10u, &s));   // not written yet
    ring.shutdown();
}

// ── Histogram ───────────────────────────────────────────────────────────
void test_histogram() {
    NdsHostProfHist hist;
    CHECK(hist.init());
    CHECK_EQ(hist.total(), 0u);
    for (int i = 0; i < 100; ++i) hist.insert(0xAAAA0000ull);
    for (int i = 0; i < 30; ++i) hist.insert(0xBBBB0000ull);
    for (int i = 0; i < 60; ++i) hist.insert(0xCCCC0000ull);
    // A zero RIP is impossible and must not claim a slot -- slot key 0 is what
    // "unclaimed" means, so accepting it would make the table unreadable.
    hist.insert(0u);
    CHECK_EQ(hist.total(), 190u);
    CHECK_EQ(hist.overflow(), 0u);

    NdsHostProfHistEntry top[4]{};
    const uint32_t n = hist.top(top, 4u);
    CHECK_EQ(n, 3u);
    CHECK_EQ(top[0].rip, 0xAAAA0000ull);
    CHECK_EQ(top[0].count, 100u);
    CHECK_EQ(top[1].rip, 0xCCCC0000ull);
    CHECK_EQ(top[1].count, 60u);
    CHECK_EQ(top[2].rip, 0xBBBB0000ull);
    CHECK_EQ(top[2].count, 30u);

    // A shorter request must still be sorted and still hold the true top.
    NdsHostProfHistEntry one{};
    CHECK_EQ(hist.top(&one, 1u), 1u);
    CHECK_EQ(one.rip, 0xAAAA0000ull);
    hist.shutdown();
}

// ── Synthetic .pdata / .xdata unwind ────────────────────────────────────
//
// Three functions in a fake image:
//   A [0x1000,0x1100)  push rbp; sub rsp,0x20            (no frame pointer)
//   B [0x1100,0x1200)  push rbp; sub rsp,0x40;
//                      lea rbp,[rsp+0x20]; mov [rbp-8],rbx   (frame pointer)
//   C [0x1200,0x1300)  UNW_FLAG_CHAININFO -> A's entry
// and a stack laid out to be exactly what C -> B -> A would produce.
constexpr uint64_t kImageBase = 0x0000000140000000ull;
constexpr uint32_t kImageSize = 0x4000u;
constexpr uint32_t kPdataRva = 0x2000u;
constexpr uint32_t kUnwindA = 0x3000u;
constexpr uint32_t kUnwindB = 0x3020u;
constexpr uint32_t kUnwindC = 0x3040u;

constexpr uint64_t kStackLo = 0x000000007FF00000ull;
constexpr uint32_t kStackBytes = 4096u;

struct SynthImage {
    std::vector<uint8_t> image;
    std::vector<uint8_t> stack;

    SynthImage() : image(kImageSize, 0u), stack(kStackBytes, 0u) {
        put_rf(kPdataRva + 0u, 0x1000u, 0x1100u, kUnwindA);
        put_rf(kPdataRva + 12u, 0x1100u, 0x1200u, kUnwindB);
        put_rf(kPdataRva + 24u, 0x1200u, 0x1300u, kUnwindC);

        // A: version 1, prolog 8 bytes, 2 codes, no frame register.
        put8(kUnwindA + 0u, 0x01u);          // version 1, flags 0
        put8(kUnwindA + 1u, 8u);             // SizeOfProlog
        put8(kUnwindA + 2u, 2u);             // CountOfCodes
        put8(kUnwindA + 3u, 0u);             // FrameRegister/Offset
        // Codes are stored last-prologue-op-first, which is unwind order.
        put8(kUnwindA + 4u, 8u);             // offset 8: sub rsp,0x20
        put8(kUnwindA + 5u, 0x32u);          // info=3, op=2 (ALLOC_SMALL) -> 32
        put8(kUnwindA + 6u, 1u);             // offset 1: push rbp
        put8(kUnwindA + 7u, 0x50u);          // info=5 (RBP), op=0 (PUSH_NONVOL)

        // B: frame register RBP with a scaled offset of 2 (0x20 bytes).
        put8(kUnwindB + 0u, 0x01u);
        put8(kUnwindB + 1u, 15u);            // SizeOfProlog
        put8(kUnwindB + 2u, 5u);             // 4 ops, one of which takes 2 slots
        put8(kUnwindB + 3u, 0x25u);          // FrameOffset=2, FrameRegister=5
        put8(kUnwindB + 4u, 14u);            // offset 14: mov [rbp-8],rbx
        put8(kUnwindB + 5u, 0x34u);          // info=3 (RBX), op=4 (SAVE_NONVOL)
        put16(kUnwindB + 6u, 3u);            // scaled slot: frame_base + 0x18
        put8(kUnwindB + 8u, 10u);            // offset 10: lea rbp,[rsp+0x20]
        put8(kUnwindB + 9u, 0x03u);          // op=3 (SET_FPREG)
        put8(kUnwindB + 10u, 5u);            // offset 5: sub rsp,0x40
        put8(kUnwindB + 11u, 0x72u);         // info=7, op=2 -> (7+1)*8 = 64
        put8(kUnwindB + 12u, 1u);            // offset 1: push rbp
        put8(kUnwindB + 13u, 0x50u);

        // C: nothing of its own; its frame is described by A's entry.
        put8(kUnwindC + 0u, 0x21u);          // version 1, flags 4 (CHAININFO)
        put8(kUnwindC + 1u, 0u);
        put8(kUnwindC + 2u, 0u);             // no codes
        put8(kUnwindC + 3u, 0u);
        // Aligned chained RUNTIME_FUNCTION: 4 header bytes + 0 code bytes.
        put_rf(kUnwindC + 4u, 0x1000u, 0x1100u, kUnwindA);
    }

    void put8(uint32_t rva, uint8_t v) { image[rva] = v; }
    void put16(uint32_t rva, uint16_t v) {
        std::memcpy(&image[rva], &v, 2);
    }
    void put32(uint32_t rva, uint32_t v) {
        std::memcpy(&image[rva], &v, 4);
    }
    void put_rf(uint32_t rva, uint32_t begin, uint32_t end, uint32_t unwind) {
        put32(rva, begin);
        put32(rva + 4u, end);
        put32(rva + 8u, unwind);
    }
    void put_stack(uint64_t addr, uint64_t v) {
        std::memcpy(&stack[addr - kStackLo], &v, 8);
    }
};

bool synth_read(void* c, uint64_t addr, void* dst, uint32_t len) {
    SynthImage* s = static_cast<SynthImage*>(c);
    if (addr >= kStackLo && addr + len <= kStackLo + kStackBytes) {
        std::memcpy(dst, &s->stack[addr - kStackLo], len);
        return true;
    }
    if (addr >= kImageBase && addr + len <= kImageBase + kImageSize) {
        std::memcpy(dst, &s->image[addr - kImageBase], len);
        return true;
    }
    return false;
}

bool synth_pdata(void* /*c*/, uint64_t pc, uint64_t* base, uint32_t* rva,
                 uint32_t* size) {
    if (pc < kImageBase || pc >= kImageBase + kImageSize) return false;
    *base = kImageBase;
    *rva = kPdataRva;
    *size = 36u;   // three entries
    return true;
}

void test_synthetic_unwind() {
    SynthImage img;

    // Derived in the same direction the machine builds it: A's post-prologue
    // RSP is the anchor, and every other address follows from the prologues.
    const uint64_t s0 = kStackLo;            // A's RSP at the sample
    const uint64_t a_entry_rsp = s0 + 40u;   // A's RSP at its first instruction
    const uint64_t b_entry_rsp = s0 + 120u;  // B's RSP at its first instruction
    const uint64_t ret_into_b = kImageBase + 0x1180u;
    const uint64_t ret_into_c = kImageBase + 0x1280u;

    img.put_stack(s0 + 32u, b_entry_rsp - 40u);   // rbp of B, pushed by A
    img.put_stack(a_entry_rsp, ret_into_b);
    img.put_stack(b_entry_rsp - 48u, 0xB0B0B0B0ull);   // rbx saved by B
    img.put_stack(b_entry_rsp - 8u, 0xC0C0C0C0ull);    // rbp pushed by B
    img.put_stack(b_entry_rsp, ret_into_c);
    // C uses A's codes: +0x20, then pop rbp, then the return address.
    img.put_stack(b_entry_rsp + 40u, 0xD0D0D0D0ull);
    img.put_stack(b_entry_rsp + 48u, 0u);   // return address 0 -> thread root

    NdsHostUnwindEnv env{synth_read, synth_pdata, &img};
    NdsHostUnwindRegs regs{};
    regs.rip = kImageBase + 0x1050u;   // inside A, past its 8-byte prologue
    regs.gpr[4] = s0;
    regs.gpr[5] = 0xDEADBEEFull;       // rbp is garbage until A restores it

    uint64_t frames[kNdsHostUnwindMaxFrames]{};
    NdsHostUnwindStop stop = NDS_HOST_UNWIND_DEPTH;
    const unsigned depth =
        nds_host_unwind(env, regs, frames, kNdsHostUnwindMaxFrames, &stop);

    CHECK_EQ(depth, 3u);
    CHECK_EQ(frames[0], kImageBase + 0x1050u);
    CHECK_EQ(frames[1], ret_into_b);
    CHECK_EQ(frames[2], ret_into_c);
    // A complete walk, not a truncated one: the chained entry resolved and the
    // root was reached.
    CHECK_EQ(stop, NDS_HOST_UNWIND_ROOT);
}

void test_partial_prologue() {
    // Sampled between `push rbp` (offset 1) and `sub rsp,0x20` (offset 8): the
    // allocation has NOT happened, so undoing it would skip 32 bytes of the
    // caller's frame and report a fabricated parent. This is the case a naive
    // implementation gets wrong, and it is common -- a 1 kHz sampler lands in
    // prologues constantly.
    SynthImage img;
    const uint64_t rsp = kStackLo + 64u;   // after push rbp, before sub
    const uint64_t ret = kImageBase + 0x1180u;
    img.put_stack(rsp, 0xAAAAAAAAull);     // the pushed rbp
    img.put_stack(rsp + 8u, ret);          // A's return address

    NdsHostUnwindEnv env{synth_read, synth_pdata, &img};
    NdsHostUnwindRegs regs{};
    regs.rip = kImageBase + 0x1002u;       // prologue offset 2
    regs.gpr[4] = rsp;

    uint64_t frames[kNdsHostUnwindMaxFrames]{};
    NdsHostUnwindStop stop = NDS_HOST_UNWIND_DEPTH;
    const unsigned depth =
        nds_host_unwind(env, regs, frames, kNdsHostUnwindMaxFrames, &stop);
    CHECK(depth >= 2u);
    CHECK_EQ(frames[0], kImageBase + 0x1002u);
    CHECK_EQ(frames[1], ret);
    CHECK_EQ(regs.gpr[4], rsp);   // by-value: the caller's copy is untouched
}

void test_leaf_and_failure_modes() {
    SynthImage img;
    // A pc inside the image but outside every .pdata entry is a leaf: the
    // return address is at [RSP] and the walk continues.
    const uint64_t rsp = kStackLo + 64u;
    img.put_stack(rsp, kImageBase + 0x1050u);
    NdsHostUnwindEnv env{synth_read, synth_pdata, &img};
    NdsHostUnwindRegs regs{};
    regs.rip = kImageBase + 0x1800u;   // no entry covers this
    regs.gpr[4] = rsp;
    uint64_t frames[kNdsHostUnwindMaxFrames]{};
    NdsHostUnwindStop stop = NDS_HOST_UNWIND_DEPTH;
    unsigned depth =
        nds_host_unwind(env, regs, frames, kNdsHostUnwindMaxFrames, &stop);
    CHECK(depth >= 2u);
    CHECK_EQ(frames[1], kImageBase + 0x1050u);

    // A pc in no module at all: frame 0 still lands, so self-time attribution
    // survives even for code the module map cannot place (a JIT page, a shard
    // that has since unloaded).
    NdsHostUnwindRegs off{};
    off.rip = 0x1234567890ull;
    off.gpr[4] = rsp;
    depth = nds_host_unwind(env, off, frames, kNdsHostUnwindMaxFrames, &stop);
    CHECK_EQ(depth, 1u);
    CHECK_EQ(frames[0], 0x1234567890ull);
    CHECK_EQ(stop, NDS_HOST_UNWIND_NO_MODULE);

    // A stack pointer outside the copied slice cannot be read, and the walk
    // says READ_FAILED rather than inventing a frame from whatever was there.
    NdsHostUnwindRegs bad{};
    bad.rip = kImageBase + 0x1050u;
    bad.gpr[4] = kStackLo + kStackBytes + 4096u;
    depth = nds_host_unwind(env, bad, frames, kNdsHostUnwindMaxFrames, &stop);
    CHECK_EQ(depth, 1u);
    CHECK_EQ(stop, NDS_HOST_UNWIND_READ_FAILED);

    // A zero-length request must not write anything.
    CHECK_EQ(nds_host_unwind(env, bad, frames, 0u, &stop), 0u);
}

// ── Live unwind against the OS ──────────────────────────────────────────
#if defined(_WIN32) && defined(__x86_64__)

using CaptureFn = USHORT (WINAPI*)(ULONG, ULONG, PVOID*, PULONG);
CaptureFn g_capture = nullptr;

// A real call chain in real compiled code. noinline so the chain survives -O3
// (build-mph-kanden is -O3, and a test that silently collapses to one frame
// there proves nothing).
struct LiveResult {
    uint64_t ours[kNdsHostUnwindMaxFrames];
    unsigned our_depth;
    void* theirs[kNdsHostUnwindMaxFrames];
    unsigned their_depth;
    uint8_t stop;
};

__attribute__((noinline)) void live_leaf(LiveResult* r) {
    r->our_depth = nds_hostprof_self_stack(r->ours, kNdsHostUnwindMaxFrames,
                                           &r->stop);
    ULONG hash = 0;
    r->their_depth = g_capture(0u, kNdsHostUnwindMaxFrames, r->theirs, &hash);
}
__attribute__((noinline)) void live_f1(LiveResult* r) { live_leaf(r); }
__attribute__((noinline)) void live_f2(LiveResult* r) { live_f1(r); }
__attribute__((noinline)) void live_f3(LiveResult* r) { live_f2(r); }
__attribute__((noinline)) void live_f4(LiveResult* r) { live_f3(r); }

void test_live_unwind_matches_os() {
    g_capture = reinterpret_cast<CaptureFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                       "RtlCaptureStackBackTrace")));
    if (!g_capture) {
        std::fprintf(stderr,
            "host_prof_test: RtlCaptureStackBackTrace unavailable; skipping "
            "the live unwind gate\n");
        return;
    }
    CHECK(nds_hostprof_refresh_modules());

    LiveResult r{};
    live_f4(&r);

    // ours[0] is the pc inside nds_hostprof_self_stack itself; ours[1] is the
    // return address in live_leaf (the call site of self_stack). theirs[0] is
    // the return address in live_leaf too, but at the OTHER call site, so the
    // chains line up from ours[2] == theirs[1] onward.
    CHECK(r.our_depth >= 6u);
    CHECK(r.their_depth >= 5u);
    if (r.our_depth < 6u || r.their_depth < 5u) {
        std::fprintf(stderr,
            "host_prof_test: live depths ours=%u theirs=%u stop=%s\n",
            r.our_depth, r.their_depth,
            nds_host_unwind_stop_name(
                static_cast<NdsHostUnwindStop>(r.stop)));
        return;
    }
    unsigned compared = 0;
    for (unsigned i = 2u; i < r.our_depth && (i - 1u) < r.their_depth; ++i) {
        const uint64_t theirs =
            reinterpret_cast<uint64_t>(r.theirs[i - 1u]);
        if (theirs == 0u) break;
        CHECK_EQ(r.ours[i], theirs);
        ++compared;
    }
    // Four noinline hops plus main's chain: if fewer than four frames actually
    // got compared the gate did not exercise anything, which must fail loudly
    // rather than pass silently. Printed unconditionally so a green run says
    // HOW MUCH it proved rather than only that it did not fail.
    std::fprintf(stderr,
        "host_prof_test: live gate compared %u frames against the OS "
        "(ours depth %u, theirs %u)\n", compared, r.our_depth, r.their_depth);
    CHECK(compared >= 4u);
    if (g_failures) {
        std::fprintf(stderr, "host_prof_test: ours(depth %u, stop %s):\n",
                     r.our_depth,
                     nds_host_unwind_stop_name(
                         static_cast<NdsHostUnwindStop>(r.stop)));
        for (unsigned i = 0; i < r.our_depth; ++i)
            std::fprintf(stderr, "  [%u] 0x%llx\n", i,
                         (unsigned long long)r.ours[i]);
        std::fprintf(stderr, "host_prof_test: theirs(depth %u):\n",
                     r.their_depth);
        for (unsigned i = 0; i < r.their_depth; ++i)
            std::fprintf(stderr, "  [%u] 0x%llx\n", i,
                         (unsigned long long)
                             reinterpret_cast<uintptr_t>(r.theirs[i]));
    }
}

// The sampler end to end: start it, register this thread, let it run, and
// require that it actually filled the ring and resolved most frames. This is
// the gate that would catch a sampler that starts but never samples -- the
// failure an always-on subsystem must never have, because nothing else would
// notice.
void test_sampler_end_to_end() {
    _putenv_s("NDS_HOSTPROF", "on");
    _putenv_s("NDS_HOSTPROF_HZ", "1000");
    _putenv_s("NDS_HOSTPROF_RING", "16384");
    nds_hostprof_start();
    CHECK(nds_hostprof_running());
    if (!nds_hostprof_running()) return;

    // Burn ~300 ms of real work so there is something to attribute.
    volatile double acc = 0.0;
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < 300u)
        for (int i = 0; i < 20000; ++i) acc += static_cast<double>(i) * 1.5;
    (void)acc;

    const std::string status = nds_hostprof_status_json();
    CHECK(status.find("\"running\":true") != std::string::npos);
    const std::string top = nds_hostprof_top_json(0.0, 8u);
    CHECK(top.find("\"ok\":true") != std::string::npos);
    // A 1 kHz sampler over 300 ms must have produced samples; a table that is
    // empty here means the suspend/context path failed silently.
    CHECK(top.find("\"samples\":0,") == std::string::npos);
    const std::string windowed = nds_hostprof_top_json(1.0, 8u);
    CHECK(windowed.find("\"ok\":true") != std::string::npos);
    CHECK(windowed.find("\"samples\":0,") == std::string::npos);

    // A dump must be a header line plus a whole number of fixed-size samples.
    const std::string path = "host_prof_test.ndshp";
    char error[256] = {};
    std::string extra;
    const bool ok = nds_hostprof_dump(path.c_str(), 0.0, 0.0, error,
                                      sizeof(error), &extra);
    CHECK(ok);
    if (ok) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        CHECK(f != nullptr);
        if (f) {
            std::string header;
            int c = 0;
            while ((c = std::fgetc(f)) != EOF && c != '\n')
                header.push_back(static_cast<char>(c));
            CHECK(header.find("\"format\":\"ndsrecomp-hostprof\"") !=
                  std::string::npos);
            CHECK(header.find("\"modules\":[{") != std::string::npos);
            const long body = [&] {
                const long here = std::ftell(f);
                std::fseek(f, 0, SEEK_END);
                const long end = std::ftell(f);
                return end - here;
            }();
            CHECK(body > 0);
            CHECK_EQ(body % static_cast<long>(sizeof(NdsHostProfSample)), 0);
            std::fclose(f);
        }
        // NDS_HOSTPROF_TEST_KEEP_DUMP leaves the file behind, which is how the
        // offline symbolizer (tools/hostprof_symbolize.py) gets a real dump to
        // be tested against without needing a full title run first.
        if (!std::getenv("NDS_HOSTPROF_TEST_KEEP_DUMP"))
            std::remove(path.c_str());
        else
            std::fprintf(stderr, "host_prof_test: kept %s\n", path.c_str());
    } else {
        std::fprintf(stderr, "host_prof_test: dump failed: %s\n", error);
    }
    nds_hostprof_stop();
    CHECK(!nds_hostprof_running());
}

// ── beads-yjp.70: an unloaded module must never fault the sampler ────────
//
// THE CRASH THIS PINS. The module map is built from a SNAPSHOT of the loaded
// module list, and the map is then built by READING each module's PE headers
// and unwind tables out of its mapped image. Between the snapshot and those
// reads, any DLL in the list may already be gone: live_overlay quarantines and
// supersedes shards by calling close_library() and then renaming the file
// (live_overlay.cpp:1558 requires the unmap to have happened first), and the OS
// itself loads and drops provider DLLs during startup. This process installs no
// SEH -- GCC has no __try/__except -- so a single dereference of an unmapped
// page terminates it instantly and silently, mid-syscall on whatever unrelated
// thread happened to be running. That is exactly how the p2-all runner died:
// the sampler faulted while the main thread was inside the WriteFile for its
// "[identity] persisted MAC" line, and the log ends 82 bytes into a 113-byte
// write with no diagnostics and no WER event.
//
// So the test does the unsafe thing on purpose: load a real DLL, prove the map
// can parse and walk it, FreeLibrary it, and then drive BOTH sampler-side
// paths at its now-unmapped address range. Neither may fault, and each must
// report an enumerated stop reason instead.
#if defined(_WIN32) && defined(__x86_64__) && defined(NDS_HOST_PROF_PROBE_DLL)

// SizeOfImage straight from the mapped headers: the map's own snapshot gets
// this from the module list, and the probe takes a [base, size) the same way.
uint64_t image_size_of(HMODULE mod) {
    const uint8_t* img = reinterpret_cast<const uint8_t*>(mod);
    uint32_t lfanew = 0;
    std::memcpy(&lfanew, img + 0x3C, 4);
    const IMAGE_NT_HEADERS64* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(img + lfanew);
    return nt->OptionalHeader.SizeOfImage;
}

bool range_is_mapped(uint64_t base) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(base)),
                      &mbi, sizeof(mbi)))
        return false;
    return mbi.State == MEM_COMMIT;
}

void test_unloaded_module_does_not_fault() {
    const char* dll_path = NDS_HOST_PROF_PROBE_DLL;
    HMODULE mod = LoadLibraryA(dll_path);
    if (!mod) {
        std::fprintf(stderr,
                     "host_prof_test: could not load probe DLL %s (err %lu)\n",
                     dll_path, GetLastError());
        ++g_failures;
        return;
    }
    const uint64_t base = reinterpret_cast<uint64_t>(mod);
    const uint64_t size = image_size_of(mod);
    CHECK(size >= 0x1000u);

    // A pc a few bytes into a real prologue, so the walk has to read the
    // module's .pdata and its UNWIND_INFO rather than bailing at env_pdata.
    FARPROC fn = GetProcAddress(mod, "nds_probe_frame");
    CHECK(fn != nullptr);
    const uint64_t pc = reinterpret_cast<uint64_t>(fn) + 8u;

    // (1) MAPPED: the snapshot-time parse finds and copies real unwind data.
    uint32_t pdata_size = 0;
    uint64_t copy_bytes = 0;
    const bool parsed =
        nds_hostprof_probe_module_parse(base, size, &pdata_size, &copy_bytes);
    CHECK(parsed);
    CHECK(pdata_size > 0u);
    CHECK(copy_bytes > 0u);

    // (2) MAPPED: the map sees the module and a walk through it terminates on
    // an enumerated stop. With no stack slice the chain cannot continue past
    // the first return address, so READ_FAILED is the expected, clean answer --
    // what matters is that the module side resolved at all, i.e. NOT NO_MODULE.
    CHECK(nds_hostprof_refresh_modules());
    uint64_t frames[32];
    uint8_t stop = 0xFFu;
    unsigned n = nds_hostprof_probe_walk_at(pc, frames, 32u, &stop);
    CHECK(n >= 1u);
    CHECK_EQ(frames[0], pc);
    CHECK(stop < NDS_HOST_UNWIND_STOP_COUNT);
    CHECK(stop != NDS_HOST_UNWIND_NO_MODULE);

    // (3) Unload it. Everything below reads an address range that the loader
    // has taken away, which is the whole point.
    CHECK(FreeLibrary(mod));
    if (range_is_mapped(base)) {
        // Something else in the process holds a reference (a debugger, a
        // shim). The test cannot prove anything in that state, so say so
        // rather than passing vacuously.
        std::fprintf(stderr, "host_prof_test: probe DLL still mapped after "
                             "FreeLibrary; unload assertions skipped\n");
        return;
    }

    // (4) STALE MAP, module gone: the walk is served entirely from the private
    // unwind copy taken while it was mapped, so it still must not fault. This
    // is the property that makes COPYING the right choice over pinning the
    // module -- pinning would have kept the image mapped and broken
    // live_overlay's rename-after-close_library quarantine path.
    stop = 0xFFu;
    n = nds_hostprof_probe_walk_at(pc, frames, 32u, &stop);
    CHECK(n >= 1u);
    CHECK_EQ(frames[0], pc);
    CHECK(stop < NDS_HOST_UNWIND_STOP_COUNT);

    // (5) THE REGRESSION. The snapshot-time parse run against the unmapped
    // range -- the precise sequence the crash hit, a module named by a
    // snapshot and unloaded before the parse read it. Before the fix these two
    // calls dereferenced the freed image and killed the process; now they must
    // report "nothing usable here" and return.
    pdata_size = 0;
    copy_bytes = 0;
    CHECK(!nds_hostprof_probe_module_parse(base, size, &pdata_size,
                                           &copy_bytes));
    CHECK_EQ(pdata_size, 0u);
    CHECK_EQ(copy_bytes, 0u);

    // (6) A refresh drops the module, after which the pc resolves to nothing.
    CHECK(nds_hostprof_refresh_modules());
    stop = 0xFFu;
    n = nds_hostprof_probe_walk_at(pc, frames, 32u, &stop);
    CHECK_EQ(n, 1u);
    CHECK_EQ(frames[0], pc);
    CHECK_EQ(stop, NDS_HOST_UNWIND_NO_MODULE);
}

#endif  // _WIN32 && __x86_64__ && NDS_HOST_PROF_PROBE_DLL

#endif  // _WIN32 && __x86_64__

}  // namespace

int main() {
    test_ring_basics();
    test_ring_eviction();
    test_histogram();
    test_synthetic_unwind();
    test_partial_prologue();
    test_leaf_and_failure_modes();
#if defined(_WIN32) && defined(__x86_64__)
    test_live_unwind_matches_os();
    test_sampler_end_to_end();
#if defined(NDS_HOST_PROF_PROBE_DLL)
    test_unloaded_module_does_not_fault();
#endif
#endif
    if (g_failures)
        std::fprintf(stderr, "host_prof_test: %d failure(s)\n", g_failures);
    else
        std::fprintf(stderr, "host_prof_test: ok\n");
    return g_failures ? 1 : 0;
}
