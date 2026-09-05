// host_info.cpp -- see host_info.h.

#include "host_info.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#if defined(NDS_HAVE_COMPUTE_RENDERER)
#include "melonds_compute/ComputeHost.h"
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#elif defined(_MSC_VER)
#include <intrin.h>
#endif

namespace {

std::string trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

#if defined(__x86_64__) || defined(__i386__) || defined(_MSC_VER)
bool cpuid_regs(unsigned leaf, unsigned regs[4]) {
#if defined(_MSC_VER) && !defined(__GNUC__)
    int out[4];
    __cpuid(out, static_cast<int>(leaf));
    for (int i = 0; i < 4; ++i) regs[i] = static_cast<unsigned>(out[i]);
    return true;
#else
    return __get_cpuid(leaf, &regs[0], &regs[1], &regs[2], &regs[3]) != 0;
#endif
}

// Leaf 0 returns the 12-byte vendor id split across EBX, EDX, ECX -- in that
// order, which is NOT the register order.
std::string cpu_vendor_string() {
    unsigned r[4] = {};
    if (!cpuid_regs(0, r)) return {};
    char buf[13] = {};
    std::memcpy(buf + 0, &r[1], 4);
    std::memcpy(buf + 4, &r[3], 4);
    std::memcpy(buf + 8, &r[2], 4);
    return trim(buf);
}

// Extended leaves 0x80000002..0x80000004 hold the 48-byte brand string, each
// leaf contributing all four registers in order. Availability is gated on
// leaf 0x80000000 reporting at least 0x80000004.
std::string cpu_brand_string() {
    unsigned probe[4] = {};
    if (!cpuid_regs(0x80000000u, probe)) return {};
    if (probe[0] < 0x80000004u) return {};
    char buf[49] = {};
    for (unsigned leaf = 0; leaf < 3u; ++leaf) {
        unsigned r[4] = {};
        if (!cpuid_regs(0x80000002u + leaf, r)) return {};
        std::memcpy(buf + leaf * 16u, r, 16);
    }
    buf[48] = '\0';
    return trim(buf);
}
#else
std::string cpu_vendor_string() { return {}; }
std::string cpu_brand_string() { return {}; }
#endif

const char* host_arch() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

#if defined(_WIN32)
// Physical core count: the number of RelationProcessorCore entries, which is
// what distinguishes an 8-core/16-thread part from a 16-core one. Logical
// count alone hides SMT, and SMT pressure is a plausible cause of exactly the
// kind of dispatch-cost difference this instrumentation is measuring.
uint32_t physical_core_count() {
    DWORD bytes = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr,
                                         &bytes) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
        return 0;
    }
    std::vector<unsigned char> buffer(bytes);
    auto* first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
        buffer.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, first,
                                          &bytes)) {
        return 0;
    }
    uint32_t cores = 0;
    DWORD offset = 0;
    while (offset < bytes) {
        auto* entry =
            reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                buffer.data() + offset);
        if (entry->Size == 0) break;
        if (entry->Relationship == RelationProcessorCore) ++cores;
        offset += entry->Size;
    }
    return cores;
}

std::string registry_string(const char* value) {
    char buf[256] = {};
    DWORD size = sizeof(buf);
    // The CurrentVersion key is the honest source. GetVersionEx lies to
    // un-manifested processes (it reports 6.2 on Windows 10/11), which would
    // make every field bundle claim Windows 8.
    if (RegGetValueA(HKEY_LOCAL_MACHINE,
                     "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", value,
                     RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) {
        return {};
    }
    return trim(buf);
}

uint32_t registry_dword(const char* value) {
    DWORD out = 0;
    DWORD size = sizeof(out);
    if (RegGetValueA(HKEY_LOCAL_MACHINE,
                     "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", value,
                     RRF_RT_REG_DWORD, nullptr, &out, &size) != ERROR_SUCCESS) {
        return 0;
    }
    return static_cast<uint32_t>(out);
}
#endif

NdsHostInfo collect() {
    NdsHostInfo info;
    info.arch = host_arch();
    info.cpu_vendor = cpu_vendor_string();
    info.cpu_brand = cpu_brand_string();
    info.cpu_cores_logical = std::thread::hardware_concurrency();

#if defined(_WIN32)
    info.cpu_cores_physical = physical_core_count();

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) info.ram_bytes = mem.ullTotalPhys;

    const std::string product = registry_string("ProductName");
    info.os_name = product.empty() ? "Windows" : product;
    const uint32_t major = registry_dword("CurrentMajorVersionNumber");
    const uint32_t minor = registry_dword("CurrentMinorVersionNumber");
    const std::string build = registry_string("CurrentBuildNumber");
    const std::string display = registry_string("DisplayVersion");
    char version[128] = {};
    if (major) {
        std::snprintf(version, sizeof(version), "%u.%u.%s%s%s%s", major, minor,
                      build.empty() ? "0" : build.c_str(),
                      display.empty() ? "" : " (", display.c_str(),
                      display.empty() ? "" : ")");
    } else {
        std::snprintf(version, sizeof(version), "%s",
                      build.empty() ? "unknown" : build.c_str());
    }
    info.os_version = version;
    // ProductName stayed "Windows 10 ..." on Windows 11 for compatibility;
    // build 22000 is the real dividing line. Say so rather than mislabelling.
    if (!build.empty() && std::strtoul(build.c_str(), nullptr, 10) >= 22000ul &&
        info.os_name.find("Windows 10") != std::string::npos) {
        info.os_name += " (reported; build >= 22000 means Windows 11)";
    }
#else
    const long conf_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (conf_cores > 0)
        info.cpu_cores_physical = static_cast<uint32_t>(conf_cores);
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        info.ram_bytes =
            static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size);
    }
    utsname uts{};
    if (uname(&uts) == 0) {
        info.os_name = uts.sysname;
        info.os_version = std::string(uts.release) + " " + uts.version;
    }
#endif

    if (info.cpu_brand.empty()) info.cpu_brand = "unknown";
    if (info.os_name.empty()) info.os_name = "unknown";
    if (info.os_version.empty()) info.os_version = "unknown";
    return info;
}

}  // namespace

const NdsHostInfo& nds_host_info() {
    static const NdsHostInfo info = collect();
    return info;
}

std::string nds_host_gpu_renderer_string() {
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    const char* renderer = nds_compute_host_gl_renderer();
    if (!renderer || !renderer[0]) return {};
    const char* vendor = nds_compute_host_gl_vendor();
    const char* version = nds_compute_host_gl_version();
    std::string out;
    if (vendor && vendor[0]) {
        out += vendor;
        out += ' ';
    }
    out += renderer;
    if (version && version[0]) {
        out += " / ";
        out += version;
    }
    return out;
#else
    return {};
#endif
}
