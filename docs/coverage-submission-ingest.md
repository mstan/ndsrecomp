# Ingesting player coverage submissions

beads-yjp.28. What a player-submitted Tier-3 coverage manifest actually
contains, what it is worth, and how to process one.

The numbers below are all measured from the first real submission:
`Metroid Prime - Hunters (USA)-coverage-20260821-205124-part00.json`, a story
playthrough captured on runner build `e9b530a`, 202,172,022 bytes of JSON
(11.7 MB as `.7z`).

## What arrived, by weight

| | |
|---|---|
| File | 202.2 MB JSON |
| Executed code pages captured | 309 versions at 259 distinct addresses |
| Actual code bytes carried | **1.27 MB** |
| Page-scoped entry records | 2,185,955 |
| Distinct `(page, PC, mode, kind)` behind them | 61,036 |
| Session-wide entry points | 59,455 (52,110 ARM9 + 7,345 ARM7) |
| Store overflow | `dropped: 0` — this manifest is **complete** |

196.5 MB of the 202.2 MB — 97% — is the pages section, and almost none of that
is code. It is the caller cross-product: the runner keyed each stored entry
record on the *caller* as well as the target, so a hot page accumulated one
record per call site. One page, ARM9 `0x0206F000`, carried **1,012,605 records
for 274 distinct PCs** because 71,610 different sites branched into it.

Measured fanout across the session: median 1, p90 3, p99 275, max 51,884.

The runner now keeps at most `kMaxCallersPerEntry` (4) distinct callers per
entry and folds every later one into a single record carrying
`caller: "0xFFFFFFFF"`, whose hit count stays exact. On this submission that
keeps 4.35% of the records. A/B'd over one MPH scenario (firmware boot to
title plus 30 scripted inputs, 9,000 vblanks, same seed both sides): the page
store is untouched — identical `captured`, `bytes`, `dropped`, `replaced` and
`revisits`, and the same `(addr, sha1)` sequence — the record count lands
exactly on the analytical prediction (17,866), the distinct
`(page, PC, mode, kind)` set is identical (13,574 both sides), and **not one
tuple's summed hit count changed**. That scenario's own fanout is only 1.5x,
so it shrinks by 14%; the 36x is a property of long sessions.

## Where the interpretation actually went

Root entries carry exact hit counts, and their sum reconciles to the last digit
with the `static_coverage` totals the same manifest reports
(152,453,002 ARM9 and 149,788,498 ARM7). So this is a complete attribution of
every interpreter entry in the session, not a sample:

| CPU | Region | Interpreter entries | Share | Closable by |
|---|---|---:|---:|---|
| ARM7 | `arm7.bin` WRAM alias `0x037F8000` | 149,757,319 | 49.5% | **config only** |
| ARM9 | `arm9.bin` main image `0x02004000` (declared) | 98,319,383 | 32.5% | seeds, image already committed |
| ARM9 | `arm9.bin` ITCM alias `0x01FF8000` | 46,411,975 | 15.4% | **config only** |
| ARM9 | overlay band `0x0210xxxx`+ | 7,721,644 | 2.6% | ingested banks |
| ARM7 | `arm7.bin` main-RAM alias `0x027E0000` | 31,179 | 0.0% | **config only** |

**100% of ARM7 interpretation, and 65% of all interpretation in the session,
sits at three load bases that no bank declares.** Closing it needs no dumped
bytes — the code is already in the repository. Another 32.5% needs only seeds
against an image already committed. Just 2.6% needs the bytes the file carried.

## What is in it that is worth anything

### 1. Load-base discoveries — the real payload

**289 of the 309 captured page versions (93.5%) are byte-identical to bytes
already in the ROM.** They are ARM9 module, ARM7 module and decompressed
overlay content, resident at a load address that no bank config declares. Three
aliases, none of them known before this file:

| CPU | Module | Declared base | Actually resident at | Guest span |
|---|---|---|---|---|
| ARM7 | `arm7.bin` | `0x02380000` | **`0x037F7E50`** | `0x037F8000..0x03808000` (64 KiB) |
| ARM7 | `arm7.bin` | `0x02380000` | **`0x027CFBC4`** | `0x027E0000..0x027F8000` (96 KiB) |
| ARM9 | `arm9.bin` | `0x02004000` | **`0x01F23440`** | `0x01FF8000..0x01FFA000` (ITCM, 8 KiB) |

The ARM7 pair is the whole story of this session. MPH's only ARM7 bank is
compiled at the ROM header address `0x02380000`. The guest relocates the module
to WRAM and to main RAM at boot and runs it from there, where nothing covers
it, so **every ARM7 instruction the game executed after that relocation ran on
the interpreter** — 4.46 billion of them, 201.5 million interpreter entries on
the WRAM alias alone, the hottest region in the entire capture.

That gap needs no player dump to close. It needs the two base addresses, which
are three lines of config. This is the highest-value thing a submission
produces and it is a few hundred bytes of the 202 MB.

### 2. Function-entry seeds

3,387 distinct `call` / `indirect` targets, of which 1,463 are ARM7 and 1,924
ARM9. Quality is good: 42.7% of the ARM9 ARM-mode targets begin with
`STMFD sp!, {..., lr}`, and exactly one word out of 1,919 decodes with
`cond == 0xF`.

1,454 of the ARM9 targets sit on pages that are byte-identical to the committed
`mph_arm9_fmv_runtime` image, and 1,298 of those appear nowhere in
`config/*.toml`, which holds 1,752 distinct seed addresses across all ten ARM9
bank configs. One file therefore takes the committed ARM9 seed set from 1,752
to 3,050 — **1.74x, against an image already in the repository**.

Spot-checked six of the 1,298 at random against the committed image: all six
decode as valid ARM (`cond == 0xE`), two are textbook `STMFD sp!, {…, lr}`
prologues, and none appear in any config.

### 3. Genuinely new code

20 page versions (~82 KB) hold code that exists nowhere in the ROM: in-place
patched pages and mixed code/data pages in the overlay band, plus one ARM7 page
at `0x03807000` with 4.45 million interpreter entries. These are the only bytes
in the file that a dump was actually required for.

### 4. What is *not* worth anything as a seed

**56,012 of the 59,455 session-wide entry points are `kind: root` — 94%.** A
root is where native code fell into the interpreter, which sounds like a missing
function entry, but the interpreter also re-enters at whatever instruction an
IRQ or a DMA stall interrupted. Over a long session that accumulates a root at
nearly every instruction of every hot interpreted function:

* 92% of root addresses have another root exactly one instruction away.
* The longest unbroken run of consecutive-instruction roots is **415**.

Roots are an execution map, not a seed list — but they are **not** disposable,
and **exactly one address per contiguous run of them is the most valuable seed
in the whole file** (beads-yjp.55, below). Their hit counts are also the only
exact cost attribution in the manifest. The defect was the *encoding*, not the
data: a dense bitmap was being written one ~96-byte JSON record per set bit.

Schema 4 stores them as bitmaps instead — ARM at word stride (128 B) and Thumb
at halfword stride (256 B) per 4 KiB page, plus 16 per-256-byte-block hit
counters. Every address and its mode survive exactly; only per-address hit
counts become a block share, and nothing consumed those. They appear twice:

* per captured page, **generation-bound**, so an overlay seeder can still tell
  overlay 9's landing pads from overlay 13's at the same address;
* in a top-level `root_map` keyed by page base, which is **never evicted** and
  survives part rotation. That one is new — before, the session-wide picture
  survived version eviction only because tier3's own map kept every address
  forever at ~96 bytes each, and eviction is common (one 9,000-vblank scenario
  reported `captured: 166` with `replaced: 87263`).

The ingest merges them into ranked interpreted spans
(`interpreted-ranges.json`) — the honest answer to "what to recompile next" —
and promotes the START of each one as a seed.

### 5. Interpreted spans — the only seed that reaches interpreted code

A `call` or `indirect` target is, by construction, an address some compiled
function branched to, so the finder had **already** discovered it. Promoting it
re-seeds code that is usually compiled already. The code the interpreter is
actually *running* is somewhere else entirely: in the holes **between**
compiled functions, where a computed branch or a jump table stopped discovery
(beads-yjp.35), and the only record of it anywhere in the manifest is the root
map.

Measured on MPH build `c6122bd` (2026-08-28 field parts): the six hottest call
targets were all seeded, all had live dispatch rows and all matched the ROM
byte for byte, while their *callers* — `0x0207B63C`, `0x0207B64C`,
`0x0207B67C`, `0x02085FB8`, `0x0208ADCC`, `0x0208B1D8` — had **zero dispatch
rows across all 251 banks**. The whole ingest that round added 0 banks and 0
ARM9 seeds: the pipeline had converged against its own inputs and was
structurally unable to reach the remaining 12.3 M interpreted instructions per
session.

**One seed per span, at its start.** Not one per root, and the reason is
structural rather than a preference for fewer seeds:

* `write_bank_dispatch` (`recompiler/src/main.cpp`) emits one dispatch row per
  **instruction** across `[fn.addr, fn.end_addr)`. A single seed at a span
  start therefore already yields an owning row for every instruction the finder
  walks from there — which is why one seed at `0x0207B630` covers
  `0x0207B63C`, `0x0207B64C` and `0x0207B67C` as well.
* `FunctionFinder::discover_one` dedups on the exact `(addr, mode)` only, and
  the boundary pass then trims the previous function's `end_addr` down to meet
  any later start. So every *extra* seed inside a body chops that body shorter
  and suppresses the fallthrough coalescing the dispatch cost depends on.

Runs are cut per **mode** and at instruction stride, because a span start is
compiled as one or the other and a run merged across modes has no single answer.

The `caller` field of every call/indirect record is folded into the same map
before the runs are cut: the interpreter was executing at that address, so it
is interpreted code by construction, and it is better attributed than a root —
it names a concrete instruction the manifest watched retire. On the four
2026-08-28 MPH parts, 186 of 316 distinct callers appear in no root bitmap at
all, so this is real coverage rather than a restatement.

Knobs: `--no-promote-ranges` reproduces the pre-yjp.55 policy exactly,
`--no-promote-callers` drops just the caller fold-in, `--min-span-hits` and
`--max-span-seeds` bound the seed budget (spans are ranked by cost, so a bound
spends it on the spans that carry the interpretation).

## How to ingest one

```
python tools/ingest_coverage_manifests.py <manifest.json>... \
    --out generated/coverage-ingest \
    --images ../metroidprimehuntersrecomp/generated/inputs \
    --rom-sha1 <expected>
```

`--images` is what enables ROM-alias resolution. Without it the tool still
runs, but on this submission it would have reported 289 pages of "new" code
that the repository already had, and hidden all three load-base findings.

Outputs:

* `ingest-report.json` — `rom_alias_findings` first; read that before anything
  else. Also counts every address refused and why.
* `entry_points.toml` — merged call/indirect seeds plus one seed per
  interpreted span, each tagged with what it was `seen as`.
* `images/` — one content-validated bank per run of genuinely new code. Span
  seeds land in the bank for the page *generation* they were interpreted
  under, which schema 4's per-page root bitmap is the only source of.
* `interpreted-ranges.json` — interpreted spans, hottest first, each with the
  mode it was recorded in. `span_promotion` in the report says how many became
  seeds and what they cost.

* `seeds/` — the merged seeds split by the module and load base their page
  resolved to. An address is only a seed for the bank whose image holds those
  bytes at that address, so this is the file you actually paste from.

Read the report before using the seeds. `manifests_incomplete: true` means the
runner hit its page-store cap and the submission is a partial view.

On the submission above this produces 3 alias findings, 3,387 merged entry
points, 19 content-validated banks for the code that is genuinely new, and
8,963 ranked interpreted spans, in about 4 seconds. `entry_points_by_target`
says where each seed goes:

```
arm7.bin  @ 0x037F7E50   1,352      arm9.bin @ 0x02004000   1,089
arm7.bin  @ 0x027CFBC4      90      arm9.bin @ 0x01F23440      32
overlay_002 @ 0x02102220   305      overlay_010 @ 0x0214C940  187
overlay_013 @ 0x02133060   125      overlay_015 @ 0x02168B20   49
overlay_000 @ 0x02102220    35      overlay_009 @ 0x02133060   25
overlay_008 @ 0x0212C600    19      unresolved                 79
```

Reading that table is how MPH overlay 13 was found to have no bank config at
all despite being executed (beads-lqa.10).

Schema 2 manifests still ingest. They carry no per-page entry association, so
banks assembled from one are seeded by address containment — which is all
schema 2 ever supported.

## Why none of this is behind a verbose flag

It is tempting to answer "the file is too big" with a `--verbose` switch. That
is the wrong shape here, and the history says so: this whole feature exists
because recording *was* gated — behind `--discover-static-misses`, which no
shipped launcher passed — so a player could finish the game and hand back an
empty map. Anything a player has to opt into is not captured.

The measured breakdown says a flag is not needed anyway. Projected size of the
same MPH story session under each policy:

| Policy | Size | vs. original |
|---|---:|---:|
| As shipped (`e9b530a`) | 202.2 MB | 1.00x |
| + caller cap 4 | 16.5 MB | 0.082x |
| + roots as bitmaps (schema 4) | **2.6 MB** | **0.013x** |
| + drop page bytes that are in the ROM | 1.0 MB | 0.005x |

Nothing was dropped to get from 202 MB to 2.6 MB — every root address, every
mode, every entry point and every code byte is still there. Only two encodings
changed. There is nothing left worth hiding behind a flag.

That last row is deliberately **not** implemented. Those bytes are 93.5% ROM
content, which sounds like the obvious thing to stop logging — but all 309
captured pages together are 1.7 MB, under 1% of the original file. They were
never the bloat. They are also the one thing that cannot be reconstructed if
the alias guess is wrong, and they are what lets the ingest verify a stranger's
manifest against the ROM at all. 1.6 MB is not worth trading that for.

## What to ask a submitter for

* **Every part.** Files are named `<base>-coverage-<runstamp>-partNN.json`. A
  session that fills the page store rotates to the next part rather than
  dropping pages, so `part00` alone can be a fraction of the run. One runstamp
  is one session; several runstamps are several sessions and all of them merge.
* **Compressed.** These are JSON and compress about 17:1 — the 202 MB file is
  11.7 MB as `.7z`.
* **Breadth over length.** The store deduplicates by content, so a second hour
  in an area already visited adds almost nothing, while ten minutes in a new
  one adds a whole overlay generation. Ask for coverage of areas, modes and
  menus not yet reached, not for long sessions.
* Nothing else. The manifest already carries the ROM SHA-1, the runner build
  id, and the identity needed to reject a mismatched dump.

## Reviewing a submission before trusting it

Manifests come from other people's machines. The tool already recomputes every
page's SHA-1 from its own bytes and rejects a manifest whose ROM SHA-1 does not
match the target, and only executed *code* pages are ever stored, so save data,
console nickname and WFC credential material are structurally absent rather
than scrubbed. Beyond that:

* `pages.dropped > 0` means the session outran the store; treat coverage as a
  lower bound and ask for the later parts.
* A page whose bytes are not in the ROM is the only thing that can carry
  something unexpected. There are usually very few — check them.
* `build_id` says which runner produced it. A manifest from an old build can
  still be merged; it just may predate a schema field.

## Known limits

* One page still costs 4 KiB even when four instructions in it executed, so a
  code page sharing its 4 KiB with churning data produces a fresh version on
  every content change. `kMaxVersionsPerAddress` caps that at 8 with LRU
  eviction; a CFG-reachability capture would replace it properly (beads-yjp.29).
* The bank assembler will only extend a run across a page boundary when the
  next address has exactly one captured version. With several versions there is
  nothing in the manifest that says which was resident alongside, and guessing
  concatenates unrelated overlay generations.
* Roots are recorded per instruction rather than as ranges, so they still cost
  ~100 bytes each in the manifest. A run-length or bitmap encoding would remove
  most of the remaining size.
