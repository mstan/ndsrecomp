// net_capture.h -- M8 packet capture/replay file format + writer/reader
// (Wiimmfi meta epic, "capture at the backend boundary"). See
// docs/m8-capture-replay-design.md for the full design and rationale; this
// header is the authoritative byte-layout spec -- tools/net_capture_tool.py
// re-implements the SAME layout independently in Python, so any change
// here must be mirrored there.
//
// ---- Why a new format, and why this shape -----------------------------
// Nothing in this repo captures packets today (generated/firmware_capture/
// holds memory snapshots, not frames) -- this is genuinely new territory.
// The task's own sketch was a text log: `NDSNETREPLAY1` with
// `cycle=... dir=RX len=...` records. A pure text log would need a
// per-line escaping/encoding scheme for arbitrary binary frame bytes
// (DHCP/DNS payloads are not safely representable as one text line without
// something like base64 -- a tax this project doesn't otherwise pay
// anywhere else), so this format keeps the same self-describing magic/
// version STRING the sketch proposed (readable at a glance in a hex dump
// or `strings`) but pairs it with a small FIXED binary record header
// followed by the raw frame bytes -- exactly as simple to write and read
// as the text sketch, with none of the encoding overhead. `cycle=...
// dir=RX len=...` becomes three fixed-width binary fields
// (NdsNetCaptureRecordHeader) instead of three key=value tokens; the
// information carried is identical.
//
// Per-frame record, not per-packet-plus-side-metadata-elsewhere: capture
// happens at exactly the boundary named in the task (Net_SendPacket/
// Net_RecvPacket in wifi_net.cpp), which already has, at the moment each
// frame crosses it, precisely the three things worth recording -- the
// guest cycle (scheduler_system_timestamp(), the same "sys" domain
// net_ring.cpp already timestamps every event against), the direction
// (0=TX/guest->host, 1=RX/host->guest, matching NdsNetTraceEntry::
// direction's existing convention exactly), and the complete Ethernet
// frame. No sequence/session framing beyond that is needed: replay
// consumes the two directions as two independently-ordered sub-sequences
// (see net_replay.h), and a flat list of (cycle, dir, frame) records is
// suficient to reconstruct both.
//
// A record COUNT is deliberately NOT stored in the file header: a writer
// that is mid-session when the process is killed (this project's own
// documented shutdown convention -- taskkill, not a clean quit; see
// CLAUDE.md's "Kill stale servers" note) would need a seek-back-and-patch
// on close to keep a stored count correct, and a killed process never
// reaches that. Instead, NdsNetCaptureWriter::Write flushes (fflush) after
// every record, so the file on disk is always exactly "N complete records"
// for whatever N was reached before the kill, and the reader simply reads
// records until a clean EOF. This also makes "the file ends mid-record"
// (a genuinely truncated/corrupted capture) and "the file ends cleanly
// between records" (a normal kill-while-idle) unambiguously distinguishable
// -- see NdsNetCaptureReader::ReadAll's exact contract below, which is the
// comparison harness's first line of defense against a corrupted capture
// (task requirement: "a comparison harness that cannot fail is not a
// comparison harness").
//
// ---- pcap/pcapng alongside the native format ---------------------------
// NdsNetCaptureWriter also writes a classic-pcap sibling file (see
// net_pcap.h) by default -- Wireshark can open it directly, which is a
// large diagnostic win for later NATNEG/TCP work even though this
// milestone's own acceptance bar is DHCP. It is a SIBLING, not a
// replacement: pcap's native per-packet timestamp is a (seconds,
// microseconds) pair, which cannot exactly carry an arbitrary 64-bit guest
// cycle count without a lossy scale/round-trip, so this project's own
// replay path (net_replay.h) reads ONLY the native NDSNETREPLAY1 file,
// never the .pcap. The .pcap is diagnostic-only.
//
// ---- Privacy is a write-time decision, not a read-time one -------------
// Sanitization (net_sanitize.h) happens INSIDE NdsNetCaptureWriter::Write,
// before a frame's bytes ever reach disk, and defaults to ON at every
// construction call site in this codebase (see wifi_net.cpp) --
// "--net-capture-raw" is the only opt-out. This file's own
// NdsNetCaptureFileHeader::sanitized flag records which choice was made,
// so a reader (this project's own CLI validation in main.cpp, or
// tools/net_capture_tool.py's `check`/`publish` subcommands) can refuse to
// treat an unsanitized file as safe to commit -- see
// docs/m8-capture-replay-design.md for the full defense-in-depth argument
// (sanitize-by-default at the point of capture, PLUS a hard refusal at the
// point of publishing into a tracked path).

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "net_sanitize.h"

// Magic/version tag -- exactly the string the task's own format sketch
// proposed, NUL-padded to a fixed 16 bytes so the header's layout past it
// never depends on strlen(). A future incompatible format change bumps
// the trailing digit (NDSNETREPLAY2, ...); NdsNetCaptureReader::Open
// rejects anything it doesn't recognize by exact match, never guesses.
inline constexpr char kNdsNetCaptureMagic[16] = "NDSNETREPLAY1";

constexpr uint32_t kNdsNetCaptureFormatVersion = 1;

// Mirrors wifi_net.cpp's kWifiNetMaxPacketBytes exactly (Net_Slirp's own
// `len > 2048` rejection) -- no frame that ever crosses the backend
// boundary this project captures at can exceed this, so it doubles as the
// reader's corruption sanity bound: a record whose length field claims
// more than this is definitely corrupt, not a legitimately large frame.
constexpr uint32_t kNdsNetCaptureMaxFrameBytes = 2048;

constexpr uint8_t kNdsNetCaptureDirTx = 0;  // guest -> host (egress)
constexpr uint8_t kNdsNetCaptureDirRx = 1;  // host -> guest (ingress)

#pragma pack(push, 1)
struct NdsNetCaptureFileHeader {
    char magic[16];
    uint32_t format_version;
    uint32_t header_size;   // sizeof(NdsNetCaptureFileHeader); self-describing,
                             // so a reader can detect "this is format_version 1
                             // but a header layout I don't recognize" rather
                             // than silently misreading record 0
    uint8_t sanitized;      // 1 = MAC/DHCP-identifier bytes were rewritten by
                             // net_sanitize_ethernet_frame before being
                             // written (see the privacy note above); 0 = raw,
                             // must never be committed
    uint8_t reserved0[3];
    uint64_t created_unix_time;  // provenance only, never guest-observable,
                                   // never compared during replay
    char rom_sha1[41];      // lowercase hex ROM SHA-1 the capture was taken
                             // against, "" if unknown; NUL-padded
    char scenario[64];      // free-form human label, e.g. "dhcp"; NUL-padded
};

struct NdsNetCaptureRecordHeader {
    uint64_t guest_cycle;  // scheduler_system_timestamp() at capture time
    uint32_t len;          // frame length in bytes; <= kNdsNetCaptureMaxFrameBytes
    uint8_t direction;     // kNdsNetCaptureDirTx or kNdsNetCaptureDirRx
    uint8_t reserved[3];
};
#pragma pack(pop)

// In-memory decoded record -- what NdsNetCaptureReader::ReadAll and
// net_replay.h actually work with.
struct NdsNetCaptureRecord {
    uint64_t guest_cycle = 0;
    uint8_t direction = 0;
    std::vector<uint8_t> frame;
};

// ---- Writer --------------------------------------------------------------
class NdsNetCaptureWriter {
public:
    ~NdsNetCaptureWriter();

    // Opens `path` for writing (truncating any existing file) and, unless
    // `write_pcap` is false, a sibling "<path>.pcap" alongside it. Returns
    // false (no file left open) on any open/write failure for the PRIMARY
    // file; a pcap sibling failure is logged by the caller and does not
    // fail this call (see net_pcap.h's own doc comment).
    bool Open(const std::string& path, bool sanitize, bool write_pcap,
              const std::string& scenario_tag, const std::string& rom_sha1);

    // Sanitizes (if enabled) a COPY of [data,len) and appends one record,
    // flushing immediately (see the design note above on why no record
    // count is tracked). Safe to call only from the thread that owns this
    // writer -- matches every net_ring call site's single-writer
    // assumption; see wifi_net.cpp's call sites (the emulation thread).
    void Write(uint64_t guest_cycle, uint8_t direction, const uint8_t* data,
               size_t len);

    void Close();
    bool is_open() const { return file_ != nullptr; }
    bool wrote_pcap() const { return pcap_ != nullptr; }

private:
    FILE* file_ = nullptr;
    bool sanitize_ = true;
    NdsNetSanitizeState sanitize_state_;
    struct NdsNetPcapWriterImpl* pcap_ = nullptr;
};

// ---- Reader ---------------------------------------------------------------
class NdsNetCaptureReader {
public:
    ~NdsNetCaptureReader();

    // Opens the file and validates the header (magic, format_version,
    // header_size). false + *error set on any problem, including a
    // missing file or a header too short to be this format at all.
    bool Open(const std::string& path, std::string* error);

    bool sanitized() const { return sanitized_; }
    const NdsNetCaptureFileHeader& header() const { return header_; }

    // Reads every remaining record from the current file position,
    // validating each one:
    //   - a truncated record header (some bytes but fewer than
    //     sizeof(NdsNetCaptureRecordHeader)) is reported by record index
    //     and byte counts;
    //   - a length field exceeding kNdsNetCaptureMaxFrameBytes is reported
    //     as corrupt (see the constant's own doc comment for why this
    //     bound is exact, not a guess);
    //   - an invalid direction byte is reported as corrupt;
    //   - a short payload read (fewer than the declared `len` bytes) is
    //     reported as truncated.
    // A clean end-of-file exactly BETWEEN two records (zero bytes read
    // where a next record header would start) is NOT an error -- returns
    // true with whatever records were read. This is the exact contract
    // the M8 task's "comparison harness that can fail" requirement needs:
    // a genuinely truncated/corrupted capture is reported with the offending
    // record index, never silently accepted as "fewer records, still a
    // valid empty-ish capture."
    bool ReadAll(std::vector<NdsNetCaptureRecord>* out, std::string* error);

    void Close();

private:
    FILE* file_ = nullptr;
    bool sanitized_ = false;
    NdsNetCaptureFileHeader header_{};
};
