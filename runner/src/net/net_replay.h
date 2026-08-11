// net_replay.h -- M8 ReplayBackend: a NetDriver SIBLING implementation
// (see runner/vendor/melonds/net/NetDriver.h, which this file includes
// read-only and never modifies -- Net_Slirp and Net_PCap are the two
// existing implementations; this is the third) that replays a previously
// captured session (net_capture.h) instead of talking to any host socket
// or libslirp. See wifi_net.cpp's design comment for how this is wired
// into the bridge, and docs/m8-capture-replay-design.md for the overall
// M8 design.
//
// Contract with the rest of the bridge (all enforced by wifi_net.cpp, not
// by this class itself, which has no thread-safety of its own -- exactly
// like Net_Slirp, whose header states the same "single caller, never
// concurrent" contract for SendPacket()/PollHostSockets()):
//   - SetCurrentCycle()/SetCurrentPCs() are called immediately before
//     every SendPacket() or Net::RecvPacket() (which reaches RecvCheck())
//     call, always from the emulation thread.
//   - SendPacket() is called synchronously and directly (no queue, no
//     worker thread -- replay never touches a host socket or libslirp, so
//     there is nothing that could block).
//   - RecvCheck() delivers every RX record whose recorded guest_cycle has
//     already been reached, via `rx_callback`, synchronously, before
//     returning -- exactly once per Net::RecvPacket() call, which is
//     itself reached only from Wifi::CheckRX's guest-cycle-scheduled poll
//     (see wifi_net.cpp's Net_RecvPacket). This is what "quantized through
//     the device model's tick exactly as live frames are" means in
//     practice: a recorded frame becomes guest-visible only once a real
//     guest tick has advanced far enough to ask for it, never on its own
//     schedule.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net_capture.h"
#include "net_sanitize.h"
#include "../../vendor/melonds/net/NetDriver.h"
#include "../../vendor/melonds/Platform.h"

namespace melonDS {

// First-divergence report. `guest_cycle`/`arm9_pc`/`arm7_pc` are captured
// via SetCurrentCycle()/SetCurrentPCs() -- wifi_net.cpp populates those
// from scheduler_system_timestamp()/scheduler_cpu_state(0|1).R[15], the
// same accessors net_ring.cpp already uses for every other ring event, so
// a mismatch's cycle/PC are directly comparable against ring evidence.
struct NdsNetReplayMismatch {
    bool present = false;
    uint64_t tx_frame_index = 0;  // ordinal among TX-direction records only
    uint64_t guest_cycle = 0;
    uint32_t arm9_pc = 0;
    uint32_t arm7_pc = 0;
    std::vector<uint8_t> expected;
    std::vector<uint8_t> actual;
    std::string reason;
};

class NetReplay : public NetDriver {
public:
    // `records` is the FULL recorded sequence (both directions, in
    // original relative order) -- already loaded and fully validated by
    // NdsNetCaptureReader::ReadAll (see main.cpp's CLI validation: a
    // corrupt/truncated FILE is a startup-time error there, never a
    // replay-time surprise here). `rx_callback` matches
    // Platform::SendPacketCallback's exact shape (the same type
    // Net_Slirp's constructor takes), so wifi_net.cpp wires either backend
    // through the identical lambda signature.
    //
    // `expectations_sanitized`: whether the loaded capture's records were
    // written through net_sanitize_ethernet_frame (see
    // NdsNetCaptureFileHeader::sanitized, threaded through from main.cpp's
    // NdsNetCaptureReader::sanitized()). This matters for SendPacket()'s
    // comparison: a sanitized capture's stored TX bytes have console-
    // identifying fields (MAC, DHCP chaddr/client-id/hostname) rewritten to
    // stable synthetic values, but the LIVE guest being compared against
    // still sends its own real, unsanitized bytes (its MAC is a
    // deterministic function of the firmware image, identical on every
    // boot -- sanitizing storage must not, and does not, change what the
    // guest itself produces). Comparing raw actual bytes against sanitized
    // expected bytes would report a spurious MAC-field mismatch on the
    // very first frame of every sanitized capture, regardless of whether
    // the session actually diverged. When true, SendPacket() sanitizes a
    // SCRATCH COPY of the incoming frame with its own NdsNetSanitizeState
    // before comparing -- never the frame actually returned/accepted -- so
    // the comparison runs in the same "sanitized space" the stored
    // expectation was written in. This is sound, not just convenient,
    // specifically because net_sanitize's MAC/opaque-id/hostname mappings
    // are DETERMINISTIC BY HASH of the original bytes, not by first-seen
    // order within a given NdsNetSanitizeState -- so a brand-new state
    // here reproduces byte-for-byte the same synthetic substitutions the
    // original (different) state produced at capture time, given the same
    // real input bytes (which the guest, booting deterministically from
    // the same firmware/ROM, reproduces exactly).
    NetReplay(std::vector<NdsNetCaptureRecord> records,
              Platform::SendPacketCallback rx_callback,
              bool expectations_sanitized) noexcept;

    int SendPacket(u8* data, int len) noexcept override;
    void RecvCheck() noexcept override;

    // Not part of the NetDriver interface -- see the class's top comment
    // for the exact calling contract. Mirrors the existing
    // nds.CurrentSystemTimestamp assignment pattern already used for
    // wifi_reg_read16/write16 in wifi_net.cpp.
    void SetCurrentCycle(uint64_t cycle) noexcept { current_cycle_ = cycle; }
    void SetCurrentPCs(uint32_t arm9_pc, uint32_t arm7_pc) noexcept {
        current_arm9_pc_ = arm9_pc;
        current_arm7_pc_ = arm7_pc;
    }

    bool HasMismatch() const noexcept { return mismatch_.present; }
    const NdsNetReplayMismatch& Mismatch() const noexcept { return mismatch_; }

    uint64_t TxMatchedCount() const noexcept { return tx_matched_; }
    uint64_t TxTotalCount() const noexcept { return tx_indices_.size(); }
    uint64_t RxDeliveredCount() const noexcept { return rx_delivered_; }
    uint64_t RxTotalCount() const noexcept { return rx_indices_.size(); }

private:
    std::vector<NdsNetCaptureRecord> records_;
    std::vector<size_t> tx_indices_;  // records_ indices, direction==TX, in order
    std::vector<size_t> rx_indices_;  // records_ indices, direction==RX, in order
    // rx_required_tx_[k] = how many TX records preceded rx_indices_[k] in
    // the ORIGINAL recorded sequence. RecvCheck() will not deliver
    // rx_indices_[k] until next_tx_ >= rx_required_tx_[k] -- i.e. not
    // until the guest has already sent every TX frame that causally
    // preceded this RX frame when the session was captured. This is
    // necessary, not defensive: recorded guest_cycle alone is NOT a
    // sufficient delivery gate, because replay's own execution timing can
    // drift slightly from the original capture (confirmed empirically --
    // see net_replay.cpp's RecvCheck comment) enough that a later RX
    // frame's recorded cycle is reached before the guest has processed an
    // earlier RX frame and sent its own reply, which would otherwise
    // deliver e.g. a DHCP ACK before the guest has even sent the REQUEST
    // it is a reply to, corrupting the guest's own protocol state machine.
    std::vector<size_t> rx_required_tx_;
    size_t next_tx_ = 0;
    size_t next_rx_ = 0;
    uint64_t tx_matched_ = 0;
    uint64_t rx_delivered_ = 0;
    uint64_t current_cycle_ = 0;
    uint32_t current_arm9_pc_ = 0;
    uint32_t current_arm7_pc_ = 0;
    NdsNetReplayMismatch mismatch_;
    Platform::SendPacketCallback rx_callback_;
    bool expectations_sanitized_ = false;
    // Learns the console's real identity MAC from the guest's own first
    // outgoing frame the first time SendPacket() sanitizes a comparison
    // copy (net_sanitize_ethernet_frame's direction=Tx learning path --
    // see net_sanitize.h's design note). RecvCheck() reuses this SAME
    // learned identity (via IdentityMac()/MapMac()) to desanitize a
    // recorded RX frame before delivery -- otherwise the guest's own DHCP
    // client rejects a DHCPOFFER/ACK whose echoed `chaddr` doesn't match
    // its real hardware address, confirmed empirically.
    NdsNetSanitizeState comparison_sanitize_state_;
};

}  // namespace melonDS
