# Overlay strategy: can psxrecomp's approach be adopted for NDS overlays?

Status: research/design note. No code in this repo was touched to produce
it. Every claim about psxrecomp below was re-derived by reading its
implementation directly (source files, headers, `CMakeLists.txt`,
build-generated cache layout) — **not from its markdown documentation**,
which was deliberately excluded as a source. psxrecomp's docs describe a
mix of shipped and intended work under plan-shaped filenames
(`AOT_OVERLAY_PLAN.md`, `overlay-plan.md`, `ASYNC_OVERLAY_COMPILE.md`); a
prior pass through this project confirmed the docs disagree with each
other and with the code in places, which is exactly the failure mode this
note avoids by not citing them at all. Every mechanism below is cited by
`file:line` in the code, or reported as "no implementation found."

## 0. Which psxrecomp tree, and why

`F:\Projects\psxrecomp` is a scratch container with dozens of sibling game
projects, throwaway build directories (`_build-*`, `_wt-*`), and stray
artifacts — not a repo itself. The real recompiler engine is a separate
git project (`https://github.com/mstan/psxrecomp.git`) consumed as a
submodule by each game project. Two on-disk copies exist:

- `F:\Projects\psxrecomp\ApeEscapeRecomp\psxrecomp-v4` — a proper git
  checkout of that submodule (`.git` present, `origin` →
  `github.com/mstan/psxrecomp.git`, `HEAD` detached at `e8441c6a`,
  Aug 9 2026, with `local-canonical/master` and dozens of feature/fix
  branches fetched). This is a live, actively-developed engine checkout.
- `F:\Projects\psxrecomp-v4\recompiler` — **not a git repository at all**
  (`git -C ... log` fails with "not a git repository"). It is a bare file
  extraction with no provenance and no way to tell what revision it is.

**Authoritative tree: `F:\Projects\psxrecomp\ApeEscapeRecomp\psxrecomp-v4`.**
It is the only copy with git history, remotes, and branches to check
against; the other is an untraceable snapshot. All citations below are
paths relative to this tree unless stated otherwise. All searches
excluded `recompiler/build/`, `_deps/` (vendored `fmt`, `rabbitizer`,
`libchdr`), and other build-output directories.

## 1. Verdict table (code-derived; ASPIRATIONAL dropped per instruction — "no implementation found" stated plainly instead)

| Mechanism | Status | Evidence |
|---|---|---|
| Per-candidate CRC32 keying of overlay code, scoped to explicit byte ranges | **SHIPPED** | `runtime/src/overlay_loader.c:70-89` (`Candidate` struct: `range_lo/range_len/nranges`, `crc_code`); `overlay_loader.c:541-547` (`cand_crc()`, CRC32 over live RAM in those ranges); `overlay_loader.c:1048-1057` (`crc_code` set from manifest at registration, initial `state` computed) |
| Same load-address, multiple overlay bodies (collision) | **SHIPPED** | `overlay_loader.c:68` (`ENTRY_VALID/ENTRY_INVALID/ENTRY_BLACKLIST` states, not deletion); `overlay_loader.c:198` (`idx_head`, hash → head index); candidate chain walk confirmed at `overlay_loader.c:3502` (`idx_head(phys)`), `3560-3564`/`3666-3676` (CRC check per chained candidate, first match wins, others marked `ENTRY_INVALID` not removed) |
| O(1)-ish dispatch (hash lookup + cheap re-validate, not full rehash every call) | **SHIPPED, with a caveat** | `overlay_loader.c:198` hash index is O(1) average; `overlay_loader.c:3148` / `3410-3412` show the **fast path still calls `cand_crc()` (a real CRC32 over the code bytes) whenever `state==ENTRY_INVALID` or on the generation-mismatch branch** — i.e. the "cheap" path is a generation-counter *gate* around an expensive CRC, not a replacement for it; a full CRC32 recompute happens on every generation change, not only once per code write across the whole session. Treat "cheap" as "cheaper than rehashing unconditionally," not "O(1) always." |
| Overlay code compiled ahead of the game needing it, from a real recompiler invocation | **SHIPPED** | `tools/compile_overlays.py:3740-3746` (`subprocess.run([args.recompiler, psx, '--seeds', ..., '--overlay', '--ws-config', ...])`); `tools/compile_overlays.py:5469-5475` (second call site, same shape, with `PSX_CPS` env for continuation-passing mode) |
| That generated C actually gets compiled to a loadable native artifact | **SHIPPED** | `tools/compile_overlays.py:4037` (`subprocess.run(cmd, ...)` invoking `tcc`); `tools/compile_overlays.py:4090` (`subprocess.run(cmd, ..., env=env)` invoking `gcc -shared`) |
| Runtime spawns a compiler process itself (not just an offline script) | **SHIPPED** | `runtime/src/autocompile.c:900` (`CreateProcessA(NULL, full, ...)`); this is a real Win32 process spawn, not a stub |
| Recompiler (the MIPS→C translator) is overlay-aware in its own codegen, not just "runs twice" | **SHIPPED** | `recompiler/src/code_generator.cpp` has 25+ live `config_.overlay_mode` branches gating instruction-patch application (`:794,820,838,890`) and continuation/exit-switch shape (`:987,1001,1023,...,1346`); fail-closed interior-entry guard emission is gated the same way (`:2555,2572,2847,2858,2895`) |
| Fail-closed guard: entering a native overlay function at an untranslated interior PC is caught, not undefined behavior | **SHIPPED** | `overlay_loader.c:252-270` (`psx_native_bad_entry`, sets `g_native_bad_entry`, consumed right after the native call returns) |
| Interpreter fallback wired into the real execution path (not test-only) | **SHIPPED** | `runtime/src/dirty_ram_interp.c:2668-2669` and `:3004-3005` both call `overlay_loader_dispatch(cpu, addr/target)` and only fall through to interpretation on a `0` return |
| Validation on mismatch fails closed (no assert/abort, no silent wrong-code execution) | **SHIPPED** | `overlay_loader.c:3791,3795` (`c->state = ENTRY_INVALID;` on CRC or delay-slot-hash mismatch, then falls through — no abort call found in this path) |
| Static/build-time-baked overlay tier (compiled into the executable itself, no runtime `LoadLibrary`) | **PARTIAL — consumer absent** | `runtime/runtime.cmake:747` gates inclusion of a game-supplied `${PSXRT_GAME_OVERLAY_STATIC_C}` on it existing; repo-wide search for that CMake variable being *set* by any game project, generated file, or script returns nothing outside this one `runtime.cmake` reference. The runtime-side plumbing to consume such a file exists; nothing produces or points it at one. |
| Recompiler-side ctest coverage for overlay codegen shape | **PARTIAL — registered but not CI-enforced** | `recompiler/CMakeLists.txt:364-366` (`overlay_cross_page_delay_codegen`), `:376-378` (`overlay_shim_compile_contract`), `:394-395` (`overlay_guard_codegen`, `overlay_store_barrier_codegen`) are real `add_test()` entries that invoke the actual recompiler binary — this is genuine coverage, not a stub. No GitHub Actions workflow file in `.github/workflows/` that runs on pull requests invokes `ctest` against this target set (confirmed by reading the workflow files directly, not a docs claim); the tests only run when a developer runs `ctest` by hand. |
| Manual forensic tools (`dump_overlays.py`, `extract_overlays.py`, `overlay_xref.py`) | **PARTIAL — real code, no automatic caller** | Each does real, non-trivial work (disc ISO9660 parsing + CRC recovery, MIPS disassembly over capture corpora, TCP-based live dump) but a repo-wide grep for each script's name finds no caller in any `CMakeLists.txt`, build script, or the runtime — they are invoked by a human, standalone |

## 2. How the shipped mechanisms actually work

**Keying.** An overlay "identity" is not a single opaque bank ID. It is a
per-function `Candidate` (`overlay_loader.c:70-89`) carrying up to 16
explicit `(start, len)` code-byte ranges plus one CRC32 (`crc_code`) that
was computed by the offline recompiler/`compile_overlays.py` pipeline over
those exact ranges when it emitted the manifest the candidate was loaded
from. At runtime the *only* question ever asked is "do the live bytes in
these ranges still hash to the value I was built from" — there is no
separate overlay-ID or table-slot concept at the runtime layer; identity
*is* content.

**Same load address, two overlays.** This is a first-class, handled case,
not a corner case bolted on. `idx_head(phys)` (`overlay_loader.c:198`)
hashes the physical entry address into a table of head indices; each
`Candidate` carries a `next` link so multiple candidates can chain off one
address. Dispatch (`overlay_loader.c:3502` onward) walks that chain, CRC32
re-checking each candidate against live RAM in turn, and the first match
wins. Non-matching candidates are marked `ENTRY_INVALID`, not deleted —
so if RAM later reverts to bytes matching a previously-invalidated
candidate, the *next* dispatch's CRC check will find it valid again
without needing to re-register anything. This is genuinely how two
overlay bodies at one PSX RAM address are told apart: whichever one's
bytes are *actually resident right now* wins, checked freshly (or via the
generation-gated fast path) on every dispatch.

**Cost on the hot path.** Table lookup is O(1) average (hash + short
chain over same-address variants, typically 1-2 deep). The CRC32
recomputation is gated by a per-page write-generation counter
(`overlay_loader.c:3148`, `:3410-3412`): if no write has touched the
candidate's backing pages since the last successful validation, the
cached `state` is trusted without rehashing; a write bumps the
generation and forces one full CRC32 pass at the next dispatch into that
candidate. This is cheaper than hashing on every call, but it is **not**
free — it is "one CRC32 per code-modification event," and a title that
writes its overlay-adjacent RAM frequently (self-relocating loaders,
scratch buffers sharing a page with code) pays that cost repeatedly. This
detail matters directly for the DS assessment in §5.

**Sharding / build organization.** The evidence here is the actual build
artifacts and CMake, not narrative: `compile_overlays.py` calls the
recompiler once per overlay unit with `--overlay` (`compile_overlays.py:
3740-3746`, `5469-5475`), producing one `_full.c` per invocation, which is
then compiled to one loadable native artifact (`.dll`/`.so`) via a real
`gcc -shared`/`tcc` subprocess call (`:4037`, `:4090`). Each overlay
therefore gets its own translation unit and its own dynamically-loadable
binary — sharding is per-overlay-unit, driven by the offline tool's loop
over units, not by any pass inside the recompiler splitting one big
translation. `runtime.cmake:747` shows a second, unused delivery path
exists in the runtime's build (a single `overlays_static.c` linked
directly into the executable, skipping `LoadLibrary` entirely) but no
code anywhere populates or points at that variable, so today sharding in
practice means "N overlays → N shared libraries," full stop.

**Dispatch.** `overlay_loader_dispatch(CPUState *cpu, uint32_t addr)`
(entry points into this logic confirmed live-wired at
`runtime/src/dirty_ram_interp.c:2668-2669` and `:3004-3005`) is called
from the interpreter's own indirect-branch handling, *before* falling
back to interpreting. It returns 0 (meaning "not handled, please
interpret") in every failure case: no candidate registered at that
address, CRC mismatch, MMIO-touching candidate (deliberately never run
natively because "the shadow diff can't safely double-execute
MMIO/SIO/DMA" per the comment structure at the relevant branch), or the
fail-closed bad-entry guard tripping after a native call returns
(`overlay_loader.c:252-270`). This is a real check-and-jump into and out
of native code on every single indirect control transfer that lands in
overlay-owned RAM, not a one-time "install a handler" hook.

**Compile timing.** Two real paths, confirmed by reading the process-spawn
call sites directly rather than inferring from filenames:
1. Offline/ahead-of-time: `tools/compile_overlays.py`, a standalone
   Python driver, calls the recompiler binary and then a real C compiler,
   producing `.dll`/`.so` artifacts before the game runs. This is a batch
   tool a developer runs, not something wired into the game's own build
   (`recompiler/CMakeLists.txt` and `runtime/runtime.cmake` have no
   `add_custom_command` invoking this script).
2. Runtime/asynchronous: `runtime/src/autocompile.c:900` really calls
   `CreateProcessA` to spawn a compiler from inside the running game
   process. This is the mechanism that turns "an overlay that has never
   been seen before" into a native artifact without the developer having
   run the offline tool first — it happens off the emulation thread (the
   call site is inside a background-worker path in `autocompile.c`, not
   inline in `overlay_loader_dispatch`).
Either way, the artifact that eventually gets used is a **precompiled
shared library on disk**, loaded via `LoadLibrary`/`dlopen` — dispatch
itself never compiles anything; it only ever picks between "run this
already-compiled candidate" and "fall to the interpreter."

**Fallback.** The dirty-RAM interpreter in `dirty_ram_interp.c` is the
sanctioned floor — every failure mode in `overlay_loader_dispatch`
degrades to it, never to undefined behavior or a crash. Recovery back to
native is automatic and generation-driven: once a page's write generation
stabilizes and a subsequent dispatch's CRC32 recheck matches `crc_code`
again, `state` flips back to `ENTRY_VALID` and the very next call into
that address runs native code — no restart, no manual re-registration.

**Discovery.** Not investigated in this pass beyond what the recompiler
itself does with the units it's handed (`--overlay` mode gates codegen
shape, as shown in §1's `code_generator.cpp` row) — this note does not
make a SHIPPED/PARTIAL claim about *how psxrecomp finds overlay entry
points in the first place* (e.g. from disc metadata vs. RAM-write
observation), because that determination would require tracing the
capture/extraction call graph in `overlay_capture.c` and the disc-reading
tools end to end, which this pass did not complete to file:line rigor.
**Flag: unconfirmed, not claimed either way.**

**Validation/safety.** Every native dispatch requires the live-RAM CRC32
over the candidate's declared ranges to equal the manifest's `crc_code`
(`overlay_loader.c:3564`/`3676`, using `cand_crc()` at `:541-547`). On
mismatch, the candidate is marked invalid and the call falls through to
the interpreter (`:3791`, `:3795`) — no abort, no assert, no silent
execution of code that doesn't match what's actually resident. This is
the load-bearing safety property of the whole design: a stale or wrong
candidate can only ever cause a miss (slow), never wrong behavior
(incorrect).

## 3. What ndsrecomp already has, mapped to each mechanism

ndsrecomp did not copy psxrecomp's file layout, but it already has
working analogues of nearly every SHIPPED mechanism above, under
different names, in code that exists today:

**Tiering.** `docs/dispatch_architecture.md:9-38` documents (and the code
implements) a three-tier model explicitly modeled on psxrecomp: Tier 1
(statically recompiled native banks), Tier 2 (a dirty-RAM JIT, **not yet
built** — `docs/dispatch_architecture.md:31` says it is "deferred until
Tiers 1+3 boot the menu"), Tier 3 (the bounded dirty-RAM ARM/THUMB
interpreter, `runner/src/tier3.h:1-7`, `runner/src/tier3.cpp`). Tier 3 is
psxrecomp's dirty-RAM interpreter fallback under a different name, and is
already the sanctioned, CLAUDE.md-blessed exception to "no interpreter on
the hot path" (`ndsrecomp-wiimmfi/CLAUDE.md` "FAILURE MODE" list: interpreter
use is invalid "except the bounded dirty-RAM interpreter running the
guest's own copied bytes").

**Content-validated dispatch banks = psxrecomp's CRC-gated candidate.**
This is real, shipped, load-bearing code, not a name I'm mapping loosely.
`NdsStaticValidation` (`recompiler/armv4t/runtime_arm.h:91-95`) attaches
an expected-byte-range check to a `NdsDispatchEntry`
(`runtime_arm.h:97-102`); the check site is `lookup_in()`
(`runner/src/runtime_arm.cpp:270-298`), which requires **both**
`bus_range_has_write_provenance(validation->addr, validation->size)`
(has the guest actually written every one of these bytes since reset,
not just does it read as zero) **and**
`bus_live_bytes_equal(validation->addr, validation->expected,
validation->size)` (do the live bytes exactly match this generation's
expected bytes) before treating a dispatch-table row as active. A
non-matching row is skipped, not run (`runtime_arm.cpp:295-297`,
`inactive_candidate`). Cache invalidation uses a per-page generation
counter (`runtime_arm.cpp:323-327`, `:369-374`), which is the same
generation-gate-around-an-expensive-check shape as psxrecomp's
`val_gen`/`cand_crc()` pairing, and it is already built to handle
**multiple mutually-exclusive code generations sharing one virtual
address** — psxrecomp's core overlay use case — because that is exactly
why the comment at `runtime_arm.h:88-90` says the field exists ("the same
virtual address for different code generations"). ndsrecomp uses exact
byte-equality where psxrecomp uses CRC32; functionally the same
guarantee (fail closed unless the live bytes are known-good), at
marginally higher per-check cost for large ranges and marginally better
collision safety (no hash-collision surface at all).

**Provenance-checked ARM7 VRAM execution** is a second, independent
instance of the same write-provenance primitive, scoped to VRAM:
`runner/src/vram.cpp:333-349` (`exec_physical_offset`) unconditionally
rejects ARM9 execution from VRAM (fail-closed, no validated title path
yet) and for ARM7 requires exactly one physical bank mapped to the
fetched window (`mask & (mask - 1u)` rejects multi-bank ambiguity) plus
per-byte write provenance via `g_vram_written[]`
(`vram.cpp:358-368`, `:93-107`). This demonstrates the *general shape*
ndsrecomp already uses for "is it safe to treat this RAM as containing
real guest code" is reusable beyond main RAM — VRAM is a second address
space with its own provenance array, following the identical pattern as
main RAM/ITCM/DTCM (`bus.cpp:139-161`, four parallel `_written`/
`_generation` arrays, one set per backing store).

**MKDS overlay inventory exists but is not fed to the recompiler.**
`generated/inputs/overlays.json` (schema: `id, file, file_id,
load_address, size, bss_size, static_init_start/end,
compressed_in_rom, compressed_size, sha1` — no dispatch/entry-point
field) is produced by `tools/prepare_mkds.py:150-179`, which extracts
each overlay's decompressed bytes and records its SHA-1, but — unlike the
ARM9/ARM7 main images, which get a full recompiler program config via
`write_seed_config()` (`prepare_mkds.py:34-81`, called at `:131-148`) —
**no equivalent config/`.toml`/entry-point generation exists for
overlays anywhere in this file.** `CMakeLists.txt:40-55` lists
`overlays.json` as a build output/dependency but nothing downstream
invokes the recompiler over the extracted `.bin` files. This confirms
the brief's framing precisely: overlays are inventoried, hashed, and
extracted, but never translated.

**`promote_mkds_static_coverage.py` actively excludes overlay space from
promotion**, by design (whether intentionally scoped or not, the effect
is the same): `MAIN_RANGES = {9: (0x02000800, 0x021773B8), 7:
(0x02380000, 0x023A89A4)}` (`promote_mkds_static_coverage.py:12-15`) gates
every promotable Tier-3 observation (`:52-56`, `if cpu not in
MAIN_RANGES or kind not in PROMOTABLE_KINDS: continue`), and the tool's
own output field documents this: `"selection": ("Tier-3 call and
indirect targets inside immutable ARM9/ARM7 main-image ranges;
slice-resume roots and runtime overlays omitted")` (`:87-90`, quoted
verbatim). Anything Tier-3 ever observes running in overlay RAM is
silently dropped from the promotion pipeline today.

**Net finding:** ndsrecomp already has the two hardest pieces of
psxrecomp's SHIPPED design — (a) a fail-closed, provenance-gated,
multiple-generations-at-one-address dispatch mechanism
(`NdsStaticValidation`), and (b) a bounded, sanctioned interpreter floor
(Tier 3) that everything degrades to safely. What it does **not** have is
psxrecomp's third leg: a pipeline that (i) extracts an overlay's bytes,
(ii) feeds them through the recompiler to get native C, (iii) compiles
that C, and (iv) registers the result as a validated dispatch-table row
keyed off the same `NdsStaticValidation` mechanism it already has. Steps
(i) exist (`prepare_mkds.py`'s extraction), (ii)-(iv) do not exist for
overlays at all.

## 4. Recommendation

**Do not port psxrecomp's runtime-compile machinery (`autocompile.c`'s
`CreateProcessA` spawn, the `LoadLibrary`-of-freshly-built-DLL loop, the
async cache-warming state machine).** That is the single most
complex and highest-risk part of psxrecomp's own design — it exists
because PSX has no comparable static-recompilation-time visibility into
which overlay units a given disc image will ever load, and a shippable
build needs *some* way to turn "never seen before" into "native" without
a human re-running the build. ndsrecomp does not have that constraint:
MKDS's overlay set is fixed, finite (4 ARM9 overlays per
`generated/inputs/overlays.json`), and known completely at build time —
every overlay's bytes are already extracted before the game ships.
Runtime compilation would add a Win32-process-spawn dependency, a second
compiler toolchain requirement at player runtime, and a whole class of
cache-poisoning/TOCTOU risk (which is exactly why psxrecomp itself needed
path-canonicalization hardening) to solve a problem ndsrecomp doesn't
have: **there is no unknown overlay to discover at runtime, only a known,
closed set to translate ahead of time.**

**Do build the missing pipeline stage, ahead-of-time, reusing what
already exists on both ends:**

1. Extend `tools/prepare_mkds.py` to emit a recompiler-consumable config
   per overlay (a `write_seed_config()`-equivalent call, mirroring what
   already happens for `arm9.toml`/`arm7.toml`), using the load
   address and size already captured in `overlays.json`.
2. Run these overlay units through the existing ARMv5TE/ARMv4T
   recompiler (`recompiler/armv4t/*`) exactly as the base images are
   recompiled today — no new codegen mode is obviously required *unless*
   an overlay can be resident at an address another overlay or the base
   image also legitimately occupies (MKDS's `overlays.json` shows three
   of the four ARM9 overlays sharing `0x021804c0`, so this is not
   hypothetical — see §5). Where that's the case, add the equivalent of
   psxrecomp's `overlay_mode`-gated fail-closed interior-entry guard
   (`code_generator.cpp`'s `psx_native_bad_entry`-shaped emission) to the
   ARMv5TE/ARMv4T codegen, since ndsrecomp's own `NdsStaticValidation`
   dispatch check already assumes exactly this "multiple generations, one
   address" shape and needs codegen cooperation for the interior-entry
   case the same way psxrecomp's does.
3. Wire each overlay's generated function set into a new
   `NdsDispatchEntry` table with `validation` populated from the
   overlay's known load address + size + expected bytes (the SHA-1 in
   `overlays.json` is a session-identity hash of the whole decompressed
   blob; the per-row `validation->expected` check should use the actual
   expected bytes at the function's declared range, matching the pattern
   already used for firmware-variant banks — do not introduce a second,
   separate hashing scheme parallel to the one that already exists).
4. Remove the `MAIN_RANGES` exclusion in
   `promote_mkds_static_coverage.py` for address ranges that are now
   backed by a compiled, validated overlay bank — once (1)-(3) exist for
   a given overlay, Tier-3 observations inside it should promote exactly
   like main-image observations do today.

**Sizing: this is a recompiler + tooling change, not a runtime
architecture change.** ndsrecomp's runtime already has the dispatch-table
validation, the generation-gated invalidation, and the Tier-3 floor it
would fall back to on a miss. The work is (a) recompiler-side: teach
codegen to emit the interior-entry guard when a unit is compiled in
"overlay mode" (new, bounded — one config flag threading through
existing codegen paths, same shape as psxrecomp's `overlay_mode`), and
(b) tooling-side: extend `prepare_mkds.py` and
`promote_mkds_static_coverage.py` to stop treating overlay address space
as out of scope. No new process-spawn, no new file-cache format, no new
runtime dispatch tier.

**First concrete step:** pick the single ARM9 overlay in
`overlays.json` that is *not* one of the three sharing
`0x021804c0` (overlay index 3, loaded at `0x021b7b60` per the schema
read during this investigation) as the pilot, because it has no
same-address collision to handle yet — get it through
`prepare_mkds.py` → recompiler → a real `NdsDispatchEntry` with
`validation` populated, and prove Tier-3 promotion works end to end for
one overlay before tackling the three-way collision at `0x021804c0` that
needs the interior-entry guard.

## 5. DS-specific risks

**Dual-CPU keying.** psxrecomp has exactly one CPU and one address space
to key overlays against. NDS overlay code in `overlays.json` is ARM9-only
(all four entries load into main RAM in the ARM9-visible range,
`0x0218...`/`0x021b...`), so the *keying* dimension does not need a CPU
tag the way the brief worried it might — an overlay resident at a given
address is either ARM9 code or it isn't, and ARM7's separate WRAM/BIOS
address space doesn't alias into it. The dual-CPU risk is not in keying;
it is in **dispatch-cache scope**. ndsrecomp's per-CPU dispatch tables
(`docs/dispatch_architecture.md:21`, "binary search over a per-CPU
`{guest_pc -> fn ptr}` table") and per-CPU Tier-3 stats/state
(`tier3.h:41-47`, `Tier3Stats entries[2]/instructions[2]`) already
partition by CPU — an overlay bank added under this design must be
registered only into the ARM9 table, and any shared-RAM aliasing check
(does an address the ARM9 overlay occupies ever get fetched by ARM7,
e.g. through the shared-WRAM window) needs to be verified false for MKDS
specifically before assuming it, not assumed by analogy to psxrecomp
(which has no such question at all).

**Cache/TCM interaction — this is the real DS-specific hazard.** The
ARM9's CP15 controls ITCM/DTCM placement and can remap where address 0
and a configurable DTCM base point
(`runner/src/cp15.cpp:33-46,138-169`, `apply_control()`/TCM
base-and-size writes call `bus_fast_refresh()` because "TCM placement
changes what an [address resolves to]" per the file's own header
comment at `cp15.cpp:2-6`). ndsrecomp's provenance/generation tracking is
already TCM-aware at the storage level — `g_itcm`/`g_dtcm` have their own
parallel `_written`/`_generation` arrays exactly like main RAM
(`bus.cpp:139-161`, `:453-463`) — but a `NdsStaticValidation` entry's
`addr`/`expected` fields are **virtual addresses at table-build time**.
If CP15 remaps DTCM base after a validated bank was registered against
the old base, the validation's page-tracking math
(`runtime_arm.cpp:369-374`, `first_page = validation->addr & ~0xFFFu`)
would be checking generations for the *wrong* physical backing unless
something re-derives `addr` through the current CP15 mapping at
validation time, not just at registration time. This is not a
hypothetical from the brief — it is a real DS mechanism (game code
routinely remaps DTCM for scratch buffers) that has no PSX analogue at
all, since PSX has no equivalent MMU/TCM remapping. **Any overlay
validation entry whose address range can be affected by CP15 remapping
must re-resolve against the live TCM mapping on every check, not cache a
stale physical target** — verify this explicitly before shipping overlay
dispatch entries; MKDS's overlays load into main RAM (`0x0218...`/
`0x021b...`), which is outside the TCM range entirely, so **the pilot
overlay in the recommended first step is unaffected by this risk** — but
any future overlay or self-relocating code that lands in
TCM-remappable address space is not automatically safe under the same
scheme without this fix.

**Per-write CRC/hash cost on a shared-RAM title.** §2 noted psxrecomp's
"cheap" path is a generation-gate around a real CRC32, paid on every
write to a tracked page, not once per session. If MKDS's WFC/DWC code
shares a RAM page with a frequently-written scratch buffer or packet
staging area (plausible for networking code), the equivalent
`bus_live_bytes_equal` check in ndsrecomp's existing mechanism would run
on every dispatch following such a write. ndsrecomp's exact-byte-compare
is at least as expensive per check as psxrecomp's CRC32 for a given range
size, so this is not a regression versus psxrecomp — but it is a cost
this design inherits, and it should be measured against MKDS's actual
WFC RAM layout before assuming it's negligible, not assumed clean by
default.

## 6. Can MKDS's overlay-resident WFC code become native, or is Tier-3 the near-term reality?

**Tier-3 interpretation is the honest near-term answer**, for a structural
reason independent of any of the above design work: none of it exists
yet. `prepare_mkds.py` extracts overlay bytes but never invokes the
recompiler over them (confirmed by reading the file directly — no
`write_seed_config`-equivalent call, no recompiler invocation, anywhere
in the overlay-handling code path); `promote_mkds_static_coverage.py`
actively excludes overlay address space from promotion by its
`MAIN_RANGES` gate. Until the pipeline stage recommended in §4 is built —
which is real, bounded engineering work (a config-generation change in
one tool, a codegen flag threading change in the recompiler, a
dispatch-table population step, and removal of one hard-coded exclusion)
but is not done today — **any PC inside an ARM9 overlay, WFC/DWC code
included, executes in Tier 3, unconditionally**, because that is the only
tier ndsrecomp currently has that can run code at an address with no
static bank.

**What that means for guest-side DWC timeouts, concretely:** Tier 3 is a
bytecode interpreter (`tier3.h:1-7`, "the reference ARM/THUMB
interpreter"), which is orders of magnitude slower per guest instruction
than the natively-recompiled Tier-1 path the rest of the game runs
through once booted. WFC/DWC networking code that is functionally
correct under interpretation can still fail a real network exchange if
the *guest's own* timeout/retry timers (driven by the DS's hardware
timers, which keep real wall-clock time regardless of host emulation
speed) expire before the interpreted code finishes producing a response
or servicing an incoming packet in time — this is a timing failure, not a
correctness failure, and it will not show up as a divergence from a
byte-accurate oracle the way a decode or dispatch bug would. It shows up
only as a live-network flake: a request that would have succeeded had
the guest not blown its own retry window. This risk is specific to code
in the hot networking path (packet assembly, ACK/retry state machines,
per-frame protocol polling) — it does not apply to overlay code that only
runs during scene load or menu transitions, where wall-clock slowdown is
merely a perceptible pause, not a protocol failure.

**Practical implication:** treat "get MKDS's WFC overlay onto Tier 1" as
a real, scoped near-term goal enabled directly by the machinery this
project already has (§3's `NdsStaticValidation` + Tier 3 fallback), not
a speculative future one — the missing piece is narrow (the
extract→recompile→register pipeline in §4), and MKDS's overlay set is
small and fully known (4 overlays, one pilot with no address collision).
But until that pipeline is built, assume WFC/DWC networking will run
interpreted, and budget for the specific failure mode above: if online
play is flaky or times out in ways that don't reproduce against a
deterministic oracle, check whether the failing code path is
overlay-resident (Tier 3) before assuming a protocol or decode bug —
Tier-3 slowdown against a real-time network peer is a distinct root
cause from a state-divergence bug, and the existing debugging playbook
(oracle-diff first divergence) does not catch it, because there is
nothing to diverge from — the guest state is correct, it's just late.

## What I could not determine / explicitly flagged as unconfirmed

- **Overlay discovery in psxrecomp** (how entry points and byte ranges
  are found in the first place — disc metadata, RAM-write observation,
  or both) was not traced to file:line rigor in this pass; §2 states this
  explicitly rather than asserting an answer.
- **Whether any shipped psxrecomp game project actually has
  `overlay_autocompile_cmd` (or equivalent) configured and exercised
  end-to-end in production** was not confirmed — the mechanism's code is
  real and wired, but this note does not claim a specific title
  currently relies on it live.
- **Whether MKDS's ARM7 ever fetches from the ARM9 overlay address range
  through a shared-WRAM aliasing path** was asserted false in §5 based on
  the address ranges in `overlays.json` alone; this should be verified
  against the actual WRAM bank-mapping configuration MKDS uses
  (`WRAMCNT`) before being treated as settled, not assumed from the
  static address list alone.
