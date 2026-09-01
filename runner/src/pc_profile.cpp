// pc_profile.cpp -- see pc_profile.h.

#include "pc_profile.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace nds_pc_detail {

// Two megabytes of BSS for the four tables (two populations x two CPUs).
// Static rather than heap so the first sampled round costs nothing but a
// store: these tables have to exist before the first scheduler round and
// before the first dispatch, and an always-on ring buffer that allocates on
// first use has an unobserved window at exactly the moment a boot-time
// regression would be visible.
NdsPcHotTable g_table[NDS_PC_HOT_KIND_COUNT][2];

// Seeded to 1 so the FIRST dispatch on each CPU is recorded and the countdown
// then runs its full period, rather than the table staying empty for the first
// kNdsPcExecGate entries of the session (the boot's first entries are the ones
// a cold-start investigation wants most).
uint64_t g_exec_gate[2] = {1u, 1u};
uint64_t g_mmio_gate[2] = {1u, 1u};

}  // namespace nds_pc_detail

const NdsPcHotTable& nds_pc_profile_table(NdsPcHotKind kind, int cpu) {
    const NdsPcHotKind safe_kind =
        kind < NDS_PC_HOT_KIND_COUNT ? kind : NDS_PC_HOT_PARK;
    return nds_pc_detail::g_table[safe_kind][cpu & 1];
}

const char* nds_pc_profile_kind_name(NdsPcHotKind kind) {
    if (kind == NDS_PC_HOT_EXEC) return "exec";
    if (kind == NDS_PC_HOT_MMIO) return "mmio";
    return "park";
}

NdsPcHotKind nds_pc_profile_kind_from_name(const char* name) {
    // Unrecognised means PARK, never an error: pc_hot answered the park
    // question before exec existed, and a probe written against the older
    // runner sends no kind at all.
    if (name && std::strcmp(name, "exec") == 0) return NDS_PC_HOT_EXEC;
    if (name && std::strcmp(name, "mmio") == 0) return NDS_PC_HOT_MMIO;
    return NDS_PC_HOT_PARK;
}

unsigned nds_pc_profile_top_delta(const NdsPcHotTable& now,
                                  const NdsPcHotTable* prev,
                                  NdsPcHotEntry* out, unsigned max) {
    if (!out || max == 0u) return 0u;
    // Selection by insertion into a `max`-long sorted array rather than a sort
    // of the whole table: max is 8 in the perf record and 256 at its cap,
    // against 32768 slots, so this is one linear pass with a bounded shuffle
    // instead of a 512 KB sort every diagnostics interval.
    unsigned filled = 0u;
    for (uint32_t i = 0; i < kNdsPcHotSlots; ++i) {
        const NdsPcHotSlot& slot = now.slots[i];
        if (slot.count == 0u) continue;
        // Slots never move or get evicted, so the same index is the same PC in
        // both snapshots. A slot claimed since `prev` was taken finds a
        // different (or zero) PC there and contributes its whole count, which
        // is exactly its delta.
        uint64_t before = 0u;
        if (prev && prev->slots[i].pc == slot.pc) before = prev->slots[i].count;
        if (slot.count <= before) continue;
        const uint64_t delta = slot.count - before;
        if (filled == max && delta <= out[max - 1u].count) continue;
        unsigned pos = filled < max ? filled : max - 1u;
        while (pos > 0u && out[pos - 1u].count < delta) {
            out[pos] = out[pos - 1u];
            --pos;
        }
        out[pos] = NdsPcHotEntry{slot.pc, delta};
        if (filled < max) ++filled;
    }
    return filled;
}

std::string nds_pc_profile_json(NdsPcHotKind kind, int cpu, unsigned top) {
    const int index = cpu & 1;
    const NdsPcHotTable& table = nds_pc_profile_table(kind, index);
    // Cap the request rather than reject it: a probe asking for more than the
    // cap wants "as much as you have", and a hard error there would only make
    // the caller retry with a smaller number.
    const unsigned wanted = std::min<unsigned>(std::max<unsigned>(top, 1u),
                                               256u);
    NdsPcHotEntry entries[256];
    // Whole run, not an interval: the debug server's contract is the same as
    // every other always-on surface here -- snapshot twice and subtract.
    const unsigned count =
        nds_pc_profile_top_delta(table, nullptr, entries, wanted);
    char buf[128];
    std::string out;
    // `kind` is echoed so a captured reply is self-describing: the two
    // populations answer different questions and a bare top-list that does not
    // say which one it is is a trap for whoever reads the capture later.
    std::snprintf(buf, sizeof(buf),
                  "{\"cpu\":%d,\"kind\":\"%s\",\"total_samples\":%llu,"
                  "\"overflow\":%llu,\"top\":[",
                  index == 0 ? 9 : 7,
                  nds_pc_profile_kind_name(kind),
                  (unsigned long long)table.total_samples,
                  (unsigned long long)table.overflow);
    out += buf;
    for (unsigned i = 0; i < count; ++i) {
        if (i) out += ',';
        std::snprintf(buf, sizeof(buf), "{\"pc\":%llu,\"count\":%llu}",
                      (unsigned long long)entries[i].pc,
                      (unsigned long long)entries[i].count);
        out += buf;
    }
    out += "]}";
    return out;
}
