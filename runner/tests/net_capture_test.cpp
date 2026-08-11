// net_capture_test.cpp -- Wiimmfi M8: format round-trip, corruption/
// truncation detection, and sanitizer determinism/checksum-fixup. Pure
// host-side logic, zero melonDS/emulator dependency (matches
// frontend_config_test.cpp's minimal-link style) -- this is the "comparison
// harness that can fail" proof at unit-test granularity, run on every
// build, independent of the full capture/replay-under-the-emulator proof
// described in docs/m8-capture-replay-design.md.

#include "net/net_capture.h"
#include "net/net_sanitize.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

bool require(bool condition, const char* what) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", what);
    return condition;
}

// A minimal, well-formed Ethernet+IPv4+UDP+BOOTP DHCP ACK frame carrying a
// chaddr, a client-id (option 61, htype=1 shape), and a hostname (option
// 12) -- enough surface for the sanitizer to exercise every rewrite path
// this milestone requires, with a correct (nonzero) UDP checksum so the
// test can verify the sanitizer recomputes it.
std::vector<uint8_t> BuildDhcpAckFrame(const uint8_t client_mac[6]) {
    // BOOTP body must be big enough to hold the fixed 240-byte header plus
    // every option written below through the End marker (240 fixed + 3
    // (opt53) + 9 (opt61) + 8 (opt12) + 1 (End) = 261 bytes) -- 262 gives a
    // one-byte margin.
    std::vector<uint8_t> f(14 + 20 + 8 + 262, 0);
    // Ethernet: dst=broadcast, src=client_mac, ethertype=IPv4
    for (int i = 0; i < 6; ++i) f[i] = 0xFF;
    std::memcpy(&f[6], client_mac, 6);
    f[12] = 0x08; f[13] = 0x00;
    // IPv4 header (20 bytes, no options)
    uint8_t* ip = &f[14];
    ip[0] = 0x45;  // version=4, ihl=5
    const uint16_t total_len = static_cast<uint16_t>(20 + 8 + 262);
    ip[2] = static_cast<uint8_t>(total_len >> 8);
    ip[3] = static_cast<uint8_t>(total_len);
    ip[8] = 64;   // TTL
    ip[9] = 17;   // UDP
    // src = 10.64.0.1 (server), dst = 255.255.255.255 (broadcast, typical
    // for a DHCP ACK reply the client hasn't fully configured yet)
    ip[12] = 10; ip[13] = 64; ip[14] = 0; ip[15] = 1;
    ip[16] = 255; ip[17] = 255; ip[18] = 255; ip[19] = 255;
    // UDP header
    uint8_t* udp = &f[34];
    udp[0] = 0; udp[1] = 67;   // src port 67 (DHCP server)
    udp[2] = 0; udp[3] = 68;   // dst port 68 (DHCP client)
    const uint16_t udp_len = static_cast<uint16_t>(8 + 262);
    udp[4] = static_cast<uint8_t>(udp_len >> 8);
    udp[5] = static_cast<uint8_t>(udp_len);
    // udp[6..8) checksum filled in below, after the payload exists.
    // BOOTP body
    uint8_t* bootp = &f[42];
    bootp[0] = 2;  // BOOTREPLY
    bootp[1] = 1;  // htype = Ethernet
    bootp[2] = 6;  // hlen = 6
    // yiaddr = 10.64.0.16 (the well-known lease from the task spec)
    bootp[16] = 10; bootp[17] = 64; bootp[18] = 0; bootp[19] = 16;
    std::memcpy(&bootp[28], client_mac, 6);  // chaddr
    // magic cookie
    bootp[236] = 0x63; bootp[237] = 0x82; bootp[238] = 0x53; bootp[239] = 0x63;
    size_t p = 240;
    // option 53 (message type) = 5 (ACK)
    bootp[p++] = 53; bootp[p++] = 1; bootp[p++] = 5;
    // option 61 (client id) = htype(1) + mac(6)
    bootp[p++] = 61; bootp[p++] = 7; bootp[p++] = 1;
    std::memcpy(&bootp[p], client_mac, 6); p += 6;
    // option 12 (hostname) = "ds0001" (6 bytes)
    const char* hostname = "ds0001";
    bootp[p++] = 12; bootp[p++] = 6;
    std::memcpy(&bootp[p], hostname, 6); p += 6;
    bootp[p++] = 0xFF;  // End

    // UDP checksum over pseudo-header + header + payload (mirrors
    // net_sanitize.cpp's ComputeUdpChecksum -- duplicated here
    // deliberately, so this test does not simply "agree with itself" by
    // calling the same function it is testing).
    uint32_t sum = 0;
    sum += (10u << 8) | 64u; sum += (0u << 8) | 1u;         // src ip
    sum += (255u << 8) | 255u; sum += (255u << 8) | 255u;   // dst ip
    sum += 17u;
    sum += udp_len;
    for (size_t i = 0; i + 1 < udp_len; i += 2) {
        if (i == 6) continue;
        sum += (uint32_t{udp[i]} << 8) | udp[i + 1];
    }
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    uint16_t csum = static_cast<uint16_t>(~sum & 0xFFFFu);
    if (csum == 0) csum = 0xFFFFu;
    udp[6] = static_cast<uint8_t>(csum >> 8);
    udp[7] = static_cast<uint8_t>(csum);

    return f;
}

bool TestSanitizeDeterminismAndChecksum() {
    const uint8_t mac[6] = {0x00, 0x09, 0xBF, 0x11, 0x22, 0x33};  // real-looking Nintendo OUI
    std::vector<uint8_t> frame = BuildDhcpAckFrame(mac);
    const std::vector<uint8_t> original = frame;

    // direction=Tx: this frame's own Ethernet source (`mac`) is what gets
    // learned as the identity to rewrite -- see net_sanitize.h's design
    // note on why only a TX frame's source can safely be treated this
    // way.
    NdsNetSanitizeState state;
    const bool changed = net_sanitize_ethernet_frame(
        frame.data(), frame.size(), kNdsNetSanitizeDirTx, state);
    if (!require(changed, "sanitize should report a change for a real DHCP frame"))
        return false;

    // Ethernet src MAC, ARP-equivalent BOOTP chaddr, and option-61 MAC must
    // all have been rewritten to something OTHER than the real MAC...
    if (!require(std::memcmp(&frame[6], mac, 6) != 0, "Ethernet src MAC unchanged"))
        return false;
    if (!require(std::memcmp(&frame[42 + 28], mac, 6) != 0, "chaddr unchanged"))
        return false;
    // ...and all THREE occurrences must have been rewritten to the SAME
    // synthetic value (the "stable mapping" requirement).
    if (!require(std::memcmp(&frame[6], &frame[42 + 28], 6) == 0,
                 "Ethernet src MAC and chaddr sanitized inconsistently"))
        return false;
    if (!require(std::memcmp(&frame[42 + 246], &frame[6], 6) == 0,
                 "option-61 client-id MAC sanitized inconsistently"))
        return false;
    // Locally-administered bit set on the synthetic MAC (never collides
    // with a real vendor-assigned address).
    if (!require((frame[6] & 0x02u) != 0, "synthetic MAC missing locally-administered bit"))
        return false;

    // Hostname (option 12) changed but kept the SAME length (6 bytes).
    if (!require(std::memcmp(&frame[42 + 254], "ds0001", 6) != 0,
                 "hostname option unchanged"))
        return false;

    // yiaddr (the lease) must be UNTOUCHED -- sanitizing must never break
    // the "same lease" replay proof.
    if (!require(frame[42 + 16] == 10 && frame[42 + 17] == 64 &&
                 frame[42 + 18] == 0 && frame[42 + 19] == 16,
                 "yiaddr was modified by the sanitizer"))
        return false;

    // UDP checksum must have been recomputed to something internally
    // consistent (recompute again externally and compare -- this test does
    // not just trust net_sanitize's own arithmetic, it recomputes with an
    // independent inline implementation and checks equality).
    {
        const uint8_t* udp = &frame[34];
        const uint16_t udp_len = static_cast<uint16_t>((udp[4] << 8) | udp[5]);
        uint32_t sum = 0;
        sum += (10u << 8) | 64u; sum += (0u << 8) | 1u;
        sum += (255u << 8) | 255u; sum += (255u << 8) | 255u;
        sum += 17u;
        sum += udp_len;
        for (size_t i = 0; i + 1 < udp_len; i += 2) {
            if (i == 6) continue;
            sum += (uint32_t{udp[i]} << 8) | udp[i + 1];
        }
        while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
        uint16_t expect_csum = static_cast<uint16_t>(~sum & 0xFFFFu);
        if (expect_csum == 0) expect_csum = 0xFFFFu;
        const uint16_t actual_csum = static_cast<uint16_t>((udp[6] << 8) | udp[7]);
        if (!require(expect_csum == actual_csum, "UDP checksum not correctly recomputed"))
            return false;
    }

    // Determinism: sanitizing the ORIGINAL frame again with a FRESH state
    // must produce byte-identical output (same input -> same output,
    // independent of any per-state ordering).
    std::vector<uint8_t> frame2 = original;
    NdsNetSanitizeState state2;
    net_sanitize_ethernet_frame(frame2.data(), frame2.size(),
                                 kNdsNetSanitizeDirTx, state2);
    if (!require(frame == frame2, "sanitize is not deterministic across fresh states"))
        return false;

    // Broadcast/multicast Ethernet addresses must NEVER be rewritten,
    // even though they occupy a MAC-shaped field position -- confirmed as
    // a real correctness requirement (Wiimmfi M8): rewriting a DHCP
    // OFFER/ACK's broadcast Ethernet destination made melonDS's own
    // WifiAP destination-address filter silently drop the frame before
    // the guest ever saw it.
    {
        uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        std::vector<uint8_t> bframe = BuildDhcpAckFrame(mac);
        std::memcpy(&bframe[0], bcast_mac, 6);  // Ethernet dst = broadcast
        NdsNetSanitizeState state3;
        // Learn identity from a plausible prior TX frame using `mac` first
        // (mirrors a real session: DISCOVER always precedes OFFER/ACK).
        std::vector<uint8_t> disc = BuildDhcpAckFrame(mac);
        net_sanitize_ethernet_frame(disc.data(), disc.size(),
                                     kNdsNetSanitizeDirTx, state3);
        net_sanitize_ethernet_frame(bframe.data(), bframe.size(),
                                     kNdsNetSanitizeDirRx, state3);
        if (!require(std::memcmp(&bframe[0], bcast_mac, 6) == 0,
                     "broadcast Ethernet destination was incorrectly rewritten"))
            return false;
    }

    return true;
}

bool TestCaptureRoundTrip(const std::filesystem::path& path) {
    const uint8_t mac[6] = {0x00, 0x09, 0xBF, 0xAA, 0xBB, 0xCC};
    // A real capture always has a TX frame (the guest's own DISCOVER)
    // BEFORE any RX frame that echoes its identity (the OFFER/ACK) --
    // mirror that ordering here so the writer's sanitize state has an
    // identity to work with by the time it processes the RX frame. Both
    // reuse BuildDhcpAckFrame's shape for simplicity; a real DISCOVER/ACK
    // pair would differ in msg_type, not in what this test is checking
    // (consistent identity rewriting across TX and RX in one session).
    const std::vector<uint8_t> tx_frame = BuildDhcpAckFrame(mac);
    const std::vector<uint8_t> rx_frame = BuildDhcpAckFrame(mac);

    {
        NdsNetCaptureWriter writer;
        if (!require(writer.Open(path.string(), /*sanitize=*/true,
                                  /*write_pcap=*/true, "unit-test", "deadbeef"),
                     "writer.Open failed"))
            return false;
        writer.Write(100, kNdsNetCaptureDirTx, tx_frame.data(), tx_frame.size());
        writer.Write(250, kNdsNetCaptureDirRx, rx_frame.data(), rx_frame.size());
        writer.Close();
    }
    if (!require(std::filesystem::exists(path.string() + ".pcap"),
                 "pcap sibling was not written"))
        return false;

    NdsNetCaptureReader reader;
    std::string error;
    if (!require(reader.Open(path.string(), &error), ("reader.Open failed: " + error).c_str()))
        return false;
    if (!require(reader.sanitized(), "header sanitized flag not set"))
        return false;

    std::vector<NdsNetCaptureRecord> records;
    if (!require(reader.ReadAll(&records, &error),
                 ("ReadAll failed: " + error).c_str()))
        return false;
    if (!require(records.size() == 2, "expected exactly 2 records"))
        return false;
    if (!require(records[0].guest_cycle == 100 &&
                 records[0].direction == kNdsNetCaptureDirTx,
                 "TX record round-tripped incorrectly"))
        return false;
    // Sanitize-by-default means the STORED TX frame is NOT byte-identical
    // to the raw input (its identity MAC/chaddr/hostname were rewritten)
    // -- but its yiaddr must be preserved.
    if (!require(records[0].frame != tx_frame,
                 "sanitize-by-default did not change the written TX frame"))
        return false;
    if (!require(records[0].frame[42 + 16] == 10 &&
                 records[0].frame[42 + 17] == 64 &&
                 records[0].frame[42 + 18] == 0 &&
                 records[0].frame[42 + 19] == 16,
                 "captured TX frame's yiaddr was altered by sanitize"))
        return false;
    if (!require(records[1].guest_cycle == 250 &&
                 records[1].direction == kNdsNetCaptureDirRx,
                 "RX record round-tripped incorrectly"))
        return false;
    // The RX frame went through sanitize-by-default, so it must NOT be
    // byte-identical to the raw input (its MAC/hostname were rewritten) --
    // but its yiaddr (offset 42+16 within the frame) must be preserved.
    if (!require(records[1].frame != rx_frame,
                 "sanitize-by-default did not change the written RX frame"))
        return false;
    if (!require(records[1].frame[42 + 16] == 10 &&
                 records[1].frame[42 + 17] == 64 &&
                 records[1].frame[42 + 18] == 0 &&
                 records[1].frame[42 + 19] == 16,
                 "captured RX frame's yiaddr was altered by sanitize"))
        return false;
    // The identity learned from record 0 (TX) must be the SAME synthetic
    // value applied to record 1 (RX)'s chaddr -- one console, one stable
    // mapping, carried across the whole writer session.
    if (!require(std::memcmp(&records[0].frame[6], &records[1].frame[42 + 28], 6) == 0,
                 "identity mapping was not carried consistently from TX to RX"))
        return false;
    return true;
}

bool TestTruncatedHeader(const std::filesystem::path& path) {
    // A file shorter than the fixed file header must be rejected at Open().
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(kNdsNetCaptureMagic, 4);  // far short of a full header
    }
    NdsNetCaptureReader reader;
    std::string error;
    if (!require(!reader.Open(path.string(), &error),
                 "Open should reject a too-short header"))
        return false;
    if (!require(!error.empty(), "Open should set an error message"))
        return false;
    return true;
}

bool TestTruncatedRecord(const std::filesystem::path& path) {
    // A well-formed header followed by a record header claiming a payload
    // longer than what actually follows must be reported by ReadAll, named
    // by record index -- never silently accepted as "0 records".
    {
        NdsNetCaptureWriter writer;
        require(writer.Open(path.string(), false, false, "trunc-test", ""),
                "writer.Open failed for truncation test");
        const std::vector<uint8_t> frame = {0xAA, 0xBB, 0xCC, 0xDD};
        writer.Write(42, kNdsNetCaptureDirTx, frame.data(), frame.size());
        writer.Close();
    }
    // Truncate the file to cut off the last 2 bytes of the one record's
    // 4-byte payload.
    const auto full_size = std::filesystem::file_size(path);
    {
        std::filesystem::resize_file(path, full_size - 2);
    }

    NdsNetCaptureReader reader;
    std::string error;
    if (!require(reader.Open(path.string(), &error), "Open failed on truncated-record file"))
        return false;
    std::vector<NdsNetCaptureRecord> records;
    const bool ok = reader.ReadAll(&records, &error);
    if (!require(!ok, "ReadAll should FAIL on a truncated record, not silently accept it"))
        return false;
    if (!require(error.find("record 0") != std::string::npos,
                 ("error should name record 0: " + error).c_str()))
        return false;
    std::fprintf(stderr, "  (expected) truncation error: %s\n", error.c_str());
    return true;
}

bool TestCorruptLength(const std::filesystem::path& path) {
    // A record header whose length field is implausibly large (beyond
    // kNdsNetCaptureMaxFrameBytes) must be reported as corrupt, not treated
    // as "keep reading and hope for the best."
    {
        NdsNetCaptureWriter writer;
        require(writer.Open(path.string(), false, false, "corrupt-test", ""),
                "writer.Open failed for corruption test");
        const std::vector<uint8_t> frame = {0x01, 0x02};
        writer.Write(7, kNdsNetCaptureDirRx, frame.data(), frame.size());
        writer.Close();
    }
    // Corrupt the record's `len` field (first 8 bytes are guest_cycle,
    // next 4 are len) to an out-of-range value.
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(static_cast<std::streamoff>(sizeof(NdsNetCaptureFileHeader) + 8));
        const uint32_t bogus_len = 0xFFFFFFu;
        f.write(reinterpret_cast<const char*>(&bogus_len), sizeof(bogus_len));
    }

    NdsNetCaptureReader reader;
    std::string error;
    if (!require(reader.Open(path.string(), &error), "Open failed on corrupt-length file"))
        return false;
    std::vector<NdsNetCaptureRecord> records;
    const bool ok = reader.ReadAll(&records, &error);
    if (!require(!ok, "ReadAll should FAIL on a corrupt length field"))
        return false;
    if (!require(error.find("corrupt record 0") != std::string::npos,
                 ("error should name record 0 as corrupt: " + error).c_str()))
        return false;
    std::fprintf(stderr, "  (expected) corruption error: %s\n", error.c_str());
    return true;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
                           ("nds-net-capture-test-" + std::to_string(stamp));
    fs::create_directories(root);

    int rc = 0;
    if (!TestSanitizeDeterminismAndChecksum()) rc = 1;
    if (rc == 0 && !TestCaptureRoundTrip(root / "roundtrip.ndsnetcap")) rc = 2;
    if (rc == 0 && !TestTruncatedHeader(root / "trunc_header.ndsnetcap")) rc = 3;
    if (rc == 0 && !TestTruncatedRecord(root / "trunc_record.ndsnetcap")) rc = 4;
    if (rc == 0 && !TestCorruptLength(root / "corrupt_len.ndsnetcap")) rc = 5;

    std::error_code ec;
    fs::remove_all(root, ec);
    if (rc == 0) std::fprintf(stderr, "net_capture_test: all checks passed\n");
    return rc;
}
