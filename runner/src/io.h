// io.h — DS I/O register model (0x04000000 region), per-CPU.
//
// Started with the registers the BIOS handshake polls: IPCSYNC (shared,
// cross-wired between the cores), POSTFLG, and the interrupt registers
// (IME/IE/IF). Everything else logs once and reads 0. Grown as the boot
// demands more (IPC FIFO, SPI, timers, DMA, the 2D engines).

#pragma once

#include <cstdint>

uint32_t nds_io_read(uint32_t addr, uint32_t width);
void     nds_io_write(uint32_t addr, uint32_t value, uint32_t width);
void     nds_io_reset();

struct NdsEventCounts {
    uint64_t vblank9;
    uint64_t vblank7;
    uint64_t ipcsync_w;
    uint64_t fifo9to7;
    uint64_t fifo7to9;
    uint64_t dma_done;
    uint64_t timer_ovf;
    // SOUNDBIAS (0x04000504) write stream — an invariant for the ARM7 boot
    // sound-bias ramp (a side-effect loop: each step writes the register).
    // Compared per-iteration against the oracle, NOT normalized away.
    uint64_t soundbias_w;       // total writes
    uint32_t soundbias_first;   // first value written
    uint32_t soundbias_last;    // most recent value written
    // Per-CPU retired-instruction counters (always-on, zeroed at reset). Used
    // as run_to_event anchors ("insn9"/"insn7") to bisect — by instruction
    // index from reset — the first ARM9/ARM7 instruction whose PC diverges
    // from the melonDS oracle. The ARM7 BIOS boot is deterministic and (early)
    // independent of the ARM9, so an insn7-indexed PC compare localizes an
    // ARM7-internal boot divergence that ipcsync_w (an ARM9-driven anchor)
    // cannot. See DEBUG.md "fp-stream microscope".
    uint64_t insn9;
    uint64_t insn7;
};

const NdsEventCounts& nds_event_counts();
uint64_t nds_event_value(const char* name);
// Bump the active CPU's retired-instruction counter (+break-check). Called
// once per retired guest instruction from BOTH the recompiled banks (via
// runtime_insn_fp, g_runtime_insn_trace on) and the Tier-3 interpreter.
void     nds_note_insn_retired(int cpu);   // cpu: 0 = ARM9, 1 = ARM7

// Set when an armed insn7/insn9 break trips; makes runtime_should_yield and
// the Tier-3 loop stop AT the target instruction (per-instruction precision).
extern bool g_nds_insn_stop;
uint32_t nds_io_debug_read(int cpu, uint32_t addr, uint32_t width);

// Sub-event break, symmetric with the oracle's. When armed, the counter-bump
// path sets a flag the moment the named event reaches `target`; the scheduler
// slice checks nds_event_break_hit() after each dispatched block and yields, so
// run_to_event stops AT the Nth event instead of overshooting a whole round
// (~2048 ARM9 / 1024 ARM7 cyc). Lets native and oracle anchor on the SAME guest
// instruction for a clean state diff. Disarmed = the flag is always false.
void     nds_event_break_arm(const char* name, uint64_t target);
void     nds_event_break_disarm();
bool     nds_event_break_hit();

// Interrupt controller. Sources raise IF bits via nds_raise_irq; the
// runtime polls nds_irq_pending(cpu) each tick and vectors when set and
// CPSR.I is clear. cpu: 0 = ARM9, 1 = ARM7. Returns IE&IF when IME enabled.
void     nds_raise_irq(int cpu, uint32_t bits);
uint32_t nds_irq_pending(int cpu);

// Display + timer clocks, advanced from the scheduler with the global
// (ARM9) cycle count; they raise VBlank / timer-overflow IRQs.
void     nds_tick_hw(unsigned long long global_cycles);
void     nds_dump_irq();
void     nds_io_load_firmware(const uint8_t* p, uint32_t n);
