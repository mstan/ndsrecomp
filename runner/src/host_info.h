// host_info.h -- static description of the machine the runner is on.
//
// This exists because performance reports arrive from machines nobody on the
// project can log into. A bundle that says "20 FPS" without saying what CPU,
// how many cores, how much RAM, which OS build and which GPU driver produced
// it cannot be triaged: the 2026-08-27 field analysis had to abandon two of
// five hypotheses purely because the host was unknown.
//
// Collected once, lazily, on first use -- none of it changes during a run --
// and then served from a cached snapshot, so the periodic diagnostics sampler
// pays nothing.
#pragma once

#include <cstdint>
#include <string>

struct NdsHostInfo {
    std::string cpu_brand;      // cpuid leaf 0x80000002..4, trimmed
    std::string cpu_vendor;     // cpuid leaf 0, e.g. "GenuineIntel"
    uint32_t cpu_cores_physical = 0;
    uint32_t cpu_cores_logical = 0;
    uint64_t ram_bytes = 0;
    std::string os_name;        // e.g. "Windows 10 Pro"
    std::string os_version;     // e.g. "10.0.19045 (22H2)"
    std::string arch;           // "x86_64", "aarch64", ...
};

// Cached snapshot; the first call performs the collection.
const NdsHostInfo& nds_host_info();

// The active renderer's GPU identification, as a display string
// ("<vendor> <renderer> / <gl version>"), or "" when no accelerated context
// was ever created (software renderer, headless run).
std::string nds_host_gpu_renderer_string();
