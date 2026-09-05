// codegen_identity.h — the SEMANTIC identity of this recompiler's generated-C
// emission, reported by `nds_recompile --codegen-identity`.
//
// WHY THIS EXISTS (beads-yjp.52)
//
// A live shard is a native DLL that binds directly to runner data symbols and
// is compiled against the runtime struct layouts. Loading a shard whose codegen
// assumptions differ from the runner's is silent memory corruption, so the
// live-shard provider identity (tools/compile_live_shards.py::provider_identity)
// has to invalidate every cached shard whenever emission changes.
//
// It used to do that by hashing the recompiler EXECUTABLE's bytes. That is
// both too strong and useless as a signal: a PE carries a link timestamp and
// build-path residue, so merely rebuilding the recompiler from an unchanged
// tree produced a different hash and threw away every player's accumulated
// shard cache. v0.6.5 could not carry the v0.6.4 prebuilt cache forward for
// exactly this reason.
//
// The identity is therefore DECLARED here and pinned by a golden test.
//
// THE CONTRACT
//
//   Bump kCodegenVersion whenever a change to this recompiler can alter a
//   single byte of generated C for ANY input.
//
// That covers src/main.cpp emission, finder/ (which decides what bodies exist
// and where they are split), and the shared ARM core under
// external/arm-recomp-core (decode, IR, codegen profile).
//
// A hand-maintained constant is only as good as the discipline around it, so
// it is not hand-maintained alone: recompiler/tests/codegen_golden_test.cpp
// runs this binary over a fixed corpus that exercises every emission mode the
// live-shard pipeline uses, hashes the generated C, and compares it against
// recompiler/tests/codegen_golden.txt. The golden file records the version it
// was captured at, and the regeneration path refuses to write a new digest
// under an unchanged version. So emission cannot move without the version
// moving, and the version cannot move without the golden moving.
//
// The version is NOT a release version and has no meaning outside the shard
// cache. It only ever increases.

#pragma once

namespace ndsrecomp {

// Bump when generated-C emission changes. See the contract above.
//
// 1 — beads-yjp.52: initial declared identity, captured at framework main
//     c6122bd (live bank ABI 6, B2 per-callsite link slots).
// 2 — beads-yjp.67: MRS CPSR reads the field inline; MSR CPSR writes the
//     masked bytes inline when the write provably cannot change mode; tiny
//     `bx lr` leaf bodies are expanded at direct BL sites behind the
//     runtime_inline_leaf_admit gate.
// 3 — beads-yjp.70 phase 2A: the per-instruction code-fetch cost is emitted
//     as nds_code_numc(pc, NDS_ARM9_CODE_K(pc, thumb)) — a codegen-time
//     packed per-class constant selected by the runtime's published ARM9
//     code-timing class — instead of a runtime_code_cycles(pc) call, and
//     condition guards call the inline arm_cond_passes_i. Emitted cycle
//     counts and tick boundaries are unchanged.
inline constexpr unsigned kCodegenVersion = 3;

// The string `--codegen-identity` prints and the shard provider identity
// folds. Tagged so that a hash of it can never collide with a bare integer
// some other producer might report.
inline constexpr const char* kCodegenIdentityPrefix = "nds-codegen-v";

}  // namespace ndsrecomp
