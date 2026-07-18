// runtime_arm.h — C-ABI surface that generated recompiled cart code
// calls into.
//
// Generated code is .cpp but uses C linkage on these symbols so the
// recompiler doesn't have to worry about C++ name mangling. The
// implementations live in runtime_arm.cpp and delegate to the
// existing C++ runtime (armv4t::CPUState, gba::GbaBus, etc.).
//
// ABI principle: the symbols declared here are the ONLY interface
// between gba_recompile's generated output and the runtime. If new
// codegen needs an operation, declare a helper here and implement
// once — never inline runtime-internal types into generated code.

#pragma once

#include <stdint.h>
#include <stdbool.h>  // generated banks are C; `bool` returns need this
#include <string.h>   // the inline bus fast path does aligned-safe memcpy

#ifdef __cplusplus
extern "C" {
#endif

// ── CPU state ──────────────────────────────────────────────────────
// Generated code reads/writes the register file via this global.
// The CPSR is packed:
//
//   bit 31 N
//   bit 30 Z
//   bit 29 C
//   bit 28 V
//   bit  7 I (IRQ disable)
//   bit  6 F (FIQ disable)
//   bit  5 T (THUMB state)
//   bits 4..0 mode
//
// Banked registers + mode switching live in the C++ runtime; rare
// events route through runtime_msr / runtime_mode_switch.

// Bank indexes match armv4t::BankedSlot — User=0, FIQ=1, IRQ=2,
// Supervisor=3, Abort=4, Undefined=5. The recomp ABI exposes these
// as a 6-wide array so generated code (and runtime helpers) can
// index by mode without dragging in the C++ enum.
#define ARM_BANK_USER       0u
#define ARM_BANK_FIQ        1u
#define ARM_BANK_IRQ        2u
#define ARM_BANK_SUPERVISOR 3u
#define ARM_BANK_ABORT      4u
#define ARM_BANK_UNDEFINED  5u
#define ARM_BANK_COUNT      6u

typedef struct ArmCpuState {
    uint32_t R[16];
    uint32_t cpsr;

    // Banked storage. SPSR for User/System is undefined and unused.
    uint32_t banked_sp[ARM_BANK_COUNT];
    uint32_t banked_lr[ARM_BANK_COUNT];
    uint32_t banked_spsr[ARM_BANK_COUNT];

    // R8..R12 have a parallel bank for FIQ. The active values live
    // in R[8..12]; the inactive bank is mirrored here.
    uint32_t r8_12_user[5];
    uint32_t r8_12_fiq[5];
} ArmCpuState;

extern ArmCpuState g_cpu;

// ── Active CPU selector (DS dual-CPU only) ─────────────────────────
// Which core is currently executing. 0 = ARM9, 1 = ARM7. Declared here
// (the ABI boundary — see the file header) rather than in a runner-only
// header, because generated per-CPU banks include ONLY this header and
// must be able to select the ARM9 cycle combine (arm9_cycle_combine,
// below) at codegen-emitted tick sites via `g_nds_active == NDS_ARM9`.
// runner/src/state.h includes this header for the C++-side runtime
// rather than redeclaring the enum, so there is exactly one definition.
typedef enum NdsCpu { NDS_ARM9 = 0, NDS_ARM7 = 1 } NdsCpu;
#ifdef NDS_STATIC_CPU
// A generated bank belongs permanently to one DS CPU. Per-source build
// specialization folds the hot ARM9/ARM7 timing ternaries at compile time;
// runner/device code still observes the live scheduler-owned selector.
#define g_nds_active ((NdsCpu)(NDS_STATIC_CPU))
#else
extern NdsCpu g_nds_active;
#endif

// Nintendo DS runner dispatch ABI.  Runtime-materialized firmware can reuse
// the same virtual address for different code generations, so generated rows
// may carry an exact byte validation for the function they enter.  Immutable
// BIOS/game banks leave validation null.
typedef struct NdsStaticValidation {
    uint32_t addr;
    uint32_t size;
    const uint8_t* expected;
} NdsStaticValidation;

typedef struct NdsDispatchEntry {
    uint32_t addr;
    uint8_t thumb;
    void (*fn)(void);
    const NdsStaticValidation* validation;
} NdsDispatchEntry;

// Candidate-only, compile-time-gated observation ABI for modular performance
// HLE work. Generated LLE bodies call none of this unless
// NDS_PROFILE_HLE_HEAT is defined for both the bank and runner.
typedef struct NdsHleProfileDescriptor {
    const char* id;
    const char* bank;
    uint32_t address;
    uint32_t end_address;
    uint8_t thumb;
    uint32_t instruction_count;
    const char* content_sha1;
    const NdsStaticValidation* validation;
} NdsHleProfileDescriptor;

typedef struct NdsHleProfileToken {
    unsigned long long host_ns;
    unsigned long long instructions;
    unsigned long long cycles;
    uint32_t irq_epoch;
    uint32_t active;
    uint32_t depth;
    uint32_t sampled;
} NdsHleProfileToken;

void nds_register_hle_profile_descriptors(
    int cpu, const NdsHleProfileDescriptor* const* descriptors,
    unsigned count);
NdsHleProfileToken runtime_hle_profile_begin(
    const NdsHleProfileDescriptor* descriptor);
void runtime_hle_profile_end(const NdsHleProfileDescriptor* descriptor,
                             NdsHleProfileToken token);

// Convenience accessors. CSR-bit constants follow ARM ARM A2.5.
#define CPSR_N_BIT (1u << 31)
#define CPSR_Z_BIT (1u << 30)
#define CPSR_C_BIT (1u << 29)
#define CPSR_V_BIT (1u << 28)
#define CPSR_Q_BIT (1u << 27)
#define CPSR_I_BIT (1u <<  7)
#define CPSR_F_BIT (1u <<  6)
#define CPSR_T_BIT (1u <<  5)

static inline uint32_t cpsr_n(void) { return (g_cpu.cpsr & CPSR_N_BIT) ? 1u : 0u; }
static inline uint32_t cpsr_z(void) { return (g_cpu.cpsr & CPSR_Z_BIT) ? 1u : 0u; }
static inline uint32_t cpsr_c(void) { return (g_cpu.cpsr & CPSR_C_BIT) ? 1u : 0u; }
static inline uint32_t cpsr_v(void) { return (g_cpu.cpsr & CPSR_V_BIT) ? 1u : 0u; }

// ARM condition-code evaluators — true if the cond passes given
// the current CPSR. cond is the 4-bit code from the instruction
// encoding. AL (1110) is unconditional (true); NV (1111) is "never"
// on ARMv4T.
int arm_cond_passes(unsigned cond);

// ── Bus ────────────────────────────────────────────────────────────
// All guest data accesses flow through the static-inline bus_read_* /
// bus_write_* pair below (B3 bus fast path). The slow functions are the
// complete reference model (region resolve, always-on access ring, I/O,
// video, unmapped diagnostics) in runner/src/bus.cpp. The inline layer
// serves main RAM / shared+ARM7 WRAM / ARM9 TCM directly from the
// exported fast-map windows — but ONLY while the deep-trace policy is
// off (g_runtime_deep_trace == 0, i.e. the interactive frontend, which
// exposes no query surface for per-access ring payloads; see the policy
// comment further down). Every mode with a query surface (--serve,
// batch) keeps g_runtime_deep_trace on and takes the slow path for every
// access, so the bus ring still records everything there.
//
// Fast-path invariants (must hold or the window must fall back):
//   * acceptance ⊆ resolve(): a fast hit returns exactly the bytes the
//     slow path would (same mirror math, same bounds; unaligned edge
//     cases that resolve() rejects fall back to the slow path);
//   * writes preserve Tier-3 provenance byte-for-byte: written[] bytes,
//     per-4KiB-page generation bump (0 skipped), and the static-guard
//     invalidation via runtime_note_code_write();
//   * windows are refreshed by bus_fast_refresh() whenever the mapping
//     inputs change (WRAMCNT, CP15 TCM config, bus_init).

uint32_t bus_read_u32_slow(uint32_t addr);
uint16_t bus_read_u16_slow(uint32_t addr);
uint8_t  bus_read_u8_slow (uint32_t addr);
void     bus_write_u32_slow(uint32_t addr, uint32_t val);
void     bus_write_u16_slow(uint32_t addr, uint16_t val);
void     bus_write_u8_slow (uint32_t addr, uint8_t  val);

// Called after any writable backing generation changes (every RAM write).
// The static runtime uses this to invalidate the currently executing
// provenance-validated bank without polling page vectors per instruction.
void runtime_note_code_write(void);

// Deep-trace policy flag — full declaration/comment further down; the
// inline fast path needs it in scope here.
extern uint32_t g_runtime_deep_trace;

// One fast-map window: a directly addressable, mirrored, power-of-two
// backing with its parallel Tier-3 provenance arrays. data == NULL means
// the window is not fast-servable right now (e.g. ARM9 shared-WRAM view
// with WRAMCNT mode 3) and accesses take the slow path.
typedef struct NdsBusFastWin {
    uint8_t*  data;     // backing bytes (window base already applied)
    uint8_t*  written;  // per-byte write-provenance flags (parallel)
    uint32_t* gen;      // per-4KiB-page provenance generations (parallel)
    uint32_t  mask;     // mirror mask: off = addr & mask (window-relative)
} NdsBusFastWin;

extern NdsBusFastWin g_busf_main;        // 0x02000000..0x02FFFFFF, both CPUs
extern NdsBusFastWin g_busf_wram_lo[2];  // 0x03000000..0x037FFFFF, per CPU
extern NdsBusFastWin g_busf_wram_hi[2];  // 0x03800000..0x03FFFFFF, per CPU
extern NdsBusFastWin g_busf_itcm;        // ARM9 ITCM (guarded by the limit)
extern NdsBusFastWin g_busf_dtcm;        // ARM9 DTCM (non-mirrored window)
extern uint32_t g_busf_itcm_limit;       // virtual span; 0 = disabled
extern uint32_t g_busf_dtcm_base;
extern uint32_t g_busf_dtcm_limit;       // min(virtual size, 16 KiB); 0 = off

// Classify an address into a fast window, mirroring resolve()'s region
// order exactly (ARM9: ITCM, then DTCM, then the shared map). Returns
// NULL when the address is not fast-servable. NDS_STATIC_CPU folds the
// per-CPU branches at compile time inside generated banks.
static inline const NdsBusFastWin* nds_busf_classify(uint32_t addr,
                                                     uint32_t* off) {
    if (g_nds_active == NDS_ARM9) {
        if (addr < g_busf_itcm_limit) {
            *off = addr & g_busf_itcm.mask;
            return &g_busf_itcm;
        }
        {
            const uint32_t d = addr - g_busf_dtcm_base;
            if (d < g_busf_dtcm_limit) { *off = d; return &g_busf_dtcm; }
        }
    }
    if (addr - 0x02000000u < 0x01000000u) {
        *off = addr & g_busf_main.mask;
        return &g_busf_main;
    }
    if (addr - 0x03000000u < 0x00800000u) {
        const NdsBusFastWin* w = &g_busf_wram_lo[g_nds_active];
        *off = addr & w->mask;
        return w;
    }
    if (addr - 0x03800000u < 0x00800000u) {
        const NdsBusFastWin* w = &g_busf_wram_hi[g_nds_active];
        *off = addr & w->mask;
        return w;
    }
    return 0;
}

// Write-side Tier-3 provenance, identical to the slow path's
// note_ram_write: bump every touched page's generation (skipping 0),
// invalidate the static guard, mark the written bytes.
static inline void nds_busf_note_write(const NdsBusFastWin* w, uint32_t off,
                                       uint32_t width) {
    const uint32_t first = off >> 12u;
    const uint32_t last = (off + width - 1u) >> 12u;
    uint32_t page;
    for (page = first; page <= last; ++page) {
        const uint32_t g = w->gen[page] + 1u;
        w->gen[page] = g ? g : 1u;
    }
    runtime_note_code_write();
    {
        uint32_t i;
        for (i = 0; i < width; ++i) w->written[off + i] = 1u;
    }
}

static inline uint32_t bus_read_u32(uint32_t addr) {
    if (!g_runtime_deep_trace) {
        uint32_t off;
        const NdsBusFastWin* w = nds_busf_classify(addr, &off);
        if (w && w->data && off + 4u <= w->mask + 1u) {
            uint32_t v;
            memcpy(&v, w->data + off, 4);
            return v;
        }
    }
    return bus_read_u32_slow(addr);
}

static inline uint16_t bus_read_u16(uint32_t addr) {
    if (!g_runtime_deep_trace) {
        uint32_t off;
        const NdsBusFastWin* w = nds_busf_classify(addr, &off);
        if (w && w->data && off + 2u <= w->mask + 1u) {
            uint16_t v;
            memcpy(&v, w->data + off, 2);
            return v;
        }
    }
    return bus_read_u16_slow(addr);
}

static inline uint8_t bus_read_u8(uint32_t addr) {
    if (!g_runtime_deep_trace) {
        uint32_t off;
        const NdsBusFastWin* w = nds_busf_classify(addr, &off);
        if (w && w->data) return w->data[off];
    }
    return bus_read_u8_slow(addr);
}

static inline void bus_write_u32(uint32_t addr, uint32_t val) {
    if (!g_runtime_deep_trace) {
        uint32_t off;
        const NdsBusFastWin* w = nds_busf_classify(addr, &off);
        if (w && w->data && off + 4u <= w->mask + 1u) {
            memcpy(w->data + off, &val, 4);
            nds_busf_note_write(w, off, 4u);
            return;
        }
    }
    bus_write_u32_slow(addr, val);
}

static inline void bus_write_u16(uint32_t addr, uint16_t val) {
    if (!g_runtime_deep_trace) {
        uint32_t off;
        const NdsBusFastWin* w = nds_busf_classify(addr, &off);
        if (w && w->data && off + 2u <= w->mask + 1u) {
            memcpy(w->data + off, &val, 2);
            nds_busf_note_write(w, off, 2u);
            return;
        }
    }
    bus_write_u16_slow(addr, val);
}

static inline void bus_write_u8(uint32_t addr, uint8_t val) {
    if (!g_runtime_deep_trace) {
        uint32_t off;
        const NdsBusFastWin* w = nds_busf_classify(addr, &off);
        if (w && w->data) {
            w->data[off] = val;
            nds_busf_note_write(w, off, 1u);
            return;
        }
    }
    bus_write_u8_slow(addr, val);
}

// ── Per-instruction cycle cost (memory + multiply) ─────────────────
// Generated code computes the fixed part of an instruction's cost
// statically (instr_cycle_base + shift/PC-write surcharges) and adds
// these runtime-dependent parts as it executes, then ticks the total
// once at the instruction boundary. `runtime_mem_cycles` returns the
// N/S access cost for the active bus region; `runtime_mul_cycles`
// returns the ARM7TDMI multiply operand wait. Both mirror the IR
// interpreter (the timing oracle) exactly. `width` is 1/2/4 bytes;
// `sequential`/`signed_variant` are 0/1 flags.
uint32_t runtime_mem_cycles(uint32_t addr, uint32_t width,
                            uint32_t sequential);
uint32_t runtime_mul_cycles(uint32_t rs_value, uint32_t signed_variant,
                            uint32_t extra);

// Code-FETCH memory timing for the instruction at `pc` on the active CPU, in
// that CPU's cycle units (ARM9 = 2x system). Charged per retired instruction on
// TOP of the exec/data cost (which is what runtime_tick already accounts for),
// from the always-on per-instruction path (runtime_insn_fp for banks, the Tier-3
// loop). Models the ARM9's region N/S bus timing + ITCM + CP15 I-cache; the ARM9
// has no sequential-fetch speedup when uncached, so the region's fetch cost is
// charged per instruction. ARM7 returns 0 for now (its timing lands separately).
// See docs/scheduler_design.md "Cycle-model design".
uint32_t runtime_code_cycles(uint32_t pc);

// Taken-branch / PC-write pipeline refill. `target` includes the destination
// ISA in bit 0 (Thumb when set); each helper returns its CPU's clock units.
uint32_t arm7_refill_cycles(uint32_t target);
uint32_t arm9_refill_cycles(uint32_t target);

// ARM9 per-instruction cycle COMBINE — melonDS's exact AddCycles_C /
// AddCycles_CI / AddCycles_CD / AddCycles_CDI model (ARM.h). Given this
// instruction's numC (this instruction's OWN code-fetch cost, i.e.
// runtime_code_cycles(pc) at ITS pc — not a branch target), numD (the sum
// of every runtime_mem_cycles() this instruction made — 0 for non-memory
// ops), numI (the static ARM9 internal-cycle count: +1 register-specified
// shift, flat 1/3 for the classic MUL family by the S bit, +2 MCR, +3
// MRC — computed at codegen time from the decoded instruction) and
// has_data (nonzero for LDR/STR/LDM/STM/LDRD/STRD/SWP/SWPB), returns:
//   has_data:  max(numC + numD - 6, max(numC, numD))   -- CD/CDI: loads
//              and stores add NO internal cycles; the first ~6 ARM9 cycles of
//              the data access overlap the code fetch already in flight.
//   !has_data: numI ? numC + numI : numC                -- CI, else C.
// A taken branch / any PC write additionally adds arm9_refill_cycles(target)
// for the pipeline refill — that term is NOT part of this function (it
// depends on the branch target, which codegen adds separately at the
// branch/PC-write tick sites); this returns only the instruction's own
// class cost. ARM9 only — every codegen tick site's ARM7 branch is the
// ORIGINAL flat _cyc expression, verbatim; this is never called for ARM7.
static inline uint32_t arm9_cycle_combine(uint32_t numC, uint32_t numD,
                                          uint32_t numI,
                                          uint32_t has_data) {
    if (has_data) {
        const uint32_t floor_v = numC > numD ? numC : numD;
        const uint32_t sum = numC + numD;
        const uint32_t overlap = sum > 6u ? sum - 6u : 0u;
        return overlap > floor_v ? overlap : floor_v;
    }
    return numI ? numC + numI : numC;
}
uint32_t arm7_cycle_combine(uint32_t flat_cycles, uint32_t numD,
                            uint32_t is_load, uint32_t has_internal);

// ── Shifter helpers ────────────────────────────────────────────────
// Generated code uses these for data-processing operand2 shifts and
// for register-shifted-by-register cases. They update the shifter
// carry-out into CPSR.C when set_carry != 0.

uint32_t arm_shift_lsl(uint32_t v, uint32_t n, int set_carry);
uint32_t arm_shift_lsr(uint32_t v, uint32_t n, int set_carry);
uint32_t arm_shift_asr(uint32_t v, uint32_t n, int set_carry);
uint32_t arm_shift_ror(uint32_t v, uint32_t n, int set_carry);

// Count-leading-zeros (ARMv5 CLZ). Returns 32 for a zero input.
// A helper (rather than inline __builtin_clz) keeps generated code
// portable across the host compilers that build the runtime.
uint32_t runtime_clz(uint32_t v);

// ── Flag updaters ──────────────────────────────────────────────────
// After an arithmetic/logical op with S=1, generated code calls one
// of these to set CPSR.NZ(CV). The C-out / V-out semantics follow
// ARM ARM A4.1 (data processing).

void arm_set_nz   (uint32_t result);
void arm_set_nzc_logic(uint32_t result, uint32_t shifter_carry);
void arm_set_nzcv_add(uint32_t a, uint32_t b, uint32_t result);
void arm_set_nzcv_adc(uint32_t a, uint32_t b, uint32_t c_in, uint32_t result);
void arm_set_nzcv_sub(uint32_t a, uint32_t b, uint32_t result);
void arm_set_nzcv_sbc(uint32_t a, uint32_t b, uint32_t c_in, uint32_t result);

// ── Dispatch ───────────────────────────────────────────────────────
// `target_pc` is a guest PC. Low bit indicates THUMB on BX/BLX. The
// dispatcher binary-searches the per-game dispatch table; if the
// target is found, the corresponding generated function is called.
// Misses route to runtime_dispatch_miss for logging + fallback.

void runtime_dispatch(uint32_t target_pc);
void runtime_discovery_note_static(uint32_t pc, uint32_t thumb);
void runtime_dispatch_with_exchange(uint32_t target_pc);
void runtime_dispatch_miss(uint32_t target_pc);

// Direct generated BL calls use the host C stack for speed and clarity.
// Return idioms (`bx lr`, `mov pc, lr`, `pop {..., pc}`) are only C
// returns when they match the top direct-call return address; otherwise
// they are real guest branches and must dispatch.
void runtime_call_push_return(uint32_t return_pc);
int  runtime_call_should_return(uint32_t target_pc);
void runtime_call_cancel_return(uint32_t return_pc);

// Save-state support: expose the host-side call-return stack so the
// snapshot orchestrator can serialize/restore it. The stack lives in
// runtime_arm.cpp (file-local); these accessors are the only sanctioned
// window. restore replaces the live stack wholesale (clamped to the
// stack capacity). Returned pointer is valid until the next push/pop.
uint32_t        runtime_call_stack_depth(void);
const uint32_t* runtime_call_stack_data(void);
void            runtime_call_stack_restore(const uint32_t* entries,
                                           uint32_t depth);

// melonDS normally commits ARM::Cycles at an instruction boundary, but HALT
// exits ARM::Execute before that commit and carries the final instruction's
// debt across sleep. The scheduler preserves this accumulator per CPU; Tier 3
// consumes it when its next interpreted instruction retires. Generated banks
// consume it from runtime_tick().
uint32_t runtime_deferred_cycles(void);
void     runtime_deferred_cycles_set(uint32_t cycles);
uint32_t runtime_deferred_cycles_take(void);

// Always-on structured execution trace. This records diagnostic state
// only; it never routes execution or substitutes for missing codegen.
#define RUNTIME_TRACE_DISPATCH  1u
#define RUNTIME_TRACE_EXCHANGE  2u
#define RUNTIME_TRACE_SWI       3u
#define RUNTIME_TRACE_MEM_WRITE 4u
#define RUNTIME_TRACE_BRANCH    5u
#define RUNTIME_TRACE_IRQ       6u
// RUNTIME_TRACE_CALL aux values:
//   1 push, 2 top-frame return, 3 no match, 4 cancel, 5 non-local return.
#define RUNTIME_TRACE_CALL      7u
#define RUNTIME_TRACE_MEM_READ  8u

typedef struct RuntimeTraceEntry {
    uint32_t seq;
    uint64_t cycles;  // cumulative guest cycles at the moment of this event
    uint32_t kind;
    uint32_t pc;
    uint32_t cpsr;
    uint32_t addr;
    uint32_t value;
    uint32_t aux;
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r4;
    uint32_t r5;
    uint32_t r12;
    uint32_t r13;
    uint32_t r14;
} RuntimeTraceEntry;

void runtime_trace_event(uint32_t kind, uint32_t pc, uint32_t addr,
                         uint32_t value, uint32_t aux);
void runtime_trace_reset(void);
void runtime_trace_dump_recent(uint32_t max_entries);

// Deep-trace policy: gates the per-access payloads (mem_r/mem_w trace events
// and the per-instruction register-image ring entry). Architectural event
// counters (insn9/insn7) always advance regardless. Default on; the
// interactive frontend turns it off because it exposes no query surface for
// those rings, while --serve and batch (exit tail dump) keep it on.
extern uint32_t g_runtime_deep_trace;
void runtime_set_deep_trace(uint32_t on);

// ── Per-instruction fingerprint ring (MC-HP-002 cycle-aligned diff) ──
// When g_runtime_insn_trace != 0 the generated per-instruction prologue calls
// runtime_insn_fp() to record the pre-execution architectural state of EVERY
// instruction: {cumulative cycles, pc, cpsr, R0..R15}. The ring is always-on
// once armed (armed at machine reset from GBARECOMP_INSN_TRACE, or via the TCP
// `insn_trace` command) and bounded by eviction; a targeted dump pulls the
// window of interest. This is the substrate for diffing the recomp against the
// bios_smoke interp oracle at identical cycle counts — the FIRST fingerprint
// that differs (pc or any register) localizes the first real divergence.
// Disarmed (default) it costs one not-taken branch per instruction.
typedef struct RuntimeFpEntry {
    unsigned long long cycles;  // cumulative guest cycles BEFORE this instruction
    uint32_t pc;
    uint32_t cpsr;
    uint32_t r[16];
} RuntimeFpEntry;

extern unsigned g_runtime_insn_trace;  // 0 = off (zero overhead)
void runtime_insn_fp(void);            // emit one fingerprint (armed-gated by caller)
void runtime_fp_reset(void);
uint32_t runtime_fp_count(void);

// ── Inline retired-instruction counters ─────────────────────────────
// The generated per-instruction prologue bumps g_insn_count[g_nds_active]
// directly (a single increment — NDS_STATIC_CPU folds the index at compile
// time) and calls runtime_insn_slow() only while g_insn_hook_armed is
// nonzero. The counters are the architectural insn9/insn7 event ordinals
// and always advance; the armed slow path carries the optional payloads
// (deep-trace register-image ring, event-break check, fp ring) and must
// NOT bump the counter. runtime_insn_fp() above remains the whole hook for
// banks generated before this scheme (it bumps the same counters through
// the runtime), so old- and new-emission banks each count exactly once.
extern uint64_t g_insn_count[2];   // [0]=ARM9, [1]=ARM7 retired-insn ordinals
extern uint32_t g_insn_hook_armed; // nonzero → per-insn runtime_insn_slow()
void runtime_insn_slow(void);      // armed-path payload; no counter bump
// Write the whole ring (oldest-first) as a compact binary file: a 16-byte
// header {u32 magic 'GFP1', u32 entry_size, u64 count} followed by `count`
// RuntimeFpEntry records. Returns the number of records written (0 on error).
uint32_t runtime_fp_save_file(const char* path);
uint32_t runtime_trace_copy_recent(RuntimeTraceEntry* out,
                                   uint32_t max_entries);
void runtime_tick(uint32_t cycles);
// Per-instruction unwind signal (terminal halt only — dispatch miss /
// unlowered op). Checked at the top of every emitted instruction.
bool runtime_should_yield(void);
// Cooperative slice-yield signal, checked ONLY at backward branches (loop
// tops, which are dispatch entries). Lets the scheduler preempt a guest
// spin to run the other core, with a clean re-dispatch on resume. Returns
// 0 in single-CPU / non-scheduled contexts. When it returns true it also
// arms runtime_unwinding() for the duration of the unwind.
bool runtime_slice_yield(void);
// True while the host stack is unwinding for a slice-yield (not a real
// guest return). The BL/BLX return-check uses this to PRESERVE the pending
// return-push instead of cancelling it — the call-return stack is saved/
// restored per-CPU across the preemption, so the guest return survives.
bool runtime_unwinding(void);
// Cumulative guest-cycle clock. Incremented by runtime_tick on EVERY tick
// (per-instruction exec ticks AND halt-pump chunks), so it is the authoritative
// total-cycle count — unlike runtime.cpp's `cycles_elapsed`, which only tallied
// the halt path (the MC-HP-002 "cycles incomparable" red herring). Reset to 0
// at machine reset (runtime_trace_reset). Stamped onto every ring entry so the
// recomp and interp oracle can be aligned by identical cycle counts. (MC-HP-002.)
extern unsigned long long g_runtime_cycles;
// Debug PC breakpoint: when nonzero, runtime_should_yield() unwinds the
// current runtime_dispatch the moment the guest PC equals this value.
// Set via the TCP set_break_pc command; 0 = disabled. (MC-HP-002.)
extern uint32_t g_runtime_break_pc;

// ── BIOS / SWI ─────────────────────────────────────────────────────
// SWI emits a call here. The runtime sets up the exception frame
// (LR_svc = PC+4, SPSR_svc = CPSR, mode=SVC, I=1, T=0) and
// dispatches to the recompiled BIOS at 0x00000008 via
// runtime_dispatch. Returns when the recompiled handler issues
// `movs pc, lr` and we land back at the recompiled site. There is
// no interpreter fallback — see PRINCIPLES.md "Interpreter is
// informative, never load-bearing (SHOWSTOPPER)".

void runtime_swi(uint32_t swi_imm);
void runtime_irq(uint32_t return_address);

// ── PSR transfer ───────────────────────────────────────────────────
// MRS/MSR helpers. Routing through the runtime lets us validate mode
// transitions and keep banked registers coherent.
//
// runtime_msr_cpsr handles bank-swap when CPSR.mode changes: the
// outgoing mode's R13/R14 are saved into banked_sp/banked_lr, and
// the incoming mode's are loaded into R[13]/R[14]. FIQ entry/exit
// additionally swaps R8..R12 with r8_12_fiq.

uint32_t runtime_mrs_cpsr(void);
uint32_t runtime_mrs_spsr(void);
void runtime_msr_cpsr(uint32_t value, uint32_t mask);
void runtime_msr_spsr(uint32_t value, uint32_t mask);

// LDM/STM with S=1 and PC absent transfers User-mode registers while
// remaining in the current mode.
uint32_t runtime_read_user_reg(uint32_t reg);
void runtime_write_user_reg(uint32_t reg, uint32_t value);

// ── Coprocessor (ARM9 CP15) ────────────────────────────────────────
// MCR/MRC/CDP lower to these. On the DS the only coprocessor is the
// ARM9's CP15 (system control: MPU regions, ITCM/DTCM base+size, cache
// control). The runtime routes by cp_num; cp_num==15 drives the CP15
// model that backs the ARM9 bus. There is no HLE — a CP15 write
// reconfigures the real memory map the recompiled code then runs over.
// `op1`/`op2`/`crn`/`crm` are the coprocessor register selectors from
// the MCR/MRC/CDP encoding.
void     runtime_coproc_write(uint32_t cp_num, uint32_t op1, uint32_t crn,
                              uint32_t crm, uint32_t op2, uint32_t value);
uint32_t runtime_coproc_read (uint32_t cp_num, uint32_t op1, uint32_t crn,
                              uint32_t crm, uint32_t op2);
void     runtime_coproc_cdp  (uint32_t cp_num, uint32_t op1, uint32_t crn,
                              uint32_t crm, uint32_t op2);

// ── Exception return ───────────────────────────────────────────────
// Implements the DP-S/LDM-S "Rd=PC" exception return path:
// SPSR_<current mode> → CPSR, bank-swap R13/R14 to the restored
// mode, set PC to new_pc. Used by lowered MOVS PC, LR and LDM with
// PC in list + S bit.

void runtime_exception_return(uint32_t new_pc);
void runtime_restore_cpsr_from_spsr(void);

// ── Fallback ───────────────────────────────────────────────────────
// Emitted for IrOps codegen hasn't lowered yet. The runtime ALWAYS
// aborts here — there is no interpreter fallback (see PRINCIPLES.md
// "Interpreter is informative, never load-bearing"). Every abort is
// a P0 codegen-completion task.

void runtime_unimplemented_op(const char* op_name, uint32_t pc);

// ── Lifecycle ──────────────────────────────────────────────────────
// Called by the runner before any cart code executes.
// `bus_handle` is a void* pointing to the active gba::GbaBus.

void runtime_init(void* bus_handle);
void runtime_shutdown(void);

#ifdef __cplusplus
}  // extern "C"
#endif
