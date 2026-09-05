#include "network_savestate_guard.h"

namespace {

constexpr uint64_t kBridgeShift = 0u;
constexpr uint64_t kBridgeMask = 0x7u << kBridgeShift;
constexpr uint64_t kAssociationShift = 3u;
constexpr uint64_t kAssociationMask = 0x3u << kAssociationShift;
constexpr uint64_t kWifiDeviceActive = 1u << 5u;
constexpr uint64_t kLocalMpListening = 1u << 6u;
constexpr uint64_t kLocalMpPeerSeen = 1u << 7u;
constexpr uint64_t kNetworkConfigured = 1u << 8u;
constexpr uint64_t kBackendActive = 1u << 9u;

constexpr uint64_t field_value(uint64_t value, uint64_t shift) {
  return value << shift;
}

} // namespace

NdsNetworkSavestateGuard::NdsNetworkSavestateGuard()
    : state_(field_value(static_cast<uint64_t>(NdsNetworkBridgePhase::Detached),
                         kBridgeShift) |
             field_value(
                 static_cast<uint64_t>(NdsWifiAssociationPhase::Disconnected),
                 kAssociationShift)) {}

void NdsNetworkSavestateGuard::ReplaceField(uint64_t mask, uint64_t value) {
  uint64_t current = state_.load(std::memory_order_relaxed);
  while (!state_.compare_exchange_weak(
      current, (current & ~mask) | (value & mask), std::memory_order_release,
      std::memory_order_relaxed)) {
  }
}

void NdsNetworkSavestateGuard::BeginAttach() {
  state_.store(
      field_value(static_cast<uint64_t>(NdsNetworkBridgePhase::Attaching),
                  kBridgeShift) |
          field_value(static_cast<uint64_t>(NdsWifiAssociationPhase::Unknown),
                      kAssociationShift),
      std::memory_order_release);
}

void NdsNetworkSavestateGuard::FinishAttach() {
  ReplaceField(kBridgeMask,
               field_value(static_cast<uint64_t>(NdsNetworkBridgePhase::Ready),
                           kBridgeShift));
  SetAssociationStatus(0u);
}

void NdsNetworkSavestateGuard::BeginDetach() {
  ReplaceField(kBridgeMask, field_value(static_cast<uint64_t>(
                                            NdsNetworkBridgePhase::Detaching),
                                        kBridgeShift));
}

void NdsNetworkSavestateGuard::FinishDetach() {
  state_.store(
      field_value(static_cast<uint64_t>(NdsNetworkBridgePhase::Detached),
                  kBridgeShift) |
          field_value(
              static_cast<uint64_t>(NdsWifiAssociationPhase::Disconnected),
              kAssociationShift),
      std::memory_order_release);
}

void NdsNetworkSavestateGuard::ResetGuestWifi() {
  uint64_t current = state_.load(std::memory_order_relaxed);
  uint64_t desired = 0;
  do {
    desired = current & ~(kAssociationMask | kWifiDeviceActive |
                          kLocalMpListening | kLocalMpPeerSeen);
    desired |= field_value(
        static_cast<uint64_t>(NdsWifiAssociationPhase::Disconnected),
        kAssociationShift);
  } while (!state_.compare_exchange_weak(
      current, desired, std::memory_order_release, std::memory_order_relaxed));
}

void NdsNetworkSavestateGuard::SetAssociationStatus(uint32_t status) {
  NdsWifiAssociationPhase phase = NdsWifiAssociationPhase::Unknown;
  if (status == 0u)
    phase = NdsWifiAssociationPhase::Disconnected;
  else if (status == 1u)
    phase = NdsWifiAssociationPhase::Authenticating;
  else if (status == 2u)
    phase = NdsWifiAssociationPhase::Associated;
  ReplaceField(kAssociationMask,
               field_value(static_cast<uint64_t>(phase), kAssociationShift));
}

void NdsNetworkSavestateGuard::SetBackendState(bool configured, bool active) {
  ReplaceField(kNetworkConfigured | kBackendActive,
               (configured ? kNetworkConfigured : 0u) |
                   (active ? kBackendActive : 0u));
}

void NdsNetworkSavestateGuard::SetWifiDeviceActive(bool active) {
  ReplaceField(kWifiDeviceActive, active ? kWifiDeviceActive : 0u);
}

void NdsNetworkSavestateGuard::SetLocalMpListening(bool listening) {
  const uint64_t mask = kLocalMpListening | (listening ? 0u : kLocalMpPeerSeen);
  ReplaceField(mask, listening ? kLocalMpListening : 0u);
}

void NdsNetworkSavestateGuard::NoteLocalMpPeer() {
  ReplaceField(kLocalMpPeerSeen, kLocalMpPeerSeen);
}

void NdsNetworkSavestateGuard::ClearLocalMpPeer() {
  ReplaceField(kLocalMpPeerSeen, 0u);
}

NdsSavestateNetworkEligibility NdsNetworkSavestateGuard::Evaluate() const {
  const uint64_t state = state_.load(std::memory_order_acquire);
  NdsSavestateNetworkEligibility out{};
  out.bridge =
      static_cast<NdsNetworkBridgePhase>((state & kBridgeMask) >> kBridgeShift);
  out.association = static_cast<NdsWifiAssociationPhase>(
      (state & kAssociationMask) >> kAssociationShift);
  out.wifi_device_active = (state & kWifiDeviceActive) != 0u;
  out.network_configured = (state & kNetworkConfigured) != 0u;
  out.backend_active = (state & kBackendActive) != 0u;
  out.local_mp_listening = (state & kLocalMpListening) != 0u;
  out.local_mp_peer_seen = (state & kLocalMpPeerSeen) != 0u;

  if (out.bridge == NdsNetworkBridgePhase::Attaching ||
      out.bridge == NdsNetworkBridgePhase::Detaching ||
      out.bridge == NdsNetworkBridgePhase::Unknown) {
    out.refusal = NdsSavestateNetworkRefusal::NetworkTransition;
  } else if (out.association == NdsWifiAssociationPhase::Unknown) {
    out.refusal = NdsSavestateNetworkRefusal::NetworkTransition;
  } else if (out.association == NdsWifiAssociationPhase::Authenticating) {
    out.refusal = NdsSavestateNetworkRefusal::WifiAuthenticating;
  } else if (out.association == NdsWifiAssociationPhase::Associated) {
    out.refusal = NdsSavestateNetworkRefusal::WifiAssociated;
  } else if (out.local_mp_peer_seen) {
    out.refusal = NdsSavestateNetworkRefusal::LocalMultiplayerPeer;
  } else if (out.wifi_device_active) {
    out.refusal = NdsSavestateNetworkRefusal::WifiDeviceStateUnavailable;
  } else {
    out.allowed = true;
    out.refusal = NdsSavestateNetworkRefusal::None;
  }
  return out;
}

bool NdsNetworkSavestateGuard::AllowSavestate(std::string *error) const {
  const NdsSavestateNetworkEligibility result = Evaluate();
  if (result.allowed)
    return true;
  if (error)
    *error = nds_savestate_network_refusal_reason(result.refusal);
  return false;
}

const char *
nds_savestate_network_refusal_reason(NdsSavestateNetworkRefusal refusal) {
  switch (refusal) {
  case NdsSavestateNetworkRefusal::None:
    return "";
  case NdsSavestateNetworkRefusal::NetworkTransition:
    return "savestate unavailable while network state is transitioning";
  case NdsSavestateNetworkRefusal::WifiAuthenticating:
    return "savestate unavailable while Wi-Fi is authenticating";
  case NdsSavestateNetworkRefusal::WifiAssociated:
    return "savestate unavailable while connected to a Wi-Fi network";
  case NdsSavestateNetworkRefusal::LocalMultiplayerPeer:
    return "savestate unavailable while a local multiplayer peer is connected";
  case NdsSavestateNetworkRefusal::WifiDeviceStateUnavailable:
    return "savestate unavailable while the Wi-Fi device is powered; offline "
           "Wi-Fi device state is not yet serialized";
  }
  return "savestate unavailable because network state is unknown";
}
