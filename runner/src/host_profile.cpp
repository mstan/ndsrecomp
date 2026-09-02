// host_profile.cpp -- see host_profile.h.

#include "host_profile.h"

#include "host_unwind_x64.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32) && defined(__x86_64__)
#define NDS_HOSTPROF_SUPPORTED 1
#include <windows.h>
#include <tlhelp32.h>
#else
#define NDS_HOSTPROF_SUPPORTED 0
#endif

namespace {

std::string json_escape_str(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(
                                      static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

}  // namespace

#if !NDS_HOSTPROF_SUPPORTED

// Non-Windows / non-x64: the whole subsystem is a no-op rather than a
// compile error, so a Linux or ARM64 build of the runner keeps building and a
// probe gets an explicit "unsupported" instead of a missing command. The .pdata
// unwinder is inherently x64-PE; a DWARF equivalent would be a separate
// implementation behind the same query surface.
void nds_hostprof_start() {}
void nds_hostprof_stop() {}
bool nds_hostprof_running() { return false; }
void nds_hostprof_register_current_thread(NdsHostProfRole) {}
void nds_hostprof_unregister_current_thread() {}

std::string nds_hostprof_top_json(double, unsigned) {
    return "{\"ok\":false,\"error\":\"hostprof unsupported on this platform\"}";
}
std::string nds_hostprof_status_json() {
    return "{\"supported\":false,\"running\":false}";
}
bool nds_hostprof_dump(const char*, double, double, char* error,
                       unsigned error_cap, std::string*) {
    if (error && error_cap) std::snprintf(error, error_cap, "unsupported");
    return false;
}
bool nds_hostprof_write_bundle(const char*, char* error, unsigned error_cap,
                               std::string*) {
    if (error && error_cap) std::snprintf(error, error_cap, "unsupported");
    return false;
}
unsigned nds_hostprof_self_stack(uint64_t*, unsigned, uint8_t*) { return 0; }
bool nds_hostprof_refresh_modules() { return false; }
bool nds_hostprof_probe_module_parse(uint64_t, uint64_t, uint32_t*,
                                     uint64_t*) { return false; }
unsigned nds_hostprof_probe_walk_at(uint64_t, uint64_t*, unsigned,
                                    uint8_t*) { return 0; }

#else   // NDS_HOSTPROF_SUPPORTED

namespace {

// The suspended-window stack slice.
//
// SIZED BY MEASUREMENT, twice. First, the copy is free: the bench below
// benchmarks 0 B, 16 KB, 32 KB, 64 KB and 128 KB at an identical ~35 us
// median, because the three syscalls dominate completely. Second, 16 KB was
// NOT ENOUGH: the first real MPH Kanden profile showed 25 percent of walks
// ending in READ_FAILED, and the depth histogram put almost all of them at
// depth 8-12 with the last resolved frame inside nds_runner.exe -- i.e. the
// walk was running out of COPIED STACK just as it reached main() and the CRT
// entry, because nds_run_interactive_frontend() and main() have multi-kilobyte
// frames. Since a bigger copy costs nothing, there is no reason to keep a bound
// that truncates real chains: 64 KB reaches past the CRT entry with room to
// spare.
constexpr uint32_t kStackCopyBytes = 64u * 1024u;
// Registered threads. Emu + render + audio + live compiler + slack. A fixed
// array (rather than a vector) is what lets the sampler walk targets with no
// lock at all: taking a lock the sampled thread could also want, around a
// suspend, is the textbook SuspendThread deadlock.
constexpr unsigned kMaxTargets = 8u;
constexpr uint32_t kDefaultRingSamples = 1u << 18;   // 36 MB
// MEASURED, not chosen, and the measurement is the whole argument.
//
// The suspended window is three syscalls and a memcpy. A decomposition bench
// (2000 suspend/context/resume cycles per size against a busy target, quiet
// box) says the memcpy is FREE and the syscalls are the entire bill:
//
//   copy=    0 B   median 32.7 us   p90 52.7   min 12.0
//   copy= 4096 B   median 32.8 us   p90 51.6   min 13.1
//   copy=16384 B   median 33.0 us   p90 51.9   min 11.7
//   copy=32768 B   median 33.0 us   p90 53.7   min 13.1
//
// Copying 32 KB of stack costs the same as copying nothing, and the ~33 us is a
// cross-core interrupt plus a context capture -- no code in this file can move
// it, and Windows exposes no cheaper in-process way to read another thread's
// registers (the kernel-side alternative is ETW, which needs an out-of-process
// session and is therefore not always-on for a player).
//
// So the RATE is the only knob that buys the overhead budget, and the budget is
// hard: this is an always-on Release feature living inside the very thread we
// are trying to make faster, so it may not cost 1 percent of it.
//
//   1000 Hz -> 33 ms/s -> 3.3 percent   (rejected)
//    500 Hz -> 16 ms/s -> 1.7 percent   (rejected)
//    250 Hz ->  8 ms/s -> 0.8 percent   (shipped)
//
// 250 Hz still yields 7500 samples in a 30 s window, which resolves any host
// symbol above roughly half a percent of the profile -- far finer than the
// question "which subsystem is eating the 36 host cycles per guest cycle"
// needs -- and it stretches the ring's retention to ~17 minutes, which makes
// the after-the-fact query this whole subsystem exists for MORE likely to
// still hold the window someone asks about.
//
// NDS_HOSTPROF_HZ=1000 remains available for a deliberate high-resolution
// session where paying 3.3 percent to resolve a narrow window is the right
// trade. It is not a default anyone should ship.
constexpr uint32_t kDefaultHz = 250u;
// Non-emu targets (2D/3D workers, audio, live compiler) are sampled every Nth
// tick. Their question -- "is this pool busy, and in what" -- needs far less
// resolution than the emu thread's, and suspending five threads every tick
// would multiply a cost that is entirely per-suspend.
constexpr uint32_t kSecondaryDivisor = 4u;
constexpr unsigned kDumpBatch = 256u;

enum : uint32_t {
    SLOT_FREE = 0u,
    SLOT_ARMING = 1u,
    SLOT_ACTIVE = 2u,
    SLOT_RETIRING = 3u,
};

struct ModuleEntry {
    uint64_t base = 0;
    uint64_t size = 0;
    uint32_t pdata_rva = 0;
    uint32_t pdata_size = 0;
    // True for the main executable only. A process image can never be
    // unmapped, so its unwind tables are read in place -- which matters
    // because on a title build that image carries the generated banks and its
    // .pdata/.xdata are tens of megabytes. Every other module gets a private
    // copy (below) because a live-overlay shard DLL genuinely can be
    // FreeLibrary'd (live_overlay.cpp close_library) while the sampler is
    // mid-walk, and reading an unmapped page would fault a Release build with
    // no SEH to catch it.
    bool direct = false;
    uint64_t copy_lo = 0;
    uint64_t copy_hi = 0;
    std::vector<uint8_t> copy;
    std::string path;
    std::string name;
};

struct Target {
    // CONTEXT is 16-byte aligned; the array of Targets inherits the alignment.
    alignas(16) CONTEXT ctx{};
    HANDLE handle = nullptr;
    DWORD tid = 0;
    NdsHostProfRole role = NDS_HOSTPROF_ROLE_OTHER;
    // The thread's stack reservation, taken once at registration. This is the
    // whole reason a thread registers ITSELF: with the bounds known, the
    // suspended-window memcpy is provably in bounds and needs no VirtualQuery
    // (a syscall) inside the window.
    //
    // BOTH ends are recorded (beads-yjp.70). Bounding only the top proves
    // nothing on its own: the copy runs from RSP upward, so a sampled RSP that
    // is not in this thread's stack at all -- a thread caught in the sliver
    // where it is still suspendable but its stack is already gone, or any
    // GetThreadContext result we would rather not trust -- yields a huge
    // `avail` and a 64 KB read of unmapped memory. GetCurrentThreadStackLimits
    // reports the whole RESERVATION (its low end is the deallocation base, not
    // the currently committed limit), so a live thread's RSP is always inside
    // [stack_low, stack_top) no matter how deep it has grown, and rejecting
    // anything outside cannot drop a legitimate sample.
    uint64_t stack_low = 0;
    uint64_t stack_top = 0;
    uint8_t* stack_copy = nullptr;   // preallocated; never allocated in-window
    uint64_t copy_base = 0;
    uint32_t copy_len = 0;
};

std::atomic<uint32_t> g_slot_state[kMaxTargets];
Target g_targets[kMaxTargets];

NdsHostProfRing g_ring;
NdsHostProfHist g_hist;

std::once_flag g_config_once;
bool g_enabled = true;
uint32_t g_hz = kDefaultHz;
uint32_t g_ring_samples = kDefaultRingSamples;
uint64_t g_qpc_freq = 1;

std::atomic<bool> g_running{false};
std::atomic<bool> g_stop_requested{false};
std::thread g_thread;

std::mutex g_modules_mutex;
std::vector<ModuleEntry> g_modules;   // sorted by base

// Observer cost, measured rather than asserted. suspend_qpc is the time the
// SAMPLED thread actually lost; walk_qpc is sampler-thread-only work.
std::atomic<uint64_t> g_ticks{0};
std::atomic<uint64_t> g_samples{0};
std::atomic<uint64_t> g_suspend_qpc{0};
std::atomic<uint64_t> g_walk_qpc{0};
std::atomic<uint64_t> g_suspend_fail{0};
std::atomic<uint64_t> g_context_fail{0};
// Samples whose RSP was not inside the target's own stack reservation, so the
// suspended-window copy was refused rather than reading unmapped memory.
std::atomic<uint64_t> g_stack_reject{0};
std::atomic<uint64_t> g_stop_counts[NDS_HOST_UNWIND_STOP_COUNT];
std::atomic<uint64_t> g_role_counts[NDS_HOSTPROF_ROLE_COUNT];

uint64_t qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return static_cast<uint64_t>(v.QuadPart);
}

uint64_t unix_ms_now() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) |
                 ft.dwLowDateTime;
    // FILETIME epoch (1601) -> Unix epoch (1970), 100ns -> ms.
    return t / 10000ull - 11644473600000ull;
}

bool parse_on_off(const char* v, bool* out) {
    if (!v) return false;
    if (std::strcmp(v, "on") == 0 || std::strcmp(v, "1") == 0 ||
        std::strcmp(v, "true") == 0) { *out = true; return true; }
    if (std::strcmp(v, "off") == 0 || std::strcmp(v, "0") == 0 ||
        std::strcmp(v, "false") == 0) { *out = false; return true; }
    return false;
}

void load_config() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_freq = f.QuadPart ? static_cast<uint64_t>(f.QuadPart) : 1u;

    // Default ON. The whole value of this subsystem is that the answer already
    // exists when the question is asked, which an opt-in flag destroys.
    bool on = true;
    if (const char* v = std::getenv("NDS_HOSTPROF")) {
        if (!parse_on_off(v, &on)) on = true;
    }
    g_enabled = on;

    if (const char* v = std::getenv("NDS_HOSTPROF_HZ")) {
        const long hz = std::strtol(v, nullptr, 10);
        if (hz >= 1 && hz <= 10000) g_hz = static_cast<uint32_t>(hz);
    }
    if (const char* v = std::getenv("NDS_HOSTPROF_RING")) {
        const unsigned long n = std::strtoul(v, nullptr, 10);
        // Power of two only (the ring indexes by mask), and bounded so a typo
        // cannot ask for a terabyte.
        if (n >= 1024ul && n <= (1ul << 22) && (n & (n - 1ul)) == 0ul)
            g_ring_samples = static_cast<uint32_t>(n);
    }
}

void ensure_config() { std::call_once(g_config_once, load_config); }

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0,
                                         nullptr, nullptr);
    if (need <= 1) return {};
    std::string out(static_cast<size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), need, nullptr, nullptr);
    return out;
}

// ── Module map ──────────────────────────────────────────────────────────
//
// GUARDED READ OF A MODULE IMAGE (beads-yjp.70).
//
// Building the map means reading each module's PE headers and unwind tables
// out of its MAPPED IMAGE. The module list itself is a SNAPSHOT: by the time
// these reads run, any DLL in it may already have been FreeLibrary'd -- a
// live-overlay shard (live_overlay.cpp close_library), a graphics/Winsock
// provider DLL the OS probes and drops during startup, anything. A plain
// dereference of an unmapped page raises an access violation, and this
// process installs no SEH (GCC has no __try/__except), so it dies instantly
// and silently -- which is precisely the failure this function used to cause:
// the sampler faulted while the main thread was mid-fprintf, tearing its
// WriteFile in half.
//
// ReadProcessMemory against our OWN process is the guarded read: for an
// unmapped or protected range it returns FALSE rather than raising. It is a
// syscall, but every caller here runs on the sampler's 0.5 Hz map refresh,
// OUTSIDE the suspended window, so the cost is not on any measured path.
bool safe_read(uint64_t addr, void* dst, size_t len) {
    if (len == 0u || addr == 0u || addr + len < addr) return false;
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void*>(
                               static_cast<uintptr_t>(addr)),
                           dst, len, &got))
        return false;
    return got == len;
}

// Reads the module's exception directory. Returns with pdata_rva/pdata_size
// left at zero for anything it cannot read or does not trust -- an unreadable
// module simply contributes no unwind data, which costs a truncated walk and
// never a fault.
void parse_exception_dir(ModuleEntry& e) {
    if (e.size < 0x1000u) return;
    uint8_t mz[2] = {0, 0};
    if (!safe_read(e.base, mz, sizeof(mz))) return;
    if (mz[0] != 'M' || mz[1] != 'Z') return;
    uint32_t lfanew = 0;
    if (!safe_read(e.base + 0x3Cu, &lfanew, 4)) return;
    if (static_cast<uint64_t>(lfanew) + sizeof(IMAGE_NT_HEADERS64) > e.size)
        return;
    IMAGE_NT_HEADERS64 nt{};
    if (!safe_read(e.base + lfanew, &nt, sizeof(nt))) return;
    if (nt.Signature != IMAGE_NT_SIGNATURE) return;
    if (nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return;
    const IMAGE_DATA_DIRECTORY& d =
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!d.VirtualAddress || !d.Size) return;
    if (static_cast<uint64_t>(d.VirtualAddress) + d.Size > e.size) return;
    e.pdata_rva = d.VirtualAddress;
    e.pdata_size = d.Size;
}

// Private copy of a non-main module's unwind data, so an unload cannot turn a
// walk into a fault. Spans the SECTIONS that hold .pdata and every
// UNWIND_INFO it points at -- section granularity rather than a tight range
// because a chained UNWIND_INFO can reference a blob no .pdata entry names
// directly, and a copy that is 100 KB too big is free while a copy 4 bytes too
// small is a wrong answer.
//
// COPY, NOT PIN (beads-yjp.70). The other way to stop an unload from faulting
// a walk is to take a loader reference on every mapped module and hold it for
// the lifetime of the map entry. That is rejected because it FIGHTS
// live_overlay's unload semantics: quarantine_shard() renames and then deletes
// the shard file immediately after close_library() (live_overlay.cpp:1558
// spells out the requirement -- "a mapped image cannot be renamed, so
// close_library() must already have run" -- and the supersede path at
// live_overlay.cpp:1651-1652 and 1830-1852 does exactly that). An extra
// reference held by the profiler would keep the image mapped and the file
// locked, so the rename would fail and a quarantined or superseded shard would
// be left in the cache to be picked back up. A profiler must never change the
// behaviour of the thing it profiles. Copying is also strictly stronger: the
// map entry becomes self-contained, so a walk touches NO foreign image memory
// at all (see env_read) rather than merely delaying the unmap.
//
// Every read below is guarded, because the module list is a snapshot and the
// module may already be gone by the time we get here. An unreadable module
// yields no copy, which costs a truncated walk (stop = READ_FAILED) and never
// a fault.
void build_unwind_copy(ModuleEntry& e) {
    e.copy.clear();
    e.copy_lo = e.copy_hi = 0;
    if (e.direct || !e.pdata_size) return;

    uint32_t lfanew = 0;
    if (!safe_read(e.base + 0x3Cu, &lfanew, 4)) return;
    if (static_cast<uint64_t>(lfanew) + sizeof(IMAGE_NT_HEADERS64) > e.size)
        return;
    IMAGE_NT_HEADERS64 nt{};
    if (!safe_read(e.base + lfanew, &nt, sizeof(nt))) return;
    if (nt.Signature != IMAGE_NT_SIGNATURE) return;

    // The section table follows the optional header, whose length the file
    // header declares. Bound it against the image before reading, so a torn or
    // hostile header cannot walk us off the end.
    const unsigned section_count = nt.FileHeader.NumberOfSections;
    const uint64_t sect_rva = static_cast<uint64_t>(lfanew) +
                              offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                              nt.FileHeader.SizeOfOptionalHeader;
    const uint64_t sect_bytes =
        static_cast<uint64_t>(section_count) * sizeof(IMAGE_SECTION_HEADER);
    std::vector<IMAGE_SECTION_HEADER> sections;
    if (section_count && sect_rva + sect_bytes <= e.size) {
        sections.resize(section_count);
        if (!safe_read(e.base + sect_rva, sections.data(),
                       static_cast<size_t>(sect_bytes)))
            sections.clear();
    }

    uint64_t lo = e.pdata_rva;
    uint64_t hi = static_cast<uint64_t>(e.pdata_rva) + e.pdata_size;
    // One guarded read of the whole .pdata block rather than a syscall per
    // entry: a shard's table is a few KB and this runs at 0.5 Hz.
    const uint32_t entries = e.pdata_size / 12u;
    std::vector<uint8_t> pdata(e.pdata_size);
    if (!safe_read(e.base + e.pdata_rva, pdata.data(), pdata.size())) return;
    for (uint32_t i = 0; i < entries; ++i) {
        uint32_t unwind = 0;
        std::memcpy(&unwind, pdata.data() + i * 12u + 8u, 4);
        if (!unwind || unwind >= e.size) continue;
        if (unwind < lo) lo = unwind;
        if (static_cast<uint64_t>(unwind) + 1u > hi) hi = unwind + 1u;
    }
    // Grow to whole sections.
    for (unsigned s = 0; s < sections.size(); ++s) {
        const uint64_t s_lo = sections[s].VirtualAddress;
        const uint64_t s_hi = s_lo + std::max<uint64_t>(
            sections[s].Misc.VirtualSize, sections[s].SizeOfRawData);
        if (s_hi <= lo || s_lo >= hi) continue;
        if (s_lo < lo) lo = s_lo;
        if (s_hi > hi) hi = s_hi;
    }
    if (hi > e.size) hi = e.size;
    if (hi <= lo) return;
    std::vector<uint8_t> buf(static_cast<size_t>(hi - lo));
    if (!safe_read(e.base + lo, buf.data(), buf.size())) return;
    e.copy = std::move(buf);
    e.copy_lo = e.base + lo;
    e.copy_hi = e.base + hi;
}

bool refresh_modules_locked() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,
                                           GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return false;
    const HMODULE self = GetModuleHandleW(nullptr);
    std::vector<ModuleEntry> next;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            ModuleEntry e;
            e.base = reinterpret_cast<uint64_t>(me.modBaseAddr);
            e.size = me.modBaseSize;
            e.path = narrow(me.szExePath);
            e.name = narrow(me.szModule);
            e.direct = (reinterpret_cast<HMODULE>(
                            static_cast<uintptr_t>(e.base)) == self);
            parse_exception_dir(e);
            next.push_back(std::move(e));
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    if (next.empty()) return false;
    std::sort(next.begin(), next.end(),
              [](const ModuleEntry& a, const ModuleEntry& b) {
                  return a.base < b.base;
              });
    // Carry unchanged modules' copies across instead of re-copying every
    // refresh: the map refreshes every 2 s and the copies are the only
    // expensive part of building it.
    for (ModuleEntry& e : next) {
        bool reused = false;
        for (ModuleEntry& old : g_modules) {
            if (old.base == e.base && old.size == e.size &&
                old.path == e.path && !old.copy.empty()) {
                e.copy = std::move(old.copy);
                e.copy_lo = old.copy_lo;
                e.copy_hi = old.copy_hi;
                reused = true;
                break;
            }
        }
        if (!reused) build_unwind_copy(e);
    }
    g_modules = std::move(next);
    return true;
}

const ModuleEntry* find_module_locked(uint64_t addr) {
    // Binary search on base; modules never overlap.
    size_t lo = 0, hi = g_modules.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (g_modules[mid].base > addr) hi = mid;
        else lo = mid + 1;
    }
    if (lo == 0) return nullptr;
    const ModuleEntry& e = g_modules[lo - 1];
    if (addr < e.base || addr >= e.base + e.size) return nullptr;
    return &e;
}

// ── Unwind environment ──────────────────────────────────────────────────
struct ReadCtx {
    const uint8_t* stack_copy;
    uint64_t stack_lo;
    uint64_t stack_hi;
};

// THE WALK TOUCHES NO FOREIGN IMAGE MEMORY (beads-yjp.70). Every read a walk
// can make is served from one of exactly three places, and none of them can be
// unmapped underneath it:
//
//   1. the suspended-window stack copy (private, preallocated, and now bounded
//      at both ends by the target's own stack reservation),
//   2. the MAIN executable's image, read in place -- the process image is
//      never unmapped and every page of a mapped PE view is committed, which
//      matters because on a title build its .pdata/.xdata are tens of MB,
//   3. the private per-module unwind copy taken at snapshot time under guarded
//      reads (build_unwind_copy).
//
// Anything else returns false, which the unwinder reports as READ_FAILED. So a
// module that vanishes mid-walk costs a truncated stack and nothing more.
bool env_read(void* c, uint64_t addr, void* dst, uint32_t len) {
    const ReadCtx* rc = static_cast<const ReadCtx*>(c);
    if (len == 0u || addr + len < addr) return false;
    if (rc->stack_copy && addr >= rc->stack_lo && addr + len <= rc->stack_hi) {
        std::memcpy(dst, rc->stack_copy + (addr - rc->stack_lo), len);
        return true;
    }
    const ModuleEntry* m = find_module_locked(addr);
    if (!m) return false;
    if (m->direct) {
        // Whole-image range: every page of a mapped PE image inside
        // [base, base+SizeOfImage) is backed by the image mapping, and the
        // main image is never unmapped.
        if (addr + len > m->base + m->size) return false;
        std::memcpy(dst, reinterpret_cast<const void*>(
                             static_cast<uintptr_t>(addr)), len);
        return true;
    }
    if (!m->copy.empty() && addr >= m->copy_lo && addr + len <= m->copy_hi) {
        std::memcpy(dst, m->copy.data() + (addr - m->copy_lo), len);
        return true;
    }
    return false;
}

bool env_pdata(void* /*c*/, uint64_t pc, uint64_t* image_base,
               uint32_t* pdata_rva, uint32_t* pdata_size) {
    const ModuleEntry* m = find_module_locked(pc);
    if (!m) return false;
    *image_base = m->base;
    *pdata_rva = m->pdata_rva;
    *pdata_size = m->pdata_size;
    return true;
}

// ── Sampling ────────────────────────────────────────────────────────────
void fill_regs(const CONTEXT& ctx, NdsHostUnwindRegs& r) {
    r.gpr[0] = ctx.Rax;  r.gpr[1] = ctx.Rcx;  r.gpr[2] = ctx.Rdx;
    r.gpr[3] = ctx.Rbx;  r.gpr[4] = ctx.Rsp;  r.gpr[5] = ctx.Rbp;
    r.gpr[6] = ctx.Rsi;  r.gpr[7] = ctx.Rdi;  r.gpr[8] = ctx.R8;
    r.gpr[9] = ctx.R9;   r.gpr[10] = ctx.R10; r.gpr[11] = ctx.R11;
    r.gpr[12] = ctx.R12; r.gpr[13] = ctx.R13; r.gpr[14] = ctx.R14;
    r.gpr[15] = ctx.R15;
    r.rip = ctx.Rip;
}

void sample_target(Target& t) {
    // ── PHASE A: the suspended window ────────────────────────────────────
    // Three OS calls and one memcpy into a preallocated buffer. NOTHING here
    // may allocate or take a lock: the suspended thread can be holding the
    // heap lock or the loader lock, and a sampler that wants either one
    // deadlocks the process for good.
    const uint64_t t0 = qpc();
    if (SuspendThread(t.handle) == static_cast<DWORD>(-1)) {
        g_suspend_fail.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    t.ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    const BOOL got = GetThreadContext(t.handle, &t.ctx);
    uint32_t copied = 0;
    uint64_t rsp = 0;
    bool stack_rejected = false;
    if (got) {
        rsp = t.ctx.Rsp;
        if (rsp >= t.stack_low && rsp < t.stack_top) {
            const uint64_t avail = t.stack_top - rsp;
            copied = static_cast<uint32_t>(
                std::min<uint64_t>(avail, kStackCopyBytes));
            std::memcpy(t.stack_copy,
                        reinterpret_cast<const void*>(
                            static_cast<uintptr_t>(rsp)),
                        copied);
        } else {
            // Counted, not logged, and counted OUTSIDE the window: this is a
            // real observation about the sampled thread, so it belongs in the
            // overhead table rather than being silently indistinguishable
            // from a shallow stack.
            stack_rejected = true;
        }
    }
    ResumeThread(t.handle);
    const uint64_t t1 = qpc();
    // ── PHASE B: everything else, on the copy ────────────────────────────
    g_suspend_qpc.fetch_add(t1 - t0, std::memory_order_relaxed);
    if (stack_rejected)
        g_stack_reject.fetch_add(1u, std::memory_order_relaxed);
    if (!got) {
        g_context_fail.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
    t.copy_base = rsp;
    t.copy_len = copied;

    NdsHostUnwindRegs regs{};
    fill_regs(t.ctx, regs);
    NdsHostProfSample s{};
    s.qpc = t0;
    s.tid = t.tid;
    s.role = static_cast<uint8_t>(t.role);
    NdsHostUnwindStop stop = NDS_HOST_UNWIND_DEPTH;
    {
        std::lock_guard<std::mutex> lock(g_modules_mutex);
        ReadCtx rc{t.stack_copy, t.copy_base,
                   t.copy_base + t.copy_len};
        NdsHostUnwindEnv env{env_read, env_pdata, &rc};
        s.depth = static_cast<uint8_t>(
            nds_host_unwind(env, regs, s.frames, kNdsHostUnwindMaxFrames,
                            &stop));
    }
    s.stop = static_cast<uint8_t>(stop);
    g_stop_counts[stop].fetch_add(1u, std::memory_order_relaxed);
    g_role_counts[t.role].fetch_add(1u, std::memory_order_relaxed);
    g_ring.push(s);
    if (s.depth) g_hist.insert(s.frames[0]);
    g_samples.fetch_add(1u, std::memory_order_relaxed);
    g_walk_qpc.fetch_add(qpc() - t1, std::memory_order_relaxed);
}

void sampler_main() {
    // Above normal, not time-critical: the sampler must land on time, but a
    // profiler that can preempt the thread it is profiling is measuring itself.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    // A high-resolution one-shot timer per tick. The periodic path takes its
    // period in whole milliseconds, which caps at 1 kHz and quantises
    // everything else; a relative 100 ns due time does not.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (!timer)
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

    const uint64_t period = std::max<uint64_t>(1u, g_qpc_freq / g_hz);
    const uint64_t refresh_period = g_qpc_freq * 2u;
    uint64_t next = qpc();
    uint64_t next_refresh = 0;
    uint64_t tick = 0;

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        ++tick;
        const uint64_t now = qpc();
        if (now >= next_refresh) {
            std::lock_guard<std::mutex> lock(g_modules_mutex);
            refresh_modules_locked();
            next_refresh = now + refresh_period;
        }
        for (unsigned i = 0; i < kMaxTargets; ++i) {
            const uint32_t state =
                g_slot_state[i].load(std::memory_order_acquire);
            if (state == SLOT_RETIRING) {
                // The owning thread asked to be dropped. Only the sampler
                // closes the handle, so a retire can never pull a handle out
                // from under an in-flight suspend.
                if (g_targets[i].handle) CloseHandle(g_targets[i].handle);
                g_targets[i].handle = nullptr;
                g_slot_state[i].store(SLOT_FREE, std::memory_order_release);
                continue;
            }
            if (state != SLOT_ACTIVE) continue;
            if (g_targets[i].role != NDS_HOSTPROF_ROLE_EMU &&
                (tick % kSecondaryDivisor) != 0u)
                continue;
            sample_target(g_targets[i]);
        }
        g_ticks.fetch_add(1u, std::memory_order_relaxed);

        next += period;
        const uint64_t after = qpc();
        if (after < next) {
            const uint64_t wait_qpc = next - after;
            bool waited = false;
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -static_cast<LONGLONG>(
                    wait_qpc * 10000000ull / g_qpc_freq);
                if (due.QuadPart < 0 &&
                    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(timer, 100);
                    waited = true;
                }
            }
            if (!waited) Sleep(1);
        } else if (after > next + period * 8u) {
            // Fell far behind (a long stall, a debugger break). Re-anchor
            // instead of firing a burst of catch-up samples, which would
            // over-weight whatever the target happens to be doing next.
            next = after;
        }
    }
    if (timer) CloseHandle(timer);
}

}  // namespace

// ── Public API ──────────────────────────────────────────────────────────
void nds_hostprof_start() {
    ensure_config();
    if (!g_enabled) return;
    if (g_running.load(std::memory_order_acquire)) return;
    for (unsigned i = 0; i < kMaxTargets; ++i)
        g_slot_state[i].store(SLOT_FREE, std::memory_order_relaxed);
    if (!g_ring.init(g_ring_samples)) return;
    if (!g_hist.init()) {
        g_ring.shutdown();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_modules_mutex);
        refresh_modules_locked();
    }
    // The caller is the emu thread, and it is the thread the perf question is
    // about, so it registers itself here rather than needing a second call.
    nds_hostprof_register_current_thread(NDS_HOSTPROF_ROLE_EMU);
    g_stop_requested.store(false, std::memory_order_relaxed);
    g_running.store(true, std::memory_order_release);
    g_thread = std::thread(sampler_main);
    std::fprintf(stderr, "[hostprof] sampler on: %u Hz, %u samples "
                         "(%.1f MB, %.0f s of history)\n",
                 g_hz, g_ring.capacity(),
                 static_cast<double>(g_ring.capacity()) *
                     sizeof(NdsHostProfSample) / (1024.0 * 1024.0),
                 static_cast<double>(g_ring.capacity()) / g_hz);
}

void nds_hostprof_stop() {
    if (!g_running.load(std::memory_order_acquire)) return;
    g_stop_requested.store(true, std::memory_order_relaxed);
    if (g_thread.joinable()) g_thread.join();
    g_running.store(false, std::memory_order_release);
    for (unsigned i = 0; i < kMaxTargets; ++i) {
        if (g_targets[i].handle) CloseHandle(g_targets[i].handle);
        g_targets[i].handle = nullptr;
        delete[] g_targets[i].stack_copy;
        g_targets[i].stack_copy = nullptr;
        g_slot_state[i].store(SLOT_FREE, std::memory_order_relaxed);
    }
    // The ring and histogram are deliberately NOT freed: the shutdown
    // diagnostics writer runs after this and its whole job is to dump them.
}

bool nds_hostprof_running() {
    return g_running.load(std::memory_order_acquire);
}

void nds_hostprof_register_current_thread(NdsHostProfRole role) {
    ensure_config();
    if (!g_enabled) return;

    // Resolved dynamically rather than called directly so the build does not
    // depend on the _WIN32_WINNT the toolchain happens to default to.
    using GetStackLimitsFn = void (WINAPI*)(PULONG_PTR, PULONG_PTR);
    static GetStackLimitsFn get_limits = reinterpret_cast<GetStackLimitsFn>(
        reinterpret_cast<void*>(GetProcAddress(
            GetModuleHandleW(L"kernel32.dll"), "GetCurrentThreadStackLimits")));
    ULONG_PTR low = 0, high = 0;
    if (get_limits) get_limits(&low, &high);
    if (!high) {
        // Without a stack top the suspended-window memcpy has no provable
        // bound, and guessing one is how a sampler reads a guard page. Refuse
        // the registration instead.
        std::fprintf(stderr, "[hostprof] no stack bounds for thread %lu; "
                             "not sampling it\n",
                     GetCurrentThreadId());
        return;
    }

    HANDLE dup = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &dup,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                             THREAD_QUERY_INFORMATION,
                         FALSE, 0))
        return;

    const DWORD tid = GetCurrentThreadId();
    for (unsigned i = 0; i < kMaxTargets; ++i) {
        uint32_t expect = SLOT_FREE;
        if (!g_slot_state[i].compare_exchange_strong(
                expect, SLOT_ARMING, std::memory_order_acq_rel))
            continue;
        Target& t = g_targets[i];
        if (!t.stack_copy)
            t.stack_copy = new (std::nothrow) uint8_t[kStackCopyBytes];
        if (!t.stack_copy) {
            g_slot_state[i].store(SLOT_FREE, std::memory_order_release);
            CloseHandle(dup);
            return;
        }
        t.handle = dup;
        t.tid = tid;
        t.role = role;
        t.stack_low = static_cast<uint64_t>(low);
        t.stack_top = static_cast<uint64_t>(high);
        t.copy_base = 0;
        t.copy_len = 0;
        g_slot_state[i].store(SLOT_ACTIVE, std::memory_order_release);
        return;
    }
    CloseHandle(dup);
}

void nds_hostprof_unregister_current_thread() {
    const DWORD tid = GetCurrentThreadId();
    for (unsigned i = 0; i < kMaxTargets; ++i) {
        if (g_slot_state[i].load(std::memory_order_acquire) != SLOT_ACTIVE)
            continue;
        if (g_targets[i].tid != tid) continue;
        uint32_t expect = SLOT_ACTIVE;
        g_slot_state[i].compare_exchange_strong(expect, SLOT_RETIRING,
                                                std::memory_order_acq_rel);
        return;
    }
}

bool nds_hostprof_refresh_modules() {
    ensure_config();
    std::lock_guard<std::mutex> lock(g_modules_mutex);
    return refresh_modules_locked();
}

unsigned nds_hostprof_self_stack(uint64_t* out_frames, unsigned max,
                                 uint8_t* out_stop) {
    ensure_config();
    if (!out_frames || max == 0u) return 0u;
    alignas(16) CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    uint64_t high_limit = 0;
    using GetStackLimitsFn = void (WINAPI*)(PULONG_PTR, PULONG_PTR);
    static GetStackLimitsFn get_limits = reinterpret_cast<GetStackLimitsFn>(
        reinterpret_cast<void*>(GetProcAddress(
            GetModuleHandleW(L"kernel32.dll"), "GetCurrentThreadStackLimits")));
    if (get_limits) {
        ULONG_PTR low = 0, high = 0;
        get_limits(&low, &high);
        high_limit = high;
    }
    if (!high_limit) return 0u;
    const uint64_t rsp = ctx.Rsp;
    if (!rsp || rsp >= high_limit) return 0u;
    const uint32_t len = static_cast<uint32_t>(
        std::min<uint64_t>(high_limit - rsp, kStackCopyBytes));
    // Heap, not stack: a 16 KB on-stack buffer would sit BELOW rsp and the
    // copy would then be reading its own destination.
    std::vector<uint8_t> copy(len);
    std::memcpy(copy.data(),
                reinterpret_cast<const void*>(static_cast<uintptr_t>(rsp)),
                len);
    NdsHostUnwindRegs regs{};
    fill_regs(ctx, regs);
    NdsHostUnwindStop stop = NDS_HOST_UNWIND_DEPTH;
    unsigned depth = 0;
    {
        std::lock_guard<std::mutex> lock(g_modules_mutex);
        ReadCtx rc{copy.data(), rsp, rsp + len};
        NdsHostUnwindEnv env{env_read, env_pdata, &rc};
        depth = nds_host_unwind(env, regs, out_frames, max, &stop);
    }
    if (out_stop) *out_stop = static_cast<uint8_t>(stop);
    return depth;
}

// ── Reporting helpers ───────────────────────────────────────────────────
namespace {

// Overhead as the fraction of wall time the SAMPLED threads spent suspended,
// plus the sampler thread's own share. Reported with every table because "what
// did the observer cost" is not a separate question from "what did it observe".
std::string overhead_json() {
    const uint64_t ticks = g_ticks.load(std::memory_order_relaxed);
    const uint64_t samples = g_samples.load(std::memory_order_relaxed);
    const uint64_t susp = g_suspend_qpc.load(std::memory_order_relaxed);
    const uint64_t walk = g_walk_qpc.load(std::memory_order_relaxed);
    const double to_ms = 1000.0 / static_cast<double>(g_qpc_freq);
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "{\"ticks\":%llu,\"samples\":%llu,\"suspended_ms\":%.3f,"
        "\"walk_ms\":%.3f,\"suspended_us_per_sample\":%.3f,"
        "\"suspend_fail\":%llu,\"context_fail\":%llu,"
        "\"stack_reject\":%llu}",
        (unsigned long long)ticks, (unsigned long long)samples,
        static_cast<double>(susp) * to_ms,
        static_cast<double>(walk) * to_ms,
        samples ? static_cast<double>(susp) * to_ms * 1000.0 /
                      static_cast<double>(samples)
                : 0.0,
        (unsigned long long)g_suspend_fail.load(std::memory_order_relaxed),
        (unsigned long long)g_context_fail.load(std::memory_order_relaxed),
        (unsigned long long)g_stack_reject.load(std::memory_order_relaxed));
    return buf;
}

std::string stops_json() {
    std::string out = "{";
    for (unsigned i = 0; i < NDS_HOST_UNWIND_STOP_COUNT; ++i) {
        if (i) out += ',';
        out += '"';
        out += nds_host_unwind_stop_name(static_cast<NdsHostUnwindStop>(i));
        out += "\":";
        out += std::to_string(g_stop_counts[i].load(std::memory_order_relaxed));
    }
    return out + "}";
}

std::string roles_json() {
    std::string out = "{";
    for (unsigned i = 0; i < NDS_HOSTPROF_ROLE_COUNT; ++i) {
        if (i) out += ',';
        out += '"';
        out += nds_hostprof_role_name(static_cast<NdsHostProfRole>(i));
        out += "\":";
        out += std::to_string(g_role_counts[i].load(std::memory_order_relaxed));
    }
    return out + "}";
}

// {"name":...,"path":...,"base":...,"size":...,"pdata_rva":...,
//  "pdata_size":...,"unwind_copy":bool}
// The base is what turns a raw RIP in the dump back into module+RVA offline,
// and it has to be recorded per run because ASLR moves it every launch.
std::string modules_json_locked() {
    std::string out = "[";
    for (size_t i = 0; i < g_modules.size(); ++i) {
        const ModuleEntry& m = g_modules[i];
        if (i) out += ',';
        out += "{\"name\":\"" + json_escape_str(m.name) + "\",\"path\":\"" +
               json_escape_str(m.path) + "\",\"base\":" +
               std::to_string(m.base) + ",\"size\":" +
               std::to_string(m.size) + ",\"pdata_rva\":" +
               std::to_string(m.pdata_rva) + ",\"pdata_size\":" +
               std::to_string(m.pdata_size) + ",\"main\":" +
               (m.direct ? "true" : "false") + ",\"unwind_copy\":" +
               (m.copy.empty() ? "false" : "true") + "}";
    }
    return out + "]";
}

struct Located {
    std::string module;
    uint64_t rva;
};

Located locate_locked(uint64_t rip) {
    const ModuleEntry* m = find_module_locked(rip);
    if (!m) return Located{std::string(), rip};
    return Located{m->name, rip - m->base};
}

// Windowed self-time top-K straight off the ring. `window_sec` seconds back
// from the newest sample the SAMPLER took -- not from the querying thread's
// clock, which can be far behind if the emu thread was stalled, and being
// stalled is exactly when this gets asked.
uint32_t window_top(double window_sec, unsigned want,
                    std::vector<NdsHostProfHistEntry>& out,
                    uint64_t* out_samples, bool* out_truncated) {
    *out_samples = 0;
    *out_truncated = false;
    const uint64_t newest = g_ring.newest_qpc();
    if (!newest) return 0u;
    const uint64_t span = static_cast<uint64_t>(
        window_sec * static_cast<double>(g_qpc_freq));
    const uint64_t lo = span < newest ? newest - span : 0u;

    // Local histogram over the window. Same open-addressing shape as the
    // running table. 65536 slots rather than the window's sample count: a
    // 30 s fight window at 500 Hz is 15000 samples but can hold nearly that
    // many DISTINCT leaf RIPs (generated code is wide and flat), and a crowded
    // table drops weight off the tail of the top list -- the one failure mode
    // that would make a ranking wrong rather than merely coarse.
    struct Bucket { uint64_t rip; uint64_t count; };
    constexpr uint32_t kSlots = 1u << 16;
    std::vector<Bucket> table(kSlots, Bucket{0u, 0u});
    uint64_t counted = 0;
    uint64_t dropped = 0;

    bool torn = false, older = false;
    // A single copy_window pass is bounded by the window length at the sample
    // rate: 5 s at 1 kHz is 5000 samples, not the whole 262144-slot ring. The
    // 1024-sample slack covers a sampler running faster than g_hz nominally
    // implies (a catch-up burst after a stall).
    const uint32_t cap = std::min<uint32_t>(
        g_ring.capacity(),
        static_cast<uint32_t>(std::min<uint64_t>(
            1u << 20, static_cast<uint64_t>(window_sec * g_hz) + 1024u)));
    std::vector<NdsHostProfSample> samples(cap);
    const uint32_t n = g_ring.copy_window(lo, ~static_cast<uint64_t>(0),
                                          samples.data(), cap, &torn, &older);
    for (uint32_t i = 0; i < n; ++i) {
        if (!samples[i].depth) continue;
        const uint64_t rip = samples[i].frames[0];
        const uint32_t home = static_cast<uint32_t>(
            (rip * 0x9E3779B97F4A7C15ull) >> 48) & (kSlots - 1u);
        bool placed = false;
        for (uint32_t p = 0; p < 8u; ++p) {
            Bucket& b = table[(home + p) & (kSlots - 1u)];
            if (b.count == 0u) { b.rip = rip; b.count = 1u; placed = true; break; }
            if (b.rip == rip) { ++b.count; placed = true; break; }
        }
        if (!placed) ++dropped;
        ++counted;
    }
    *out_samples = counted;
    *out_truncated = older || dropped != 0u;
    (void)torn;

    out.clear();
    for (const Bucket& b : table)
        if (b.count) out.push_back(NdsHostProfHistEntry{b.rip, b.count});
    std::sort(out.begin(), out.end(),
              [](const NdsHostProfHistEntry& a, const NdsHostProfHistEntry& b) {
                  return a.count > b.count;
              });
    if (out.size() > want) out.resize(want);
    return static_cast<uint32_t>(out.size());
}

// The metadata line that opens a dump file. `sample_count` is a fixed-width
// STRING so the writer can seek back and patch it once the sample count is
// known -- a JSON number with leading zeros is not JSON, and a trailer would
// make the file unreadable if the process died mid-dump.
constexpr const char* kCountPlaceholder = "00000000000000000000";

std::string dump_header_locked(uint64_t qpc_lo, uint64_t qpc_hi,
                               const char* count_field) {
    std::string out = "{\"format\":\"ndsrecomp-hostprof\",\"version\":1";
    out += ",\"sample_bytes\":" + std::to_string(sizeof(NdsHostProfSample));
    out += ",\"frames_per_sample\":" +
           std::to_string(kNdsHostUnwindMaxFrames);
    out += ",\"sample_count\":\"" + std::string(count_field) + "\"";
    out += ",\"qpc_freq\":" + std::to_string(g_qpc_freq);
    out += ",\"qpc_lo\":" + std::to_string(qpc_lo);
    out += ",\"qpc_hi\":" + std::to_string(qpc_hi);
    out += ",\"qpc_now\":" + std::to_string(qpc());
    out += ",\"unix_ms_now\":" + std::to_string(unix_ms_now());
    out += ",\"hz\":" + std::to_string(g_hz);
    out += ",\"pid\":" + std::to_string(GetCurrentProcessId());
    out += ",\"ring_capacity\":" + std::to_string(g_ring.capacity());
    out += ",\"ring_written\":" + std::to_string(g_ring.written());
    out += ",\"ring_evicted\":" + std::to_string(g_ring.evicted());
    out += ",\"hist_total\":" + std::to_string(g_hist.total());
    out += ",\"hist_overflow\":" + std::to_string(g_hist.overflow());
    out += ",\"stops\":" + stops_json();
    out += ",\"roles\":" + roles_json();
    out += ",\"overhead\":" + overhead_json();
    out += ",\"modules\":" + modules_json_locked();
    out += "}\n";
    return out;
}

bool dump_impl(const char* path, double window_sec, double end_sec_ago,
               char* error, unsigned error_cap, std::string* out_summary) {
    auto fail = [&](const char* what) {
        if (error && error_cap) std::snprintf(error, error_cap, "%s", what);
        return false;
    };
    if (!path || !path[0]) return fail("no path");
    if (!g_ring.ready()) return fail("hostprof is off (NDS_HOSTPROF=off)");

    const uint64_t newest = g_ring.newest_qpc();
    if (!newest) return fail("no samples yet");
    uint64_t hi = newest;
    if (end_sec_ago > 0.0) {
        const uint64_t back = static_cast<uint64_t>(
            end_sec_ago * static_cast<double>(g_qpc_freq));
        hi = back < newest ? newest - back : 0u;
    }
    uint64_t lo = 0;
    if (window_sec > 0.0) {
        const uint64_t span = static_cast<uint64_t>(
            window_sec * static_cast<double>(g_qpc_freq));
        lo = span < hi ? hi - span : 0u;
    }

    const std::string tmp = std::string(path) + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return fail("could not open the dump file for writing");

    std::string header;
    {
        std::lock_guard<std::mutex> lock(g_modules_mutex);
        header = dump_header_locked(lo, hi, kCountPlaceholder);
    }
    // Where in the file the count placeholder lives, so it can be patched.
    // Keyed off the FIELD NAME rather than the run of zeros: a later field
    // that happened to be zero-padded would otherwise capture the search and
    // corrupt the header.
    const size_t field = header.find("\"sample_count\":\"");
    const size_t count_pos =
        field == std::string::npos
            ? std::string::npos
            : field + std::strlen("\"sample_count\":\"");
    if (std::fwrite(header.data(), 1, header.size(), f) != header.size()) {
        std::fclose(f);
        std::remove(tmp.c_str());
        return fail("short write on the dump header");
    }

    // Streamed in batches so an explicit dump of a 36 MB ring never needs a
    // 36 MB scratch buffer on the emu thread.
    std::vector<NdsHostProfSample> batch(kDumpBatch);
    uint64_t written_samples = 0;
    bool torn = false;
    const uint64_t ring_written = g_ring.written();
    const uint64_t oldest = ring_written > g_ring.capacity()
                                ? ring_written - g_ring.capacity()
                                : 0u;
    uint32_t in_batch = 0;
    bool io_ok = true;
    for (uint64_t idx = oldest; idx < ring_written && io_ok; ++idx) {
        // Forward stream, oldest first. copy_window() is the query API but its
        // backward scan needs the whole result resident; read_index() applies
        // the same eviction/tear rule one sample at a time.
        NdsHostProfSample s{};
        if (!g_ring.read_index(idx, &s)) {
            torn = true;
            continue;
        }
        if (s.qpc < lo || s.qpc > hi) continue;
        batch[in_batch++] = s;
        ++written_samples;
        if (in_batch == kDumpBatch) {
            io_ok = std::fwrite(batch.data(), sizeof(NdsHostProfSample),
                                in_batch, f) == in_batch;
            in_batch = 0;
        }
    }
    if (io_ok && in_batch)
        io_ok = std::fwrite(batch.data(), sizeof(NdsHostProfSample), in_batch,
                            f) == in_batch;
    if (io_ok && count_pos != std::string::npos) {
        char patched[32];
        std::snprintf(patched, sizeof(patched), "%020llu",
                      (unsigned long long)written_samples);
        io_ok = std::fseek(f, static_cast<long>(count_pos), SEEK_SET) == 0 &&
                std::fwrite(patched, 1, 20, f) == 20;
    }
    std::fclose(f);
    if (!io_ok) {
        std::remove(tmp.c_str());
        return fail("short write on the dump body");
    }
    // Rename over the target so a half-written dump is never handed to anyone.
    std::remove(path);
    if (std::rename(tmp.c_str(), path) != 0) {
        std::remove(tmp.c_str());
        return fail("could not rename the dump into place");
    }

    if (out_summary) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            ",\"samples\":%llu,\"header_bytes\":%llu,\"torn\":%s",
            (unsigned long long)written_samples,
            (unsigned long long)header.size(), torn ? "true" : "false");
        *out_summary = buf;
    }
    return true;
}

}  // namespace

std::string nds_hostprof_top_json(double window_sec, unsigned top) {
    ensure_config();
    if (!g_ring.ready())
        return "{\"ok\":false,\"error\":\"hostprof is off "
               "(NDS_HOSTPROF=off)\"}";
    const unsigned want = std::min<unsigned>(std::max<unsigned>(top, 1u), 512u);

    std::vector<NdsHostProfHistEntry> entries;
    uint64_t population = 0;
    bool truncated = false;
    if (window_sec > 0.0) {
        window_top(window_sec, want, entries, &population, &truncated);
    } else {
        entries.resize(want);
        const uint32_t n = g_hist.top(entries.data(), want);
        entries.resize(n);
        population = g_hist.total();
    }

    std::string out = "{\"ok\":true,\"hz\":" + std::to_string(g_hz);
    out += ",\"window_sec\":" +
           std::to_string(window_sec > 0.0 ? window_sec : 0.0);
    out += ",\"samples\":" + std::to_string(population);
    out += ",\"run_samples\":" + std::to_string(g_hist.total());
    out += ",\"truncated\":" + std::string(truncated ? "true" : "false");
    out += ",\"stops\":" + stops_json();
    out += ",\"roles\":" + roles_json();
    out += ",\"overhead\":" + overhead_json();
    out += ",\"top\":[";
    {
        std::lock_guard<std::mutex> lock(g_modules_mutex);
        for (size_t i = 0; i < entries.size(); ++i) {
            const Located loc = locate_locked(entries[i].rip);
            if (i) out += ',';
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "{\"rip\":%llu,\"count\":%llu,\"share\":%.5f,"
                          "\"module\":\"",
                          (unsigned long long)entries[i].rip,
                          (unsigned long long)entries[i].count,
                          population ? static_cast<double>(entries[i].count) /
                                           static_cast<double>(population)
                                     : 0.0);
            out += buf;
            out += json_escape_str(loc.module);
            out += "\",\"rva\":" + std::to_string(loc.rva) + "}";
        }
    }
    return out + "]}";
}

std::string nds_hostprof_status_json() {
    ensure_config();
    std::string out = "{\"supported\":true,\"running\":" +
                      std::string(nds_hostprof_running() ? "true" : "false");
    out += ",\"enabled\":" + std::string(g_enabled ? "true" : "false");
    out += ",\"hz\":" + std::to_string(g_hz);
    out += ",\"ring_capacity\":" + std::to_string(g_ring.capacity());
    out += ",\"ring_written\":" + std::to_string(g_ring.written());
    out += ",\"ring_evicted\":" + std::to_string(g_ring.evicted());
    out += ",\"ring_bytes\":" +
           std::to_string(static_cast<uint64_t>(g_ring.capacity()) *
                          sizeof(NdsHostProfSample));
    out += ",\"history_sec\":" +
           std::to_string(static_cast<double>(g_ring.capacity()) / g_hz);
    out += ",\"hist_total\":" + std::to_string(g_hist.total());
    out += ",\"hist_overflow\":" + std::to_string(g_hist.overflow());
    out += ",\"stops\":" + stops_json();
    out += ",\"roles\":" + roles_json();
    out += ",\"overhead\":" + overhead_json();
    unsigned targets = 0;
    for (unsigned i = 0; i < kMaxTargets; ++i)
        if (g_slot_state[i].load(std::memory_order_relaxed) == SLOT_ACTIVE)
            ++targets;
    out += ",\"targets\":" + std::to_string(targets);
    {
        std::lock_guard<std::mutex> lock(g_modules_mutex);
        out += ",\"module_count\":" + std::to_string(g_modules.size());
    }
    return out + "}";
}

bool nds_hostprof_dump(const char* path, double window_sec, double end_sec_ago,
                       char* error, unsigned error_cap,
                       std::string* out_summary_json) {
    return dump_impl(path, window_sec, end_sec_ago, error, error_cap,
                     out_summary_json);
}

bool nds_hostprof_write_bundle(const char* path, char* error,
                               unsigned error_cap,
                               std::string* out_summary_json) {
    return dump_impl(path, 0.0, 0.0, error, error_cap, out_summary_json);
}

// ── beads-yjp.70 regression hooks ────────────────────────────────────────
bool nds_hostprof_probe_module_parse(uint64_t base, uint64_t size,
                                     uint32_t* out_pdata_size,
                                     uint64_t* out_copy_bytes) {
    ensure_config();
    if (out_pdata_size) *out_pdata_size = 0u;
    if (out_copy_bytes) *out_copy_bytes = 0u;
    // Exactly what refresh_modules_locked() does to a freshly snapshotted
    // entry, on a caller-supplied range and without touching the live map.
    ModuleEntry e;
    e.base = base;
    e.size = size;
    e.direct = false;
    parse_exception_dir(e);
    build_unwind_copy(e);
    if (out_pdata_size) *out_pdata_size = e.pdata_size;
    if (out_copy_bytes) *out_copy_bytes = static_cast<uint64_t>(e.copy.size());
    return e.pdata_size != 0u && !e.copy.empty();
}

unsigned nds_hostprof_probe_walk_at(uint64_t pc, uint64_t* out_frames,
                                    unsigned max, uint8_t* out_stop) {
    ensure_config();
    if (out_stop) *out_stop = static_cast<uint8_t>(NDS_HOST_UNWIND_READ_FAILED);
    if (!out_frames || max == 0u) return 0u;
    NdsHostUnwindRegs regs{};
    regs.rip = pc;
    // No stack slice at all: any step that wants a return address reports
    // READ_FAILED instead of reading memory. What is under test is the module
    // side -- env_pdata plus every UNWIND_INFO read env_read serves.
    NdsHostUnwindStop stop = NDS_HOST_UNWIND_DEPTH;
    std::lock_guard<std::mutex> lock(g_modules_mutex);
    ReadCtx rc{nullptr, 0, 0};
    NdsHostUnwindEnv env{env_read, env_pdata, &rc};
    const unsigned n = nds_host_unwind(env, regs, out_frames, max, &stop);
    if (out_stop) *out_stop = static_cast<uint8_t>(stop);
    return n;
}

#endif  // NDS_HOSTPROF_SUPPORTED
