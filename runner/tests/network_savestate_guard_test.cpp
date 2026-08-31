#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "network_savestate_guard.h"

namespace {

bool expect(bool condition, const char *message) {
  if (!condition)
    std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}

bool offline_backend_is_allowed() {
  NdsNetworkSavestateGuard guard;
  bool ok = expect(guard.AllowSavestate(nullptr),
                   "detached network allows savestate");
  guard.BeginAttach();
  std::string error;
  ok &= expect(!guard.AllowSavestate(&error),
               "attach transition refuses savestate");
  ok &= expect(error ==
                   "savestate unavailable while network state is transitioning",
               "attach transition has precise reason");
  guard.FinishAttach();
  guard.SetBackendState(true, true);
  ok &= expect(guard.AllowSavestate(&error),
               "ready disconnected backend allows savestate");
  const NdsSavestateNetworkEligibility snapshot = guard.Evaluate();
  ok &= expect(snapshot.network_configured && snapshot.backend_active,
               "eligibility reports configured active backend separately");

  // Listening/configuration alone is not proof of another instance.
  guard.SetLocalMpListening(true);
  ok &= expect(guard.AllowSavestate(&error),
               "local transport listening without peer is allowed");
  guard.SetLocalMpListening(false);
  return ok;
}

bool connection_transitions_are_conservative() {
  NdsNetworkSavestateGuard guard;
  guard.BeginAttach();
  guard.FinishAttach();
  std::string error;
  bool ok = true;

  guard.SetAssociationStatus(1u);
  ok &=
      expect(!guard.AllowSavestate(&error), "authentication refuses savestate");
  ok &= expect(error == "savestate unavailable while Wi-Fi is authenticating",
               "authentication has precise reason");

  guard.SetAssociationStatus(2u);
  ok &= expect(!guard.AllowSavestate(&error), "association refuses savestate");
  ok &= expect(error ==
                   "savestate unavailable while connected to a Wi-Fi network",
               "association has precise reason");

  guard.SetAssociationStatus(99u);
  ok &= expect(!guard.AllowSavestate(&error),
               "unknown association state refuses savestate");
  guard.SetAssociationStatus(0u);
  ok &= expect(guard.AllowSavestate(&error),
               "deauthentication returns to eligible state");

  guard.SetLocalMpListening(true);
  guard.NoteLocalMpPeer();
  ok &= expect(!guard.AllowSavestate(&error),
               "validated local peer refuses savestate");
  ok &= expect(
      error ==
          "savestate unavailable while a local multiplayer peer is connected",
      "local peer has precise reason");
  guard.SetLocalMpListening(false);
  ok &= expect(guard.AllowSavestate(&error),
               "ending local transport clears peer lifecycle");

  guard.SetLocalMpListening(true);
  guard.NoteLocalMpPeer();
  guard.ClearLocalMpPeer();
  ok &= expect(guard.AllowSavestate(&error),
               "guest leaving MP mode clears a known peer");
  guard.SetLocalMpListening(false);
  return ok;
}

bool powered_wifi_policy_is_honest() {
  NdsNetworkSavestateGuard guard;
  guard.BeginAttach();
  guard.FinishAttach();
  guard.SetWifiDeviceActive(true);
  std::string error;
  bool ok = expect(!guard.AllowSavestate(&error),
                   "powered offline Wi-Fi refuses until device is serialized");
  ok &= expect(error ==
                   "savestate unavailable while the Wi-Fi device is powered; "
                   "offline Wi-Fi device state is not yet serialized",
               "powered device refusal identifies the remaining owner");
  guard.SetWifiDeviceActive(false);
  ok &= expect(guard.AllowSavestate(&error),
               "powered-off disconnected Wi-Fi is eligible");
  return ok;
}

bool connected_snapshot_is_race_safe() {
  NdsNetworkSavestateGuard guard;
  guard.BeginAttach();
  guard.FinishAttach();
  guard.SetAssociationStatus(2u);

  std::atomic<bool> start{false};
  std::atomic<uint32_t> false_allows{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 3; ++i) {
    threads.emplace_back([&] {
      while (!start.load(std::memory_order_acquire)) {
      }
      for (int n = 0; n < 100000; ++n) {
        if (guard.Evaluate().allowed)
          false_allows.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  threads.emplace_back([&] {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (int n = 0; n < 100000; ++n) {
      guard.SetWifiDeviceActive((n & 1) != 0);
      guard.SetLocalMpListening((n & 2) != 0);
    }
  });
  start.store(true, std::memory_order_release);
  for (std::thread &thread : threads)
    thread.join();
  return expect(
      false_allows.load(std::memory_order_relaxed) == 0u,
      "concurrent unrelated transitions cannot tear associated state");
}

} // namespace

int main() {
  bool ok = true;
  ok &= offline_backend_is_allowed();
  ok &= connection_transitions_are_conservative();
  ok &= powered_wifi_policy_is_honest();
  ok &= connected_snapshot_is_race_safe();
  return ok ? 0 : 1;
}
