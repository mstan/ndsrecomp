// nds_recompile — Nintendo DS ARM7/ARM9 static recompiler driver.
//
// Reads a per-binary TOML config (bios/biosnds{7,9}.toml), loads the
// binary at its base, verifies SHA-1, discovers functions from the
// config seeds + jump tables, and either:
//   --audit : decode-walk every function and report the instruction
//             histogram + the codegen gaps (unimplemented ops) and the
//             undefined encodings (= the ARMv5 ops still to add). This
//             is the execution-driven gap list that drives Phase 1.
//   (default, --out <dir>) : emit recompiled C banks  [TODO — next step]
//
// Discipline: this surfaces what the REAL BIOS uses, instead of guessing
// which instructions to implement. See docs/dispatch_architecture.md.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "config.h"
#include "function_finder.h"
#include "arm_decode.h"
#include "thumb_decode.h"
#include "arm_codegen.h"
#include "arm_ir.h"
#include "sha1.h"

using namespace ndsrecomp;

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

struct Image {
    const uint8_t* p; std::size_t n; uint32_t base;
    bool in(uint32_t a, uint32_t len) const {
        return a >= base && (uint64_t)(a - base) + len <= n;
    }
    uint32_t u32(uint32_t a) const {
        uint32_t o = a - base;
        return p[o] | (p[o+1]<<8) | (p[o+2]<<16) | ((uint32_t)p[o+3]<<24);
    }
    uint16_t u16(uint32_t a) const {
        uint32_t o = a - base; return p[o] | (p[o+1]<<8);
    }
};

// ─────────────────────────────────────────────────────────────────────
// C bank emission
//
// Ported from gbarecomp/tools/gba_recompile (our sibling project; identical
// codegen ABI), adapted to emit plain C (generated/<bank>.c) and to be
// parameterized by a per-CPU/per-image "bank" name so the ARM9 and ARM7
// banks (arm9_bios, arm7_bios, …) live side by side. See
// docs/dispatch_architecture.md and CLAUDE.md "BUILD LOOP".
// ─────────────────────────────────────────────────────────────────────

namespace {

struct BankNames {
    std::string bank;          // stable title-visible bank identity
    std::string header;        // <bank>.h
    std::string body;          // <bank>.c
    std::string dispatch;      // <bank>_dispatch.c
    std::string table_symbol;  // g_dispatch_<bank>
    std::string table_len;     // g_dispatch_<bank>_len
    std::string guard;         // <BANK>_H
    std::string fn_prefix;     // <bank>_  — namespaces emitted fn symbols
};

struct ResolvedHleRoutine {
    HleProfileRoutine manifest;
    uint32_t source_address = 0;
    uint32_t instruction_count = 0;
    std::string content_sha1;
    std::string descriptor_symbol;
};

BankNames bank_names(const std::string& bank) {
    BankNames n;
    n.bank         = bank;
    n.header       = bank + ".h";
    n.body         = bank + ".c";
    n.dispatch     = bank + "_dispatch.c";
    n.table_symbol = "g_dispatch_" + bank;
    n.table_len    = "g_dispatch_" + bank + "_len";
    n.fn_prefix    = bank + "_";
    for (char c : bank) n.guard += static_cast<char>(std::toupper(
        static_cast<unsigned char>(c)));
    n.guard += "_H";
    return n;
}

std::vector<Function> split_functions_for_emission(
        const std::vector<Function>& funcs, uint32_t max_bytes,
        const uint8_t* rom, std::size_t rom_size, uint32_t rom_base) {
    if (max_bytes == 0u) return funcs;
    std::vector<Function> out;
    for (const Function& fn : funcs) {
        const uint32_t step = fn.mode == CpuMode::Thumb ? 2u : 4u;
        uint32_t limit = std::max(step, max_bytes - (max_bytes % step));
        uint32_t at = fn.addr;
        unsigned part_index = 0u;
        while (at < fn.end_addr) {
            uint32_t end = std::min(fn.end_addr, at + limit);
            // A Thumb BL/BLX immediate is a prefix+suffix pair. Never place a
            // host chunk boundary between its two halfwords.
            if (fn.mode == CpuMode::Thumb && end < fn.end_addr &&
                end >= fn.addr + 2u) {
                const uint32_t original_source = fn.source_addr
                    ? fn.source_addr : fn.addr;
                const uint64_t source = uint64_t{original_source} +
                                        (end - fn.addr) - 2u;
                if (source >= rom_base && source - rom_base + 2u <= rom_size) {
                    const std::size_t off = static_cast<std::size_t>(source - rom_base);
                    const uint16_t hw = static_cast<uint16_t>(
                        rom[off] | (uint16_t{rom[off + 1u]} << 8u));
                    const armv4t::Instr ins =
                        armv4t::ThumbDecoder::decode(hw, end - 2u);
                    if (ins.op == armv4t::IrOp::BL_prefix) end += 2u;
                }
            }

            Function part = fn;
            part.addr = at;
            part.end_addr = end;
            if (fn.source_addr)
                part.source_addr = fn.source_addr + (at - fn.addr);
            if (part_index != 0u) {
                char suffix[32];
                std::snprintf(suffix, sizeof suffix, "_chunk_%u", part_index);
                part.name += suffix;
            }
            part.has_indirect_transfer =
                fn.has_indirect_transfer && end == fn.end_addr;
            out.push_back(std::move(part));
            at = end;
            ++part_index;
        }
    }
    return out;
}

bool same_function(const Function& fn, const HleProfileRoutine& routine) {
    return fn.addr == routine.address &&
           fn.end_addr == routine.end_address &&
           fn.mode == routine.mode;
}

bool verify_sampled_leaf(const Function& fn, const uint8_t* rom,
                         std::size_t rom_size, uint32_t rom_base,
                         uint32_t* instruction_count) {
    const uint32_t step = fn.mode == CpuMode::Thumb ? 2u : 4u;
    const uint32_t source = fn.source_addr ? fn.source_addr : fn.addr;
    const uint64_t size = uint64_t{fn.end_addr} - fn.addr;
    if (source < rom_base || uint64_t{source - rom_base} + size > rom_size)
        return false;
    uint32_t count = 0u;
    for (uint32_t pc = fn.addr; pc < fn.end_addr; pc += step) {
        const std::size_t offset = static_cast<std::size_t>(
            source - rom_base + (pc - fn.addr));
        const armv4t::Instr ins = fn.mode == CpuMode::Thumb
            ? armv4t::ThumbDecoder::decode(
                static_cast<uint16_t>(rom[offset] |
                    (uint16_t{rom[offset + 1u]} << 8u)), pc)
            : armv4t::ArmDecoder::decode(
                uint32_t{rom[offset]} |
                (uint32_t{rom[offset + 1u]} << 8u) |
                (uint32_t{rom[offset + 2u]} << 16u) |
                (uint32_t{rom[offset + 3u]} << 24u), pc);
        if (ins.is_undefined || ins.op == armv4t::IrOp::SWI || ins.is_call)
            return false;
        const bool final = pc + step == fn.end_addr;
        if (!final && (ins.is_branch || ins.is_pc_writing || ins.is_return))
            return false;
        if (final && (!ins.is_return || ins.cond != armv4t::Cond::AL))
            return false;
        ++count;
    }
    *instruction_count = count;
    return count != 0u;
}

bool resolve_hle_routines(const HleProfileManifest& manifest,
                          const Config& cfg, const std::string& bank,
                          const std::vector<Function>& discovered,
                          const std::vector<Function>& emitted,
                          const uint8_t* rom, std::size_t rom_size,
                          std::vector<ResolvedHleRoutine>& out) {
    if (manifest.bank != bank) {
        std::fprintf(stderr,
            "[emit] HLE manifest bank '%s' does not match --bank '%s'\n",
            manifest.bank.c_str(), bank.c_str());
        return false;
    }
    std::string program_sha1 = cfg.identity.sha1;
    std::transform(program_sha1.begin(), program_sha1.end(),
                   program_sha1.begin(), [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    if (program_sha1.rfind("0x", 0u) == 0u) program_sha1.erase(0u, 2u);
    if (manifest.program_sha1 != program_sha1) {
        std::fprintf(stderr,
            "[emit] HLE manifest program SHA-1 does not match verified input\n");
        return false;
    }
    for (std::size_t index = 0; index < manifest.routines.size(); ++index) {
        const HleProfileRoutine& routine = manifest.routines[index];
        const Function* found = nullptr;
        for (const Function& fn : discovered) {
            if (same_function(fn, routine)) {
                if (found) {
                    std::fprintf(stderr,
                        "[emit] HLE routine '%s' resolves ambiguously\n",
                        routine.id.c_str());
                    return false;
                }
                found = &fn;
            }
        }
        if (!found) {
            std::fprintf(stderr,
                "[emit] HLE routine '%s' is not an exact discovered function "
                "[0x%08X,0x%08X)\n", routine.id.c_str(), routine.address,
                routine.end_address);
            return false;
        }
        unsigned emitted_matches = 0u;
        for (const Function& fn : emitted)
            if (same_function(fn, routine)) ++emitted_matches;
        if (emitted_matches != 1u) {
            std::fprintf(stderr,
                "[emit] HLE routine '%s' was split or changed before emission\n",
                routine.id.c_str());
            return false;
        }
        ResolvedHleRoutine resolved;
        resolved.manifest = routine;
        resolved.source_address = found->source_addr
            ? found->source_addr : found->addr;
        if (!verify_sampled_leaf(*found, rom, rom_size,
                                 cfg.program.load_address,
                                 &resolved.instruction_count)) {
            std::fprintf(stderr,
                "[emit] HLE routine '%s' is not a straight-line leaf with "
                "one final return\n", routine.id.c_str());
            return false;
        }
        const uint32_t size = routine.end_address - routine.address;
        resolved.content_sha1 = gba::sha1(
            rom + (resolved.source_address - cfg.program.load_address),
            size).hex();
        resolved.descriptor_symbol = "g_hle_desc_" + bank + "_" +
                                     std::to_string(index);
        out.push_back(std::move(resolved));
    }
    return true;
}

const ResolvedHleRoutine* find_hle_routine(
        const std::vector<ResolvedHleRoutine>& routines,
        const Function& fn) {
    for (const auto& routine : routines)
        if (same_function(fn, routine.manifest)) return &routine;
    return nullptr;
}

// Emit one guest function's body. Decodes every word in [addr, end_addr)
// and lowers it via the codegen, with a pre-pass that (1) marks
// in-function backward branch targets so a `L_<pc>:` label is emitted for
// them, and (2) classifies `bx`/`mov pc` returns that alias LR so they
// C-return instead of dispatching. Mirrors the proven gbarecomp emitter.
void emit_function_body(std::FILE* f, const Function& fn,
                        const uint8_t* rom, std::size_t rom_size,
                        uint32_t rom_base,
                        const std::unordered_map<uint64_t, std::string>&
                            func_names_by_key) {
    const uint32_t step = (fn.mode == CpuMode::Thumb) ? 2u : 4u;
    armv4t::CodegenCtx ctx;
    ctx.names_by_key = &func_names_by_key;
    ctx.current_function_addr = fn.addr;
    ctx.current_function_end_addr = fn.end_addr;
    ctx.current_function_thumb = (fn.mode == CpuMode::Thumb);
    const uint32_t fn_source_addr = fn.source_addr ? fn.source_addr : fn.addr;

    // Every decoded instruction is a resumable static entry. Normal calls
    // arrive at fn.addr and skip the switch; scheduler preemption or a real
    // computed branch into the function arrives at an interior PC and jumps
    // directly to that instruction. This keeps instruction-granular CPU
    // interleaving entirely native (no static-ROM interpreter fallback).
    std::fprintf(f,
        "    if (g_cpu.R[15] != 0x%08Xu) {\n"
        "        switch (g_cpu.R[15]) {\n",
        fn.addr);
    for (uint32_t resume_pc = fn.addr + step;
         resume_pc < fn.end_addr; resume_pc += step) {
        std::fprintf(f, "            case 0x%08Xu: goto L_%08X;\n",
                     resume_pc, resume_pc);
    }
    std::fprintf(f,
        "            default: runtime_dispatch_miss(g_cpu.R[15]); return;\n"
        "        }\n"
        "    }\n");

    auto source_offset_for = [&](uint32_t guest_pc, uint32_t len,
                                 std::size_t* out) -> bool {
        int64_t delta = static_cast<int64_t>(guest_pc) -
            static_cast<int64_t>(fn.addr);
        int64_t source_pc = static_cast<int64_t>(fn_source_addr) + delta;
        if (source_pc < static_cast<int64_t>(rom_base)) return false;
        uint64_t off64 = static_cast<uint64_t>(
            source_pc - static_cast<int64_t>(rom_base));
        if (off64 + len > rom_size) return false;
        *out = static_cast<std::size_t>(off64);
        return true;
    };

    auto decode_at = [&](uint32_t guest_pc, std::size_t off) -> armv4t::Instr {
        if (fn.mode == CpuMode::Thumb) {
            uint16_t hw = static_cast<uint16_t>(
                rom[off] | (rom[off + 1] << 8));
            return armv4t::ThumbDecoder::decode(hw, guest_pc);
        }
        uint32_t w = static_cast<uint32_t>(rom[off])
            | (static_cast<uint32_t>(rom[off + 1]) << 8)
            | (static_cast<uint32_t>(rom[off + 2]) << 16)
            | (static_cast<uint32_t>(rom[off + 3]) << 24);
        return armv4t::ArmDecoder::decode(w, guest_pc);
    };

    auto plain_reg_source = [](const armv4t::Instr& ins, uint8_t* rm) {
        if (ins.op != armv4t::IrOp::MOV ||
            ins.op2.kind != armv4t::Op2::Kind::Shifted ||
            ins.op2.shifted.by_register ||
            ins.op2.shifted.type != armv4t::ShiftType::LSL ||
            ins.op2.shifted.imm_or_rs != 0) {
            return false;
        }
        *rm = ins.op2.shifted.rm;
        return true;
    };

    auto invalidate_written_aliases = [](const armv4t::Instr& ins,
                                         bool alias[16]) {
        auto clear = [&](uint8_t reg) { if (reg < 16) alias[reg] = false; };
        switch (ins.op) {
            case armv4t::IrOp::AND: case armv4t::IrOp::EOR:
            case armv4t::IrOp::SUB: case armv4t::IrOp::RSB:
            case armv4t::IrOp::ADD: case armv4t::IrOp::ADC:
            case armv4t::IrOp::SBC: case armv4t::IrOp::RSC:
            case armv4t::IrOp::ORR: case armv4t::IrOp::MOV:
            case armv4t::IrOp::BIC: case armv4t::IrOp::MVN:
            case armv4t::IrOp::LDR: case armv4t::IrOp::LDRB:
            case armv4t::IrOp::LDRH: case armv4t::IrOp::LDRSB:
            case armv4t::IrOp::LDRSH: case armv4t::IrOp::SWP:
            case armv4t::IrOp::SWPB: case armv4t::IrOp::MRS:
            case armv4t::IrOp::CLZ: case armv4t::IrOp::MRC:
            case armv4t::IrOp::QADD: case armv4t::IrOp::QSUB:
            case armv4t::IrOp::QDADD: case armv4t::IrOp::QDSUB:
            case armv4t::IrOp::SMLAxy: case armv4t::IrOp::SMLAWy:
            case armv4t::IrOp::SMULWy: case armv4t::IrOp::SMULxy:
                clear(ins.rd);
                break;
            case armv4t::IrOp::MUL: case armv4t::IrOp::MLA:
                clear(ins.rd);
                break;
            case armv4t::IrOp::UMULL: case armv4t::IrOp::UMLAL:
            case armv4t::IrOp::SMULL: case armv4t::IrOp::SMLAL:
            case armv4t::IrOp::SMLALxy:
                clear(ins.rd);
                clear(ins.rn);
                break;
            case armv4t::IrOp::LDRD:
                clear(ins.rd);
                clear((ins.rd + 1u) & 15u);
                break;
            case armv4t::IrOp::LDM:
                if (ins.block.load) {
                    for (uint8_t reg = 0; reg < 16; ++reg)
                        if (ins.block.reg_list & (1u << reg)) clear(reg);
                }
                break;
            case armv4t::IrOp::BL:
            case armv4t::IrOp::BLX_reg: case armv4t::IrOp::BLX_imm:
            case armv4t::IrOp::BL_prefix:
            case armv4t::IrOp::BL_suffix:
                clear(14);
                break;
            default:
                break;
        }
    };

    std::unordered_set<uint32_t> backward_targets;
    std::unordered_set<uint32_t> bx_c_return_pcs;
    bool lr_alias[16] = {};
    lr_alias[14] = true;
    armv4t::Instr prev_scan_ins{};
    bool have_prev_scan_ins = false;

    // Seed the previous instruction from ROM so a function that begins at
    // a `bx rN` (split from its `pop {rN}` by the finder) still classifies
    // the stack-pop return idiom correctly.
    if (fn_source_addr >= rom_base && (fn_source_addr - rom_base) >= step) {
        std::size_t prev_off = 0;
        if (source_offset_for(fn.addr - step, step, &prev_off)) {
            prev_scan_ins = decode_at(fn.addr - step, prev_off);
            have_prev_scan_ins = true;
        }
    }

    for (uint32_t scan_pc = fn.addr; scan_pc < fn.end_addr; scan_pc += step) {
        std::size_t scan_off = 0;
        if (!source_offset_for(scan_pc, step, &scan_off)) break;
        armv4t::Instr scan_ins = decode_at(scan_pc, scan_off);
        if (scan_ins.op == armv4t::IrOp::B &&
            scan_ins.branch_target >= fn.addr &&
            scan_ins.branch_target < fn.end_addr &&
            scan_ins.branch_target < scan_pc) {
            backward_targets.insert(scan_ins.branch_target);
        }
        if (scan_ins.op == armv4t::IrOp::BX &&
            have_prev_scan_ins &&
            prev_scan_ins.op == armv4t::IrOp::LDM &&
            prev_scan_ins.block.load &&
            prev_scan_ins.block.writeback &&
            prev_scan_ins.block.rn == 13 &&
            scan_ins.rm < 16 &&
            (prev_scan_ins.block.reg_list &
                static_cast<uint16_t>(1u << scan_ins.rm)) != 0) {
            bx_c_return_pcs.insert(scan_pc);
        }
        if (scan_ins.op == armv4t::IrOp::BX &&
            scan_ins.rm < 16 && lr_alias[scan_ins.rm]) {
            bx_c_return_pcs.insert(scan_pc);
        }

        bool sets_lr_alias = false;
        uint8_t alias_dst = 0, alias_src = 0;
        if (plain_reg_source(scan_ins, &alias_src) &&
            scan_ins.rd < 16 && scan_ins.cond == armv4t::Cond::AL &&
            alias_src < 16 && lr_alias[alias_src]) {
            sets_lr_alias = true;
            alias_dst = scan_ins.rd;
        }
        invalidate_written_aliases(scan_ins, lr_alias);
        if (sets_lr_alias) lr_alias[alias_dst] = true;

        prev_scan_ins = scan_ins;
        have_prev_scan_ins = true;
    }

    uint32_t pc = fn.addr;
    while (pc < fn.end_addr) {
        std::size_t off = 0;
        if (!source_offset_for(pc, step, &off)) break;
        armv4t::Instr ins = decode_at(pc, off);

        std::fprintf(f, "L_%08X:\n", pc);

        std::fprintf(f, "    /* %08X  %s */\n", pc,
                     armv4t::format_ir(ins).c_str());
        bool ni = false;
        ctx.force_bx_c_return = bx_c_return_pcs.count(pc) != 0;
        std::fputs(armv4t::ArmCodegen::emit_instr(ins, ctx, &ni).c_str(), f);
        (void)ni;  // abort path is in the emit; tracked at audit time
        pc += step;
    }
    // Fall-through tail dispatch: if the body ended by hitting end_addr
    // (clipped to the next function) rather than a terminator, hand
    // control to the adjacent function so the runtime doesn't spin.
    std::fprintf(f,
        "    /* fall-through to 0x%08X */\n"
        "    g_cpu.R[15] = 0x%08Xu;\n"
        "    runtime_dispatch(0x%08Xu);\n"
        "    return;\n",
        fn.end_addr, fn.end_addr, fn.end_addr);
}

void write_bank_header(const std::string& dir,
                       const std::vector<Function>& funcs,
                       const BankNames& names) {
    std::FILE* f = std::fopen((dir + "/" + names.header).c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", names.header.c_str()); return; }
    std::fprintf(f,
        "/* AUTO-GENERATED by nds_recompile. DO NOT EDIT. */\n"
        "#ifndef %s\n#define %s\n\n"
        "/* Total functions: %zu */\n\n",
        names.guard.c_str(), names.guard.c_str(), funcs.size());
    for (const auto& fn : funcs)
        std::fprintf(f, "void %s%s(void);  /* 0x%08X %s */\n",
                     names.fn_prefix.c_str(), fn.name.c_str(), fn.addr,
                     fn.mode == CpuMode::Thumb ? "thumb" : "arm");
    std::fprintf(f, "\n#endif /* %s */\n", names.guard.c_str());
    std::fclose(f);
}

void write_bank_dispatch(const std::string& dir,
                         const std::vector<Function>& funcs,
                         const uint8_t* rom, std::size_t rom_size,
                         uint32_t rom_base, const BankNames& names,
                         bool validate_live_bytes,
                         const std::vector<ResolvedHleRoutine>& hle_routines) {
    std::FILE* f = std::fopen((dir + "/" + names.dispatch).c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", names.dispatch.c_str()); return; }
    std::fprintf(f,
        "/* AUTO-GENERATED by nds_recompile. DO NOT EDIT. */\n"
        "/* {guest addr, thumb-bit, generated fn} sorted by address; the\n"
        "   runtime binary-searches by (addr, CPSR.T) per CPU. */\n"
        "#include \"runtime_arm.h\"\n#include \"%s\"\n\n",
        names.header.c_str());

    std::vector<std::string> validation_symbols(funcs.size());
    if (validate_live_bytes) {
        for (std::size_t index = 0; index < funcs.size(); ++index) {
            const Function& fn = funcs[index];
            const uint32_t size = fn.end_addr - fn.addr;
            const uint32_t source = fn.source_addr ? fn.source_addr : fn.addr;
            if (source < rom_base || uint64_t(source - rom_base) + size > rom_size) {
                std::fprintf(stderr,
                    "[emit] validation source outside image: fn=0x%08X source=0x%08X size=%u\n",
                    fn.addr, source, size);
                std::fclose(f);
                return;
            }
            char symbol[128];
            std::snprintf(symbol, sizeof symbol, "g_validation_%s_%zu",
                          names.fn_prefix.c_str(), index);
            validation_symbols[index] = std::string{"&"} + symbol;
            std::fprintf(f, "static const uint8_t %s_bytes[] = {", symbol);
            const uint32_t offset = source - rom_base;
            for (uint32_t i = 0; i < size; ++i) {
                if ((i & 15u) == 0u) std::fputs("\n    ", f);
                std::fprintf(f, "0x%02Xu,", unsigned(rom[offset + i]));
            }
            std::fprintf(f,
                "\n};\nstatic const NdsStaticValidation %s = "
                "{0x%08Xu, %uu, %s_bytes};\n\n",
                symbol, fn.addr, size, symbol);
        }
    }

    if (!hle_routines.empty()) {
        std::fputs("#ifdef NDS_PROFILE_HLE_HEAT\n", f);
        for (const auto& routine : hle_routines) {
            std::size_t function_index = funcs.size();
            for (std::size_t index = 0; index < funcs.size(); ++index) {
                if (same_function(funcs[index], routine.manifest)) {
                    function_index = index;
                    break;
                }
            }
            if (function_index == funcs.size() ||
                validation_symbols[function_index].empty()) {
                std::fprintf(stderr,
                    "[emit] missing live validation for HLE routine '%s'\n",
                    routine.manifest.id.c_str());
                std::fclose(f);
                return;
            }
            std::fprintf(f,
                "const NdsHleProfileDescriptor %s = {\n"
                "    \"%s\", \"%s\", 0x%08Xu, 0x%08Xu, %uu, %uu,\n"
                "    \"%s\", %s\n"
                "};\n\n",
                routine.descriptor_symbol.c_str(),
                routine.manifest.id.c_str(), names.bank.c_str(),
                routine.manifest.address, routine.manifest.end_address,
                routine.manifest.mode == CpuMode::Thumb ? 1u : 0u,
                routine.instruction_count, routine.content_sha1.c_str(),
                validation_symbols[function_index].c_str());
        }
        std::fprintf(f,
            "const NdsHleProfileDescriptor* const g_hle_profile_%s[] = {\n",
            names.bank.c_str());
        for (const auto& routine : hle_routines)
            std::fprintf(f, "    &%s,\n",
                         routine.descriptor_symbol.c_str());
        std::fprintf(f,
            "};\nconst unsigned g_hle_profile_%s_len = %zuu;\n"
            "#endif\n\n",
            names.bank.c_str(),
            hle_routines.size());
    }

    std::fprintf(f, "const NdsDispatchEntry %s[] = {\n",
                 names.table_symbol.c_str());
    struct Row {
        uint32_t addr;
        uint8_t thumb;
        std::string fn;
        std::string validation;
    };
    std::vector<Row> rows;
    for (std::size_t index = 0; index < funcs.size(); ++index) {
        const auto& fn = funcs[index];
        const uint32_t step = (fn.mode == CpuMode::Thumb) ? 2u : 4u;
        for (uint32_t pc = fn.addr; pc < fn.end_addr; pc += step) {
            rows.push_back({pc, fn.mode == CpuMode::Thumb ? uint8_t{1}
                                                         : uint8_t{0},
                            names.fn_prefix + fn.name,
                            validate_live_bytes ? validation_symbols[index]
                                                : "0"});
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.addr != b.addr) return a.addr < b.addr;
        if (a.thumb != b.thumb) return a.thumb < b.thumb;
        return a.fn < b.fn;
    });
    rows.erase(std::unique(rows.begin(), rows.end(),
                           [](const Row& a, const Row& b) {
                               return a.addr == b.addr && a.thumb == b.thumb;
                           }), rows.end());
    for (const auto& row : rows)
        std::fprintf(f, "    {0x%08Xu, %uu, %s, %s},\n", row.addr,
                     unsigned(row.thumb), row.fn.c_str(),
                     row.validation.c_str());
    std::fprintf(f, "};\nconst unsigned %s = %zuu;\n",
                 names.table_len.c_str(), rows.size());
    std::fclose(f);
}

void write_bank_body(const std::string& dir,
                     const std::vector<Function>& funcs,
                     const uint8_t* rom, std::size_t rom_size,
                     uint32_t rom_base, const BankNames& names,
                     const std::string& output_name,
                     std::size_t first, std::size_t last,
                     bool allow_direct_calls,
                     const std::vector<ResolvedHleRoutine>& hle_routines) {
    std::unordered_map<uint64_t, std::string> name_by_key;
    // Direct C calls stay within a body shard. Cross-shard transfers use the
    // normal runtime dispatcher, which keeps shards independently compilable
    // when the corpus grows and avoids an all-bank header dependency.
    // Content-validated RAM variants must dispatch every inter-function edge:
    // a direct host call would bypass the callee's live-byte/provenance check.
    if (allow_direct_calls) {
        for (std::size_t index = first; index < last; ++index) {
            const auto& fn = funcs[index];
            uint64_t key = (static_cast<uint64_t>(fn.addr) << 1u) |
                (fn.mode == CpuMode::Thumb ? 1u : 0u);
            name_by_key[key] = names.fn_prefix + fn.name;
        }
    }
    std::FILE* f = std::fopen((dir + "/" + output_name).c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", output_name.c_str()); return; }
    std::fprintf(f,
        "/* AUTO-GENERATED by nds_recompile. DO NOT EDIT.\n"
        "   Each guest function lowers to a void C function over the recomp\n"
        "   ABI (g_cpu, bus_*, runtime_*; see runtime_arm.h). The interpreter\n"
        "   is never consulted at runtime — an unlowered op aborts via\n"
        "   runtime_unimplemented_op (PRINCIPLES.md). */\n"
        "#include \"runtime_arm.h\"\n\n");
    for (std::size_t index = first; index < last; ++index)
        std::fprintf(f, "void %s%s(void);\n", names.fn_prefix.c_str(),
                     funcs[index].name.c_str());
    std::fputs("\n", f);
    for (std::size_t index = first; index < last; ++index) {
        const auto& fn = funcs[index];
        const ResolvedHleRoutine* hle = find_hle_routine(hle_routines, fn);
        if (hle) {
            std::fprintf(f,
                "#ifdef NDS_PROFILE_HLE_HEAT\n"
                "extern const NdsHleProfileDescriptor %s;\n"
                "#define NDS_HLE_BODY_STORAGE static\n"
                "#define NDS_HLE_BODY_NAME %s%s__lle\n"
                "#else\n"
                "#define NDS_HLE_BODY_STORAGE\n"
                "#define NDS_HLE_BODY_NAME %s%s\n"
                "#endif\n",
                hle->descriptor_symbol.c_str(), names.fn_prefix.c_str(),
                fn.name.c_str(), names.fn_prefix.c_str(), fn.name.c_str());
        }
        std::fprintf(f,
            "/* 0x%08X  mode=%s  end=0x%08X  branches=%zu%s */\n"
            "%svoid %s(void) {\n",
            fn.addr, fn.mode == CpuMode::Thumb ? "thumb" : "arm",
            fn.end_addr, fn.direct_branch_targets.size(),
            fn.has_indirect_transfer ? "  indirect" : "",
            hle ? "NDS_HLE_BODY_STORAGE " : "",
            hle ? "NDS_HLE_BODY_NAME" :
                  (names.fn_prefix + fn.name).c_str());
        emit_function_body(f, fn, rom, rom_size, rom_base, name_by_key);
        std::fprintf(f, "}\n\n");
        if (hle) {
            std::fprintf(f,
                "#ifdef NDS_PROFILE_HLE_HEAT\n"
                "void %s%s(void) {\n"
                "    NdsHleProfileToken token = runtime_hle_profile_begin(&%s);\n"
                "    %s%s__lle();\n"
                "    runtime_hle_profile_end(&%s, token);\n"
                "}\n"
                "#endif\n"
                "#undef NDS_HLE_BODY_NAME\n"
                "#undef NDS_HLE_BODY_STORAGE\n\n",
                names.fn_prefix.c_str(), fn.name.c_str(),
                hle->descriptor_symbol.c_str(), names.fn_prefix.c_str(),
                fn.name.c_str(), hle->descriptor_symbol.c_str());
        }
    }
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path, bin_path, out_dir, bank, hle_manifest_path;
    bool audit = false;
    bool validate_live_bytes = false;
    bool dispatch_only = false;
    bool stable_address_shards = false;
    unsigned shards = 1u;
    uint32_t max_function_bytes = 0u;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]{ return (i+1 < argc) ? argv[++i] : ""; };
        if (a == "--config") config_path = next();
        else if (a == "--bin") bin_path = next();
        else if (a == "--out") out_dir = next();
        else if (a == "--bank") bank = next();
        else if (a == "--hle-manifest") hle_manifest_path = next();
        else if (a == "--audit") audit = true;
        else if (a == "--validate-live-bytes") validate_live_bytes = true;
        else if (a == "--dispatch-only") dispatch_only = true;
        else if (a == "--stable-address-shards") stable_address_shards = true;
        else if (a == "--shards") shards = static_cast<unsigned>(
            std::strtoul(next(), nullptr, 0));
        else if (a == "--max-function-bytes") max_function_bytes =
            static_cast<uint32_t>(std::strtoul(next(), nullptr, 0));
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); return 2; }
    }
    if (config_path.empty() || bin_path.empty()) {
        std::fprintf(stderr,
            "usage: nds_recompile --config <toml> --bin <binary> "
            "[--audit | --out <dir>]\n");
        return 2;
    }

    Config cfg;
    if (!load_config(config_path, cfg)) return 1;
    std::vector<uint8_t> bin = read_file(bin_path);
    if (bin.empty()) { std::fprintf(stderr, "cannot read %s\n", bin_path.c_str()); return 1; }
    if (!verify_identity(cfg, bin.data(), bin.size())) return 1;
    print_config_summary(cfg);

    HleProfileManifest hle_manifest;
    if (!hle_manifest_path.empty()) {
        if (audit || bank.empty() || !validate_live_bytes) {
            std::fprintf(stderr,
                "--hle-manifest requires emission, an explicit --bank, and "
                "--validate-live-bytes\n");
            return 2;
        }
        if (!load_hle_profile_manifest(hle_manifest_path, hle_manifest))
            return 1;
    }

    Image img{bin.data(), bin.size(), cfg.program.load_address};

    FunctionFinder finder(bin.data(), bin.size(), cfg.program.load_address);
    finder.set_authoritative_seeds(cfg.program.authoritative_entry_points);
    // Program entry historically defaulted to ARM. Runtime-materialized
    // firmware may enter in Thumb, in which case an explicit entry_point at
    // the same address is authoritative and must suppress the bogus ARM walk.
    const bool explicit_program_entry = std::any_of(
        cfg.extra_funcs.begin(), cfg.extra_funcs.end(),
        [&](const auto& ef) { return ef.addr == cfg.program.entry_pc; });
    if (!explicit_program_entry)
        finder.add_seed({cfg.program.entry_pc, CpuMode::Arm, "entry", 0, 0});
    for (const auto& ef : cfg.extra_funcs)
        finder.add_seed({ef.addr, ef.mode, ef.name, 0, ef.size});
    for (const auto& dr : cfg.data_ranges)
        finder.add_data_range(dr.start, dr.end, dr.note);
    for (const auto& cc : cfg.code_copies)
        finder.add_code_copy(cc.runtime_start, cc.source_start, cc.size, cc.name);
    for (const auto& ex : cfg.exclude_funcs)
        finder.add_exclude(ex.addr, ex.reason);
    // Expand each declared jump table into per-target seeds (bit0 = mode
    // for entries_mode=auto), and mark the table bytes as data.
    for (const auto& jt : cfg.jump_tables) {
        for (uint32_t k = 0; k < jt.count; k++) {
            uint32_t ea = jt.addr + k * jt.stride;
            if (!img.in(ea, 4)) break;
            uint32_t e = img.u32(ea);
            if (e == 0) continue;                 // unimplemented slot
            CpuMode m = (jt.entries_mode == JumpTableEntriesMode::Thumb) ? CpuMode::Thumb
                      : (jt.entries_mode == JumpTableEntriesMode::Arm)   ? CpuMode::Arm
                      : ((e & 1) ? CpuMode::Thumb : CpuMode::Arm);
            char nm[64]; std::snprintf(nm, sizeof nm, "%s_%u", jt.name.c_str(), k);
            finder.add_seed({e & ~1u, m, nm, 0});
        }
        finder.add_data_range(jt.addr, jt.addr + jt.count * jt.stride,
                              "jump_table:" + jt.name);
    }

    finder.run();
    const auto& st = finder.stats();
    std::printf("\n[finder] functions=%zu (arm=%zu thumb=%zu) "
                "indirect=%zu undefined=%zu auto_jump_tables=%zu "
                "landing_pads=%zu\n",
                st.functions_total, st.functions_arm, st.functions_thumb,
                st.indirect_transfer_count, st.undefined_instr_count,
                st.auto_jump_tables, st.landing_pads_discovered);
    if (!finder.collisions().empty()) {
        std::printf("[finder] %zu data-range collisions (flow into data):\n",
                    finder.collisions().size());
        for (const auto& c : finder.collisions())
            std::printf("    0x%08X <- %s @0x%08X (%s)\n", c.flow_target_addr,
                        c.flow_origin_name.c_str(), c.flow_origin_addr,
                        c.range_note.c_str());
    }

    if (!audit) {
        if (out_dir.empty()) {
            std::fprintf(stderr,
                "[emit] need --out <dir> (and optionally --bank <name>)\n");
            return 2;
        }
        // Default the bank name from the binary's filename stem (e.g.
        // biosnds9). The build loop passes --bank arm9_bios / arm7_bios.
        if (bank.empty()) {
            std::size_t s = bin_path.find_last_of("/\\");
            std::string stem = (s == std::string::npos) ? bin_path
                                                         : bin_path.substr(s + 1);
            std::size_t d = stem.find_last_of('.');
            if (d != std::string::npos) stem = stem.substr(0, d);
            bank = stem.empty() ? "bank" : stem;
        }
        std::vector<Function> funcs = split_functions_for_emission(
            finder.functions(), max_function_bytes, bin.data(), bin.size(),
            cfg.program.load_address);
        std::vector<ResolvedHleRoutine> hle_routines;
        if (!hle_manifest_path.empty() &&
            !resolve_hle_routines(hle_manifest, cfg, bank,
                                  finder.functions(), funcs, bin.data(),
                                  bin.size(), hle_routines))
            return 1;
        auto emission_addr = [](const Function& fn) {
            return fn.source_addr ? fn.source_addr : fn.addr;
        };
        if (stable_address_shards) {
            // Runtime code copies can be far outside the source image (for
            // example ARM7 WRAM at 0x037F8000 whose captured bytes are
            // appended after main RAM at 0x02400000). Fixed shards must be
            // keyed by source-image address, otherwise every copied function
            // falls past the final band and overwrites the last shard.
            std::stable_sort(funcs.begin(), funcs.end(),
                [&](const Function& a, const Function& b) {
                    const uint32_t as = emission_addr(a);
                    const uint32_t bs = emission_addr(b);
                    if (as != bs) return as < bs;
                    if (a.addr != b.addr) return a.addr < b.addr;
                    return a.mode < b.mode;
                });
            const uint64_t source_begin = cfg.program.load_address;
            const uint64_t source_end = source_begin + cfg.program.size;
            for (const Function& fn : funcs) {
                const uint64_t source = emission_addr(fn);
                if (source < source_begin || source >= source_end) {
                    std::fprintf(stderr,
                        "[emit] stable shard source outside image: "
                        "fn=0x%08X source=0x%08llX image=[0x%08llX,0x%08llX)\n",
                        fn.addr, (unsigned long long)source,
                        (unsigned long long)source_begin,
                        (unsigned long long)source_end);
                    return 1;
                }
            }
        }
        BankNames names = bank_names(bank);
        unsigned emitted_shards = 0u;
        if (!dispatch_only) {
            write_bank_header(out_dir, funcs, names);
            if (shards == 0u) shards = 1u;
            auto emit_shard = [&](unsigned shard, std::size_t first,
                                  std::size_t last) {
                std::string output_name = names.body;
                if (shards > 1u) {
                    char suffix[24];
                    std::snprintf(suffix, sizeof suffix, "_%02u.c", shard);
                    output_name.resize(output_name.size() - 2u);
                    output_name += suffix;
                }
                write_bank_body(out_dir, funcs, bin.data(), bin.size(),
                                cfg.program.load_address, names, output_name,
                                first, last, !validate_live_bytes,
                                hle_routines);
                ++emitted_shards;
            };
            if (stable_address_shards) {
                std::size_t first = 0u;
                const uint64_t base = cfg.program.load_address;
                const uint64_t span = cfg.program.size;
                for (unsigned shard = 0; shard < shards; ++shard) {
                    const uint64_t end = base +
                        (span * uint64_t{shard + 1u}) / shards;
                    std::size_t last = first;
                    while (last < funcs.size() && emission_addr(funcs[last]) < end)
                        ++last;
                    // Emit empty bands too so an older non-empty shard at the
                    // same stable index cannot survive a regeneration.
                    emit_shard(shard, first, last);
                    first = last;
                }
                if (first < funcs.size()) {
                    std::fprintf(stderr,
                        "[emit] internal error: %zu functions remained after "
                        "stable source-address sharding\n",
                        funcs.size() - first);
                    return 1;
                }
            } else {
                uint64_t remaining_weight = 0u;
                for (const Function& fn : funcs)
                    remaining_weight += std::max<uint32_t>(
                        1u, fn.end_addr - fn.addr);
                std::size_t first = 0u;
                for (unsigned shard = 0; shard < shards; ++shard) {
                    if (first >= funcs.size()) break;
                    const uint64_t remaining_shards = shards - shard;
                    const uint64_t target =
                        (remaining_weight + remaining_shards - 1u) /
                        remaining_shards;
                    uint64_t weight = 0u;
                    std::size_t last = first;
                    while (last < funcs.size() &&
                           (last == first || weight < target)) {
                        weight += std::max<uint32_t>(
                            1u, funcs[last].end_addr - funcs[last].addr);
                        ++last;
                    }
                    emit_shard(shard, first, last);
                    first = last;
                    remaining_weight -= weight;
                }
            }
        }
        write_bank_dispatch(out_dir, funcs, bin.data(), bin.size(),
                            cfg.program.load_address, names,
                            validate_live_bytes, hle_routines);
        std::printf("\n[emit] bank '%s': %zu functions (%u body shard%s%s) -> %s/{%s,%s,%s}\n",
                    bank.c_str(), funcs.size(), emitted_shards,
                    emitted_shards == 1u ? "" : "s",
                    dispatch_only ? ", dispatch only" : "",
                    out_dir.c_str(),
                    names.body.c_str(), names.header.c_str(),
                    names.dispatch.c_str());
        return 0;
    }

    // ── Audit: per-function linear decode to the first terminator. ──
    std::map<std::string, uint64_t> op_hist;     // IrOp name -> count
    std::map<std::string, uint64_t> not_impl;    // op name codegen can't lower
    std::map<uint32_t, uint64_t> undef_arm;      // raw word -> count (ARM)
    std::map<uint32_t, uint64_t> undef_thumb;    // raw hw -> count (Thumb)
    uint64_t decoded = 0;
    armv4t::CodegenCtx ctx;

    for (const auto& fn : finder.functions()) {
        bool thumb = (fn.mode == CpuMode::Thumb);
        uint32_t pc = fn.addr;
        for (int steps = 0; steps < 4096; steps++) {
            uint32_t w = thumb ? 2 : 4;
            if (!img.in(pc, w)) break;
            armv4t::Instr in = thumb
                ? armv4t::ThumbDecoder::decode(img.u16(pc), pc)
                : armv4t::ArmDecoder::decode(img.u32(pc), pc);
            decoded++;
            if (in.is_undefined) {
                if (thumb) undef_thumb[img.u16(pc)]++; else undef_arm[img.u32(pc)]++;
                break;  // stop the body at an undecodable word (likely a v5 op or a pool)
            }
            op_hist[armv4t::ir_op_name(in.op)]++;
            bool ni = false;
            (void)armv4t::ArmCodegen::emit_instr(in, ctx, &ni);
            if (ni) not_impl[armv4t::ir_op_name(in.op)]++;
            // Stop at an unconditional terminator (B/BX/return), else advance.
            bool terminator = (in.is_return) ||
                (in.cond == armv4t::Cond::AL && in.is_pc_writing && !in.is_call);
            if (terminator) break;
            pc += w;
        }
    }

    std::printf("\n[audit] decoded %llu instructions across %zu functions\n",
                (unsigned long long)decoded, finder.functions().size());
    std::printf("\n[audit] IrOp histogram:\n");
    for (auto& [k, v] : op_hist) std::printf("    %-10s %llu\n", k.c_str(),
                                             (unsigned long long)v);
    std::printf("\n[audit] codegen NOT-IMPLEMENTED ops (v4t gaps to close):\n");
    if (not_impl.empty()) std::printf("    (none)\n");
    for (auto& [k, v] : not_impl) std::printf("    %-10s %llu\n", k.c_str(),
                                              (unsigned long long)v);
    std::printf("\n[audit] UNDEFINED encodings (distinct=%zu arm, %zu thumb) "
                "— likely ARMv5 ops or literal pools:\n",
                undef_arm.size(), undef_thumb.size());
    int shown = 0;
    for (auto& [w, c] : undef_arm) {
        std::printf("    ARM   0x%08X  x%llu\n", w, (unsigned long long)c);
        if (++shown >= 24) { std::printf("    ... (%zu more)\n", undef_arm.size()-24); break; }
    }
    shown = 0;
    for (auto& [w, c] : undef_thumb) {
        std::printf("    THUMB 0x%04X      x%llu\n", w, (unsigned long long)c);
        if (++shown >= 12) { std::printf("    ... (%zu more)\n", undef_thumb.size()-12); break; }
    }
    return 0;
}
