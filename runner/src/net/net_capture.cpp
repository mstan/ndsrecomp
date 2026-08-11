// net_capture.cpp -- see net_capture.h for the format spec and design
// rationale.

#include "net_capture.h"

#include <cstring>
#include <ctime>

#include "net_pcap.h"

// ---- Writer ----------------------------------------------------------------

NdsNetCaptureWriter::~NdsNetCaptureWriter() { Close(); }

bool NdsNetCaptureWriter::Open(const std::string& path, bool sanitize,
                                bool write_pcap, const std::string& scenario_tag,
                                const std::string& rom_sha1) {
    Close();
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) return false;

    sanitize_ = sanitize;
    sanitize_state_ = NdsNetSanitizeState();

    NdsNetCaptureFileHeader hdr{};
    std::memcpy(hdr.magic, kNdsNetCaptureMagic, sizeof(hdr.magic));
    hdr.format_version = kNdsNetCaptureFormatVersion;
    hdr.header_size = static_cast<uint32_t>(sizeof(NdsNetCaptureFileHeader));
    hdr.sanitized = sanitize ? 1u : 0u;
    hdr.created_unix_time = static_cast<uint64_t>(std::time(nullptr));
    std::snprintf(hdr.rom_sha1, sizeof(hdr.rom_sha1), "%s", rom_sha1.c_str());
    std::snprintf(hdr.scenario, sizeof(hdr.scenario), "%s",
                  scenario_tag.c_str());

    if (std::fwrite(&hdr, 1, sizeof(hdr), file_) != sizeof(hdr)) {
        Close();
        return false;
    }
    std::fflush(file_);

    if (write_pcap) pcap_ = nds_net_pcap_open((path + ".pcap").c_str());
    return true;
}

void NdsNetCaptureWriter::Write(uint64_t guest_cycle, uint8_t direction,
                                 const uint8_t* data, size_t len) {
    if (!file_ || !data || len == 0) return;
    if (len > kNdsNetCaptureMaxFrameBytes) len = kNdsNetCaptureMaxFrameBytes;

    std::vector<uint8_t> buf(data, data + len);
    if (sanitize_)
        net_sanitize_ethernet_frame(buf.data(), buf.size(), direction, sanitize_state_);

    NdsNetCaptureRecordHeader rh{};
    rh.guest_cycle = guest_cycle;
    rh.len = static_cast<uint32_t>(buf.size());
    rh.direction = direction;

    std::fwrite(&rh, 1, sizeof(rh), file_);
    std::fwrite(buf.data(), 1, buf.size(), file_);
    // Flush after every record -- see net_capture.h's design note on why
    // no record count is tracked in the header: this is what makes "file
    // ends between records" (a normal taskkill) unambiguous from "file
    // ends mid-record" (real truncation/corruption).
    std::fflush(file_);

    if (pcap_) nds_net_pcap_write(pcap_, guest_cycle, direction, buf.data(),
                                   buf.size());
}

void NdsNetCaptureWriter::Close() {
    if (pcap_) { nds_net_pcap_close(pcap_); pcap_ = nullptr; }
    if (file_) { std::fclose(file_); file_ = nullptr; }
}

// ---- Reader ----------------------------------------------------------------

NdsNetCaptureReader::~NdsNetCaptureReader() { Close(); }

bool NdsNetCaptureReader::Open(const std::string& path, std::string* error) {
    Close();
    file_ = std::fopen(path.c_str(), "rb");
    if (!file_) {
        if (error) *error = "cannot open '" + path + "' for reading";
        return false;
    }

    NdsNetCaptureFileHeader hdr{};
    const size_t got = std::fread(&hdr, 1, sizeof(hdr), file_);
    if (got != sizeof(hdr)) {
        if (error)
            *error = "truncated header in '" + path + "': expected " +
                      std::to_string(sizeof(hdr)) + " bytes, got " +
                      std::to_string(got);
        Close();
        return false;
    }
    if (std::memcmp(hdr.magic, kNdsNetCaptureMagic, sizeof(hdr.magic)) != 0) {
        if (error)
            *error = "bad magic in '" + path +
                      "' -- not an NDSNETREPLAY1 capture";
        Close();
        return false;
    }
    if (hdr.format_version != kNdsNetCaptureFormatVersion) {
        if (error)
            *error = "unsupported capture format_version " +
                      std::to_string(hdr.format_version) + " (reader expects " +
                      std::to_string(kNdsNetCaptureFormatVersion) + ")";
        Close();
        return false;
    }
    if (hdr.header_size != sizeof(NdsNetCaptureFileHeader)) {
        if (error)
            *error = "header_size mismatch in '" + path + "': file says " +
                      std::to_string(hdr.header_size) + ", reader expects " +
                      std::to_string(sizeof(NdsNetCaptureFileHeader));
        Close();
        return false;
    }

    header_ = hdr;
    sanitized_ = hdr.sanitized != 0;
    return true;
}

bool NdsNetCaptureReader::ReadAll(std::vector<NdsNetCaptureRecord>* out,
                                   std::string* error) {
    if (!file_) {
        if (error) *error = "capture not open";
        return false;
    }
    if (out) out->clear();

    uint64_t index = 0;
    for (;;) {
        NdsNetCaptureRecordHeader rh{};
        const size_t got = std::fread(&rh, 1, sizeof(rh), file_);
        if (got == 0) {
            if (std::feof(file_)) return true;  // clean EOF between records
            if (error)
                *error = "read error after record " + std::to_string(index);
            return false;
        }
        if (got != sizeof(rh)) {
            if (error)
                *error = "truncated record header at record " +
                          std::to_string(index) + ": expected " +
                          std::to_string(sizeof(rh)) + " bytes, got " +
                          std::to_string(got);
            return false;
        }
        if (rh.len > kNdsNetCaptureMaxFrameBytes) {
            if (error)
                *error = "corrupt record " + std::to_string(index) +
                          ": length field " + std::to_string(rh.len) +
                          " exceeds max frame size " +
                          std::to_string(kNdsNetCaptureMaxFrameBytes);
            return false;
        }
        if (rh.direction != kNdsNetCaptureDirTx &&
            rh.direction != kNdsNetCaptureDirRx) {
            if (error)
                *error = "corrupt record " + std::to_string(index) +
                          ": invalid direction byte " +
                          std::to_string(rh.direction);
            return false;
        }

        NdsNetCaptureRecord rec;
        rec.guest_cycle = rh.guest_cycle;
        rec.direction = rh.direction;
        rec.frame.resize(rh.len);
        if (rh.len) {
            const size_t got_payload =
                std::fread(rec.frame.data(), 1, rh.len, file_);
            if (got_payload != rh.len) {
                if (error)
                    *error = "truncated record " + std::to_string(index) +
                              ": expected " + std::to_string(rh.len) +
                              " payload bytes, got " +
                              std::to_string(got_payload);
                return false;
            }
        }
        if (out) out->push_back(std::move(rec));
        ++index;
    }
}

void NdsNetCaptureReader::Close() {
    if (file_) { std::fclose(file_); file_ = nullptr; }
    sanitized_ = false;
    header_ = NdsNetCaptureFileHeader{};
}
