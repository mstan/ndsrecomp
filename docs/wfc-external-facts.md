# WFC external facts (hardware docs, service contract, protocol oracles)

Research artifact for the Wiimmfi integration effort. Produced by the
"external facts" research strand (R4): hardware documentation, licensing,
and the live Wiimmfi/Kaeru service contract. Every claim below is marked
**VERIFIED** (with source) or **UNCONFIRMED**. Where sources conflict this
says so and states which is trusted and why. See `docs/networking-oracles.md`
for the licensing ledger this document's Strand D and (partially) Strand A
material feeds into.

Companion plan: `E:\Downloads\PLAN_ndsrecomp_wiimmfi.md`. This document does
not repeat the architecture; it supplies the facts the architecture depends on.

---

## Method and confidence note (read this before trusting exact hex values)

The Strand A register tables below were extracted from the live GBATEK pages
at `problemkaputt.de` using an automated fetch-and-summarize pass (the
fetch tool renders the page then has a small model extract the requested
table). That pipeline is good at preserving structure and is internally
consistent across ~10 separately-fetched pages — every address independently
came back in the `0x04800000`–`0x0480FFFF` range and in the expected
7-hex-digit form, which is a reasonable cross-check. **One extraction is
flagged as a probable transcription artifact**: `W_BEACONINT` and
`W_LISTENCOUNT` came back as address `4808088Ch`, which is outside the
16-bit Wi-Fi I/O window (it decodes to `0x4808088C`, i.e. an offset of
`0x8088C` from the region base — clearly wrong for a register in a 64KB
region). The *existence* and *purpose* of these two registers is corroborated
by GBATEK's own section list (`DS Wifi Timers`) and by the akkit.org/melonDS
tradition of a beacon-interval register in this area, but **do not trust the
specific address `4808088Ch` for either register** — re-derive it from a
direct, literal read of `https://problemkaputt.de/gbatek-ds-wifi-timers.htm`
before encoding it into `game.toml`/recompiler config. Every other address in
the tables below is internally consistent and is treated as VERIFIED, but
none of this replaces byte-for-byte confirmation against melonDS's own
register constants (an oracle check, not a copy) before the recompiler
inventory (Phase 1 of the plan) is finalized.

---

## Strand A — DS Wi-Fi hardware documentation

**Primary source (VERIFIED unless noted):** GBATEK, "DS Wireless
Communications" chapter — `https://problemkaputt.de/gbatek-ds-wireless-communications.htm`
and its per-topic sub-pages (`gbatek-ds-wifi-*.htm`,
`gbatek-ds-firmware-wifi-*.htm`). This is the DS/DS-Lite Wi-Fi block
(MMIO `0x04800000`–`0x0480FFFF`), **not** the unrelated DSi-only "Atheros
Wifi" chapter (a different chip, different address space, out of scope for
this project's LLE-first DS target).

### Address space overview

- Wi-Fi I/O registers: `0x04800000`–`0x0480FFFF`. **VERIFIED**, GBATEK "DS
  Wifi I/O Map".
- Wi-Fi internal RAM used for RX/TX packet buffers: roughly
  `0x04804000`–`0x04805FFF`. **VERIFIED**, same page.
- WEP key storage RAM: `0x04805F80`–`0x04805FFF`. **VERIFIED**, same page.

### Chip identification

| Register | Address | R/W | Semantics |
|---|---|---|---|
| `W_ID` | `0x04808000` | R | Chip ID: `0x1440` on original NDS, `0xC340` on NDS-Lite. |

**VERIFIED** (GBATEK "DS Wifi Control").

### Power / RF / BB state

| Register | Address | R/W | Semantics |
|---|---|---|---|
| `W_MODE_RST` | `0x04808004` | R/W | Hardware mode/reset control. Bit 0 = TX Master Enable; bits 13–14 trigger port resets (write-only effect). |
| `W_MODE_WEP` | `0x04808006` | R/W | Software mode (bits 0–2) and WEP key size (bits 3–5: 64/128/152-bit). |
| `W_POWER_US` | `0x04808036` | R/W | Bit 0 disables `W_US_COUNT` and the BB ports (0=enable,1=disable); controls RFU activation and the 22MHz oscillator output. |
| `W_POWER_TX` | `0x04808038` | R/W | Bit 0 = Auto-Wakeup (leave idle mode a while after the pre-beacon IRQ15); Bit 1 = Auto-Sleep (enter idle mode on post-beacon IRQ13). GBATEK states it is firmware-initialized from firmware offset `0x05C`. |
| `W_POWERSTATE` | `0x0480803C` | R/W (bits 8–9 effectively read-only) | Bit 1 = request power enable (queued); bits 8/9 report pending/current power state. Queueing a change requires `W_POWER_US`=0 and `W_MODE_RST`=1. |
| `W_POWERFORCE` | `0x04808040` | R/W | Bit 0 = new value to force into `W_POWERSTATE` bit 9; bit 15 = apply-now flag. |
| `W_BB_CNT` / `W_BB_WRITE` / `W_BB_READ` / `W_BB_BUSY` | `0x04808158` / `0x0480815A` / `0x0480815C` / `0x0480815E` | R/W / W / R / R | Indirect access port into the baseband (BB) chip's own register file (indices `0x00`–`0x68`). `W_BB_CNT` carries the index plus a direction code (5=write, 6=read); `W_BB_BUSY` must be polled after a `W_BB_CNT` write and is documented as needing "a good number of clock cycles" before it responds. |
| `W_RF_DATA2` (and an implied `W_RF_DATA1` not independently re-verified here) | `0x0480817C` | R/W | RF chip indirect data-register access, structurally analogous to the BB port above. |
| `W_RF_PINS` | `0x0480819C` | R | RF control-signal status: carrier sense / TX main / TX on / RX on flags. GBATEK gives two competing bit interpretations for bits 0–1 and flags bit 6 ("TX.ON") as "often set even when not transmitting" — **UNCONFIRMED exact meaning**. |
| `W_RF_STATUS` | `0x04808214` | R | Coarse TX/RX state machine code in bits 0–3 (0=power-up ... 9=idle; includes RX mode=1, TX mode=3, and several transitional/multiplay-specific codes GBATEK itself marks uncertain). |

**VERIFIED** for addresses and the plain-language semantics quoted;
**UNCONFIRMED** for the exact bit-level meaning of `W_RF_PINS` bits 0–2/6 and
several `W_RF_STATUS` transitional codes — GBATEK's own text flags these as
guessed/uncertain (see "top ambiguous areas" below).

### BB register semantics (the indirect port's target)

GBATEK documents the **access mechanism** for BB registers `0x00`–`0x68`
precisely (see `W_BB_CNT` above) but states the **semantic meaning of most
individual BB register indices is undocumented** — they are "initialized by
firmware bootcode, and most of these settings do not need to be changed."
Only a couple of indices have a stated function (e.g. index `0x13` for CCA —
clear-channel assessment — and `0x35` for energy-detection criteria).
**VERIFIED** that the access mechanism is documented; **UNCONFIRMED /
explicitly undocumented by GBATEK itself** for the meaning of the majority of
BB register contents. Source: `gbatek-ds-wifi-baseband-chip-bb.htm`.

### TX buffers and control

| Register | Address | R/W | Semantics |
|---|---|---|---|
| `W_TXBUF_WR_ADDR` | `0x04808068` | R/W | TX buffer write pointer. |
| `W_TXSTATCNT` | `0x04808008` | R/W | Enables `W_TXSTAT` updates after specific TX types (bits 12–15: after REPLY / CMD-ACK / CMD-DATA / BEACON). LOC1–3 transmissions always update `W_TXSTAT` regardless. |
| `W_TXREQ_RESET` / `W_TXREQ_SET` / `W_TXREQ_READ` | `0x048080AC` / `0x048080AE` / `0x048080B0` | W / W / R(/W) | Bits 0–3 address the four TX buffer slots (LOC1, CMD, LOC2, LOC3). Priority when multiple bits are set: LOC3 first, LOC1 last. Bits 0, 2, 3 auto-clear on IRQ14. |
| `W_TXBUSY` | `0x048080B6` | R | Per-slot busy flag (bits 0–3 = LOC1/CMD/LOC2/LOC3, bit 4 = beacon); updated on IRQ01 (TX complete). |
| `W_TXSTAT` | `0x048080B8` | R | Bit 0 = packet completed, bit 1 = TX error, bits 8–11 = a type code that varies by which buffer/beacon/command produced it. GBATEK explicitly states it has **no confirmed mechanism for how bits 0/1 get cleared** once set. |
| `W_TX_HDR_CNT` | `0x04808194` | R/W | Disables automatic 802.11-header field patching by hardware: bit 0 = Frame Control/Duration, bit 1 = FCS/CRC32, bit 2 = Sequence Control. |
| `W_TX_SEQNO` | `0x04808210` | R | 12-bit auto-incrementing TX sequence counter (increments on IRQ07); can be suppressed per-frame via a TX buffer header flag or via `W_TX_HDR_CNT` bit 2. |

**VERIFIED** for the addresses and behavior quoted, with the `W_TXSTAT`
clear-mechanism and several high bits explicitly called out by GBATEK itself
as unknown (see ambiguous-areas summary). Source: `gbatek-ds-wifi-transmit-control.htm`.

### RX buffers and control

| Register | Address | R/W | Semantics |
|---|---|---|---|
| `W_RXCNT` | `0x04808030` | R/W | Bit 0 = force RX buffer empty (write-strobe); bit 15 = enable queuing received data into the RX FIFO; several other bits (1–3, 8–14) are undocumented. |
| `W_RXBUF_BEGIN` | `0x04808050` | R/W | RX buffer start address in the shared Wi-Fi RAM window. |
| `W_RXFILTER` | `0x048080D0` | R/W | Bit 0 = broadcast-acceptance mode; bit 8 = accept vs. ignore "empty" packets; bit 10 = enables receiving beacons (and possibly other frame types — GBATEK hedges "and maybe others"); bit 12 affects RX-buffer update timing. Most other bits undocumented. |
| `W_RXFILTER2` | `0x048080E0` | R/W | Secondary filter; GBATEK is explicitly uncertain about most of its bits, noting only observed firmware-written values (`0x08`, `0x0B`, `0x0D`) without a confirmed bit-by-bit decode. |

Address filtering itself is documented at a higher level: incoming frames are
matched by destination address and BSSID against `W_MACADDR_0..2` and
`W_BSSID_0..2`; broadcast handling is conditioned on `W_RXFILTER` bit 0;
control/PS-Poll frames reportedly bypass the filter registers entirely.
**VERIFIED** for the addresses; **UNCONFIRMED** for the majority of
`W_RXFILTER`/`W_RXFILTER2` bit semantics — GBATEK itself says so. Source:
`gbatek-ds-wifi-receive-control.htm`.

### IRQ flags and mask

| Register | Address | R/W | Semantics |
|---|---|---|---|
| `W_IF` | `0x04808010` | R/W | Interrupt request flags, write-1-to-clear. Bits: 0=RX complete, 1=TX complete, 2=RX event increment, 3=TX error increment, 4=RX event half-overflow, 5=TX error half-overflow, 6=start-of-RX, 7=start-of-TX, 8=TX-buf-count expired, 9=RX-buf-count expired, 10=unused(always 0), 11=RF wakeup, 12=multiplay CMD done/failed, 13=post-beacon timeslot, 14=beacon timeslot, 15=pre-beacon timeslot. Half-overflow flags additionally require the associated counter's MSBs to be zero before they can be cleared. |
| `W_IE` | `0x04808012` | R/W | Same bit layout as `W_IF`; 1=enabled. Bit 10 is read/write but has no functional effect. |

The Wi-Fi peripheral's line into the ARM7 interrupt controller (`IF` bit 24,
per GBATEK's cross-reference) asserts only on a `(W_IF & W_IE) != 0`
transition and stays asserted until `(W_IF & W_IE)` returns to zero — i.e.
level-triggered against the masked flag word, not edge-triggered per bit.
**VERIFIED**, source: `gbatek-ds-wifi-interrupts.htm`.

### MAC address / BSSID / association

| Register | Address | R/W | Semantics |
|---|---|---|---|
| `W_MACADDR_0/1/2` | `0x04808018`/`0x0480801A`/`0x0480801C` | R/W | Console's own 48-bit MAC address; hardware receive filtering is gated on frames addressed to this value. |
| `W_BSSID_0/1/2` | `0x04808020`/`0x04808022`/`0x04808024` | R/W | BSSID captured/matched from beacon frames. |
| `W_AID_LOW` | `0x04808028` | R/W | Bits 0–3: multiplay slave number (1–15, or 0). |
| `W_AID_FULL` | `0x0480802A` | R/W | Bits 0–10: 802.11 Association ID (1–2007, or 0 = unassociated). |

**VERIFIED**, source: `gbatek-ds-wifi-i-o-map.htm` and `gbatek-ds-wifi-control.htm`.

### Timers / counters

| Register | Address | R/W | Semantics |
|---|---|---|---|
| `W_US_COUNTCNT` | `0x048080E8` | R/W | Enables the microsecond counter and its beacon/post-beacon decrement behavior. |
| `W_US_COUNT0..3` | `0x048080F8`–`0x048080FE` | R | 64-bit free-running counter driven by the 22.00MHz RFU-board oscillator through a divide-by-22 prescaler (i.e. effectively microsecond-resolution). |
| `W_US_COMPARECNT` | `0x048080EA` | R/W | Enables IRQ14 on a compare match; bit 1 can force IRQ14. |
| `W_US_COMPARE0..3` | `0x048080F0`–`0x048080F6` | R/W | 64-bit compare target for the above; GBATEK notes it is usually programmed to a value ("`0xFFFFFFFFFFFFFC00`") that practically never matches. |
| `W_PRE_BEACON` | `0x04808110` | R/W | Microseconds of advance warning (IRQ15) before the beacon timeslot IRQ (IRQ14). |
| `W_BEACON_COUNT` | `0x0480811C` | R/W | 16-bit countdown to the next beacon timeslot; reloads from `W_BEACONINT` on expiry, triggers IRQ14/IRQ15. |
| `W_POST_BEACON` | `0x04808134` | R/W | 16-bit countdown after a beacon timeslot; triggers IRQ13; hardware-initialized to `0xFFFF`. |
| `W_BEACONINT` | address **UNCONFIRMED** (extraction artifact, see Method note above) | R/W | 10-bit beacon-interval reload value in milliseconds; GBATEK suggests it "should be initialized randomly to `0xCE`..`0xDE`" in some contexts. |
| `W_LISTENINT` / `W_LISTENCOUNT` | address for `W_LISTENCOUNT` **UNCONFIRMED** (same artifact); `W_LISTENINT` not independently re-derived either | R/W | Listen-interval reload (in beacon intervals) and its decrementing counter, ticked on IRQ14. |
| `W_CONTENTFREE` | `0x0480810C` | R/W | A decrementing microsecond counter GBATEK describes as "seems to stay fixed at `0x0000`" once reached; likely relevant only to a power-save mode. Marked **UNCONFIRMED** purpose by GBATEK itself. |

Source: `gbatek-ds-wifi-timers.htm`. Addresses other than the two flagged are
**VERIFIED**; the two flagged are **UNCONFIRMED** pending a literal re-read.

### DS Firmware Wi-Fi calibration block (factory RF/BB init data)

Location: immediately after the firmware header, at **absolute file offset
`0x02A`**, extending to `0x1FF`, inside the 256KB firmware image. This is
distinct from the user-configurable access-point profile blocks described
next.

| Offset | Size | Field |
|---|---|---|
| `0x02A` | 2 | CRC16 (initial value 0) covering `[0x02C .. 0x02C+config_length-1]` |
| `0x02C` | 2 | `config_length` (observed value usually `0x0138`) |
| `0x02F` | 1 | Version |
| `0x036` | 6 | Console's factory-programmed 48-bit Wi-Fi MAC address |
| `0x03C` | 2 | Bitmask of enabled channels, ANDed with `0x7FFE` |
| `0x040` | 1 | RF chip type identifier |
| `0x044`–`0x062` | multiple 2-byte entries | Initial values loaded into the `W_CONFIG`-family registers |
| `0x064` | 105 bytes | Initial 8-bit values for BB register indices `0x00..0x68` (the same indices reached indirectly via `W_BB_CNT`/`W_BB_WRITE`) |
| `0x0CE`–`0x154` | variable | Per-channel RF/BB calibration tables ("Type 2"/"Type 3" formats depending on RF chip type) |
| `0x162` | 1 | Unknown field |
| `0x1FD`–`0x1FF` | 3 | DSi/3DS board/flash identifier bytes; filled with `0xFF` on plain DS |

**VERIFIED**, source: `gbatek-ds-firmware-wifi-calibration-data.htm`. GBATEK
notes explicitly that this block (like the rest of the firmware header) is
**not copied into RAM at boot** — the driver reads it directly out of the
firmware SPI flash image via the SPI firmware-read command path, which is the
same firmware access path this project's boot sequence already has to model
for other firmware reads (see `docs/firmware_boot.md`).

### DS Firmware Wi-Fi internet access point profiles (user-configurable)

These are **separate** from the calibration block above, and live near the
opposite end of the settings area, addressed relative to a pointer stored in
the firmware header:

- `Base = FirmwareHeader[0x020] * 8`
- Connection 1: `Base - 0x400`, 256 bytes
- Connection 2: `Base - 0x300`, 256 bytes
- Connection 3: `Base - 0x200`, 256 bytes
- A "hidden" 4th slot: `Base - 0x100`, 256 bytes
- DSi adds three more, larger (512-byte) slots at `Base - 0xA00` /
  `Base - 0x800` / `Base - 0x600`, extended with WPA/WPA2 and proxy fields.

Per-block (NDS, 256-byte block) layout, offsets relative to the block start:

| Offset | Size | Field |
|---|---|---|
| `0x000` | 64 | Unknown/proxy-related data |
| `0x040` | 32 | Network name (SSID) |
| `0x060` | 32 | WEP64/AOSS SSID |
| `0x080`–`0x0B0` | 64 (4×16) | Four WEP keys |
| `0x0C0`–`0x0CC` | 16 | IP address, gateway, primary/secondary DNS |
| `0x0D0` | 1 | Subnet mask |
| `0x0E6` | 1 | WEP encryption mode |
| `0x0E7` | 1 | Status |
| `0x0EF` | 1 | Configuration flags |
| `0x0F0` | 6 | Nintendo Wi-Fi Connection user ID |
| `0x0FE` | 2 | CRC16 checksum |

**VERIFIED**, source: `gbatek-ds-firmware-wifi-internet-access-points.htm`.
GBATEK notes the NDS firmware's own Wi-Fi settings UI does not use these
slots directly — they exist for **games** to read/write (this is exactly the
guest-visible surface the plan's Phase 3/AP work needs to make readable: a
game reads a saved AP profile out of firmware SPI flash, not out of anything
`ndsrecomp` should synthesize).

### Top ambiguous areas needing oracle confirmation (Strand A summary)

In order of how much they are likely to block Phase 1/2 of the plan:

1. **`W_TXSTAT` clear semantics** (`0x048080B8`). GBATEK states outright it
   has no confirmed mechanism for clearing bits 0/1 once a TX result is
   latched. Any implementation guess here is exactly the kind of "return what
   the game expects without understanding the hardware" the plan's Phase 2
   explicitly forbids — this needs an melonDS-oracle trace of the real
   post-TX register sequence.
2. **`W_RF_STATUS` / `W_RF_PINS` transitional state codes** (`0x04808214`,
   `0x0480819C`). GBATEK gives two different candidate interpretations for
   several bits and flags them uncertain; the DS Wi-Fi driver's init/scan
   state machine polls these, so an oracle trace across the real
   power-up → idle → RX/TX transitions is needed before Phase 2 can be
   confident it modeled the right sequence.
3. **`W_RXFILTER`/`W_RXFILTER2` bit-exact filtering behavior**
   (`0x048080D0`, `0x048080E0`). GBATEK documents only a handful of bits with
   confidence and explicitly hedges the rest; getting this wrong would either
   over-deliver frames the real driver doesn't expect or silently drop
   beacons/data the AP-association phase (plan Phase 3) depends on.
4. (Honorable mention) The two beacon/listen timer addresses flagged in the
   Method note above need a literal re-fetch before they're trustworthy at
   all, independent of any semantic ambiguity.

---

## Strand C — Wiimmfi / Kaeru service contract

### How a real DS reaches Wiimmfi without a ROM patch

**VERIFIED, but with an important nuance the plan should absorb:**
Wiimmfi's own site (`https://wiimmfi.de/`, fetched directly) states as its
general/official model:

> "To use Wiimmfi, the games must be patched to use the new domains and also
> to change some other settings. See »Wiimmfi Patcher« for more details."

Wiimmfi *does* also publish a DNS-only patcher
(`https://wiimmfi.de/patcher/dnspatch`, fetched directly), but that page is
explicit that it is a **Wii/WiiU-specific** mechanism: it exploits an IOS SSL
certificate-validation bug ("Wiimmfi then uses a specially crafted SSL
certificate which some games consider valid due to an IOS bug") to get an
unpatched disc past the login handshake, and then pushes the *actual*
Mario-Kart-Wii-specific patches to the running game live, over the network,
during the first online session ("Wiimmfi is using a different bug in Mario
Kart Wii in order to send and execute the rest of the Mario-Kart-specific
Wiimmfi patches to your game"). Per that same page, this only applies to
"Mario Kart Wii, SSBB and a couple other Wii games" — **nothing on
wiimmfi.de's own DNS-patcher page claims this mechanism extends to Nintendo
DS.**

The DNS-only, no-client-modification path for **DS specifically** comes from
a related but organizationally distinct project:

**VERIFIED**, source: `https://kaeru.world/projects/wfc` (fetched directly,
Kaeru Team's own site):

> "No hacks, patches, nor flashcards are required" to play DS and DSi games
> through Kaeru WFC.

Kaeru's documented setup for DS: open "Nintendo WFC Settings" in the game,
set "Auto-obtain DNS" to No, set both Primary and Secondary DNS to
**`178.62.43.212`**, save, connect. Kaeru also documents DSi Enhanced titles
(3DS/DSi advanced connection slots) with the same DNS.

Per community sourcing (GBAtemp thread search summaries, **UNCONFIRMED**
detail but consistent with Kaeru's own "no patch" claim and with the
technical shape of the problem): Kaeru operates a **NAS proxy** that fronts
the Wiimmfi backend — i.e. Kaeru's server, not Wiimmfi's own
`95.217.77.151`/`95.217.77.181`, is what a DS's DNS query for
`nas.nintendowifi.net` should resolve to; Kaeru's proxy then does whatever
protocol/certificate handling is needed to satisfy an unpatched DS client
before relaying into the shared Wiimmfi service backend.

**Reconciling the two "DNS-only" claims (Kaeru vs. the community wiki) —
this is the part that directly touches a plan assumption:**

- The DS-Homebrew wiki (`https://github.com/DS-Homebrew/wiki`,
  `pages/_en-US/ds-index/wifi.md`, fetched directly) lists **two** DNS
  options for DS — Wiimmfi at `178.62.43.212` and "AltWFC/WFCZwei" at
  `172.104.88.237` — and says: *"You might need to NoSSL patch your game at
  this point, depending on the game."* (It presents ROM patching via
  WfcPatcher as a fallback "in case your ISP blocks custom DNS servers," not
  as something every game needs.)
- Note the address `178.62.43.212` is attributed to "Wiimmfi" by that wiki
  page but is the same address Kaeru's own site attributes to **Kaeru**, not
  to Wiimmfi's own official infrastructure (`95.217.77.x`). This is either
  the wiki using "Wiimmfi" loosely to mean "the Wiimmfi *ecosystem*" (Kaeru
  included), or a naming conflation. Kaeru's own site is trusted here as the
  authoritative source for what its own IP is and does.
- **Net assessment**: no source found states MKDS *by name* requires a
  patch. Kaeru's blanket claim is "no patch, ever, for DS/DSi." The
  DS-Homebrew wiki's hedge ("depending on the game") is about the DS WFC
  ecosystem in general, not MKDS specifically, and was not corroborated with
  a per-title compatibility list in the time available for this pass.

**Confidence and recommendation:** Medium-high confidence that DNS-only
(pointed at a Kaeru-style proxy, not at Wiimmfi's own official IP) is
sufficient for Mario Kart DS specifically, based on (a) Kaeru's direct,
specific claim, and (b) live evidence in the next section that MKDS is
actively being played through the Wiimmfi ecosystem today, which would not
be happening at scale if a ROM patch were required and undocumented. This is
**not** a certainty on the level of a primary Nintendo/Wiimmfi statement
naming MKDS explicitly — treat it as **VERIFIED for the general DS case,
UNCONFIRMED for a MKDS-specific guarantee**, and keep the plan's existing
"ROM patching may be supported later as a diagnostic/compatibility option"
(plan §2.2) exactly because of this residual uncertainty — do not remove that
escape hatch on the strength of this research alone.

### Is Mario Kart DS currently online/supported on Wiimmfi?

**VERIFIED, directly, with live data.** Fetching
`https://wiimmfi.de/stats/game/mariokartds` (Wiimmfi's own real-time stats
page) on 2026-08-10 showed:

- Mario Kart DS listed under "Games with active users" with **9** players
  shown as currently logged in (`Total: 15 [games] 175+1 [players]` across
  all currently-active games site-wide).
- A live roster of individual sessions (friend codes, online/login status
  columns, some flagged `oPg`/`oGvS` — group/host-related status codes not
  decoded here).

This confirms Mario Kart DS is not merely "on a supported list" but has real
concurrent traffic through the Wiimmfi ecosystem at the moment of checking.

### DNS resolution targets

**VERIFIED** (each from the operator's own site, fetched directly):

| Hostname/purpose | Resolves via | Notes |
|---|---|---|
| Wiimmfi's own official DNS (Wii/WiiU-oriented) | `95.217.77.181` | The DNS-patcher-specific address ("one digit different from the usual Wiimmfi DNS"); documented as forwarding `*.nintendowifi.net` queries, not blanket-resolving them: *"forward all DNS queries for subdomains of nintendowifi.net to the Wiimmfi name server ... do not make them all resolve to this IP, forward the queries to this IP."* |
| Wiimmfi's other official DNS | `95.217.77.151` | Used by the older Wii `str2hax` EULA-exploit patcher path; same operator. |
| Kaeru WFC (DS/DSi entry point) | `178.62.43.212` | Kaeru's own published DNS for DS/DSi; per Kaeru's site, both primary and secondary DNS fields on the DS should be set to this same address. |
| AltWFC / "WFCZwei" (community alternative) | `172.104.88.237` | From the DS-Homebrew wiki; a separate community-run relay, not evaluated further here. |

**UNCONFIRMED / not independently resolved this pass:** the specific IP
addresses that `nas.nintendowifi.net`, `gpcm.gs.nintendowifi.net`,
`gpsp.gs.nintendowifi.net`, the QR/server-browser hosts, and the NATNEG hosts
*individually* resolve to once routed through Kaeru's or Wiimmfi's DNS —
only the DNS-server-selection layer was confirmed, not a live `dig`/`nslookup`
trace of each subdomain. If precise backend IPs are needed later (e.g. to
pre-seed a `ReplayBackend` fixture), that should be a follow-up live capture
against a running Kaeru-configured client rather than a guess.

### Service-side quirks, rate limits, abuse policy

**VERIFIED** (from wiimmfi.de's own front page and patcher page):

- Wiimmfi is explicitly a free, best-effort service run by two named
  individuals (Wiimm & Leseratte): *"Wiimmfi is a free service, so please be
  patient if we stop the servers temporarily to fix bugs or to update the
  system."*
- The site publishes a public ban list ("Last Bans") and per-game/global
  real-time statistics — i.e. abuse is actively moderated and visible.
  Respect this: this project's testing should not hammer live Wiimmfi/Kaeru
  infrastructure with automated regression runs. This is exactly why Strand D
  (below) matters — a local oracle server exists specifically so early
  development/regression testing does not have to touch live third-party
  infrastructure.
- No published rate-limit numbers were found; treat the absence of a
  published limit as a reason for *more* caution in automated testing, not
  less.

### Recommended `[network.wfc]` provider configuration shape

The facts above argue for named, overridable providers rather than a single
compiled-in address — Wiimmfi's own IP, Kaeru's IP, and a community
alternative are all *distinct services with distinct behavior*, not
interchangeable mirrors of the same one, and a local oracle needs to slot
into the same mechanism per Strand D. This directly implements the plan's
§11 `WfcProvider` sketch and its instruction not to "permanently embed
current infrastructure addresses into guest-facing behavior if they can be
configuration":

```toml
[network]
enabled = true
backend = "slirp"

[network.wfc]
enabled = false        # explicit opt-in: this reaches a live third-party service by default
provider = "wiimmfi"   # DS-compatible Wiimmfi ecosystem route; "kaeru" is the same DNS
dns_mode = "all"       # "all" = Option A (plan §11): all guest DNS -> provider's server.
                        # "wfc-domains" = Option B: only *.nintendowifi.net -> provider; else normal DNS.

# Each provider is just a DNS server address plus provenance notes.
# ndsrecomp never hardcodes IPs into guest-facing logic -- only into this table,
# which is meant to be hand-edited/updated without a rebuild (plan §3.3, §27).

[network.wfc.providers.kaeru]
dns_server = "178.62.43.212"
description = "Kaeru WFC: no client patch documented as required for DS/DSi. Proxies into the shared Wiimmfi backend."
source = "https://kaeru.world/projects/wfc"

[network.wfc.providers.wiimmfi]
dns_server = "178.62.43.212"
description = "User-facing DS-compatible Wiimmfi ecosystem route. This intentionally aliases Kaeru WFC because DS guides commonly call this Wiimmfi, and raw Wiimmfi DNS returns 20100 for stock MKDS in local validation."
source = "https://kaeru.world/projects/wfc"

[network.wfc.providers.wiimmfi-direct]
dns_server = "95.217.77.181"
description = "Wiimmfi's own official DNS. Wiimmfi's general/official model expects a client patch; this address's documented DNS-only patcher is Wii/WiiU-specific, not confirmed for stock DS."
source = "https://wiimmfi.de/patcher/dnspatch"

[network.wfc.providers.altwfc]
dns_server = "172.104.88.237"
description = "Community AltWFC/WFCZwei relay, per the DS-Homebrew wiki. Not evaluated in depth by this research pass."
source = "https://github.com/DS-Homebrew/wiki"

[network.wfc.providers.local-oracle]
dns_server = "127.0.0.1"
description = "Local dwc_network_server_emulator instance (AGPL-3.0, protocol oracle only, never linked). Development/regression use only -- see Strand D."
source = "https://github.com/barronwaffles/dwc_network_server_emulator"
```

Notes for whoever wires this up:

- `provider` should be swappable at runtime config, not at compile time —
  none of these addresses should end up baked into `game.toml` or generated
  code; they belong only in this kind of user-editable network config.
- Default `enabled = false` and no default `provider` value is deliberate:
  turning WFC on always reaches *someone's* server (a volunteer-run free
  service, in every case except `local-oracle`), so it should require an
  explicit choice, consistent with the abuse/rate-limit caution in Strand C.
- The `local-oracle` entry is what M5–M7 development should default to per
  the Strand D recommendation below; live `kaeru`/`wiimmfi` are for the
  plan's actual milestone acceptance runs.

---

## Strand D — protocol oracle inventory

| Server role | Implementation | Runnable locally? | License | Fit for this project |
|---|---|---|---|---|
| NAS (authentication), GPCM (profile/presence), GPSP (player search), server browser, QR, NATNEG, GameStats, DLS | `dwc_network_server_emulator` (canonical: `github.com/barronwaffles/dwc_network_server_emulator`; several forks exist — `ubergeek77`, `florensie`, `EnergyCube`, `ZehCariocaRj/...kyle95wm` — of varying maintenance activity) | **Yes.** Its wiki documents self-hosting a full DWC/GameSpy stack. | **AGPL-3.0**, confirmed by fetching the raw `LICENSE` file directly (`GNU AFFERO GENERAL PUBLIC LICENSE, Version 3`). | Protocol-shape oracle only, per this project's standing AGPL policy (last-resort, proof-only, never incorporated — see `docs/networking-oracles.md`). Its real value here is as a **local stand-in for live Wiimmfi** during early development: point a test DNS at a self-hosted instance instead of at `wiimmfi.de`/`kaeru.world`, so debugging NAS/GPCM/matchmaking flows doesn't generate load against someone else's free production service (directly serves the plan's Phase 8/9 failure-methodology and the "do not hammer live infra" concern above). |
| Front-end/admin website for the above | `EnergyCube/CoWFC` | Yes (paired with the emulator above) | Not independently confirmed this pass; assume same family (AGPL-likely) until checked — **UNCONFIRMED**, do not rely on this without checking its own LICENSE file first. | Optional convenience only; not required to stand up a working local oracle. |

**Recommendation:** for M5–M7 of the plan (NAS authentication through
NATNEG), stand up a local `dwc_network_server_emulator` instance and a local
DNS server pointed at it as the *first* test target, before pointing a
guest ROM at live Wiimmfi/Kaeru. This gives:

- A deterministic, resettable backend for building the packet-capture/replay
  fixtures the plan calls for in §21–22, without embedding live credentials
  or session identifiers in committed fixtures (plan §28).
- Zero risk of tripping Wiimmfi's abuse detection or wasting a volunteer-run
  free service's capacity while `ndsrecomp`'s guest-side Wi-Fi stack is still
  under active, iterative debugging (which is exactly the phase where a
  buggy client generates the most malformed/retried traffic).
- A protocol oracle whose behavior can be diffed against without any
  question of whether observed behavior is "real Wiimmfi" or "a proxy
  quirk" — useful precisely because Kaeru's DS entry point is, by its own
  description, a proxy layer with its own translation behavior layered on
  top of the base DWC/GameSpy protocol.

Live Wiimmfi/Kaeru remains the correct target for the plan's actual milestone
acceptance criteria (M5–M8, and especially the two-client NATNEG/race
validation in Phase 10–11, which needs a real second peer) — the local
oracle is a development aid, not a replacement for the real acceptance bar.
