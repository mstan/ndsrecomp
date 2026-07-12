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
extern NdsCpu g_nds_active;

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
// All cart reads/writes flow through these. The runtime sets up the
// bus pointer before any cart code runs; bus_set_active() is called
// from the runner's main().

uint32_t bus_read_u32(uint32_t addr);
uint16_t bus_read_u16(uint32_t addr);
uint8_t  bus_read_u8 (uint32_t addr);
void     bus_write_u32(uint32_t addr, uint32_t val);
void     bus_write_u16(uint32_t addr, uint16_t val);
void     bus_write_u8 (uint32_t addr, uint8_t  val);

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
//              and stores add NO internal cycles; the first ~6 cycles of
//              the data access overlap the code fetch already in flight.
//   !has_data: numI ? numC + numI : numC                -- CI, else C.
// A taken branch / any PC write additionally adds 2*numC(target region)
// for the pipeline refill — that term is NOT part of this function (it
// depends on the branch target, which codegen adds separately at the
// branch/PC-write tick sites); this returns only the instruction's own
// class cost. ARM9 only — every codegen tick site's ARM7 branch is the
// ORIGINAL flat _cyc expression, verbatim; this is never called for ARM7.
uint32_t arm9_cycle_combine(uint32_t numC, uint32_t numD, uint32_t numI,
                            uint32_t has_data);

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
