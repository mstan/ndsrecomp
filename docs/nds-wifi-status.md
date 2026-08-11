# NDS Wi-Fi Implementation Status (Inventory)

Scope: `runner/src/wifi.cpp` (844 lines) + `runner/src/wifi.h`, every call site in
`runner/`, and the firmware-calibration plumbing that feeds it. This is an
inventory only — no code changes. Every claim below cites the exact
`file:line` read to support it. Claims about real DS hardware behavior that
the source code does not itself assert are marked **needs oracle
confirmation** rather than guessed.

---

## (a) Verdict

The current `wifi.cpp` is a **local MMIO register/state model that has never
been exercised by any recompiled guest code path in this repo, and has no
network I/O of any kind.** It correctly reproduces the *bus-level*
contract — the 0x04800000–0x0480FFFF aperture, 16-bit device width with
32→2×16 splitting and byte-read-not-byte-write semantics, the ARM7-only
visibility rule, and the POWCNT2 gate that zeroes the whole aperture when the
Wi-Fi block is powered off (`runner/src/wifi.h:22-27`, `runner/src/wifi.cpp:821-843`).
It also reproduces a detailed and *guest-clock-driven* register/IRQ state
machine for power sequencing, the µs/beacon/listen timers, and TX-slot
completion sequencing, modeled closely on melonDS's `Wifi.cpp` register names
(`runner/src/wifi.cpp:23-25` "Names follow melonDS Wifi.h and GBATEK"). None
of this has been reached by the firmware-menu boot the project has already
proven byte-exact: the firmware-menu banks captured so far
(`bios/firmware_banks/fw_arm7_*.toml`) are all system-settings-menu scenarios
(calibration/date-alarm/download-play/pictochat/profile/shutdown/system-options);
none of them is the ARM7 "wifi code" overlay the firmware header itself
points at (`tools/fw_inspect.py:32,93,134`), so as far as this codebase has
observed, **the Wi-Fi block has literally zero guest-driven register traffic
today.** For a real Nintendo ARM7 Wi-Fi driver, this gets you through basic
power/BB/RF register bring-up and TX-slot bookkeeping with correct guest-clock
timing, and then hits a wall the instant the driver needs an actual over-the-air
result: there is no virtual AP, no beacon/probe/auth/assoc frame exchange, no
RX frame delivery path, and no DMA-on-Wi-Fi-event trigger. TX "completion" is
synthesized locally (IRQ + status register updates) without any frame ever
leaving the device (`runner/src/wifi.cpp:338-339`), and RX is register
plumbing over packet RAM that nothing ever populates. The device model is a
solid, hardware-faithful floor for register/IRQ semantics; it is not yet
connected to anything a driver would recognize as "the world."

---

## (b) Register-by-register inventory

All addresses are offsets within `0x04800000`–`0x0480FFFF`; the aperture
mirrors every 0x8000 (`offset = addr & 0x7FFEu`, `runner/src/wifi.cpp:404,483`).
"Named" = constant exists in `runner/src/wifi.cpp:26-97`; unnamed offsets are
cited by raw hex. IRQ index numbers refer to bit positions in `W_IF`/`W_IE`
(`0x010`/`0x012`); the source carries no per-bit GBATEK names, so bit names
below are **not asserted**, only the triggering code path.

### Identification / mode / power

| Reg | Addr | Width | Read | Write | IRQ | DMA | Status | Evidence |
|---|---|---|---|---|---|---|---|---|
| W_ID | 0x000 | 16 | returns device ID (0x1440 retail / 0xC340 Lite) | read-only, write ignored | none | none | **partial** (static ID only, see §e) | `wifi.cpp:26,468-479,700,725-732` |
| W_ModeReset | 0x004 | 16 | generic readback | bit0 toggles `update_power_status`; bit0x2000/0x4000 trigger full register-bank reset bursts | none directly | none | **implemented** | `wifi.cpp:547-591` |
| W_ModeWEP | 0x006 | 16 | generic readback | masks to 0x7F; feeds power-down-ctrl state machine | none | none | **partial** (state transitions only; no WEP crypto) | `wifi.cpp:592-600` |
| W_TXStatCnt | 0x008 | 16 | generic readback | generic store (cleared by ModeReset 0x4000) | gates IRQ1 on TX slot 4 completion | none | **partial** | `wifi.cpp:29,342-343,568` |
| W_TRXPower | 0x034 | 16 | read-only | **read-only**, host-set only via `update_power_status` | none | none | **implemented** | `wifi.cpp:35,222-259,470` |
| W_PowerUS | 0x036 | 16 | generic readback | masks to 3 bits, drives `update_power_on` | indirectly (IRQ11 via power-on) | none | **implemented** | `wifi.cpp:43,603-606,185-191` |
| W_PowerTX | 0x038 | 16 | generic readback | masks to 2 bits, feeds power-down-ctrl | none | none | **implemented** | `wifi.cpp:44,607-614` |
| W_PowerState | 0x03C | 16 | generic readback | conditionally accepted only when ModeWEP&7==3 | IRQ11 (power-up interval) via `update_power_status` | none | **implemented** | `wifi.cpp:45,217-273,615-624` |
| W_PowerForce | 0x040 | 16 | generic readback | masks to 0x8001 | drives forced power state | none | **implemented** | `wifi.cpp:46,227-228,625-628` |
| W_PowerDownCtrl | 0x048 | 16 | generic readback | masks to 2 bits | none | none | **implemented** | `wifi.cpp:47,629-636` |
| W_Random | 0x044 | 16 | **read-driven 11-bit LFSR step** (mutates state on every read) | read-only | none | none | **implemented** | `wifi.cpp:37,413-418,470` |
| MACADDR0-2 | 0x018,0x01A,0x01C | 16 ea | generic readback | generic store; cleared to 0xFFFF at reset, 0 on ModeReset 0x4000 | none | none | **partial**, no named constant, no auto-seed from firmware (see §e) | `wifi.cpp:570,720-721` |
| BSSID0-2 | 0x020,0x022,0x024 | 16 ea | generic readback | generic store; same reset pattern as MACADDR | none | none | **partial**, no named constant | `wifi.cpp:571,721` |
| W_AIDLow | 0x028 | 16 | generic readback | masked to 4 bits | none | none | **implemented** | `wifi.cpp:32,572,601` |
| W_AIDFull | 0x02A | 16 | generic readback | masked to 11 bits | none | none | **implemented** | `wifi.cpp:33,572,602` |
| W_TXRetryLimit | 0x02C | 16 | generic readback | generic store (reset default 0x0707) | none | none | **implemented** (value only, no actual retry logic) | `wifi.cpp:34,574` |

### RF / baseband

| Reg | Addr | Width | Read | Write | IRQ | DMA | Status | Evidence |
|---|---|---|---|---|---|---|---|---|
| W_RFStatus | 0x214 | 16 | read-only, driven by `set_status()` state table | read-only | none | none | **implemented** (status pin model) | `wifi.cpp:96,207-215,470` |
| W_RFPins | 0x19C | 16 | read-only, driven by `set_status()` | read-only | none | none | **implemented** | `wifi.cpp:94,207-215,470` |
| W_RFBusy | 0x180 | 16 | **hardcoded 0** | not writable (falls through to generic store, but nothing reads it back specially) | none | none | **stub** (always "not busy") | `wifi.cpp:41,452-454` |
| RFCnt | 0x184 | 16 | generic readback | masked to 0x413F | none | none | **stub** — control register stored, but no RF chip model behind it | `wifi.cpp:651` |
| RF data path (W_RFData1/2, GBATEK ~0x18C/0x18E) | — | — | — | — | — | — | **absent** — no constant, no read/write case anywhere in the file | full-file read, no match |
| W_BBCnt | 0x158 | 16 | generic readback | command 0x5000 latches `g_bb[index]` from 0x15A (unless read-only) | none | none | **implemented** | `wifi.cpp:38,534-541` |
| W_BBRead | 0x15C | 16 | returns `g_bb[index]` only if BBCnt upper nibble == 0x6000, else 0 | read-only | none | none | **implemented** | `wifi.cpp:39,449-451,470` |
| W_BBBusy | 0x15E | 16 | **hardcoded 0** | read-only | none | none | **stub** (always "not busy") | `wifi.cpp:40,452-454,470` |
| BB register file `g_bb[0x100]` | — | 8-bit array | reset seeds ~20 fixed melonDS-default bytes (`reset_bb`), rest 0 with read-only latch per byte | writable per-byte via BBCnt/0x15A unless latched read-only | — | — | **partial** — plausible defaults, not this cartridge/firmware's actual calibration values (see §e) | `wifi.cpp:146-167` |

### TX path

| Reg | Addr | Width | Read | Write | IRQ | DMA | Status | Evidence |
|---|---|---|---|---|---|---|---|---|
| W_TXBufWriteAddr | 0x068 | 16 | generic readback | masked 0x1FFE | none | classified `DmaRegion::Wifi` for timing | **implemented** | `wifi.cpp:53,652-654` |
| W_TXBufCount | 0x06C | 16 | generic readback | masked 0x0FFF | IRQ8 when count reaches 0 | timing only | **implemented** | `wifi.cpp:54,524,655-657` |
| W_TXBufDataWrite | 0x070 | 16 | n/a (write-only path) | writes packet RAM at `TXBufWriteAddr`, advances/gap-skips, decrements count | IRQ8 on count exhaustion | goes through generic bus path so DMA-driven fill works | **implemented** | `wifi.cpp:55,517-526` |
| W_TXBufGapAddr/GapSize | 0x074/0x076 | 16 | generic readback | masked (0x1FFE / 0x0FFF) | none | none | **implemented** | `wifi.cpp:56-57,652-657` |
| W_TXSlotBeacon/Cmd/Reply1/Reply2/Loc1-3 | 0x080,0x090,0x094,0x098,0x0A0,0x0A4,0x0A8 | 16 ea | generic readback (Reply2 read-only) | generic store; Cmd/Loc/Reply "reset" bits clear high bit via W_TXSlotReset | slot-dependent, via `process_tx`/`start_tx_beacon` | none | **implemented** for the beacon/cmd slot state machine | `wifi.cpp:58-64,292-353,637-645` |
| W_TXReqRead/Set/Reset | 0x0B0 (RO)/0x0AE/0x0AC | 16 | TXReqRead read-only, reflects live bits | Set ORs, Reset ANDs-clears | feeds slot dispatch in `nds_wifi_run_events` | none | **implemented** | `wifi.cpp:65-67,87,508-515,775-812` |
| W_TXBusy | 0x0B6 | 16 | masks to 0x001F | read-only | drives `nds_wifi_run_events` slot scheduling | none | **implemented** | `wifi.cpp:42,455-456,470` |
| W_TXStat | 0x0B8 | 16 | read-only | read-only, host-set (0x0301 on slot-4 completion) | IRQ1 on write | none | **implemented** for that one path only | `wifi.cpp:68,342-344,470` |
| W_TXSeqNo | 0x210 | 16 | read-only | read-only, host-incremented per frame | none | none | **implemented** | `wifi.cpp:95,281-289,470` |
| W_TXHeaderCnt | 0x194 | 16 | generic readback | generic store (cleared on ModeReset 0x4000) | affects sequence-number injection | none | **implemented** | `wifi.cpp:93,286,584` |
| W_Preamble | 0x0BC | 16 | masked to 2 bits on read | generic store (reset default 1) | affects TX phase timing (`preamble_length`) | none | **implemented** | `wifi.cpp:36,275-279,419-420,579` |
| Six TX hardware slots (`g_tx_slots[6]`) | internal | — | n/a | n/a | IRQ7 (phase 0 start), IRQ1/status on completion | none | **implemented as local state machine, no frame ever transmitted** (see §c) | `wifi.cpp:117-126,304-353` |

### RX path

| Reg | Addr | Width | Read | Write | IRQ | DMA | Status | Evidence |
|---|---|---|---|---|---|---|---|---|
| W_RXBufReadAddr | 0x058 | 16 | generic readback | masked 0x1FFE | none | classified `DmaRegion::Wifi` | **implemented** (address bookkeeping only) | `wifi.cpp:48,652-654` |
| W_RXBufCount | 0x05C | 16 | generic readback | masked 0x0FFF | IRQ9 when count reaches 0 | timing only | **implemented** (bookkeeping; never non-zero from a real RX event because nothing ever writes an inbound frame) | `wifi.cpp:49,446,528,655-657` |
| W_RXBufDataRead | 0x060 | 16 | reads packet RAM at `RXBufReadAddr`, advances with begin/end wraparound and gap-skip, decrements count, may IRQ9 | write also decrements count/IRQ9 (matches melonDS quirk) | IRQ9 | goes through generic bus path | **implemented mechanically, never fed real data** | `wifi.cpp:50,431-448,527-530` |
| W_RXBufGapAddr/GapSize | 0x062/0x064 | 16 | generic readback | masked (0x1FFE / 0x0FFF) | none | none | **implemented** | `wifi.cpp:51-52,437-443,652-657` |
| RXBufBegin/End (0x050/0x052) | 0x050/0x052 | 16 | generic readback | set to fixed 0x4000/0x4800 on ModeReset 0x4000 | none | none | **implemented as fixed defaults**, no dynamic RX ring resize beyond what guest writes | `wifi.cpp:437-443,576-577` |
| RXBufWriteAddr (0x056) | 0x056 | 16 | generic readback | zeroed on ModeReset 0x2000 | none | none | **stub** — nothing ever advances a "write" side because no inbound frame is ever queued | `wifi.cpp:560` |
| W_RXFilter | 0x0D0 | 16 | generic readback | generic store (reset default 0x0401) | none | none | **stub** — value stored, no actual address/type filtering logic anywhere | `wifi.cpp:91,580` |
| W_RXFilter2 | 0x0D2 | 16 | generic readback | generic store (reset default 0x0008) | none | none | **stub**, same as above | `wifi.cpp:92,582` |
| RX frame delivery (host → guest packet RAM) | — | — | — | — | — | — | **absent** — no function anywhere in the file injects a frame into `g_ram` from any source other than the guest's own writes | full-file read, no match |

### IRQ / timers / counters

| Reg | Addr | Width | Read | Write | IRQ | DMA | Status | Evidence |
|---|---|---|---|---|---|---|---|---|
| W_IF | 0x010 | 16 | generic readback | write clears set bits (`&= ~value`) | edge-detected via `check_irq` | none | **implemented** | `wifi.cpp:30,193-203,498-501` |
| W_IE | 0x012 | 16 | generic readback | full replace, re-evaluates pending IRQ | raises ARM7 IF bit 24 (`nds_raise_irq(1, 0x01000000)`) on 0→nonzero transition | none | **implemented** | `wifi.cpp:31,193-203,492-497` |
| W_IFSet | 0x21C | 16 | generic readback | ORs into IF (masked 0xFBFF), re-checks IRQ | same path as IE | none | **implemented** | `wifi.cpp:71,502-507` |
| W_USCountCnt | 0x0E8 | 16 | generic readback | masked to 1 bit, gates the µs counter tick | gates all downstream MS/beacon timer logic | none | **implemented** | `wifi.cpp:72,646,756-767` |
| W_USCompareCnt | 0x0EA | 16 | generic readback | bit1 forces IRQ14, masked to 1 bit stored | gates IRQ14/IRQ15 compare logic | none | **implemented** | `wifi.cpp:73,361-374,382-386,647-650` |
| W_USCompare0-3 | 0x0F0-0x0F6 | 16 ea (64-bit value) | reconstructs `g_us_compare` piecewise | writes piecewise into `g_us_compare`; low word write can force `g_block_beacon_irq14` | feeds IRQ14 comparison | none | **implemented** | `wifi.cpp:74-76,425-428,673-689` |
| W_USCount0-3 | 0x0F8-0x0FE | 16 ea (64-bit value) | reconstructs `g_us_counter` piecewise | writes piecewise into `g_us_counter` | feeds ms_timer/beacon | none | **implemented** | `wifi.cpp:77-78,421-424,658-672` |
| W_CmdCountCnt | 0x0EE | 16 | generic readback | generic store, gates `g_cmd_counter` decrement | none directly | none | **implemented** | `wifi.cpp:74,768-770` |
| W_CmdCount | 0x118 | 16 | returns `(g_cmd_counter+9)/10` | sets `g_cmd_counter = value*10` | none | none | **implemented** | `wifi.cpp:81,429-430,690-692` |
| W_ContentFree | 0x10C | 16 | generic readback | generic store; decremented every tick | none | none | **implemented** (value counts down; no consumer logic beyond the register itself) | `wifi.cpp:79,771-773` |
| W_PreBeacon | 0x110 | 16 | generic readback | generic store | feeds IRQ15 (pre-beacon) compare | none | **implemented** | `wifi.cpp:80,376-379,759-765` |
| W_BeaconCount1 | 0x11C | 16 | generic readback | generic store | drives IRQ14 (beacon interval elapsed) | none | **implemented** | `wifi.cpp:82,363,389-394` |
| W_BeaconCount2 | 0x134 | 16 | generic readback | generic store | drives IRQ13 (beacon-count-2 elapsed) | none | **implemented** | `wifi.cpp:83,368,396-399` |
| W_BeaconInterval | 0x08C | 16 | generic readback | generic store | feeds BeaconCount1 reload | none | **implemented** | `wifi.cpp:85,363,393-394` |
| W_ListenCount/Interval | 0x088/0x08E | 16 ea | generic readback | generic store | consumed inside IRQ14 handler | none | **implemented** | `wifi.cpp:84,86,372-373` |
| W_TXBeaconTIM | 0x084 | 16 | generic readback | generic store (reset default 0) | none | none | **stub** — value stored, TIM-bit semantics (indicating buffered traffic) not modeled | `wifi.cpp:90,578` |
| W_CmdTotalTime | 0x1C0 | 16 | generic readback | generic store (cleared on ModeReset 0x2000) | none | none | **stub**, value only | `wifi.cpp:88,561` |
| W_CmdReplyTime | 0x1C4 | 16 | read-only | read-only, cleared on ModeReset 0x2000 | none | none | **stub**, value only | `wifi.cpp:89,470,562` |
| W_CMDStat0-7 | 0x1D0-0x1DE | 16 ea | **read-and-clear** (any even offset in range) | generic store | none | none | **implemented** mechanically, never populated by a real command-reply frame | `wifi.cpp:69-70,458-463` |
| W_RXTXAddr | 0x268 | 16 | read-only | read-only, host-driven during TX phase 1 (address walk) | none | none | **implemented** | `wifi.cpp:97,310,328,470` |

### Wi-Fi RAM / unmapped mirrors

| Region | Addr range | Width | Read | Write | Status | Evidence |
|---|---|---|---|---|---|---|
| Packet RAM | 0x4000-0x5FFF (mirrored every 0x8000) | 16 (8 KiB backing store) | direct `g_ram` read | direct `g_ram` write | **implemented** as flat byte storage; content is whatever the guest itself wrote — no host-injected frames | `wifi.cpp:100,134-144,406-409,485-488` |
| Unpopulated region | 0x2000-0x3FFF | 16 | **hardcoded 0xFFFF** (pull-up model) | **ignored** | **implemented** (deliberate open-bus behavior) | `wifi.cpp:410,489` |
| Everything past 0x8000 aperture end | ≥ `kWifiEnd` (0x04810000) | any | **hardcoded 0** | **ignored** | **implemented** guard | `wifi.cpp:403,482` |

---

## (c) Access paths that fall through to a default/zero/ignore behavior

These are the specific things a real driver would notice as "the hardware
didn't respond the way it should":

1. **No frame ever leaves the device.** TX slot completion (`process_tx`,
   `wifi.cpp:304-353`) raises the correct IRQs and updates `W_TXBusy`/`W_TXStat`
   but never calls into any transport; the file's own comment says so directly:
   "packets are not handed to a host network backend" (`wifi.cpp:338-339`).
2. **No frame ever arrives.** There is no function anywhere in the file that
   writes a received frame into `g_ram` from any source other than the
   guest's own MMIO writes. `W_RXBufCount`/`W_RXBufDataRead` are fully wired
   registers with nothing to read (full-file read, no RX-injection call site).
3. **RF chip register data path is absent.** Only `W_RFCnt`-equivalent
   (offset 0x184) exists as a masked store; there is no `W_RFData1`/`W_RFData2`
   equivalent anywhere, so an actual RF-chip read/write sequence (channel
   select, TX power, filter programming) has no register to land on
   (`wifi.cpp:651`, full-file read confirms no other RF data register).
4. **`W_RFBusy` and `W_BBBusy` are hardcoded to always report "not busy."**
   (`wifi.cpp:452-454`). A driver polling these for a real multi-cycle SPI
   transaction to the RF/BB chips will always see it "already done."
5. **RX/TX address filtering registers are inert.** `W_RXFilter`/`W_RXFilter2`
   are stored but never consulted by anything (`wifi.cpp:91-92,580,582`).
6. **`W_TXBeaconTIM`, `W_CmdTotalTime`, `W_CmdReplyTime` are value-only.**
   Nothing computes or consumes a TIM bit or a real command round-trip time
   (`wifi.cpp:88-90,561-562,578`).
7. **DMA is never auto-triggered by a Wi-Fi event.** `nds_dma_trigger` is
   called only from the gamecard IRQ path (`runner/src/io.cpp:2276`) and the
   3D engine FIFO path (`runner/src/gpu3d.cpp:195`); `wifi.cpp` calls it
   nowhere (full-file grep of `runner/src`, confirmed above). A DMA channel
   configured with a Wi-Fi start-timing value will never fire automatically
   regardless of any Wi-Fi IRQ.
8. **`W_ID` and MAC/BSSID registers are static/generic, not firmware-derived
   beyond one console-type byte** — see §(e) below.
9. **ARM9 accesses to the aperture are not specially handled at all.** Because
   `nds_wifi_address` gates on `cpu == 7` only (`wifi.cpp:817-819`), an ARM9
   access to 0x04800000-0x0480FFFF falls through `bus.cpp`'s `is_io(addr)`
   check (`bus.cpp:417`, true for all of 0x04000000-0x05000000) into
   `nds_io_read`/`nds_io_write`'s generic default case, which is not
   `io_backed` for that range (`io.cpp:1607`, backing store is only
   0x04000000-0x04002000) and returns 0 with a `stub→0` stderr warning
   (`io.cpp:2823-2830`) rather than any documented open-bus/ARM7-only-aperture
   behavior. Functionally harmless (ARM9 has no legitimate reason to touch
   this range) but not a *modeled* hardware fact — it is the generic
   catch-all stub path.

---

## (d) Event / timing model

**Everything is driven by the guest cycle counter, never host wall-clock.**

- `nds_wifi_next_event_time()` returns `g_timer_deadline`
  (`wifi.cpp:739`), a value computed purely from guest-cycle arithmetic in
  `schedule_timer` (`wifi.cpp:174-183`), which converts a fixed
  `kTimerInterval` (8 Wi-Fi µs, `wifi.cpp:98`) into system cycles via the
  33,513,982 Hz NDS system clock constant and an error-accumulator for exact
  fractional-cycle tracking (`wifi.cpp:176-179`), matching the same pattern
  used by the RTC/SPU/LCD scheduler deadlines
  (`runner/src/scheduler.cpp:57-83`).
- The main scheduler folds this deadline into its global "next event" minimum
  (`nds_wifi_next_event_time()` is one term in the `std::min` chain at
  `scheduler.cpp:81-83`), and actually advances Wi-Fi state by calling
  `nds_wifi_run_events(rendezvous)` at every scheduler rendezvous point
  (`scheduler.cpp:367,378`), where `rendezvous` is the guest system-cycle
  timestamp for that rendezvous, not a host time value.
- Inside `nds_wifi_run_events`, every state advance — the µs counter, the MS
  timer (beacon/pre-beacon compare), the command-counter decay, and the TX
  slot state machine — is stepped in fixed `kTimerInterval` (8 µs)
  increments driven by the `while (g_power_on && g_timer_deadline <=
  timestamp)` loop (`wifi.cpp:744-814`), where `timestamp` is the caller's
  guest-cycle rendezvous value.
- `active_system_timestamp()` (used when `W_PowerUS` is written) derives its
  timestamp from `g_runtime_cycles` with a CPU-relative shift
  (`wifi.cpp:169-172`) — again the guest cycle counter, not
  `std::chrono`/wall-clock.
- **No wall-clock dependency was found anywhere in `wifi.cpp` or its call
  sites.** There is no `std::chrono`, no `time()`, no host-timer read in the
  file or in the scheduler glue reviewed. This satisfies plan §18's "do not
  make networking host-wall-clock-driven" rule for everything currently
  implemented — but note this only covers the *local device model*; no
  network backend exists yet to test against that rule (per plan §18's actual
  concern: host packet arrival timing quantized through the guest device
  model). That boundary hasn't been built, so it cannot yet violate or
  satisfy the rule either way.

---

## (e) Firmware Wi-Fi calibration plumbing status

**Largely unplumbed.** Two separate things exist in the firmware image per
`tools/fw_inspect.py`'s documented header layout (verified against
melonDS/DeSmuME per that file's own header comment, `tools/fw_inspect.py:29-48`):

1. **A real factory MAC address at firmware offset 0x36-0x3B**
   (`tools/fw_inspect.py:47,110-112`).
2. **An "ARM7 wifi code" boot part** — a second compressed/encrypted
   executable overlay pointed to by header offset 0x02 ("ARM7 wifi code
   offset ('part2')", `tools/fw_inspect.py:32,93,134`), distinct from the
   ARM9 GUI/menu part1 overlay. This is presumably the actual Wi-Fi
   driver/settings code a game or the firmware invokes to configure an access
   point, complete with its own checksum/length/version fields at header
   offsets 0x30/0x32/0x34 (`tools/fw_inspect.py:44-46`).

`nds_wifi_load_firmware` (`wifi.cpp:725-732`) touches **none of this**. It
reads exactly one byte — offset 0x1D, the console-type byte — and uses it
only to pick between two hardcoded device-ID constants (0x1440 vs 0xC340,
`wifi.cpp:729-731`). It does not read the MAC address at 0x36, does not
locate or stage the "wifi code" part2 overlay, and does not seed any BB/RF
default register value from this specific firmware image — the BB defaults
in `reset_bb()` (`wifi.cpp:151-167`) are fixed melonDS-derived constants
applied to every firmware image, not derived from this cartridge/firmware's
actual calibration bytes.

Separately, there **is** a generic, content-agnostic path that a real guest
driver could use correctly without any host shortcut: the firmware SPI chip
emulation holds the entire raw firmware image in `g_fw`
(`runner/src/io.cpp:1014,1981-1984`) and serves arbitrary byte reads to the
guest via the SPI firmware-read command handling (`io.cpp:1051-1053`
read, `io.cpp:1068` write). If the real ARM7 driver reads its MAC address (or
BB/RF calibration bytes) from flash via this SPI path and then writes them
into `W_MACADDR`/`W_BBCnt` via ordinary MMIO, that MMIO write lands correctly
in the generic register-store path (`wifi.cpp:695`, `wifi.cpp:534-540`) — so
the *architecture* for guest-driven calibration loading is sound; only the
"wifi code" part2 overlay that would actually execute this sequence has not
been captured/analyzed/promoted to a recompiled bank yet (see verdict, §a).

**Firmware-menu Wi-Fi touch today: none observed.** None of the 23
`bios/firmware_banks/*.toml` scenario banks
(`calibration_save`, `date_alarm_save`, `download_play_shutdown`, `early`,
`intermediate`, `irq_ready`, `main_menu_controls`, `menu`,
`pictochat_room_a`, `profile_save`, `shared_ready`, `shutdown`,
`system_options_save`, ARM7+ARM9 each) is the wifi-code part2 overlay, and
`bios/biosnds7.toml` has no Wi-Fi-related content (grep across `bios/`
returned no hits for wifi/POWCNT2/0x0480 outside the two calibration_save
program-name-string matches, which are unrelated "calibration_save" boot
scenarios, not RF calibration). This is consistent with the project's
firmware-menu gate being scoped to system settings, not Nintendo WFC setup.

---

## (f) Ordered list of specific hardware behaviors a stock driver needs next

Most-blocking first:

1. **A virtual access point and 802.11 management-frame exchange (beacon,
   probe request/response, auth, assoc).** Nothing in `wifi.cpp` produces or
   consumes an over-the-air frame at all (§c items 1-2); a real driver's very
   first WFC-setup action — scanning for and associating with an AP — has
   no counterpart to respond to it. This blocks every later step.
2. **RX frame delivery into packet RAM from something other than the guest
   itself.** The RX register machinery (`RXBufDataRead`/`RXBufCount`/gap
   handling) is fully implemented and correct, but with nothing to feed it, a
   driver's scan/associate/DHCP will simply never see a response
   (`wifi.cpp:431-448` mechanics vs. no injection path anywhere).
3. **TX frames actually reaching a transport.** Symmetric to #2: outbound
   management/data frames need to leave the device (`wifi.cpp:338-339`
   explicitly defers this) before any AP or bridge above it can react.
4. **RF chip data register(s) (W_RFData1/2 or equivalent).** A real init
   sequence programs the RF chip through control+data register pairs; only
   the control half exists (`wifi.cpp:651`), so any driver code that expects
   to write/read back RF chip register values through a data port has
   nowhere to land. Needed for RF bring-up validation, which typically
   precedes any transmit attempt.
5. **`W_BBBusy`/`W_RFBusy` real busy-cycle modeling.** Both are hardcoded to
   0 (`wifi.cpp:452-454`); a driver that polls these to pace a real chip
   transaction gets an immediate "done" every time, which is silently
   correct for us today only because there's no real transaction underneath
   to desync from — but it means any timing-sensitive BB/RF init sequence
   the driver expects (poll until busy clears) is unverified in this model.
6. **RX/TX address filtering (`W_RXFilter`/`W_RXFilter2`).** Stored but
   inert (§c item 5). Not blocking until real frames exist, but once they do,
   a driver expecting hardware-level address filtering to drop irrelevant
   frames will instead see everything the (future) AP delivers.
7. **DMA auto-trigger on Wi-Fi RX/TX events.** `nds_dma_trigger` is never
   called from `wifi.cpp` (§c item 7). Retail drivers commonly use DMA
   triggered by a Wi-Fi start-timing value to move packet RAM efficiently;
   until real RX/TX frames exist this is moot, but it will matter the moment
   they do.
8. **The firmware "wifi code" part2 overlay itself.** Per §e, this is the
   actual ARM7 Wi-Fi driver/settings code path and it has not been captured,
   decompressed, or promoted to a recompiled firmware bank
   (`tools/fw_inspect.py:32,93,134,137-139` — explicitly deferred as "Phase-3
   concern" in that tool's own note). Everything above concerns the register
   model this code would drive; until this overlay is analyzed, no guest
   code in this repo has ever actually exercised any of it, so all of the
   above is unverified against a real driver's actual instruction stream.

---

## Open questions / not established from code (flag explicitly)

- Exact real-hardware semantics of every `W_IF`/`W_IE` bit index used in
  `set_irq(n)` calls (7, 8, 9, 11, 13, 14, 15) are not named anywhere in this
  source file; the table above describes them only by triggering code path,
  not by asserted GBATEK bit name. **Needs oracle confirmation** if precise
  bit-name mapping is required.
- Whether the melonDS-derived fixed BB default bytes in `reset_bb()`
  (`wifi.cpp:151-167`) match *this specific* firmware's real calibration
  values, or only a generic retail default, was not established — the code
  offers no evidence either way beyond "these are melonDS's defaults, applied
  unconditionally." **Needs oracle confirmation.**
- The precise on-flash location/format of the "wifi config" data block
  referenced by firmware header fields 0x30/0x32/0x34 (checksum/length/
  version) was not traced beyond `tools/fw_inspect.py`'s header decode; that
  tool documents the header fields but does not locate or dump the
  calibration payload itself. **Not established from code in this repo.**
- Whether any DS title other than the firmware-menu system-settings scenarios
  already has a captured LLE bank that touches the Wi-Fi aperture was not
  checked (no per-game banks exist in this worktree yet — only firmware/BIOS
  banks were inventoried, per task scope).
