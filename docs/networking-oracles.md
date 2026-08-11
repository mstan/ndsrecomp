# Networking provenance ledger (Wiimmfi/WFC effort)

Companion to [`docs/references.md`](references.md) and
[`THIRD_PARTY_ATTRIBUTION.md`](../THIRD_PARTY_ATTRIBUTION.md), scoped to the
Nintendo WFC / Wiimmfi networking effort specifically. Same policy, same
enforcement: **library dependency → must be permissive-compatible.
Oracle/reference → any license, as long as no code is incorporated into the
permissively-licensed runner.**

This file is maintained by the "external facts" research strand (R4). It
records what was *inspected* as of 2026-08-10, not a promise about what the
implementation strands have actually wired up — check `git log`/the runner
source for current integration state.

## Ledger

| Project | License | Used as dependency? | Used as oracle? | Code copied into permissive runner? | Notes |
|---|---|---|---|---|---|
| **melonDS** | GPL-3.0-or-later | Yes, but *only* for the unrelated 3D geometry engine (`runner/vendor/melonds/`, see `THIRD_PARTY_ATTRIBUTION.md:49-91`) — **not** for networking. | Yes — behavioral/timing oracle for Wi-Fi register semantics, MAC behavior, IRQ sequencing, AP/association behavior, and the Slirp integration boundary (plan `E:\Downloads\PLAN_ndsrecomp_wiimmfi.md` §3.2). | No, for the networking subsystem specifically. GPU3D code is copied (separate boundary, already declared and already makes the runner binary a GPL combined work — see attribution doc). | `src/net/Net_Slirp.cpp`/`.h` (melonDS's own libslirp glue, GPL-3.0-or-later) must **not** be read-and-ported line-for-line into `runner/src/net/slirp_backend.cpp`. Its *shape* (poll-fd adapter around `slirp_new`/`slirp_pollfds_fill`/`slirp_pollfds_poll`/`slirp_input`) is public libslirp API usage and is fine to reproduce independently; the melonDS-authored glue text is not. |
| **libslirp** (vendored copy at `F:\Projects\ndsrecomp\ndsref\third_party\melonDS\src\net\libslirp\`) | **BSD-3-Clause** — confirmed in two independent places in the vendored tree: `meson.build:3` (`license : 'BSD-3-Clause'`) and `COPYRIGHT:12-40` (Danny Gasparovski's 1995/1996 3-clause BSD text; `libslirp.h:1` also carries an SPDX `BSD-3-Clause` header). `COPYRIGHT:52-59` lists incorporated code from Juha Pirkola, Univ. of California Regents, CMU, ANU, and RSA Data Security — all permissively/BSD-style licensed pieces (e.g. RSA's is the classic permissive MD5 reference code), so none of these downgrade the top-level grant. | **Recommended: yes**, as a real dependency (see verdict below). Not yet vendored into `ndsrecomp-wiimmfi` as of this writing. | Also usable as a build-configuration oracle: melonDS's own `net/libslirp/CMakeLists.txt` and `net/CMakeLists.txt` show a working mingw-buildable CMake path that bypasses the upstream meson+real-glib2 requirement (see `wfc-external-facts.md` Strand B for detail). | No — if vendored, vendor pristine upstream `libslirp` source (BSD-3-Clause) directly, not melonDS's copy-with-modifications. melonDS's `net/libslirp/glib/glib.c`/`.h` shim and its hand-written `CMakeLists.txt` are melonDS-authored files sitting *inside* the otherwise-BSD libslirp tree — check their own header/license note before treating them as BSD; they were not part of upstream libslirp's original release and should be re-derived independently (they are simple enough — a glib-subset shim and a source-list CMakeLists — that this is low-risk) rather than copied. | Confirm license on the actual upstream repo (`gitlab.freedesktop.org/slirp/libslirp`) before vendoring, in case the local copy is stale; nothing found here contradicts BSD-3-Clause. |
| **Wiimmfi** (wiimmfi.de, operated by Wiimm & Leseratte) | N/A — live third-party network service, not source-distributed code. | No — never a compile-time/link-time dependency; only ever a *configured, overridable* network endpoint (plan §3.3, §11). | Yes, informally — its published behavior (DNS redirection scheme, patch requirements, per-game stats) is the ground truth for "what does a real client have to do." | No. | Service, not code. See `wfc-external-facts.md` Strand C for the full contract and the important caveat that Wiimmfi's *own* documented baseline model expects a client patch; DNS-only is a third-party (Kaeru) capability layered in front of it, not a first-party Wiimmfi feature for DS. |
| **Kaeru WFC** (kaeru.world) | N/A — live third-party network service (a proxy in front of Wiimmfi). | No. | Yes, informally — same reasoning as Wiimmfi; it is the source for the "no client patch" DS claim. | No. | Operated by a separate team from Wiimmfi; per community sourcing, Kaeru runs a NAS proxy that fronts the Wiimmfi backend for unpatched DS/DSi clients. Treat as a distinct legal/operational entity from Wiimmfi even though they interoperate. |
| **dwc_network_server_emulator** (`barronwaffles/dwc_network_server_emulator` on GitHub, and its several forks) | **AGPL-3.0** (confirmed by fetching the raw `LICENSE` file: `GNU AFFERO GENERAL PUBLIC LICENSE, Version 3, 19 November 2007`). | No, never. | Yes — protocol-shape oracle only, for NAS, GPCM (profile), GPSP (player search), server browsing, QR, NATNEG, and GameStats (per its file listing: `nas_server.py`, `gamespy_profile_server.py`, `gamespy_player_search_server.py`, `gamespy_server_browser_server.py`, `gamespy_qr_server.py`, `gamespy_natneg_server.py`, `gamespy_gamestats_server.py`, `dls1_server.py`). | **No, never** — and this needs the strictest handling of anything in this table. Per `docs/references.md:26-27` this project's own policy for AGPL sources is "last-resort, proof-only": may be run *out-of-process* as a local test server to observe protocol behavior, but nothing from its source may be read-and-reused, and any transient use to *prove* a model correct must be tagged `AGPL-PROVE` and clean-roomed out before distribution. | Runnable locally/standalone (its wiki documents self-hosting) — this is the offline-testing option for Strand D. See `wfc-external-facts.md` for setup notes and the local-server recommendation. |
| **GBATEK** (Martin Korth, problemkaputt.de) | Free documentation; not an open-source software license, no code to incorporate. | No — it is a specification, the intended primary source per `docs/references.md:16-17` project policy ("Implementing ... DS hardware from GBATEK[] is not a derivative work of any emulator"). | N/A (it *is* the spec, not something separately oracled against). | No. | Primary source for the Strand A register/calibration map in `wfc-external-facts.md`. Extraction here was via automated fetch+AI-summarization of the live pages, not a byte-for-byte manual transcription — flagged explicitly where that introduced a suspected transcription artifact (see the register map's "Method and confidence" note). |
| **akkit.org DS Wifi Hardware Reference** (Rick Wong / "akkit") | Unknown/unstated; independent community documentation, not code. | No. | Not consulted in depth this pass — surfaced by search as a second independent hardware-doc source, cross-referenced by GBATEK's own text, but its content was not fetched/verified here. | No. | Listed for completeness. Treat as **UNCONFIRMED** until actually read; do not cite specific facts to it yet. |

## The clean-room rule as applied to the Wi-Fi device model

The project's standing rule (`docs/references.md:29-30`) is: *"the committed
implementation must trace to a spec or a permissive source, never to
copyleft code."* Applied to the Wi-Fi MMIO device model specifically:

- **Register addresses, bit layouts, and documented semantics** trace to
  GBATEK (free documentation) and, where GBATEK is silent or admits
  uncertainty, to independently-run measurement against the melonDS *oracle*
  process (a separate executable, diffed over TCP the same way the ARM9/ARM7
  core oracle already works per `docs/oracle_bringup.md` — not a
  console-in-process read of melonDS's own source). This is exactly the same
  epistemic shape the project already uses for CP15, 2D engines, and IPC: **spec
  first, measured-oracle-behavior second, GPL source code never.**
- **What must never happen**: opening melonDS's `Wifi.cpp` and porting its
  register-handling logic (even "cleaned up" or renamed) into
  `runner/src/nds/wifi/`. That is exactly the kind of derivative-work risk
  `THIRD_PARTY_ATTRIBUTION.md:86-91` already tries to audit against for the
  3D engine; the Wi-Fi device model should not create a second, undocumented
  instance of the same risk. If a GBATEK register description is ambiguous
  and melonDS's behavior must be *measured* to resolve it, that is legitimate
  oracle use — but the measurement is "does the guest driver see IRQ13 assert
  at cycle N," not "read melonDS's C++ and translate it."
- **libslirp is the one place a GPL boundary question does *not* arise**,
  precisely because its real upstream license is permissive (BSD-3-Clause,
  verified above). It can be vendored as an actual dependency without
  triggering the GPL "combined work" concern that already applies to the
  vendored GPU3D code. Do not, however, copy melonDS's *glue* code
  (`Net_Slirp.cpp`/`.h`, GPL) when writing the equivalent `SlirpBackend` —
  only the libslirp public API itself (`libslirp.h`) is being consumed, and
  that header is BSD-3-Clause.
- **The AGPL DWC server emulator is oracle-only in the strictest sense.**
  Unlike melonDS (GPL, already an accepted oracle/combined-work boundary
  elsewhere in this project), nothing from `dwc_network_server_emulator`
  should be treated as a design template to imitate closely. Run it, watch
  its wire behavior with a packet capture, and reimplement *nothing* of it in
  `ndsrecomp` — the plan (`PLAN_ndsrecomp_wiimmfi.md` §3.4) is explicit that
  this project does not implement DWC/GameSpy server logic at all; the guest
  ROM's own client code is what runs. The only legitimate use of this project
  is as a **local stand-in for the live Wiimmfi service** during development,
  so debugging doesn't have to happen against someone else's production
  infrastructure — see the Strand D section of `wfc-external-facts.md`.
