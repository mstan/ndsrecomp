// net_ring_test.cpp -- host-side coverage for the always-on network ring's
// compact DNS hostname side table.

#include "net/net_ring.h"

#include "runtime_arm.h"

#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
uint64_t g_insn_count[2] = {0, 0};
}

namespace {

ArmCpuState g_test_cpu[2] = {};
uint64_t g_test_cycles[2] = {};
uint64_t g_test_sys = 0;

bool require(bool condition, const char* what) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", what);
    return condition;
}

}  // namespace

const ArmCpuState& scheduler_cpu_state(int cpu) {
    return g_test_cpu[cpu ? 1 : 0];
}

uint64_t scheduler_cpu_cycles(int cpu) {
    return g_test_cycles[cpu ? 1 : 0];
}

uint64_t scheduler_system_timestamp() {
    return g_test_sys++;
}

bool BasicHostnameLookup() {
    net_ring_reset();
    const uint64_t count = net_ring_push(
        NDS_NET_EVENT_DNS_QUERY, 0, 0, 0,
        nullptr, nullptr, 0x0A400020u, 0xB23E2BD4u,
        50203, 53, 0, 0);
    net_ring_set_hostname(count, "naswii.nintendowifi.net");

    NdsNetTraceEntry entry{};
    char hostname[kNetHostnameMaxLen] = {};
    return require(net_ring_get(count, &entry), "DNS event still retained") &&
           require(entry.has_hostname == 1, "DNS event has hostname flag") &&
           require(net_ring_get_hostname(count, hostname, sizeof(hostname)),
                   "hostname lookup succeeds") &&
           require(std::strcmp(hostname, "naswii.nintendowifi.net") == 0,
                   "hostname text matches");
}

bool HostnameSideRingEviction() {
    net_ring_reset();
    const uint64_t first = net_ring_push(
        NDS_NET_EVENT_DNS_QUERY, 0, 0, 0,
        nullptr, nullptr, 0x0A400020u, 0xB23E2BD4u,
        50203, 53, 0, 0);
    net_ring_set_hostname(first, "first.example");

    uint64_t last = 0;
    std::string last_name;
    for (uint32_t i = 0; i < kNetHostnameSlots; ++i) {
        last = net_ring_push(
            NDS_NET_EVENT_DNS_RESPONSE, 1, 0, 0,
            nullptr, nullptr, 0xB23E2BD4u, 0x0A400020u,
            53, 50203, 0, 0);
        last_name = "host" + std::to_string(i) + ".example";
        net_ring_set_hostname(last, last_name.c_str());
    }

    NdsNetTraceEntry first_entry{};
    char hostname[kNetHostnameMaxLen] = {};
    return require(net_ring_get(first, &first_entry),
                   "main trace still retains side-evicted DNS event") &&
           require(first_entry.has_hostname == 1,
                   "side-evicted event keeps hostname flag") &&
           require(!net_ring_get_hostname(first, hostname, sizeof(hostname)),
                   "side-evicted hostname lookup fails") &&
           require(net_ring_get_hostname(last, hostname, sizeof(hostname)),
                   "latest hostname lookup succeeds") &&
           require(hostname == last_name, "latest hostname text matches");
}

int main() {
    bool ok = true;
    ok &= BasicHostnameLookup();
    ok &= HostnameSideRingEviction();
    return ok ? 0 : 1;
}
