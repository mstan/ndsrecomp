# Runner bring-up (Phase 2)

The runner links the generated banks and runs them on a DS-shaped runtime.
This doc tracks the bring-up; the architecture follows
`docs/dispatch_architecture.md` (3-tier) and the DUAL-CPU / OBSERVABILITY
rules in CLAUDE.md.

## Layout

```
runner/
  src/state.h         shared C++ state (active CPU, CP15, dispatch decls)
  src/runtime_arm.cpp  recomp C ABI impl (cond/shift/flags/banking/call-
                       return reused verbatim from the verified single-CPU
                       runtime; per-CPU dispatch + DS exception vectors,
                       trace ring, cooperative yield/halt)
  src/bus.cpp          DS memory map + always-on bus-access ring
  src/cp15.cpp         ARM9 CP15 model (control reg, ITCM/DTCM, MPU regs)
  src/main.cpp         load+SHA-1-verify dumps, reset, run, report
  CMakeLists.txt       links generated/arm9_bios.c + dispatch + the above
```

Build/run:
```
cmake -G Ninja -B runner/build runner && cmake --build runner/build
./runner/build/nds_runner.exe bios <cycle-cap>
```

## Milestone 1 — ARM9 BIOS boots to the ARM7-wait (DONE)

Single CPU. `nds_runner` loads + SHA-1-verifies biosnds9/biosnds7/firmware
(refuses to start otherwise), maps the ARM9 BIOS, sets the reset state
(SVC, I+F masked, PC=0xFFFF0000), and dispatches.

**Result:** the recompiled ARM9 BIOS executes its full reset/init with
**zero unimplemented ops, zero dispatch misses, zero CP15-unmodeled
warnings**, and idles in the ARM7-wait. Observed sequence (from the bus
ring): CP15 setup → POSTFLG read → gamecard (AUXSPICNT/ROMCTRL) + EXMEMCNT
+ IME init → IPCSYNC write (0x0100, signaling ARM7) → IPCSYNC poll →
progresses into System/THUMB mode (CPSR=0x8000007F) with IRQs enabled →
idles. Final CP15: `control=0x00012078` (high vectors + DTCM enabled),
DTCM @ 0x00800000 / 16 KB. The stack (SP=0x00803EB8) lives in DTCM —
i.e. the CP15 TCM model is load-bearing and correct, since stack accesses
resolve through it.

This validates end-to-end, on real hardware code: the codegen output
(the whole BIOS reset runs), the runtime ABI, per-CPU dispatch, the CP15
model, and the bus/TCM — all with no gaps.

### What is stubbed (intentionally, until a later slice needs it)
- I/O registers (0x04000000–0x04FFFFFF): read 0, writes logged. The boot
  only needed IPCSYNC/IME/gamecard/EXMEM so far; real models land as the
  boot demands them.
- Timing: `runtime_mem_cycles`/`runtime_mul_cycles` are flat (1). DS
  waitstates are a later accuracy pass; not needed to boot.
- The ARM9 idles because there is no ARM7 yet — the next slice unblocks it.

## Milestone 2 — dual-CPU + ARM7, both cores running (DONE)

Both banks link (function symbols are bank-prefixed — `arm9_bios_*` /
`arm7_bios_*` — so the shared ndsbios names don't collide). The scheduler
(`scheduler.cpp`) interleaves the two cores: one `g_cpu` swapped between
per-CPU `ArmCpuState` slots, ARM9 ~2× the cycles/round. **Result:** 7434
rounds, **zero dispatch misses, zero unimplemented ops**. ARM9 boots to
its ARM7-wait idle (0xFFFF03AA, CP15+DTCM up); ARM7 boots through to a
firmware/SPI wait (~0x1206, stack in WRAM). Both then spin on each
other / on not-yet-modeled hardware (IPC, SPI) until the budget.

### Cooperative preemption model (the hard part)
A spin on one core must be preempted to run the other, but the generated
BL/BLX use the host C call stack, which is coupled to the guest return
stack — naively unwinding to switch CPUs loses pending returns. The model:

1. **Preempt only at backward branches** (loop tops), which the finder
   seeds as dispatch entries — so the resume PC is always re-dispatchable
   (`runtime_slice_yield()` checked there in codegen; per-instruction
   `runtime_should_yield()` is now terminal-halt only).
2. **Preserve the return stack on the preemption unwind**: when a
   slice-yield fires it arms `runtime_unwinding()`, and the BL/BLX
   return-checks return *without* cancelling their pushed return.
3. **Save/restore the call-return stack per-CPU** across the swap (a
   preempted spin may be deep in a call chain).
4. **Seed every call's return address as a dispatch entry** (finder):
   after a preemption the guest return is re-dispatched by the run loop
   (the host frame is gone), so the return address must be a real entry.
   ARM9 99→109 funcs, ARM7 583→594.

## Milestone 3 — IPC handshake + THUMB BLX, both cores boot deep (DONE)

The I/O register model (`io.cpp`) wires the cross-core handshake:
**IPCSYNC (0x04000180)** is cross-wired (CPU X's out-data bits 11..8 →
CPU Y's in-data bits 3..0), plus POSTFLG and IME/IE/IF. With it, the ARM9
reset wait clears and **both cores boot far past the IPC point**.

Two gaps surfaced and were fixed by execution:
- **THUMB BLX** (the `11101` suffix form, THUMB→ARM) — the ARM9 firmware
  boot uses it; the decoder had it as undefined. Added decode → IR
  (BL_suffix + `branch_exchange`) → codegen (word-align + exchange to ARM)
  → finder (seed the ARM-mode target). ARM9 undefined count → 0.
- **Split THUMB BL/BLX pairs:** a function could end between a BL prefix
  and its suffix, orphaning the suffix; the finder now extends a function
  to never end mid-pair.

**Runner perf:** built `-O0` before (no build type) — now Release, and
the scheduler quantum raised to 2048/1024. ~600× faster: 100M cycles in
0.6 s. Iterable.

**State at 100M cycles** (zero misses, zero unimplemented):
- ARM9 idle at `0xFFFF03E4`, polling IPCSYNC input — **waiting for the
  ARM7 to send IPCSYNC data = 3**.
- ARM7 idle at `0x18A4`, polling **ROMCTRL (0x040001A4) bit 23**
  (gamecard data-ready) after starting a transfer.

## Milestone 4 — peripherals + robust preemption, cores reach the IRQ wait (DONE)

- **ROMCTRL (0x040001A4)** modeled (gamecard: start → data-ready bit 23,
  empty-slot data 0xFFFFFFFF) — the ARM7 gamecard probe completes.
- The ARM7 then bit-bangs the **RTC (0x04000138)** and advances to a HALT
  loop.
- **Indirect-loop preemption:** a loop whose back-edge is an indirect
  transfer (BX / pop pc / computed jump) has no backward-branch yield
  site, so `run_slice` could spin forever inside one `runtime_dispatch`.
  Fixed by also yielding at `runtime_dispatch` entry (the target is always
  a dispatch entry, so resume is clean). 100M cycles now run in ~1.3 s.

**State now** (zero misses): ARM9 still waits for IPCSYNC=3; **ARM7 is in
a HALT loop** — `mov r14,#0x80 ; strb r14,[r12,#0x301]` writes **HALTCNT
(0x04000301)** then loops checking a condition. It is waiting for an
interrupt.

## Milestone 5 — interrupt controller + IRQ delivery: full BIOS boot (DONE)

The interrupt subsystem (`io.cpp`): IE/IF/IME, `nds_raise_irq` /
`nds_irq_pending`; **IRQ delivery in `runtime_tick`** (when `IME & IE & IF`
and CPSR.I clear → `runtime_irq` vectors to `exc_base+0x18`). Sources, all
driven from the global clock via `nds_tick_hw` (called per scheduler
round): **VBlank** (IF bit 0), and **timers** (4 per CPU, prescaler +
cascade, overflow → IF bit 3+N). Plus **HALTCNT** and the **gamecard
transfer-complete IRQ** (bit 19, raised when an empty-slot block transfer
starts). The IRQ vectors (`0x18` / `0xFFFF0018`) and the user IRQ handlers
(reached via the BIOS's computed `[0x0380FFFC]` pointer, unfollowable
statically) were seeded through the **dispatch-miss loop** into the TOMLs.

**Result: the dual-CPU BIOS boot completes.** The ARM7 services its
timer/gamecard IRQs, advances, and drives the IPCSYNC handshake to
completion; the ARM9 unblocks from its `cmp #3` wait. Final state (zero
dispatch misses through the BIOS): **both `IPCSYNC.out = 0x0300`**, ARM9
`R0 = 0x301`. Both cores then jump to the **firmware entry** — ARM7 →
`0x03810000`, ARM9 → `0x00000000` — which are garbage *because the
firmware boot parts are not loaded yet* (no SPI). That jump is the
BIOS→firmware handoff: the BIOS boot is done.

## Milestone 6 — SPI + LLE firmware boot: firmware DECRYPTED into RAM (DONE)

The firmware **SPI flash** device (`io.cpp`) — SPICNT/SPIDATA, READ
(0x03 + 24-bit addr) + RDSR — serves `bios/firmware.bin`. **The recompiled
ARM7 BIOS does the full real LLE firmware boot**: reads the header (flash
`0x0`), reads the encrypted ARM9 boot stream (`0x180`…), **KEY1-decrypts
it** (the BIOS `EncryptionKeys`/`Decrypt_64bit` code runs correctly), and
copies the decrypted stage-1 loader to RAM `0x0380F800`, then jumps to it.

**Verified:** the bytes at `0x0380F800` are `E28F0001 E12FFF10` =
ARM `add r0,pc,#1 ; bx r0` (the canonical ARM→THUMB trampoline) — valid
code, and **completely different from the raw encrypted flash**
(`ada65066…`). So SPI + decrypt + copy all work end to end. The dispatch
miss at `0x0380F800` is the **Tier-3 dirty-RAM boundary**: the firmware
loader is RAM-resident copied code and needs the interpreter to run it
(PRINCIPLES.md "the one exception").

## Milestone 7 — Tier-3 dirty-RAM interpreter: firmware loader RUNS (DONE)

`recompiler/armv4t/interpreter.cpp` is linked into the runner (`tier3.cpp`):
on a dispatch miss whose target is RAM (0x02000000-0x04000000, copied
firmware code), the runner runs the guest's **own bytes** through the
reference interpreter instead of aborting (PRINCIPLES.md "the one
exception"). Glue: a `Bus` adapter → the C bus ABI; `sync_in`/`sync_out`
between `g_cpu` (packed CPSR) and `armv4t::CPUState` (banked tables share
layout); a fetch→decode→`step` loop that hands back to Tier-1 when the PC
reaches a **bank** entry, with SWI (`enter_swi`) and IRQ (`enter_irq`)
vectored to the BIOS handler banks, and a clean slice-yield (all state is
in the struct — no host-stack coupling, so Tier-3 preempts trivially).

**Result:** the firmware stage-1 loader **runs** in Tier-3 — it executes,
calls BIOS functions (Tier-1 banks) for memset/copy, returns, all
cross-tier transitions working. A divergence surfaced and was fixed:
**KEYINPUT (0x04000130) was stubbed 0 (= all buttons held)**, so the
firmware took its GBA-boot path (jumping to the GBA cart entry 0x080000C0);
modeling it as `0x3FF` (nothing pressed) + EXTKEYIN released fixed it. The
ARM7 now progresses through the firmware load.

## Milestone 8 — cross-tier call-return fix + I/O backing store; blockers localized (IN PROGRESS)

Four changes pushed the ARM7 deep into hardware-init and pinned down both
cores' precise blockers:

1. **Tier-3 call-return balance (the big fix).** A bank that `bl`s into
   Tier-3 (through a `bx rN` veneer) pushes a host call-return entry, but
   the matching guest `bx lr` runs *inside the interpreter*, which never
   popped it → the stack leaked and overflowed (1024) → ARM7 halted
   "call-return overflow" at `0x3300`. `tier3.cpp` now mirrors the banks:
   push on `is_call` (excluding `BL_prefix`), `runtime_call_should_return`
   on a `Branched` non-call (pops iff it matches). This alone advanced the
   ARM7 by orders of magnitude.
2. **SWI vector seed.** `0x08` (`arm`, `swi_vector_entry`) added to
   `biosnds7.toml` — the first ARM7 firmware SWI vectored to `0x08`, which
   wasn't a dispatch entry → miss.
3. **IPC FIFO** in `io.cpp`: `0x04000184` (IPCFIFOCNT) / `0x04000188`
   (SEND) / `0x04100000` (RECV) — send/recv/count + empty/full status +
   IRQ bits 17/18. (Correct + needed, though not the actual blocker.)
4. **I/O backing store (SOUNDBIAS fix + completeness).** Unmodeled regs in
   `0x04000000..0x04002000` now store-and-return via `g_io_mem[]` (the
   default read/write cases; special-cased regs override; the gap warning
   stays). Root cause: the ARM7 BIOS *ramps* SOUNDBIAS (`0x04000504`)
   `0→0x200` via read/inc/write+delay and spins forever if reads don't
   reflect writes (it was a stub returning 0). Generalized to all config
   latches (POWCNT / DISPCNT / WRAMCNT / sound channels).

**Both blockers are now precisely localized — the melonDS oracle (task #7)
is the right next tool (the earliest divergence has the root cause):**

- **ARM7**: past SOUNDBIAS, it now spins in an **interrupt-poll wait**
  (`0x2EC4-0x2F08`): read `IE & IF` (`0x04000210/214`), dispatch on bit 19
  (gamecard) / 6 (Timer3) / 0 (VBlank → default delay), else delay and
  re-check. **`IE = 0` in this loop**, so `IE & IF` is always 0 and it
  never advances. The real ARM7 must have IE set here → an **upstream
  divergence** (mine reached this loop via a wrong path or skipped an IE
  setup). It never reaches the firmware-part load — no writes to
  `0x021F0000` / `0x0380CC00`, only zeroing main RAM + filling the no-cart
  header (`0x027FFE00..` ← `0xFFFFFFFF`).
- **ARM9**: waits `IPCSYNC` in-data `== 3` (`0xFFFF03E4`, `r4 = 0x04000180`),
  then reads its boot params from RAM and runs a BIOS bit-unpack
  **decompress** (`0xFFFF0A00`) into ITCM (`0x0`). The params are 0 (the
  ARM7 hasn't produced them), so it decompresses garbage, a function
  pointer in the result is `0`, and it does `blx 0` → dispatch miss at
  `0x00000000`. (ITCM being disabled here is expected — the firmware
  enables it later; the empty params are the issue.)

**Next: build the melonDS oracle (task #7).** Run the same BIOS + firmware
in melonDS (GPLv3, **tool-use only** — run + diff, never copy its source),
dump per-step ARM7/ARM9 PC + registers + IE/IF, and diff against the
runner's trace ring to find the **first** divergence (likely where the real
ARM7 sets IE or takes a different hardware-init path). Then resume the
wait-loop / firmware-load bring-up against ground truth. Resync at every
cross-CPU boundary (DUAL-CPU RULE).

### SPI device details (`io.cpp`)

SPICNT (0x040001C0, chipselect-hold managed, busy always clear) + SPIDATA
(0x040001C2), serving `bios/firmware.bin` for device-select=1. State
machine: **READ** (0x03 + 24-bit address → streams flash bytes) and
**RDSR** (0x05 → ready). The runner loads the firmware via
`nds_io_load_firmware`. The ARM7 reads in PIO (no DMA), via the SPI
routine at BIOS PC `0x2368-0x2384`. How the boot's `0x0380F800` dest is
computed: a copy descriptor at `0x0380FE58` {flash 0x1A0, dest 0x0380F800,
…}. Debugged entirely from the always-on trace ring + targeted temp
instrumentation (all removed; the dispatch-miss now dumps RAM target bytes
to expose the Tier-3 case).
