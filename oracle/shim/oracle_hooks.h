// oracle_hooks.h — ndsrecomp oracle event counters.
//
// The oracle syncs the native runtime against melonDS on *hardware events*
// (never frame indices — see ../../TCP.md). These counters are the always-on
// ring of those events. melonDS bumps the IRQ-derived counters from a single
// hook in NDS::SetIRQ (guarded by MELONDS_ORACLE_HOOKS); the register-write
// counters are bumped by the OracleNDS subclass's IO-write overrides.
//
// This header is part of the oracle shim (not stock melonDS). NDS.cpp does NOT
// include it — it calls ::Oracle_OnSetIRQ via a one-line extern declaration so
// the melonDS patch stays minimal.
#ifndef ORACLE_HOOKS_H
#define ORACLE_HOOKS_H

#include <cstdint>

struct OracleCounters
{
    uint64_t vblank9   = 0;  // SetIRQ(0, IRQ_VBlank)
    uint64_t vblank7   = 0;  // SetIRQ(1, IRQ_VBlank)
    uint64_t ipcsync_w = 0;  // writes to IPCSYNC (0x04000180), either CPU
    uint64_t fifo9to7  = 0;  // ARM9 IPC FIFO sends (write 0x04000188)
    uint64_t fifo7to9  = 0;  // ARM7 IPC FIFO sends
    uint64_t dma_done  = 0;  // SetIRQ(cpu, IRQ_DMA0..3)
    uint64_t timer_ovf = 0;  // SetIRQ(cpu, IRQ_Timer0..3)
    uint64_t spi_w     = 0;  // ARM7 writes to SPIDATA (0x040001C2)
    uint64_t irq9      = 0;  // accepted ARM9 IRQ exceptions
    uint64_t irq7      = 0;  // accepted ARM7 IRQ exceptions
    // SOUNDBIAS (0x04000504) write stream — invariant for the ARM7 boot
    // sound-bias ramp. Symmetric with the native runtime's NdsEventCounts.
    uint64_t soundbias_w     = 0;  // total ARM7 writes to SOUNDBIAS
    uint32_t soundbias_first = 0;  // first value written
    uint32_t soundbias_last  = 0;  // most recent value written
    // Per-CPU retired-instruction counters, from power-on reset. Bumped once
    // per architectural guest instruction (ARM or THUMB, taken or condition-
    // failed) by Oracle_OnInsnRetire, hooked into ARMv5::Execute/ARMv4::Execute
    // (see ../patches/0004-nds-insn-retire-hook.patch). A Thumb BL long branch
    // is two separate table entries (prefix + suffix halfword), so it bumps
    // the counter twice — matching real ARMv5TE/ARMv4T semantics.
    uint64_t insn9 = 0;  // ARM9 (ARMv5TE) instructions retired
    uint64_t insn7 = 0;  // ARM7 (ARMv4T)  instructions retired
};

// Single global ring — the oracle runs exactly one NDS instance.
extern OracleCounters g_oracle_counts;

#endif // ORACLE_HOOKS_H
