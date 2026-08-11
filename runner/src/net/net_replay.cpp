// net_replay.cpp -- see net_replay.h for the design rationale and calling
// contract.

#include "net_replay.h"

#include <cstdio>
#include <cstring>

namespace melonDS {

NetReplay::NetReplay(std::vector<NdsNetCaptureRecord> records,
                      Platform::SendPacketCallback rx_callback,
                      bool expectations_sanitized) noexcept
    : records_(std::move(records)), rx_callback_(std::move(rx_callback)),
      expectations_sanitized_(expectations_sanitized) {
    size_t tx_seen = 0;
    for (size_t i = 0; i < records_.size(); ++i) {
        if (records_[i].direction == kNdsNetCaptureDirTx) {
            tx_indices_.push_back(i);
            ++tx_seen;
        } else {
            rx_indices_.push_back(i);
            // See rx_required_tx_'s doc comment in net_replay.h: this RX
            // record may not be delivered until the guest has sent at
            // least `tx_seen` TX frames -- i.e. every TX frame that
            // preceded it when this session was originally captured.
            rx_required_tx_.push_back(tx_seen);
        }
    }
}

int NetReplay::SendPacket(u8* data, int len) noexcept {
    if (mismatch_.present) return len;  // first divergence already latched
    if (len <= 0) return 0;

    if (next_tx_ >= tx_indices_.size()) {
        // The guest sent an OUTBOUND frame past the end of the recorded TX
        // sequence -- a live session that needed one more TX frame than
        // the capture has. This IS a divergence: report it exactly like a
        // content mismatch, just with an empty "expected".
        mismatch_.present = true;
        mismatch_.tx_frame_index = next_tx_;
        mismatch_.guest_cycle = current_cycle_;
        mismatch_.arm9_pc = current_arm9_pc_;
        mismatch_.arm7_pc = current_arm7_pc_;
        mismatch_.expected.clear();
        mismatch_.actual.assign(data, data + len);
        mismatch_.reason =
            "unexpected TX frame: capture exhausted after " +
            std::to_string(tx_indices_.size()) + " recorded TX frame(s)";
        Platform::Log(Platform::Error,
            "[net_replay] MISMATCH at TX frame #%llu (guest_cycle=%llu "
            "arm9_pc=0x%08X arm7_pc=0x%08X): %s\n",
            (unsigned long long)mismatch_.tx_frame_index,
            (unsigned long long)mismatch_.guest_cycle, mismatch_.arm9_pc,
            mismatch_.arm7_pc, mismatch_.reason.c_str());
        return len;
    }

    const NdsNetCaptureRecord& expected = records_[tx_indices_[next_tx_]];

    // See the constructor's doc comment in net_replay.h: a sanitized
    // capture's stored bytes have console-identifying fields rewritten to
    // stable synthetic values, so the comparison must run in that same
    // "sanitized space" -- sanitize a SCRATCH COPY of the live frame,
    // never the frame this function actually returns/accepts.
    std::vector<u8> actual_for_compare(data, data + len);
    if (expectations_sanitized_) {
        // direction=Tx: this ALSO learns comparison_sanitize_state_'s
        // identity MAC from this frame's Ethernet source the first time
        // it runs -- see net_sanitize.h's design note. RecvCheck() below
        // relies on that same learned identity to desanitize RX frames.
        net_sanitize_ethernet_frame(actual_for_compare.data(),
                                     actual_for_compare.size(),
                                     kNdsNetSanitizeDirTx,
                                     comparison_sanitize_state_);
    }

    const bool same_len = expected.frame.size() == actual_for_compare.size();
    const bool same_bytes =
        same_len && std::memcmp(expected.frame.data(),
                                 actual_for_compare.data(),
                                 actual_for_compare.size()) == 0;

    if (!same_len || !same_bytes) {
        mismatch_.present = true;
        mismatch_.tx_frame_index = next_tx_;
        mismatch_.guest_cycle = current_cycle_;
        mismatch_.arm9_pc = current_arm9_pc_;
        mismatch_.arm7_pc = current_arm7_pc_;
        mismatch_.expected = expected.frame;
        // Diagnostic bytes are the RAW live frame (what the guest actually
        // put on the wire), not the sanitized comparison copy -- a human
        // reading a mismatch report wants the real bytes, even though the
        // PASS/FAIL decision above was made in sanitized space.
        mismatch_.actual.assign(data, data + len);

        char reason_buf[192];
        if (!same_len) {
            std::snprintf(reason_buf, sizeof(reason_buf),
                "length mismatch: expected %zu bytes, actual %zu bytes%s",
                expected.frame.size(), actual_for_compare.size(),
                expectations_sanitized_ ? " (compared post-sanitize)" : "");
        } else {
            size_t off = 0;
            while (off < expected.frame.size() &&
                   expected.frame[off] == actual_for_compare[off])
                ++off;
            std::snprintf(reason_buf, sizeof(reason_buf),
                "byte mismatch at offset %zu: expected 0x%02X, actual 0x%02X%s",
                off, expected.frame[off], actual_for_compare[off],
                expectations_sanitized_ ? " (compared post-sanitize)" : "");
        }
        mismatch_.reason = reason_buf;

        Platform::Log(Platform::Error,
            "[net_replay] MISMATCH at TX frame #%llu (guest_cycle=%llu "
            "arm9_pc=0x%08X arm7_pc=0x%08X): %s\n",
            (unsigned long long)mismatch_.tx_frame_index,
            (unsigned long long)mismatch_.guest_cycle, mismatch_.arm9_pc,
            mismatch_.arm7_pc, mismatch_.reason.c_str());
        // Still "accept" the send at the driver-interface level: the guest
        // must not stall or be denied progress because a comparison
        // failed -- the failure is REPORTED, never enforced as a hang.
        return len;
    }

    ++next_tx_;
    ++tx_matched_;
    return len;
}

void NetReplay::RecvCheck() noexcept {
    while (next_rx_ < rx_indices_.size()) {
        const NdsNetCaptureRecord& rec = records_[rx_indices_[next_rx_]];
        if (rec.guest_cycle > current_cycle_) break;  // not due yet
        // Causal gate -- see rx_required_tx_'s doc comment in
        // net_replay.h. Without this, a later RX frame's recorded cycle
        // being reached (e.g. because replay's own pacing drifted from
        // the original capture's) can deliver it before the guest has
        // sent the TX frame it is causally a reply to -- confirmed
        // empirically to corrupt a live DHCP client's state machine
        // (delivering the ACK before the guest had sent its REQUEST made
        // the guest abandon the exchange and restart from DISCOVER).
        if (next_tx_ < rx_required_tx_[next_rx_]) break;
        if (rx_callback_) {
            if (expectations_sanitized_ && comparison_sanitize_state_.HasIdentity()) {
                // Deliver a DESANITIZED COPY -- never mutate records_
                // itself, so a later diagnostic dump or repeated delivery
                // always sees the exact bytes the capture file stored.
                // See net_replay.h's member doc comment and
                // net_sanitize.h's function doc comment for why this
                // reversal is necessary for protocol correctness, not
                // just cosmetic. The identity/synthetic MAC pair comes
                // from the SAME comparison_sanitize_state_ SendPacket()
                // already populated -- one learned identity, shared by
                // both the TX comparison and the RX desanitize paths.
                const std::array<u8, 6> identity =
                    comparison_sanitize_state_.IdentityMac();
                const std::array<u8, 6> synthetic =
                    comparison_sanitize_state_.MapMac(identity.data());
                std::vector<u8> to_deliver = rec.frame;
                net_desanitize_ethernet_frame_for_replay(
                    to_deliver.data(), to_deliver.size(), synthetic.data(),
                    identity.data());
                rx_callback_(to_deliver.data(),
                             static_cast<int>(to_deliver.size()));
            } else {
                rx_callback_(rec.frame.data(),
                             static_cast<int>(rec.frame.size()));
            }
        }
        ++next_rx_;
        ++rx_delivered_;
    }
}

}  // namespace melonDS
