// function_finder.cpp — implementation.
//
// Discovery is a prioritized worklist:
//   - Pop (addr, mode) from the worklist.
//   - If already visited, skip.
//   - Decode instructions from addr forward, in `mode`.
//   - For each direct branch (B / BL / conditional B), record the
//     target and add it to the worklist as a NEW function root near
//     the current function (the
//     finder is intentionally pessimistic — every branch target is
//     treated as a potential function entry; the codegen later
//     consolidates).
//   - For BX/BLX-reg/computed: mark `has_indirect_transfer` and stop
//     this function's body.
//   - Stop at: undefined, return-shaped instr (BX LR / pop {pc} /
//     ldm rN, {..., pc}), or after a hard cap (4096 instrs per
//     function as a sanity bound).

#include "function_finder.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

#include "arm_decode.h"
#include "thumb_decode.h"
#include "arm_ir.h"

namespace ndsrecomp {

namespace {

uint64_t visit_key(uint32_t addr, CpuMode mode) {
    return (static_cast<uint64_t>(addr) << 1) |
           static_cast<uint64_t>(mode);
}

std::string default_name(uint32_t addr, CpuMode mode) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s_%08X",
                  mode == CpuMode::Thumb ? "tfunc" : "afunc",
                  addr);
    return std::string(buf);
}

uint32_t ror32(uint32_t v, unsigned n) {
    n &= 31u;
    if (n == 0) return v;
    return (v >> n) | (v << (32u - n));
}

// Diagnostic toggle: GBARECOMP_NO_JT=1 disables automatic jump-table
// detection, so a run can isolate the detector's cost from the baseline
// finder. Read once.
bool jump_table_detection_enabled() {
    static const bool enabled = (std::getenv("GBARECOMP_NO_JT") == nullptr);
    return enabled;
}

// Diagnostic toggle: GBARECOMP_JT_REPORT=1 prints, to stderr, every
// confirmed jump table and the emitter's decision (emitted N / rejected
// why). Used to measure how many confirmed tables the count-terminator
// actually emits — the MC-HP-000 coverage metric. Read once.
bool jt_report_enabled() {
    static const bool enabled = (std::getenv("GBARECOMP_JT_REPORT") != nullptr);
    return enabled;
}

}  // namespace

FunctionFinder::FunctionFinder(const uint8_t* rom_bytes,
                                std::size_t rom_size,
                                uint32_t rom_base)
    : rom_(rom_bytes), rom_size_(rom_size), rom_base_(rom_base) {}

void FunctionFinder::add_seed(const FunctionSeed& seed) {
    seeds_.push_back(seed);
}

void FunctionFinder::add_data_range(uint32_t start, uint32_t end,
                                     const std::string& note) {
    data_ranges_.push_back(DataRange{start, end, note});
}

void FunctionFinder::add_code_copy(uint32_t runtime_start,
                                    uint32_t source_start,
                                    uint32_t size,
                                    const std::string& note) {
    code_copies_.push_back(CodeCopyRange{
        runtime_start, source_start, size, note});
}

void FunctionFinder::add_exclude(uint32_t addr,
                                  const std::string& reason) {
    excluded_.insert(addr);
    exclude_reasons_[addr] = reason;
}

bool FunctionFinder::map_addr_to_source(uint32_t addr,
                                         uint32_t* source) const {
    if (addr_in_rom(addr)) {
        *source = addr;
        return true;
    }
    for (const auto& cc : code_copies_) {
        if (addr >= cc.runtime_start &&
            addr - cc.runtime_start < cc.size) {
            uint32_t mapped = cc.source_start + (addr - cc.runtime_start);
            if (!addr_in_rom(mapped)) return false;
            *source = mapped;
            return true;
        }
    }
    return false;
}

bool FunctionFinder::can_read_at(uint32_t addr, uint32_t len) const {
    uint32_t source = 0;
    if (!map_addr_to_source(addr, &source)) return false;
    return (source - rom_base_ + len) <= rom_size_;
}

bool FunctionFinder::addr_in_data_range(uint32_t addr) const {
    for (const auto& dr : data_ranges_) {
        if (addr >= dr.start && addr < dr.end) return true;
    }
    return false;
}

const DataRange* FunctionFinder::find_data_range(uint32_t addr) const {
    for (const auto& dr : data_ranges_) {
        if (addr >= dr.start && addr < dr.end) return &dr;
    }
    return nullptr;
}

void FunctionFinder::record_collision(uint32_t target,
                                       uint32_t origin_addr,
                                       const std::string& origin_name,
                                       const std::string& origin_kind) {
    DataRangeCollision col{};
    col.flow_target_addr  = target;
    col.flow_origin_addr  = origin_addr;
    col.flow_origin_name  = origin_name;
    col.flow_origin_kind  = origin_kind;
    if (const DataRange* dr = find_data_range(target)) {
        col.range_start = dr->start;
        col.range_end   = dr->end;
        col.range_note  = dr->note;
    }
    collisions_.push_back(std::move(col));
}

uint32_t FunctionFinder::read_u32(uint32_t addr) const {
    uint32_t source = addr;
    map_addr_to_source(addr, &source);
    std::size_t off = source - rom_base_;
    return  static_cast<uint32_t>(rom_[off])
         | (static_cast<uint32_t>(rom_[off + 1]) << 8)
         | (static_cast<uint32_t>(rom_[off + 2]) << 16)
         | (static_cast<uint32_t>(rom_[off + 3]) << 24);
}

uint32_t FunctionFinder::read_u32_public(uint32_t addr) const {
    if (!can_read_at(addr, 4)) return 0;
    return read_u32(addr);
}

uint16_t FunctionFinder::read_u16(uint32_t addr) const {
    uint32_t source = addr;
    map_addr_to_source(addr, &source);
    std::size_t off = source - rom_base_;
    return  static_cast<uint16_t>(rom_[off])
         | (static_cast<uint16_t>(rom_[off + 1]) << 8);
}

void FunctionFinder::discover_one(uint32_t entry_addr, CpuMode entry_mode,
                                   const std::string& seed_name,
                                   uint32_t seed_source_addr,
                                   uint32_t exact_size) {
    uint32_t entry_source = 0;
    if (seed_source_addr != 0) {
        entry_source = seed_source_addr;
        if (!addr_in_rom(entry_source)) return;
    } else if (!map_addr_to_source(entry_addr, &entry_source)) {
        return;
    }
    uint64_t key = visit_key(entry_addr, entry_mode);
    if (visited_.find(key) != visited_.end()) return;

    Function fn{};
    fn.addr = entry_addr;
    fn.source_addr = entry_source;
    fn.mode = entry_mode;
    fn.name = seed_name.empty() ? default_name(entry_addr, entry_mode)
                                : seed_name;
    fn.end_addr = entry_addr;  // updated below

    visited_[key] = functions_.size();  // index will be re-sorted at end

    uint32_t pc = entry_addr;
    const uint32_t step = (entry_mode == CpuMode::Thumb) ? 2u : 4u;
    const uint64_t exact_end64 = static_cast<uint64_t>(entry_addr) + exact_size;
    if (exact_size != 0u &&
        (exact_end64 >= 0x100000000ull ||
         !can_read_at(static_cast<uint32_t>(exact_end64 - step), step))) {
        return;
    }
    const uint32_t exact_end = exact_size == 0u
        ? 0u : static_cast<uint32_t>(exact_end64);
    constexpr std::size_t kMaxInstrs = 4096;
    std::size_t count = 0;

    // Narrow intra-function constant tracker. Lets the finder
    // follow the classic ARM↔THUMB interworking pattern:
    //   add R0, PC, #1
    //   bx  R0
    // (and the THUMB equivalent: `mov Rn, pc; bx Rn`). When we see
    // BX with rm != 14 (non-return), we look up Rm in this table.
    // If it has a tracked value, we add it as a branch target
    // (with mode inferred from the low bit, like the SWI table's
    // entries_mode="auto").
    //
    // Coverage is deliberately narrow:
    //   - tracks `MOV Rn, #imm`  (clears the rest)
    //   - tracks `ADD Rn, PC, #imm`  (computes PC+8+imm for ARM,
    //     PC+4+imm for THUMB)
    //   - any other write to Rn invalidates the entry
    //   - any control-flow break (branch, call) wipes the table
    //
    // False positives are avoided by being conservative: we only
    // resolve a BX target when we're certain we saw the constant
    // load IN THIS BASIC BLOCK with no intervening write to Rm.
    struct RegConst {
        bool     known = false;
        uint32_t value = 0;
    };
    RegConst reg_const[16];

    // Jump-table address tracking (MC-HP-000). The compiler builds a
    // dispatch like:  LDR Rb,=table ; LSL Ri,Ri,#k ; ADD Ra,Ri,Rb ;
    // LDR Rt,[Ra] ; BX Rt (or BL into the bx-rN veneer). reg_const
    // already resolves Rb from its literal load; these two track the
    // scaled index and the resulting element-address register so the
    // LDR that fetches the entry can reveal the table.
    struct RegScaled {  // Rd = (unknown index) << shift
        bool     known = false;
        uint8_t  shift = 0;
        // Exact range bound carried from a `CMP index,#N; B{hi,cs}` guard
        // (max_index is the largest in-range index). Lets the jump-table
        // emitter size the table precisely instead of guessing.
        bool     bound_known = false;
        uint32_t max_index   = 0;
    };
    RegScaled reg_scaled[16];
    struct RegTableAddr {  // Rd = base + (scaled index)
        bool     known  = false;
        uint32_t base   = 0;
        uint32_t stride = 0;
        bool     bound_known = false;  // carried from RegScaled
        uint32_t max_index   = 0;
    };
    RegTableAddr reg_table[16];

    // Range bound for a register established by a `CMP Ri,#N` switch
    // guard. The bound travels with the index through the scale/add chain
    // and gives the jump-table emitter an EXACT entry count — the precise
    // terminator that replaces the old "stop at first non-prologue" walk.
    // Two guard shapes feed it: the in-block `B{hi,cs}`-to-default form
    // (bound set here as we walk past it), and the inverted
    // `B{ls,cc}`-to-dispatch form, where this walk IS the dispatch target
    // and the bound was parked in branch_target_bounds_ by the predecessor.
    struct RegBound {
        bool     known     = false;
        uint32_t max_index = 0;
    };
    RegBound reg_bound[16];
    // Inverted-guard bound parked for this seed (see header). Re-seed it
    // so the dispatch's scale->add->load chain can size the table.
    {
        auto it = branch_target_bounds_.find(entry_addr);
        if (it != branch_target_bounds_.end() && it->second.reg < 16) {
            reg_bound[it->second.reg] = {true, it->second.max_index};
        }
    }
    struct LastCmp {  // most recent `CMP Ri,#imm`, consumed by the next branch
        bool     known = false;
        uint8_t  reg   = 0;
        uint32_t imm   = 0;
        uint32_t pc    = 0;
    };
    LastCmp last_cmp;

    // A jump-table candidate awaiting confirmation. Set when an indexed
    // load fetches a would-be entry into `dest`; promoted to a real
    // table ONLY when `dest` is subsequently used as a control-flow
    // target (BX dest / MOV pc,dest / BL into a `bx dest` veneer). This
    // is what distinguishes a jump table from an ordinary indexed load
    // of a DATA pointer (which is dereferenced, not branched to) — the
    // distinction that keeps the detector from seeding garbage.
    struct PendingJt {
        bool     active      = false;
        uint32_t base        = 0;
        uint32_t stride      = 0;
        uint8_t  dest        = 0;
        // Exact entry count from the dispatcher's CMP-bound, when present.
        bool     count_known = false;
        uint32_t count       = 0;
    };
    PendingJt pend;

    struct RegSym {
        bool    known = false;
        uint8_t rn = 0;
        int32_t offset = 0;
    };
    RegSym reg_sym[16];

    struct MemConst {
        bool     known = false;
        uint8_t  rn = 0;
        int32_t  offset = 0;
        uint32_t value = 0;
    };
    MemConst mem_const[8];

    auto clear_mem_const = [&]() {
        for (auto& slot : mem_const) {
            slot.known = false;
        }
    };

    auto invalidate_mem_base = [&](uint8_t rn) {
        for (auto& slot : mem_const) {
            if (slot.known && slot.rn == rn) {
                slot.known = false;
            }
        }
    };

    auto invalidate_sym_base = [&](uint8_t rn) {
        for (auto& sym : reg_sym) {
            if (sym.known && sym.rn == rn) {
                sym.known = false;
            }
        }
    };

    auto resolve_symbolic_base = [&](uint8_t base_reg, int32_t offset,
                                     uint8_t* rn,
                                     int32_t* resolved_offset) -> bool {
        if (base_reg >= 15) return false;
        if (reg_sym[base_reg].known) {
            *rn = reg_sym[base_reg].rn;
            *resolved_offset = reg_sym[base_reg].offset + offset;
        } else {
            *rn = base_reg;
            *resolved_offset = offset;
        }
        return true;
    };

    auto eval_symbolic_mem_addr = [&](const armv4t::Instr& i,
                                      uint8_t* rn,
                                      int32_t* offset) -> bool {
        if (i.mem.by_register || i.mem.rn >= 15) return false;
        int32_t mem_offset = 0;
        if (!i.mem.pre_indexed) {
            mem_offset = 0;
        } else {
            int32_t imm = static_cast<int32_t>(i.mem.imm_offset);
            mem_offset = i.mem.add ? imm : -imm;
        }
        return resolve_symbolic_base(i.mem.rn, mem_offset, rn, offset);
    };

    auto remember_mem_const = [&](uint8_t rn, int32_t offset,
                                  uint32_t value) {
        for (auto& slot : mem_const) {
            if (slot.known && slot.rn == rn && slot.offset == offset) {
                slot.value = value;
                return;
            }
        }
        for (auto& slot : mem_const) {
            if (!slot.known) {
                slot.known = true;
                slot.rn = rn;
                slot.offset = offset;
                slot.value = value;
                return;
            }
        }
        mem_const[0].known = true;
        mem_const[0].rn = rn;
        mem_const[0].offset = offset;
        mem_const[0].value = value;
    };

    auto lookup_mem_const = [&](uint8_t rn, int32_t offset,
                                uint32_t* out) -> bool {
        for (const auto& slot : mem_const) {
            if (slot.known && slot.rn == rn && slot.offset == offset) {
                *out = slot.value;
                return true;
            }
        }
        return false;
    };

    auto forget_mem_const = [&](uint8_t rn, int32_t offset) {
        for (auto& slot : mem_const) {
            if (slot.known && slot.rn == rn && slot.offset == offset) {
                slot.known = false;
            }
        }
    };

    auto reg_list_count = [](uint16_t list) -> int {
        int n = 0;
        while (list != 0) {
            n += static_cast<int>(list & 1u);
            list >>= 1;
        }
        return n;
    };

    auto regs_before = [](uint16_t list, int reg) -> int {
        int n = 0;
        for (int r = 0; r < reg; ++r) {
            if (list & (1u << r)) ++n;
        }
        return n;
    };

    auto eval_block_reg_offset = [&](const armv4t::Instr& i, int reg,
                                     bool relative_to_writeback_base,
                                     int32_t* offset) -> bool {
        if (i.block.rn >= 15) return false;
        uint16_t list = i.block.reg_list;
        bool empty_list = (list == 0);
        if (empty_list && reg != 15) return false;
        if (!empty_list && (list & (1u << reg)) == 0) return false;
        int n = empty_list ? 1 : reg_list_count(list);

        int32_t start = 0;
        if (empty_list) {
            if (i.block.add) {
                start = i.block.pre_indexed ? 4 : 0;
            } else {
                start = i.block.pre_indexed ? -0x40 : -0x3c;
            }
        } else if (i.block.add) {
            start = i.block.pre_indexed ? 4 : 0;
        } else {
            start = i.block.pre_indexed ? -4 * n : -4 * (n - 1);
        }
        int32_t final_delta = i.block.add
            ? (empty_list ? 0x40 : 4 * n)
            : (empty_list ? -0x40 : -4 * n);
        int32_t addr_offset = start +
            (empty_list ? 0 : 4 * regs_before(list, reg));
        int32_t base_delta =
            (relative_to_writeback_base && i.block.writeback)
                ? final_delta : 0;
        *offset = addr_offset - base_delta;
        return true;
    };

    auto enqueue_resolved_target = [&](uint32_t raw, CpuMode target_mode,
                                       const char* kind) {
        uint32_t target = raw & ~uint32_t{1};
        const uint32_t target_step =
            (target_mode == CpuMode::Thumb) ? 2u : 4u;
        if (target == 0 || !can_read_at(target, target_step)) return;
        if (addr_in_data_range(target)) {
            record_collision(target, fn.addr, fn.name, kind);
            return;
        }
        if (target_mode == entry_mode) {
            fn.direct_branch_targets.push_back(target);
        } else {
            mode_switch_seeds_.push_back(
                FunctionSeed{target, target_mode, ""});
        }
        ++stats_.branch_targets_discovered;
    };

    // Code-vs-data discriminator for a jump-table entry target. Accepts
    // any plausible CODE target (NOT just a function prologue — a real
    // `switch` table points at mid-function case labels that open with
    // arbitrary instructions, e.g. `add r0,r4,#0`), while still rejecting
    // DATA words that merely happen to fall in ROM. The old gate required
    // a `push`/`stmfd` prologue and so rejected every switch table whose
    // cases aren't function entries (this is exactly what blocked
    // MC-HP-001). Checks, in order: correct alignment for the mode; lands
    // in ROM and not in a known data_range; decodes to defined
    // instruction(s) in `mode`. `strict` (used only when no exact count
    // bound exists) decodes a short run and requires every instruction
    // defined — a single `!is_undefined` is too weak to terminate an
    // unbounded walk, since most 0x08xxxxxx words decode to *something*.
    auto looks_like_code = [&](uint32_t tgt, CpuMode mode,
                               bool strict) -> bool {
        if (mode == CpuMode::Thumb) {
            if (tgt & 1u) return false;
        } else if (tgt & 3u) {
            return false;
        }
        if (!addr_in_rom(tgt) || addr_in_data_range(tgt)) return false;
        uint32_t src = 0;
        if (!map_addr_to_source(tgt, &src)) return false;
        const uint32_t k = strict ? 6u : 1u;
        uint32_t a = tgt;
        for (uint32_t j = 0; j < k; ++j) {
            if (mode == CpuMode::Thumb) {
                if (!can_read_at(a, 2)) return j > 0;
                if (armv4t::ThumbDecoder::decode(read_u16(a), a).is_undefined)
                    return false;
                a += 2;
            } else {
                if (!can_read_at(a, 4)) return j > 0;
                if (armv4t::ArmDecoder::decode(read_u32(a), a).is_undefined)
                    return false;
                a += 4;
            }
        }
        return true;
    };

    // Recognize an abs32 jump table at `base` revealed by an indexed load
    // at `site_pc`. Mirrors the manual [[jump_table]] expansion in
    // main.cpp (mark table bytes as data + seed each entry) but derives
    // base/count/mode automatically.
    //
    //  * `interworking`  — how the dispatcher consumes an entry. true for
    //    `BX Rt` / a `bx`-veneer reached by BL: bit0 of each word selects
    //    ARM/THUMB (the classic abs32-with-thumb-bit table). false for
    //    `MOV pc,Rt`: a non-interworking write to PC that KEEPS the
    //    dispatcher's current mode, so the words carry no mode bit and
    //    every target inherits `entry_mode`. Deriving mode from bit0 here
    //    (the old behavior) mis-seeded MC-HP-001's even THUMB targets as
    //    ARM.
    //  * `count_hint`    — exact entry count from the dispatcher's
    //    `CMP index,#N; B{hi,cs} default` bound, or 0 if none was
    //    recovered. When known it is the authoritative terminator (no
    //    guessing); a within-range entry that fails the code check then
    //    means the detection was bogus, so the whole table is rejected.
    //    When unknown we fall back to a strict validate-and-stop walk.
    //
    // Overlap with a manual entry is benign: a manual [[jump_table]]
    // already marks the bytes as a data_range, so the walk yields nothing.
    auto emit_jump_table = [&](uint32_t base, uint32_t stride,
                               uint32_t site_pc, bool interworking,
                               uint32_t count_hint) {
        if (stride != 4) return;  // abs32 only in this pass
        // Account each distinct base once, no matter how many overlapping
        // seed walks confirm the same dispatcher.
        if (!jt_seen_bases_.insert(base).second) return;
        const bool bounded = count_hint > 0;
        const uint32_t limit = bounded ? count_hint : 1024u;
        std::vector<FunctionSeed> targets;
        // Benign overlap: the table bytes are already covered by a
        // data_range (a manual [[jump_table]] hint, or a prior auto table).
        // The walk yields nothing and we suppress silently — this is the
        // detector independently rediscovering a hand-annotated table, not
        // a failure. Distinguished from a genuine bound mismatch below.
        bool overlap = false;
        for (uint32_t i = 0; i < limit; ++i) {
            uint32_t pos = base + i * stride;
            if (!can_read_at(pos, 4)) break;
            if (addr_in_data_range(pos)) { if (i == 0) overlap = true; break; }
            uint32_t raw = read_u32(pos);
            CpuMode tmode;
            uint32_t tgt;
            if (interworking) {
                tmode = (raw & 1u) ? CpuMode::Thumb : CpuMode::Arm;
                tgt = raw & ~uint32_t{1};
            } else {
                tmode = entry_mode;  // MOV pc keeps the dispatcher's mode
                tgt = raw & (entry_mode == CpuMode::Thumb ? ~uint32_t{1}
                                                          : ~uint32_t{3});
            }
            if (tgt == 0 || !looks_like_code(tgt, tmode, /*strict=*/!bounded))
                break;
            targets.push_back(FunctionSeed{tgt, tmode, ""});
        }

        const bool ok = bounded
            ? (targets.size() == count_hint)  // every bounded entry was code
            : (targets.size() >= 2);          // long enough to trust
        if (jt_report_enabled()) {
            std::fprintf(stderr,
                "[jt] base=0x%08X site=0x%08X %-6s bound=%-4s "
                "want=%u got=%zu -> %s\n",
                base, site_pc, interworking ? "BX" : "MOVpc",
                bounded ? "yes" : "no", count_hint, targets.size(),
                ok ? "EMIT" : (overlap ? "overlap" : "reject"));
        }
        if (!ok) {
            if (overlap)       ++stats_.jt_overlap_suppressed;
            else if (bounded)  ++stats_.jt_rejected_bound_mismatch;
            else               ++stats_.jt_rejected_unsized;
            return;
        }

        uint32_t count = static_cast<uint32_t>(targets.size());
        data_ranges_.push_back(
            DataRange{base, base + count * stride, "auto jump_table"});
        for (uint32_t i = 0; i < count; ++i) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "autojt_%08X_%02u", base, i);
            targets[i].name = buf;
            mode_switch_seeds_.push_back(targets[i]);
            ++stats_.auto_jump_table_targets;
        }
        auto_jump_tables_.push_back(
            AutoJumpTable{base, stride, count, site_pc, entry_mode,
                          interworking, bounded});
        ++stats_.auto_jump_tables;
    };

    auto eval_dp_imm_pc_write = [&](const armv4t::Instr& i,
                                    uint32_t* out) -> bool {
        if (i.rd != 15 || i.op2.kind != armv4t::Op2::Kind::Imm) {
            return false;
        }

        switch (i.op) {
            case armv4t::IrOp::MOV:
                *out = i.op2.imm_value;
                return true;
            case armv4t::IrOp::MVN:
                *out = ~i.op2.imm_value;
                return true;
            default:
                break;
        }

        uint32_t lhs = 0;
        if (i.rn == 15) {
            lhs = i.pc + (entry_mode == CpuMode::Thumb ? 4u : 8u);
        } else if (i.rn < 16 && reg_const[i.rn].known) {
            lhs = reg_const[i.rn].value;
        } else {
            return false;
        }

        uint32_t rhs = i.op2.imm_value;
        switch (i.op) {
            case armv4t::IrOp::ADD:
                *out = lhs + rhs;
                return true;
            case armv4t::IrOp::SUB:
                *out = lhs - rhs;
                return true;
            case armv4t::IrOp::RSB:
                *out = rhs - lhs;
                return true;
            case armv4t::IrOp::AND:
                *out = lhs & rhs;
                return true;
            case armv4t::IrOp::EOR:
                *out = lhs ^ rhs;
                return true;
            case armv4t::IrOp::ORR:
                *out = lhs | rhs;
                return true;
            case armv4t::IrOp::BIC:
                *out = lhs & ~rhs;
                return true;
            default:
                // ADC/SBC/RSC depend on CPSR.C; leave opaque.
                return false;
        }
    };

    auto eval_dp_reg_pc_write = [&](const armv4t::Instr& i,
                                    uint32_t* out) -> bool {
        if (i.rd != 15 ||
            i.op2.kind != armv4t::Op2::Kind::Shifted ||
            i.op2.shifted.by_register ||
            i.op2.shifted.imm_or_rs != 0 ||
            i.op2.shifted.rm >= 16 ||
            !reg_const[i.op2.shifted.rm].known) {
            return false;
        }

        uint32_t rhs = reg_const[i.op2.shifted.rm].value;
        switch (i.op) {
            case armv4t::IrOp::MOV:
                *out = rhs;
                return true;
            case armv4t::IrOp::MVN:
                *out = ~rhs;
                return true;
            default:
                break;
        }

        uint32_t lhs = 0;
        if (i.rn == 15) {
            lhs = i.pc + (entry_mode == CpuMode::Thumb ? 4u : 8u);
        } else if (i.rn < 16 && reg_const[i.rn].known) {
            lhs = reg_const[i.rn].value;
        } else {
            return false;
        }

        switch (i.op) {
            case armv4t::IrOp::ADD:
                *out = lhs + rhs;
                return true;
            case armv4t::IrOp::SUB:
                *out = lhs - rhs;
                return true;
            case armv4t::IrOp::RSB:
                *out = rhs - lhs;
                return true;
            case armv4t::IrOp::AND:
                *out = lhs & rhs;
                return true;
            case armv4t::IrOp::EOR:
                *out = lhs ^ rhs;
                return true;
            case armv4t::IrOp::ORR:
                *out = lhs | rhs;
                return true;
            case armv4t::IrOp::BIC:
                *out = lhs & ~rhs;
                return true;
            default:
                return false;
        }
    };

    auto eval_mem_imm_addr = [&](const armv4t::Instr& i,
                                 uint32_t* out) -> bool {
        if (i.mem.by_register) return false;

        uint32_t base = 0;
        if (i.mem.rn == 15) {
            base = i.pc + (entry_mode == CpuMode::Thumb ? 4u : 8u);
            if (entry_mode == CpuMode::Thumb) base &= ~uint32_t{3};
        } else if (i.mem.rn < 16 && reg_const[i.mem.rn].known) {
            base = reg_const[i.mem.rn].value;
        } else {
            return false;
        }

        uint32_t indexed = i.mem.add
            ? (base + i.mem.imm_offset)
            : (base - i.mem.imm_offset);
        *out = i.mem.pre_indexed ? indexed : base;
        return true;
    };

    while (count++ < kMaxInstrs && can_read_at(pc, step) &&
           (exact_end == 0u || pc < exact_end)) {
        // If we ever decode from inside a data_range, that's a hard
        // error — TOML asserted "no code here" but our walk got here
        // anyway. Recorded; finder continues so multiple collisions
        // surface at once.
        if (addr_in_data_range(pc)) {
            record_collision(pc, fn.addr, fn.name, "branch");
            break;
        }

        armv4t::Instr ins;
        if (entry_mode == CpuMode::Thumb) {
            ins = armv4t::ThumbDecoder::decode(read_u16(pc), pc);
        } else {
            ins = armv4t::ArmDecoder::decode(read_u32(pc), pc);
        }

        if (ins.is_undefined) {
            ++stats_.undefined_instr_count;
            break;
        }

        // Record direct branch targets. The decoder fills
        // branch_target with the absolute guest address.
        const bool thumb_bl_half =
            entry_mode == CpuMode::Thumb &&
            (ins.op == armv4t::IrOp::BL_prefix ||
             ins.op == armv4t::IrOp::BL_suffix);
        if (ins.is_branch && !ins.is_indirect &&
            ins.branch_target != 0 && !thumb_bl_half) {
            // BLX (immediate) switches instruction set: an ARM caller
            // targets THUMB and vice-versa. Route it to the opposite-mode
            // seed list so the target is decoded in the right state.
            const bool mode_switch = (ins.op == armv4t::IrOp::BLX_imm);
            const CpuMode target_mode = mode_switch
                ? (entry_mode == CpuMode::Arm ? CpuMode::Thumb : CpuMode::Arm)
                : entry_mode;
            if (addr_in_data_range(ins.branch_target)) {
                record_collision(ins.branch_target, fn.addr,
                                  fn.name, "branch");
                // Don't push the target as a target; the collision
                // is already recorded. Drop it to avoid a worklist
                // entry that would re-trip the same error.
            } else if (mode_switch) {
                mode_switch_seeds_.push_back(FunctionSeed{
                    ins.branch_target & ~uint32_t{1}, target_mode, ""});
                ++stats_.branch_targets_discovered;
            } else {
                fn.direct_branch_targets.push_back(ins.branch_target);
                ++stats_.branch_targets_discovered;
            }
        }

        // THUMB BL pair: the decoder splits BL into BL_prefix
        // (upper halfword) + BL_suffix (lower halfword). Neither
        // half alone has the FULL target — prefix has
        // branch_target = (PC+4)+(upper<<12) which is just an
        // intermediate base, suffix has branch_target = 0 plus
        // swi_imm = (imm11<<1). Combining them gives the real
        // target. We do the combine here in the finder so the
        // worklist gets the right address.
        if (entry_mode == CpuMode::Thumb &&
            ins.op == armv4t::IrOp::BL_prefix &&
            can_read_at(pc + 2, 2)) {
            armv4t::Instr lower = armv4t::ThumbDecoder::decode(
                read_u16(pc + 2), pc + 2);
            if (lower.op == armv4t::IrOp::BL_suffix) {
                // BLX (branch_exchange) targets ARM, word-aligned, and
                // switches mode; plain BL stays THUMB.
                const bool blx = lower.branch_exchange;
                uint32_t full = ins.branch_target + lower.swi_imm;
                full &= blx ? ~uint32_t{3} : ~uint32_t{1};
                const CpuMode tmode = blx ? CpuMode::Arm : CpuMode::Thumb;
                if (full != 0 && can_read_at(full, blx ? 4u : 2u)) {
                    if (addr_in_data_range(full)) {
                        record_collision(full, fn.addr, fn.name,
                                          "branch");
                    } else if (blx) {
                        mode_switch_seeds_.push_back(
                            FunctionSeed{full, tmode, ""});
                        ++stats_.branch_targets_discovered;
                    } else {
                        fn.direct_branch_targets.push_back(full);
                        ++stats_.branch_targets_discovered;
                    }
                }

                // The instruction after a THUMB BL pair is always a
                // valid continuation address. Some real BIOS paths
                // and gba-suite cases later reach that continuation
                // through an indirect transfer (saved LR, empty-list
                // LDM, callback trampoline). Seed the continuation
                // explicitly instead of relying on the BL_prefix
                // half's intermediate target, which is not the call
                // target and can point into data for long BLs.
                uint32_t return_pc = pc + 4u;
                if (can_read_at(return_pc, 2)) {
                    enqueue_resolved_target(return_pc, entry_mode, "branch");
                }
            }
        }

        // Indirect = computed; we can't follow statically — UNLESS
        // our narrow constant tracker resolved the source register
        // to a known value just above the BX (or MOV PC, Rn).
        if (ins.op == armv4t::IrOp::SWI) {
            enqueue_resolved_target(ins.pc + step, entry_mode, "branch");
        }

        if (ins.is_indirect) {
            fn.has_indirect_transfer = true;
            ++stats_.indirect_transfer_count;

            // Some PC-writing instructions look "indirect" to the
            // decoder but still have a fully-known target. Keep this
            // intentionally narrow so misses stay loud and false
            // positives stay rare.
            bool indirect_resolved = false;
            uint32_t resolved = 0;
            if (eval_dp_imm_pc_write(ins, &resolved)) {
                indirect_resolved = true;
                CpuMode target_mode = entry_mode;
                if (ins.rn != 15 && (resolved & 1u)) {
                    target_mode = CpuMode::Thumb;
                }
                enqueue_resolved_target(resolved, target_mode, "branch");
            } else if (eval_dp_reg_pc_write(ins, &resolved)) {
                indirect_resolved = true;
                enqueue_resolved_target(resolved, entry_mode, "branch");
            } else if (ins.op == armv4t::IrOp::LDR && ins.rd == 15) {
                uint32_t ea = 0;
                if (eval_mem_imm_addr(ins, &ea) && can_read_at(ea, 4)) {
                    indirect_resolved = true;
                    uint32_t raw = read_u32(ea & ~uint32_t{3});
                    raw = ror32(raw, (ea & 3u) * 8u);
                    CpuMode target_mode = (raw & 1u)
                        ? CpuMode::Thumb : entry_mode;
                    enqueue_resolved_target(raw, target_mode, "branch");
                } else {
                    uint8_t rn = 0;
                    int32_t offset = 0;
                    uint32_t raw = 0;
                    if (eval_symbolic_mem_addr(ins, &rn, &offset) &&
                        lookup_mem_const(rn, offset, &raw)) {
                        indirect_resolved = true;
                        CpuMode target_mode = (raw & 1u)
                            ? CpuMode::Thumb : entry_mode;
                        enqueue_resolved_target(raw, target_mode, "branch");
                    }
                }
            } else if (ins.op == armv4t::IrOp::LDM &&
                       ins.block.load &&
                       ((ins.block.reg_list & (1u << 15)) != 0 ||
                        ins.block.reg_list == 0)) {
                int32_t offset = 0;
                uint8_t rn = 0;
                int32_t resolved_offset = 0;
                uint32_t raw = 0;
                if (eval_block_reg_offset(
                        ins, 15, /*relative_to_writeback_base=*/false,
                        &offset) &&
                    resolve_symbolic_base(ins.block.rn, offset,
                                          &rn, &resolved_offset) &&
                    lookup_mem_const(rn, resolved_offset, &raw)) {
                    indirect_resolved = true;
                    enqueue_resolved_target(raw, entry_mode, "branch");
                }
            }

            // Two cases land here with a tracked register source:
            //   1. BX Rm                  — `ins.rm` is the source
            //   2. MOV PC, Rn (DP-MOV)    — `ins.op2.shifted.rm`
            //      is the source (op2 with no shift)
            // BX is the ARMv4T interworking instruction, so it gets
            // target mode from bit 0. Plain DP writes to PC keep the
            // current instruction-set state; bit 0 is not an exchange.
            uint8_t src_reg = 255;  // sentinel
            bool src_is_bx = false;
            if ((ins.op == armv4t::IrOp::BX ||
                 ins.op == armv4t::IrOp::BLX_reg) && ins.rm < 16) {
                // BX and BLX(reg) both interwork from Rm bit0.
                src_reg = ins.rm;
                src_is_bx = true;
            } else if (ins.rd == 15 &&
                       ins.op2.kind == armv4t::Op2::Kind::Shifted &&
                       !ins.op2.shifted.by_register &&
                       ins.op2.shifted.imm_or_rs == 0 &&
                       ins.op2.shifted.rm < 16) {
                // DP write to PC with op2 = plain register: this is
                // either `mov pc, Rn` (computed jump) or some other
                // DP-PC pattern. If the source register is tracked,
                // we know the target.
                src_reg = ins.op2.shifted.rm;
            }
            if (src_reg < 16) {
                uint32_t raw = 0;
                bool raw_known = false;
                if (src_reg == 15) {
                    raw = ins.pc + (entry_mode == CpuMode::Thumb ? 4u : 8u);
                    if (entry_mode == CpuMode::Thumb) raw &= ~uint32_t{3};
                    raw_known = true;
                } else if (reg_const[src_reg].known) {
                    raw = reg_const[src_reg].value;
                    raw_known = true;
                }
                if (raw_known) {
                    indirect_resolved = true;
                    uint32_t tgt = raw & ~uint32_t{1};
                    CpuMode tgt_mode = entry_mode;
                    if (src_is_bx) {
                        tgt_mode = (raw & 1u) ? CpuMode::Thumb : CpuMode::Arm;
                    }
                    if (tgt != 0 && can_read_at(
                            tgt, tgt_mode == CpuMode::Thumb ? 2u : 4u) &&
                        !addr_in_data_range(tgt)) {
                        mode_switch_seeds_.push_back(
                            FunctionSeed{tgt, tgt_mode, ""});
                        ++stats_.branch_targets_discovered;
                    }
                }
            }
            if (ins.op == armv4t::IrOp::BX &&
                ins.rm != 14 &&
                reg_const[14].known) {
                uint32_t raw = reg_const[14].value;
                CpuMode return_mode = (raw & 1u)
                    ? CpuMode::Thumb : CpuMode::Arm;
                enqueue_resolved_target(raw, return_mode, "branch");
            }

            // Tier-2 landing-pad auto-discovery. The return-pad idiom sets LR
            // to a code address, then transfers control OUT via an indirect
            // PC-write the finder can't resolve (e.g. `adr lr,X; ldr pc,
            // [dtcm+0x3FFC]` — the BIOS IRQ handler calling the firmware's
            // installed handler). The callee returns with `bx lr`, landing at
            // X, which NO static branch ever targets — so without this it is a
            // dispatch miss. Seed X as a dispatch entry (the finder then splits
            // the containing function there, exactly like any other entry).
            //
            // Soundness gates (see the multi-entry-dispatch design):
            //   - LR holds a statically-known constant — reg_const tracks the
            //     load and clears on any clobber, so "known" ⇒ live + unclobbered;
            //   - it points into executable ROM and decodes to a valid aligned
            //     instruction (looks_like_code);
            //   - the transfer is genuinely UNRESOLVED (else the callee is known
            //     and there is nothing to call back to);
            //   - the transfer is a call-OUT, not a return shape (bx/mov pc via
            //     LR, or any LDM pop {...,pc}) — those land AT the pad, not past it.
            if (!indirect_resolved && reg_const[14].known) {
                bool is_return_shape = false;
                if (ins.op == armv4t::IrOp::LDM) {
                    is_return_shape = true;  // pop {...,pc} is a return
                } else if ((ins.op == armv4t::IrOp::BX ||
                            ins.op == armv4t::IrOp::BLX_reg) &&
                           ins.rm == 14) {
                    is_return_shape = true;  // bx lr
                } else if (ins.rd == 15 &&
                           ins.op2.kind == armv4t::Op2::Kind::Shifted &&
                           !ins.op2.shifted.by_register &&
                           ins.op2.shifted.imm_or_rs == 0 &&
                           ins.op2.shifted.rm == 14) {
                    is_return_shape = true;  // mov pc, lr
                }
                uint32_t pad_raw = reg_const[14].value;
                uint32_t pad = pad_raw & ~uint32_t{1};
                CpuMode pad_mode = (pad_raw & 1u) ? CpuMode::Thumb
                                                  : CpuMode::Arm;
                if (!is_return_shape && pad != 0 &&
                    addr_in_rom(pad) && !addr_in_data_range(pad) &&
                    looks_like_code(pad, pad_mode, /*strict=*/false)) {
                    char nm[64];
                    std::snprintf(nm, sizeof nm, "lpad_%08X", pad);
                    mode_switch_seeds_.push_back(
                        FunctionSeed{pad, pad_mode, nm});
                    ++stats_.landing_pads_discovered;
                }
            }
        }

        // Jump-table confirmation (MC-HP-000). If a candidate from a
        // prior indexed load is pending, check whether THIS instruction
        // uses the loaded register as a control-flow target. That
        // promotion is the proof it's a jump table rather than an
        // indexed DATA-pointer load (which is dereferenced, not branched
        // to). Without this gate the detector seeds garbage and the walk
        // explodes.
        if (pend.active) {
            bool confirmed = false;
            // How the dispatcher consumes the loaded entry decides target
            // mode: BX / bx-veneer interwork on bit0; MOV pc keeps the
            // dispatcher's current mode. See emit_jump_table().
            bool interworking = true;
            const armv4t::Op2& po = ins.op2;
            if (ins.op == armv4t::IrOp::BX && ins.rm == pend.dest) {
                confirmed = true;  // BX dest
                interworking = true;
            } else if (ins.rd == 15 &&
                       po.kind == armv4t::Op2::Kind::Shifted &&
                       !po.shifted.by_register &&
                       po.shifted.imm_or_rs == 0 &&
                       po.shifted.rm == pend.dest) {
                confirmed = true;  // MOV pc, dest (computed jump)
                interworking = false;
            } else if (ins.is_call && ins.branch_target != 0 &&
                       can_read_at(ins.branch_target, 2)) {
                // ARM BL into a `bx dest` veneer (single-instruction BL).
                armv4t::Instr v = armv4t::ThumbDecoder::decode(
                    read_u16(ins.branch_target), ins.branch_target);
                if (v.op == armv4t::IrOp::BX && v.rm == pend.dest) {
                    confirmed = true;
                }
            } else if (entry_mode == CpuMode::Thumb &&
                       ins.op == armv4t::IrOp::BL_prefix &&
                       can_read_at(ins.pc + 2, 2)) {
                // THUMB BL is a prefix/suffix halfword PAIR; the full
                // target only exists once combined. This is the dominant
                // dispatch form — `bl <bx-dest veneer>` — so without it
                // every THUMB-BL-dispatched table is missed. Combine the
                // pair (same math as the finder's direct-BL handling),
                // then check the veneer it lands on.
                armv4t::Instr lo = armv4t::ThumbDecoder::decode(
                    read_u16(ins.pc + 2), ins.pc + 2);
                if (lo.op == armv4t::IrOp::BL_suffix) {
                    uint32_t full =
                        (ins.branch_target + lo.swi_imm) & ~uint32_t{1};
                    if (can_read_at(full, 2)) {
                        armv4t::Instr v = armv4t::ThumbDecoder::decode(
                            read_u16(full), full);
                        if (v.op == armv4t::IrOp::BX &&
                            v.rm == pend.dest) {
                            confirmed = true;
                        }
                    }
                }
            }
            if (confirmed) {
                ++stats_.jt_confirmations;
                emit_jump_table(pend.base, pend.stride, ins.pc, interworking,
                                pend.count_known ? pend.count : 0u);
                pend.active = false;
            } else if (ins.rd == pend.dest) {
                pend.active = false;  // dest overwritten before any branch
            }
        }

        // Jump-table detection. An indexed load of a would-be table entry
        // whose address is (tracked ROM-pointer base) + (index<<2). Two
        // shapes:
        //   (a) register-offset:  LDR Rt,[Rb, Ri, LSL #2]
        //   (b) the common THUMB form where a prior ADD folded
        //       base + (index<<2) into reg_table[] (see maintenance below).
        // Records a pending candidate; emission waits for the
        // confirmation above. Detection runs before the tracker update so
        // reg_table[] from the preceding ADD is still live even when
        // Rt == the base reg.
        if (jump_table_detection_enabled() && ins.op == armv4t::IrOp::LDR) {
            uint32_t jt_base = 0;
            bool jt = false;
            bool     count_known = false;
            uint32_t count = 0;
            if (ins.mem.by_register && ins.mem.rn < 16 &&
                ins.mem.reg_offset.rm < 16 &&
                ins.mem.reg_offset.type == armv4t::ShiftType::LSL &&
                !ins.mem.reg_offset.by_register &&
                ins.mem.reg_offset.imm_or_rs == 2 &&
                reg_const[ins.mem.rn].known &&
                addr_in_rom(reg_const[ins.mem.rn].value)) {
                jt_base = reg_const[ins.mem.rn].value;
                jt = true;
                // Inline-scaled form: the index is the offset register.
                uint8_t ireg = ins.mem.reg_offset.rm;
                if (reg_bound[ireg].known) {
                    count_known = true;
                    count = reg_bound[ireg].max_index + 1u;
                }
            } else if (!ins.mem.by_register && ins.mem.rn < 16 &&
                       reg_table[ins.mem.rn].known &&
                       reg_table[ins.mem.rn].stride == 4) {
                int32_t off = 0;
                if (ins.mem.pre_indexed) {
                    int32_t imm = static_cast<int32_t>(ins.mem.imm_offset);
                    off = ins.mem.add ? imm : -imm;
                }
                jt_base = reg_table[ins.mem.rn].base +
                          static_cast<uint32_t>(off);
                jt = true;
                if (reg_table[ins.mem.rn].bound_known) {
                    count_known = true;
                    count = reg_table[ins.mem.rn].max_index + 1u;
                }
            }
            if (jt) {
                pend.active      = true;
                pend.base        = jt_base;
                pend.stride      = 4;
                pend.dest        = ins.rd;
                pend.count_known = count_known;
                pend.count       = count;
            }
        }

        // CMP-bound capture for exact jump-table sizing (MC-HP-000). The
        // canonical switch guard is `CMP Ri,#N` immediately followed by a
        // `B{hi,cs}` to the default case; the in-range index span gives the
        // table's entry count with no guessing. Record the compare, then on
        // the very next instruction (the guard branch) stamp reg_bound[Ri].
        // The bound then rides the index through the scale/add chain below.
        if (ins.op == armv4t::IrOp::CMP &&
            ins.op2.kind == armv4t::Op2::Kind::Imm && ins.rn < 16) {
            last_cmp.known = true;
            last_cmp.reg   = ins.rn;
            last_cmp.imm   = ins.op2.imm_value;
            last_cmp.pc    = ins.pc;
        } else if (last_cmp.known && ins.is_branch &&
                   ins.pc == last_cmp.pc + step) {
            // Max in-range index from the compare. HI/LS bound at N
            // (Ri {>,<=} N); CS/CC bound at N-1 (Ri {>=,<} N).
            bool have = false; uint32_t maxi = 0;
            if (ins.cond == armv4t::Cond::HI ||
                ins.cond == armv4t::Cond::LS) {
                have = true; maxi = last_cmp.imm;
            } else if ((ins.cond == armv4t::Cond::CS ||
                        ins.cond == armv4t::Cond::CC) && last_cmp.imm > 0) {
                have = true; maxi = last_cmp.imm - 1u;
            }
            if (have) {
                // HI/CS: taken branches AWAY to the default, so the
                // indexed dispatch is the fall-through of THIS walk —
                // set the bound directly. LS/CC: taken branches TO the
                // dispatch, which the finder walks as its own seed —
                // park the bound for that target (re-seeded at entry).
                if (ins.cond == armv4t::Cond::HI ||
                    ins.cond == armv4t::Cond::CS) {
                    reg_bound[last_cmp.reg] = {true, maxi};
                } else if (ins.branch_target != 0) {
                    branch_target_bounds_[ins.branch_target] =
                        SeedBound{last_cmp.reg, maxi};
                }
            }
            last_cmp.known = false;
        } else {
            // any other instruction ends the (adjacent-only) cmp window
            last_cmp.known = false;
        }

        if (ins.op == armv4t::IrOp::STR) {
            uint8_t rn = 0;
            int32_t offset = 0;
            uint32_t stored = 0;
            bool known_value = false;
            if (ins.rd == 15) {
                stored = ins.pc + (entry_mode == CpuMode::Thumb ? 4u : 12u);
                if (entry_mode == CpuMode::Thumb) {
                    stored &= ~uint32_t{3};
                }
                known_value = true;
            } else if (ins.rd < 16 && reg_const[ins.rd].known) {
                stored = reg_const[ins.rd].value;
                known_value = true;
            }

            if (!ins.mem.writeback &&
                eval_symbolic_mem_addr(ins, &rn, &offset) &&
                known_value) {
                remember_mem_const(rn, offset, stored);
            } else {
                clear_mem_const();
            }
        } else if (ins.op == armv4t::IrOp::STRB ||
                   ins.op == armv4t::IrOp::STRH) {
            clear_mem_const();
        } else if (ins.op == armv4t::IrOp::STM && !ins.block.load) {
            if (ins.block.rn >= 15 || ins.block.reg_list == 0) {
                clear_mem_const();
            } else if (ins.block.writeback &&
                       reg_sym[ins.block.rn].known) {
                clear_mem_const();
            } else {
                if (ins.block.writeback) {
                    invalidate_mem_base(ins.block.rn);
                }
                for (int reg = 0; reg < 16; ++reg) {
                    if ((ins.block.reg_list & (1u << reg)) == 0) continue;
                    int32_t offset = 0;
                    if (!eval_block_reg_offset(
                            ins, reg,
                            /*relative_to_writeback_base=*/true,
                            &offset)) {
                        clear_mem_const();
                        break;
                    }

                    uint32_t stored = 0;
                    bool known_value = false;
                    if (reg == 15) {
                        stored = ins.pc +
                            (entry_mode == CpuMode::Thumb ? 4u : 12u);
                        if (entry_mode == CpuMode::Thumb) {
                            stored &= ~uint32_t{3};
                        }
                        known_value = true;
                    } else if (reg_const[reg].known) {
                        stored = reg_const[reg].value;
                        known_value = true;
                    }

                    uint8_t rn = 0;
                    int32_t resolved_offset = 0;
                    bool resolved_base = resolve_symbolic_base(
                        ins.block.rn, offset, &rn, &resolved_offset);

                    if (known_value && resolved_base) {
                        remember_mem_const(rn, resolved_offset, stored);
                    } else if (resolved_base) {
                        forget_mem_const(rn, resolved_offset);
                    } else {
                        clear_mem_const();
                    }
                }
            }
        } else if (ins.op == armv4t::IrOp::LDM &&
                   ins.block.load &&
                   ins.block.writeback &&
                   ins.block.rn < 16) {
            reg_const[ins.block.rn].known = false;
            reg_sym[ins.block.rn].known = false;
            reg_scaled[ins.block.rn].known = false;
            reg_table[ins.block.rn].known = false;
            invalidate_mem_base(ins.block.rn);
            invalidate_sym_base(ins.block.rn);
        }

        // Update the narrow constant tracker. Do this AFTER the
        // BX check above (we want to read the value as-of the BX,
        // not after the BX clears it).
        //
        // MOV Rd, #imm  →  track value
        // ADD Rd, PC, #imm  →  track (pc+8 ARM, pc+4 THUMB) + imm
        // Anything else writing Rd  →  invalidate
        if (ins.rd < 16) {
            if (ins.op == armv4t::IrOp::MOV &&
                ins.op2.kind == armv4t::Op2::Kind::Imm) {
                reg_const[ins.rd].known = true;
                reg_const[ins.rd].value = ins.op2.imm_value;
                reg_sym[ins.rd].known = false;
            } else if (ins.op == armv4t::IrOp::MOV &&
                       ins.op2.kind == armv4t::Op2::Kind::Shifted &&
                       !ins.op2.shifted.by_register &&
                       ins.op2.shifted.imm_or_rs == 0 &&
                       ins.op2.shifted.rm < 16) {
                uint8_t rm = ins.op2.shifted.rm;
                if (rm == 15) {
                    uint32_t pc_val = ins.pc +
                        (entry_mode == CpuMode::Thumb ? 4u : 8u);
                    if (entry_mode == CpuMode::Thumb) pc_val &= ~uint32_t{3};
                    reg_const[ins.rd].known = true;
                    reg_const[ins.rd].value = pc_val;
                } else if (reg_const[rm].known) {
                    reg_const[ins.rd].known = true;
                    reg_const[ins.rd].value = reg_const[rm].value;
                } else {
                    reg_const[ins.rd].known = false;
                }
                if (rm < 15) {
                    if (reg_sym[rm].known) {
                        reg_sym[ins.rd] = reg_sym[rm];
                    } else {
                        reg_sym[ins.rd].known = true;
                        reg_sym[ins.rd].rn = rm;
                        reg_sym[ins.rd].offset = 0;
                    }
                } else {
                    reg_sym[ins.rd].known = false;
                }
            } else if (ins.op == armv4t::IrOp::ADD &&
                       ins.rn == 15 &&
                       ins.op2.kind == armv4t::Op2::Kind::Imm) {
                uint32_t pc_val = ins.pc +
                    (entry_mode == CpuMode::Thumb ? 4u : 8u);
                if (entry_mode == CpuMode::Thumb) pc_val &= ~uint32_t{3};
                reg_const[ins.rd].known = true;
                reg_const[ins.rd].value = pc_val + ins.op2.imm_value;
                reg_sym[ins.rd].known = false;
            } else if ((ins.op == armv4t::IrOp::ADD ||
                        ins.op == armv4t::IrOp::SUB) &&
                       ins.rn < 16 &&
                       ins.op2.kind == armv4t::Op2::Kind::Imm &&
                       reg_const[ins.rn].known) {
                reg_const[ins.rd].known = true;
                if (ins.op == armv4t::IrOp::ADD) {
                    reg_const[ins.rd].value =
                        reg_const[ins.rn].value + ins.op2.imm_value;
                } else {
                    reg_const[ins.rd].value =
                        reg_const[ins.rn].value - ins.op2.imm_value;
                }
                reg_sym[ins.rd].known = false;
            } else if (ins.op == armv4t::IrOp::LDR &&
                       !ins.mem.writeback) {
                uint32_t ea = 0;
                if (eval_mem_imm_addr(ins, &ea) && can_read_at(ea, 4)) {
                    uint32_t raw = read_u32(ea & ~uint32_t{3});
                    raw = ror32(raw, (ea & 3u) * 8u);
                    reg_const[ins.rd].known = true;
                    reg_const[ins.rd].value = raw;
                } else {
                    reg_const[ins.rd].known = false;
                }
                reg_sym[ins.rd].known = false;
            } else {
                // Any other write to Rd invalidates the tracker
                // entry. We don't try to detect "no write" — a
                // few false-invalidations (over-clearing) are fine
                // and bias toward conservative behavior.
                reg_const[ins.rd].known = false;
                reg_sym[ins.rd].known = false;
            }
            invalidate_mem_base(ins.rd);
            invalidate_sym_base(ins.rd);
        }

        // Maintain the jump-table address trackers (independent of the
        // reg_const tracker above; consumed by the LDR detection). A
        // write to Rd clears its scaled/table state unless this same
        // instruction re-establishes it.
        if (ins.rd < 16) {
            bool set_scaled = false, set_table = false;
            const armv4t::Op2& o = ins.op2;
            if (ins.op == armv4t::IrOp::MOV &&
                o.kind == armv4t::Op2::Kind::Shifted &&
                !o.shifted.by_register &&
                o.shifted.type == armv4t::ShiftType::LSL &&
                o.shifted.imm_or_rs > 0 && o.shifted.rm < 16) {
                // Rd = (unknown index) << k
                reg_scaled[ins.rd].known = true;
                reg_scaled[ins.rd].shift = o.shifted.imm_or_rs;
                reg_scaled[ins.rd].bound_known = false;
                if (o.shifted.rm < 16 && reg_bound[o.shifted.rm].known) {
                    reg_scaled[ins.rd].bound_known = true;
                    reg_scaled[ins.rd].max_index =
                        reg_bound[o.shifted.rm].max_index;
                }
                set_scaled = true;
            } else if (ins.op == armv4t::IrOp::ADD &&
                       o.kind == armv4t::Op2::Kind::Shifted &&
                       !o.shifted.by_register && o.shifted.rm < 16 &&
                       ins.rn < 16) {
                const uint8_t rm = o.shifted.rm;
                auto rom_const = [&](uint8_t r) {
                    return reg_const[r].known &&
                           addr_in_rom(reg_const[r].value);
                };
                if (o.shifted.type == armv4t::ShiftType::LSL &&
                    o.shifted.imm_or_rs > 0) {
                    // One-instruction scaled add: ADD Rd, Rbase, Ri LSL#k
                    // (Ri is the inline-scaled index; bound rides on it.)
                    if (rom_const(ins.rn)) {
                        reg_table[ins.rd] = {true, reg_const[ins.rn].value,
                                             1u << o.shifted.imm_or_rs};
                        if (reg_bound[rm].known) {
                            reg_table[ins.rd].bound_known = true;
                            reg_table[ins.rd].max_index =
                                reg_bound[rm].max_index;
                        }
                        set_table = true;
                    }
                } else if (o.shifted.imm_or_rs == 0) {
                    // Plain reg add: one side a ROM-pointer base, the
                    // other a previously-scaled index (bound carried).
                    if (rom_const(ins.rn) && reg_scaled[rm].known) {
                        reg_table[ins.rd] = {true, reg_const[ins.rn].value,
                                             1u << reg_scaled[rm].shift};
                        if (reg_scaled[rm].bound_known) {
                            reg_table[ins.rd].bound_known = true;
                            reg_table[ins.rd].max_index =
                                reg_scaled[rm].max_index;
                        }
                        set_table = true;
                    } else if (rom_const(rm) && reg_scaled[ins.rn].known) {
                        reg_table[ins.rd] = {true, reg_const[rm].value,
                                             1u << reg_scaled[ins.rn].shift};
                        if (reg_scaled[ins.rn].bound_known) {
                            reg_table[ins.rd].bound_known = true;
                            reg_table[ins.rd].max_index =
                                reg_scaled[ins.rn].max_index;
                        }
                        set_table = true;
                    }
                }
            }
            if (!set_scaled) reg_scaled[ins.rd].known = false;
            if (!set_table)  reg_table[ins.rd].known = false;
            // The cmp-derived bound has been consumed into reg_scaled/
            // reg_table above; any non-branch write to Rd retires it. A
            // branch is skipped because the guard branch itself is what
            // SET the bound (see the CMP-bound capture above).
            if (!ins.is_branch) reg_bound[ins.rd].known = false;
        }

        if ((ins.op == armv4t::IrOp::LDR ||
             ins.op == armv4t::IrOp::LDRB ||
             ins.op == armv4t::IrOp::LDRH ||
             ins.op == armv4t::IrOp::LDRSB ||
             ins.op == armv4t::IrOp::LDRSH ||
             ins.op == armv4t::IrOp::STR ||
             ins.op == armv4t::IrOp::STRB ||
             ins.op == armv4t::IrOp::STRH) &&
            ins.mem.writeback && ins.mem.rn < 16) {
            reg_const[ins.mem.rn].known = false;
            reg_sym[ins.mem.rn].known = false;
            reg_scaled[ins.mem.rn].known = false;
            reg_table[ins.mem.rn].known = false;
            invalidate_mem_base(ins.mem.rn);
            invalidate_sym_base(ins.mem.rn);
        }

        // Seed every call's return address as a function entry. A guest
        // return reached AFTER a scheduler preemption is re-dispatched by
        // the run loop (the host C call stack having been unwound to swap
        // CPUs), so the return address must be a real dispatch entry, not
        // a mid-body location. THUMB BL pairs already seed their
        // continuation explicitly above, so skip those here.
        if (ins.is_call) {
            const bool thumb_bl_pair =
                entry_mode == CpuMode::Thumb &&
                (ins.op == armv4t::IrOp::BL_prefix ||
                 ins.op == armv4t::IrOp::BL_suffix);
            if (!thumb_bl_pair)
                enqueue_resolved_target(ins.pc + step, entry_mode,
                                        "call-return");
        }

        pc += step;
        fn.end_addr = pc;

        // A conditional indirect transfer is not a terminator when
        // its condition fails. Because the finder stops at indirects,
        // seed the fall-through root explicitly. An indirect CALL
        // (BLX register) likewise returns to the following instruction,
        // so seed its continuation even when unconditional.
        if ((ins.is_return || ins.is_indirect) &&
            (ins.cond != armv4t::Cond::AL || ins.is_call)) {
            enqueue_resolved_target(pc, entry_mode, "branch");
        }

        // Stop at unconditional terminators:
        //   1. A return-shaped instruction (BX LR, pop {pc}, ldm
        //      r?, {..., pc}).
        //   2. An indirect control transfer.
        //   3. An unconditional branch (B with cond=AL) — we'll
        //      reach the target by adding it to the worklist.
        if (ins.is_return || ins.is_indirect) {
            break;
        }
        // A THUMB BL/BLX is a prefix+suffix HALFWORD PAIR; the prefix
        // half is branch-shaped but must NOT end the function — the walk
        // has to continue through the suffix (the actual call) and the
        // code after it. Ending here splits the pair across a function
        // boundary, orphaning the suffix as a non-dispatchable address.
        // (`thumb_bl_half` was computed at the top of this iteration.)
        const bool unconditional_branch =
            ins.is_branch &&
            ins.cond == armv4t::Cond::AL &&
            !ins.is_call &&
            !thumb_bl_half;
        if (unconditional_branch) {
            break;
        }
    }

    functions_.push_back(std::move(fn));
}

void FunctionFinder::run(std::size_t max_functions) {
    // Validate seeds against data_ranges before any walking. A seed
    // landing inside a data_range is a collision (the TOML author
    // is contradicting themselves; or a jump_table expanded onto
    // bytes that another data_range marked as "not code").
    for (const auto& s : seeds_) {
        if (addr_in_data_range(s.addr)) {
            record_collision(s.addr, 0, s.name, "seed");
        }
    }

    // Snapshot seed (addr, mode) keys so we can later compute the
    // manual-seed/walk-overlap breakdown for the discovery summary.
    std::unordered_set<uint64_t> seed_keys;
    std::unordered_map<uint64_t, FunctionSeed> seed_by_key;
    for (const auto& s : seeds_) {
        uint64_t k = visit_key(s.addr, s.mode);
        seed_keys.insert(k);
        seed_by_key.emplace(k, s);
    }
    stats_.manual_seeds_total = seeds_.size();

    // Dedup-at-push: every (addr,mode) ever queued. discover_one already
    // dedups at visit time, so the worklist otherwise fills with
    // already-queued/visited no-ops — on Minish Cap it bloated to ~170k
    // of mostly-duplicate branch targets. Skipping a re-push is
    // output-neutral (the discovered closure is unchanged) and is the
    // dominant regen-speed cost. Seeded with the initial seed keys.
    std::unordered_set<uint64_t> enqueued = seed_keys;

    struct QueuedSeed {
        FunctionSeed seed;
        bool required;
    };

    // Seed worklist. Walk-discovered control-flow roots are inserted
    // immediately after the current item so local continuations cannot
    // be starved behind thousands of metadata seeds when max_functions
    // is intentionally bounded during bring-up.
    std::vector<QueuedSeed> worklist;
    worklist.reserve(seeds_.size());
    for (const auto& s : seeds_) {
        worklist.push_back(QueuedSeed{s, true});
    }
    std::size_t required_remaining = seeds_.size();

    // Per-key origin marker: was this (addr, mode) reached as a
    // seed, by walk, or both? Walks add "walk"; seeds add "seed".
    // Both → "redundant_manual".
    std::unordered_map<uint64_t, uint8_t> origin_mask;  // 1=seed, 2=walk
    for (const auto& s : seeds_) {
        origin_mask[visit_key(s.addr, s.mode)] |= 1u;
    }

    std::size_t pos = 0;
    while (pos < worklist.size() &&
           (functions_.size() < max_functions || required_remaining > 0)) {
        // Safety net: discovery must converge (worklist drains toward
        // empty). A runaway past this bound means a detector or decoder
        // change is seeding garbage — halt loudly rather than exhaust
        // memory. Baseline Minish Cap discovery peaks near ~170k, so
        // 2M is far above any healthy run.
        if (worklist.size() > 2000000u) {
            std::fprintf(stderr,
                "[finder] WORKLIST BRAKE at %zu (pos=%zu functions=%zu) "
                "— halting; discovery is not converging\n",
                worklist.size(), pos, functions_.size());
            std::fflush(stderr);
            break;
        }
        QueuedSeed queued = worklist[pos++];
        if (queued.required && required_remaining > 0) {
            --required_remaining;
        }

        FunctionSeed s = queued.seed;
        uint64_t s_key = visit_key(s.addr, s.mode);
        auto seed_it = seed_by_key.find(s_key);
        bool current_required = queued.required || seed_it != seed_by_key.end();
        if (!current_required && functions_.size() >= max_functions) {
            continue;
        }
        if (seed_it != seed_by_key.end()) {
            if (!seed_it->second.name.empty()) {
                s.name = seed_it->second.name;
            }
            if (s.source_addr == 0) {
                s.source_addr = seed_it->second.source_addr;
            }
            if (s.exact_size == 0) {
                s.exact_size = seed_it->second.exact_size;
            }
        }

        std::size_t before = functions_.size();
        discover_one(s.addr, s.mode, s.name, s.source_addr, s.exact_size);
        if (functions_.size() == before) continue;

        // The function we just added is at the end of functions_.
        const Function& fn = functions_.back();
        std::vector<QueuedSeed> discovered_seeds;

        // Each direct branch target becomes a new seed at the SAME
        // mode (control flow within a function doesn't switch
        // ARM/THUMB without a BX, which is indirect).
        for (uint32_t t : fn.direct_branch_targets) {
            if (authoritative_seeds_) break;
            uint64_t k = visit_key(t, fn.mode);
            // Mark walk-origin. If this address was ALSO a seed,
            // it now has both bits set (redundant_manual).
            origin_mask[k] |= 2u;
            if (!enqueued.insert(k).second) continue;  // already queued
            discovered_seeds.push_back(
                QueuedSeed{FunctionSeed{t, fn.mode, ""}, current_required});
            if (current_required) {
                ++required_remaining;
            }
        }
        // Drain any mode-switching seeds discovered during this
        // function's walk (BX Rm to a tracked-constant THUMB
        // target from ARM code, and vice versa).
        for (const auto& ms : mode_switch_seeds_) {
            if (authoritative_seeds_) break;
            uint64_t k = visit_key(ms.addr, ms.mode);
            origin_mask[k] |= 2u;
            if (!enqueued.insert(k).second) continue;  // already queued
            discovered_seeds.push_back(QueuedSeed{ms, current_required});
            if (current_required) {
                ++required_remaining;
            }
        }
        mode_switch_seeds_.clear();
        if (!discovered_seeds.empty()) {
            worklist.insert(worklist.begin() + static_cast<std::ptrdiff_t>(pos),
                            discovered_seeds.begin(),
                            discovered_seeds.end());
        }
        // If THIS function came from a seed (not a walk), it was
        // discovered "as a seed", but the function's body still
        // counts toward "walked from here" for its branches.
        uint64_t this_key = visit_key(fn.addr, fn.mode);
        if (seed_keys.count(this_key) == 0) {
            // It came from a walk (a branch target reached during
            // a prior function's discovery).
            origin_mask[this_key] |= 2u;
        }
    }

    // Apply exclude_func filter — remove functions whose entry
    // address was declared in [[exclude_func]].
    if (!excluded_.empty()) {
        std::size_t before = functions_.size();
        auto last = std::remove_if(functions_.begin(), functions_.end(),
            [&](const Function& fn) {
                return excluded_.count(fn.addr) > 0;
            });
        functions_.erase(last, functions_.end());
        stats_.excluded_count = before - functions_.size();
    }

    // Sort by address.
    std::sort(functions_.begin(), functions_.end(),
              [](const Function& a, const Function& b) {
                  if (a.addr != b.addr) return a.addr < b.addr;
                  return a.mode < b.mode;
              });
    // Set end_addr to the next same-mode function start when adjacent
    // (approximate boundary). ARM and THUMB entries occupy the same byte
    // address space but do not share instruction boundaries; a false-start
    // THUMB entry in the middle of ARM code must not truncate the ARM body.
    for (std::size_t i = 0; i < functions_.size(); ++i) {
        for (std::size_t j = i + 1; j < functions_.size(); ++j) {
            if (functions_[j].mode != functions_[i].mode) continue;
            if (functions_[j].addr > functions_[i].addr) {
                functions_[i].end_addr = std::min(
                    functions_[i].end_addr,
                    functions_[j].addr);
            }
            break;
        }
    }

    // Never end a THUMB function in the middle of a BL/BLX prefix+suffix
    // pair. If the last halfword is a BL prefix, pull in its suffix so the
    // call is emitted whole — otherwise the function's fall-through would
    // dispatch the orphaned suffix address, which is no function entry
    // (the boundary can land here because the prefix is branch-shaped).
    for (auto& fn : functions_) {
        if (fn.mode != CpuMode::Thumb) continue;
        for (int guard = 0; guard < 4; ++guard) {
            if (fn.end_addr < fn.addr + 2 || !can_read_at(fn.end_addr - 2, 2))
                break;
            armv4t::Instr last = armv4t::ThumbDecoder::decode(
                read_u16(fn.end_addr - 2), fn.end_addr - 2);
            if (last.op == armv4t::IrOp::BL_prefix &&
                can_read_at(fn.end_addr, 2)) {
                fn.end_addr += 2;  // include the suffix
            } else {
                break;
            }
        }
    }

    // Stats
    stats_.functions_total = functions_.size();
    stats_.functions_arm = 0;
    stats_.functions_thumb = 0;
    stats_.discovered_by_walk_only = 0;
    stats_.redundant_manual = 0;
    stats_.manual_seeds_only = 0;
    for (const auto& fn : functions_) {
        if (fn.mode == CpuMode::Thumb) ++stats_.functions_thumb;
        else                            ++stats_.functions_arm;
    }
    // Walk-source breakdown for emitted functions only.
    // origin_mask: 1=seed, 2=walk, 3=both.
    for (const auto& fn : functions_) {
        uint8_t mask = origin_mask[visit_key(fn.addr, fn.mode)];
        switch (mask) {
            case 1:
                ++stats_.manual_seeds_only;
                break;
            case 2:
                ++stats_.discovered_by_walk_only;
                break;
            case 3:
                ++stats_.redundant_manual;
                break;
            default: break;
        }
    }
}

}  // namespace ndsrecomp
