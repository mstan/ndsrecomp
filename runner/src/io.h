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
// Opt-in host-clock RTC (--rtc-host): when true, every guest boot
// (nds_io_reset) starts the RTC at the host's local time instead of the
// fixed deterministic power-on datetime. Default false — the oracle gates
// (G1/G3) and all parity work require the deterministic clock.
extern bool g_nds_rtc_host;
void     nds_set_touch(uint16_t x, uint16_t y, bool down);
void     nds_set_key_mask(uint32_t mask);
uint64_t nds_next_system_event_time();
uint64_t nds_next_timer_overflow_time();
uint64_t nds_debug_spi_deadline();
uint64_t nds_debug_card_deadline();
void     nds_run_system_events(uint64_t timestamp);

struct NdsEventCounts {
    uint64_t vblank9;
    uint64_t vblank7;
    uint64_t ipcsync_w;
    uint64_t fifo9to7;
    uint64_t fifo7to9;
    uint64_t dma_done;
    uint64_t timer_ovf;
    uint64_t spi_w;
    uint64_t irq9;
    uint64_t irq7;
    // SOUNDBIAS (0x04000504) write stream — an invariant for the ARM7 boot
    // sound-bias ramp (a side-effect loop: each step writes the register).
    // Compared per-iteration against the oracle, NOT normalized away.
    uint64_t soundbias_w;       // total writes
    uint32_t soundbias_first;   // first value written
    uint32_t soundbias_last;    // most recent value written
};

const NdsEventCounts& nds_event_counts();
uint64_t nds_event_value(const char* name);

// Per-CPU retired-instruction counters ([0]=ARM9, [1]=ARM7; always-on,
// zeroed at reset). Used as run_to_event anchors ("insn9"/"insn7") to
// bisect — by instruction index from reset — the first ARM9/ARM7
// instruction whose PC diverges from the melonDS oracle. The ARM7 BIOS
// boot is deterministic and (early) independent of the ARM9, so an
// insn7-indexed PC compare localizes an ARM7-internal boot divergence that
// ipcsync_w (an ARM9-driven anchor) cannot. See DEBUG.md "fp-stream
// microscope". Flat C globals (not NdsEventCounts fields) because the
// generated per-instruction prologue bumps them inline — see runtime_arm.h
// "Inline retired-instruction counters".
extern "C" uint64_t g_insn_count[2];
// Nonzero while any per-insn payload is armed (the deep-trace register
// ring or an event break); generated code then calls runtime_insn_slow()
// after its inline bump. Recomputed by nds_insn_hook_recompute() whenever
// either input changes.
extern "C" uint32_t g_insn_hook_armed;
void nds_insn_hook_recompute();

// Always-on, non-perturbing trace of the most recent ARM7 SPIDATA writes.
// `count` is the absolute spi_w ordinal from reset, allowing native/oracle
// samples to be aligned after stopping on a later hardware event.
struct NdsSpiTraceEntry {
    uint64_t count;
    uint64_t sys;
    uint64_t cyc9;
    uint64_t cyc7;
    uint64_t insn7;
    uint32_t pc;
    uint32_t value;
};
bool nds_spi_trace_get(uint64_t count, NdsSpiTraceEntry* out);

struct NdsIrqTraceEntry {
    uint64_t count;
    uint64_t sys;
    uint64_t cyc9;
    uint64_t cyc7;
    uint64_t insn;
    uint32_t return_address;
    uint32_t pending;
    uint16_t wifi_if;
    uint16_t wifi_ie;
};
void nds_note_irq_accept(int cpu, uint32_t return_address);
bool nds_irq_trace_get(int cpu, uint64_t count, NdsIrqTraceEntry* out);

// Always-on trace of DMA channel completions (every completion, IRQ-raising
// or not — unlike NdsEventCounts::dma_done, which mirrors the oracle's
// SetIRQ-hook counting). `count` is the absolute completion ordinal from
// reset; the latest ordinal is returned by nds_dma_trace_count().
struct NdsDmaTraceEntry {
    uint64_t count;
    uint64_t sys;
    uint64_t cyc;        // completing CPU's own cycle timestamp
    uint64_t insn;       // completing CPU's retired instructions
    uint32_t cnt;        // channel CNT at completion
    uint32_t src, dst;   // programmed base addresses
    uint8_t cpu;         // 0 = ARM9, 1 = ARM7
    uint8_t ch;
    uint8_t start_mode;  // runner encoding (ARM9 0..7; ARM7 0x10|mode)
};
uint64_t nds_dma_trace_count();
bool nds_dma_trace_get(uint64_t count, NdsDmaTraceEntry* out);

struct NdsInsnTraceEntry {
    uint64_t count;
    uint64_t sys;
    uint64_t cycles;
    uint32_t pc;
    uint32_t cpsr;
    uint32_t pending;
    uint32_t r[15];
};
bool nds_insn_trace_get(int cpu, uint64_t count, NdsInsnTraceEntry* out);

struct NdsFifoTraceEntry {
    uint64_t count;
    uint64_t sys;
    uint64_t cyc9;
    uint64_t cyc7;
    uint64_t insn9;
    uint64_t insn7;
    uint32_t value;
};
bool nds_fifo_trace_get(int source_cpu, uint64_t count, NdsFifoTraceEntry* out);

enum NdsCardTraceKind : uint8_t {
    NDS_CARD_TRACE_ROMCTRL = 0,
    NDS_CARD_TRACE_COMMAND = 1,
    NDS_CARD_TRACE_DATA_READY = 2,
    NDS_CARD_TRACE_COMPLETE = 3,
};

// Passive gamecard protocol observer. No accessor reads ROMDATA or advances a
// transfer; data words are copied from the already-prepared response buffer.
struct NdsCardTraceEntry {
    uint64_t seq;
    uint64_t sys;
    uint64_t cyc9;
    uint64_t cyc7;
    uint64_t insn9;
    uint64_t insn7;
    uint8_t kind;
    uint8_t owner; // 0 = ARM9, 1 = ARM7
    uint8_t command[8];
    uint32_t requested_romctrl;
    uint32_t romctrl;
    uint32_t auxspicnt;
    uint32_t transfer_pos;
    uint32_t transfer_len;
    uint32_t transfer_dir;
    uint32_t word;
    uint32_t command_mode_before;
    uint32_t command_mode_after;
    uint32_t data_mode_before;
    uint32_t data_mode_after;
    uint8_t start;
};

struct NdsCardDebugState {
    uint8_t present;
    uint8_t owner;
    uint8_t command[8];
    uint8_t game_code[4];
    uint32_t chip_id;
    uint32_t rom_size;
    uint32_t auxspicnt;
    uint32_t romctrl;
    uint32_t transfer_pos;
    uint32_t transfer_len;
    uint32_t transfer_dir;
    uint32_t command_mode;
    uint32_t data_mode;
    uint64_t produced;
    uint64_t oldest;
    uint32_t capacity;
};
void nds_card_debug_state(NdsCardDebugState* out);
uint32_t nds_card_debug_trace_copy(NdsCardTraceEntry* out,
                                   uint32_t max_entries);
// Bump the active CPU's retired-instruction counter (+break-check). Called
// once per retired guest instruction from BOTH the recompiled banks (via
// runtime_insn_fp, g_runtime_insn_trace on) and the Tier-3 interpreter.
void     nds_note_insn_retired(int cpu);   // cpu: 0 = ARM9, 1 = ARM7

// Set when an armed insn7/insn9 break trips; makes runtime_should_yield and
// the Tier-3 loop stop AT the target instruction (per-instruction precision).
extern bool g_nds_insn_stop;
uint32_t nds_io_debug_read(int cpu, uint32_t addr, uint32_t width);
// EXMEMCNT is split per CPU: ARM9 owns/shared bits 15..7 and its own low
// timing bits; ARM7 owns only its low timing bits.  The bus timing and GBA-slot
// open-bus paths query the architectural latch through this accessor.
uint16_t nds_exmemcnt(int cpu);
// ARM7 POWCNT2/WIFIWAITCNT control the 0x04800000 Wi-Fi bus wait states.
uint16_t nds_powercontrol7();
uint16_t nds_powercontrol9();
uint16_t nds_wifiwaitcnt();
uint8_t nds_wramcnt();
bool nds_powered_off();

// Symmetric RTC state checkpoint used by the reset/replay bisector.  This is
// architectural device state (not an armed trace), so querying it cannot
// perturb the serial transfer or either CPU timeline.
struct NdsRtcDebugState {
    uint16_t io;
    uint8_t status1;
    uint8_t status2;
    uint8_t datetime[7];
};
void nds_rtc_debug_state(NdsRtcDebugState* out);

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
// Level-style sources (GXFIFO) deassert their IF bit when the condition
// clears, matching melonDS NDS::ClearIRQ.
void     nds_clear_irq(int cpu, uint32_t bits);
uint32_t nds_irq_pending(int cpu);
// Exact IME&IE&IF value maintained at every interrupt-register mutation.
// runtime_tick may read this directly in its parity-safe common path; HALT
// wake rules must continue to use nds_halt_wake_pending instead.
extern uint32_t g_nds_irq_pending_cache[2];

// Guest CPU low-power state. HALTCNT/CP15 halt is cooperative rather than a
// terminal runner failure: the scheduler advances a sleeping CPU's timestamp
// without retiring instructions, then wakes it on the hardware's interrupt
// condition. ARM7 HALT wake observes IE&IF even with IME clear; actual IRQ
// exception entry still requires IME and CPSR.I clear.
bool     nds_cpu_halted(int cpu);
void     nds_cpu_enter_halt(int cpu);
bool     nds_halt_wake_pending(int cpu);
void     nds_cpu_wake(int cpu);
unsigned long long nds_halt_entry_cycle(int cpu);

// Four-channel DMA controller per CPU.  An enabled transfer stalls that CPU
// while the scheduler advances DMA bus units in place of guest instructions.
// The enabling instruction's cost remains pending across the stall, matching
// melonDS ARM::Halted==2 ordering.
bool     nds_dma_cpu_stalled(int cpu);
unsigned long long nds_dma_entry_cycle(int cpu);
void     nds_dma_run(int cpu, unsigned long long target_cycles);
void     nds_dma_trigger(int cpu, uint32_t start_mode);

// GXFIFO CPU-stall primitive (ARM9 only), modeled on the DMA stall: a write
// to a full geometry FIFO parks the ARM9 while the geometry engine drains
// the overflow queue, matching melonDS CPUStop_GXStall. The vendored GPU3D
// sets/clears the flag; the scheduler-side consumption lands with the
// geometry engine's Run() wiring (3D Phase 2).
void     nds_gxfifo_set_stall(bool stalled);
bool     nds_gxfifo_stalled();

// melonDS ordering: each CPU's timers advance at that CPU's own post-Execute
// timestamp; display/system time advances only after ARM7 catches up.
void     nds_tick_timers(int cpu, unsigned long long cpu_cycles);
void     nds_tick_display(unsigned long long system_cycles);
void     nds_tick_rtc(unsigned long long system_cycles);
void     nds_dump_irq();
void     nds_io_load_firmware(const uint8_t* p, uint32_t n);
// Installs user-provided cartridge data and derives KEY1 from the user's
// ARM7 BIOS at runtime. Neither input is compiled into native objects.
bool     nds_io_load_cartridge(const uint8_t* rom, uint32_t rom_size,
                               const uint8_t* arm7_bios, uint32_t bios_size);
