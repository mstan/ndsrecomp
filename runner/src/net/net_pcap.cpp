// net_pcap.cpp -- see net_pcap.h for the design rationale.

#include "net_pcap.h"

#include <cstdio>

namespace {

#pragma pack(push, 1)
struct PcapGlobalHeader {
    uint32_t magic;          // 0xa1b2c3d4 (native byte order, microsecond ts)
    uint16_t version_major;  // 2
    uint16_t version_minor;  // 4
    int32_t thiszone;        // 0 (UTC)
    uint32_t sigfigs;        // 0 (unused, always 0 per spec)
    uint32_t snaplen;        // 65535
    uint32_t network;        // LINKTYPE_ETHERNET = 1
};

struct PcapRecordHeader {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
};
#pragma pack(pop)

}  // namespace

struct NdsNetPcapWriterImpl {
    FILE* file = nullptr;
};

NdsNetPcapWriterImpl* nds_net_pcap_open(const char* path) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return nullptr;

    PcapGlobalHeader gh{};
    gh.magic = 0xa1b2c3d4u;
    gh.version_major = 2;
    gh.version_minor = 4;
    gh.thiszone = 0;
    gh.sigfigs = 0;
    gh.snaplen = 65535u;
    gh.network = 1u;  // DLT_EN10MB

    if (std::fwrite(&gh, 1, sizeof(gh), f) != sizeof(gh)) {
        std::fclose(f);
        return nullptr;
    }
    std::fflush(f);

    auto* impl = new NdsNetPcapWriterImpl();
    impl->file = f;
    return impl;
}

void nds_net_pcap_write(NdsNetPcapWriterImpl* writer, uint64_t guest_cycle,
                        uint8_t direction, const uint8_t* data, size_t len) {
    (void)direction;
    if (!writer || !writer->file || !data) return;

    const double seconds = static_cast<double>(guest_cycle) / kNdsNetPcapClockHz;
    const uint32_t ts_sec = static_cast<uint32_t>(seconds);
    const uint32_t ts_usec =
        static_cast<uint32_t>((seconds - static_cast<double>(ts_sec)) * 1e6);

    PcapRecordHeader rh{};
    rh.ts_sec = ts_sec;
    rh.ts_usec = ts_usec;
    rh.incl_len = static_cast<uint32_t>(len);
    rh.orig_len = static_cast<uint32_t>(len);

    std::fwrite(&rh, 1, sizeof(rh), writer->file);
    std::fwrite(data, 1, len, writer->file);
    std::fflush(writer->file);
}

void nds_net_pcap_close(NdsNetPcapWriterImpl* writer) {
    if (!writer) return;
    if (writer->file) std::fclose(writer->file);
    delete writer;
}
