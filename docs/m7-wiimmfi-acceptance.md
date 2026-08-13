# M7 Wiimmfi peer-race acceptance

Date: 2026-08-13

Mario Kart DS reached a real two-client Nintendo WFC Friends race through the
Wiimmfi-compatible DS route.

## Accepted result

- Two separate ndsrecomp MKDS instances used distinct prepared save/firmware
  profiles.
- Both clients authenticated through the live WFC/Wiimmfi service path.
- Both friend codes were registered through the in-game Friend Code UI.
- Both clients found each other in Friends matchmaking.
- The game advanced past search into the online course-selection flow, and the
  owner confirmed visible real multiplayer gameplay.
- The final M7 validator passed with
  `race_entry_confirmed_with_bidirectional_peer_udp`.

## Network evidence

The final post-run validator observed bidirectional direct client UDP between
the two guest DS clients:

```text
m7 transport verdict: direct_client_udp_bidirectional_observed
m7 acceptance verdict: race_entry_confirmed_with_bidirectional_peer_udp
direct client UDP packets: 3904
backend errors: 0
```

The validated topology is the expected DS online model: Wiimmfi-compatible
services coordinate authentication, presence, Friend Roster, and matchmaking;
gameplay traffic then flows directly between the two guest clients on the same
pcap-backed Layer 2 network.

## Local evidence

The raw local evidence was written under the game repo's ignored capture tree:

```text
mariokartdsrecomp/generated/captures/m7-friend-roster-owner-20260813-01/final-post-run/evidence.json
```

Do not commit the raw capture directory. It can contain real local network,
DHCP, firmware, save, or service-session identifiers.
