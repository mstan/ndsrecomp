#pragma once

#include <atomic>
#include <cstdint>
#include <string>

enum class NdsNetworkBridgePhase : uint8_t {
  Detached = 0,
  Attaching = 1,
  Ready = 2,
  Detaching = 3,
  Unknown = 4,
};

enum class NdsWifiAssociationPhase : uint8_t {
  Disconnected = 0,
  Authenticating = 1,
  Associated = 2,
  Unknown = 3,
};

enum class NdsSavestateNetworkRefusal : uint8_t {
  None = 0,
  NetworkTransition,
  WifiAuthenticating,
  WifiAssociated,
  LocalMultiplayerPeer,
  WifiDeviceStateUnavailable,
};

struct NdsSavestateNetworkEligibility {
  bool allowed = false;
  NdsSavestateNetworkRefusal refusal =
      NdsSavestateNetworkRefusal::NetworkTransition;
  NdsNetworkBridgePhase bridge = NdsNetworkBridgePhase::Unknown;
  NdsWifiAssociationPhase association = NdsWifiAssociationPhase::Unknown;
  bool wifi_device_active = false;
  bool network_configured = false;
  bool backend_active = false;
  bool local_mp_listening = false;
  bool local_mp_peer_seen = false;
};

// Thread-safe connection and device-lifecycle state used only at save/load
// transaction boundaries. All fields occupy one atomic word so a query can
// never combine halves of two network transitions.
class NdsNetworkSavestateGuard {
public:
  NdsNetworkSavestateGuard();

  void BeginAttach();
  void FinishAttach();
  void BeginDetach();
  void FinishDetach();
  void ResetGuestWifi();

  // WifiAP ClientStatus: 0=disconnected, 1=authenticated/in transition,
  // 2=associated. Other values are treated as unknown and refused.
  void SetAssociationStatus(uint32_t status);
  void SetBackendState(bool configured, bool active);
  void SetWifiDeviceActive(bool active);
  void SetLocalMpListening(bool listening);
  void NoteLocalMpPeer();
  void ClearLocalMpPeer();

  [[nodiscard]] NdsSavestateNetworkEligibility Evaluate() const;
  [[nodiscard]] bool AllowSavestate(std::string *error) const;

private:
  void ReplaceField(uint64_t mask, uint64_t value);
  std::atomic<uint64_t> state_;
};

const char *
nds_savestate_network_refusal_reason(NdsSavestateNetworkRefusal refusal);
