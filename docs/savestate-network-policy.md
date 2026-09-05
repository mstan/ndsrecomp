# Save-state network policy

Save/load is a local deterministic transaction. It does not attempt to
snapshot, rewind, or reconnect a host socket, libslirp session, pcap adapter,
capture writer, replay cursor, or another runner process.

## Eligibility sources

`NdsNetworkSavestateGuard` is updated at the lifecycle owners rather than
polled on the emulation hot path:

- `WifiAP::ClientStatus` transitions are authoritative for infrastructure
  Wi-Fi authentication and association.
- `Platform::MP_Begin` / `MP_End` own the guest Wi-Fi power and local
  transport lifetime.
- Receipt of a validated local-MP datagram is authoritative evidence of a
  local peer. Successful UDP `sendto()` is not evidence because UDP succeeds
  when no process is listening. The vendored guest MP state machine's reset,
  deauthentication, host-loss, and beacon-disable transitions clear that peer.
- Bridge attach/detach transitions are explicit. Backend selection, an open
  idle backend, and a worker thread are not treated as a connection.

All fields are packed into one atomic word. A save/load boundary therefore
cannot observe a torn combination of association and local-peer state.

## Current decisions

- Refuse while authenticating, associated, connected to a known local peer,
  or in an attach/detach/unknown transition.
- Allow a configured or attached backend when the guest is provably
  disconnected and its Wi-Fi device is powered off.
- Refuse while the guest Wi-Fi device is powered, even if disconnected.

The final rule is a device-state correctness blocker, not a network-state
shortcut. The vendored `Wifi::DoSavestate` payload is not sufficient for the
ndsrecomp container: it stores host-native scalar bytes, does not include the
`WifiAP` client/beacon/reply state, and does not include the runner NDS shim's
pending Wi-Fi event deadline. The runner also has bounded TX/RX queues and
host backends that must never be presented as resumable socket state.

Offline powered-Wi-Fi states can become eligible only after the deterministic
guest device and event timeline has explicit little-endian encoding,
prevalidation, transactional apply, and rollback. Live host queues and socket
sessions remain excluded; connected sessions will continue to be refused.
