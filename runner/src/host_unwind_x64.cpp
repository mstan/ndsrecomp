// host_unwind_x64.cpp -- see host_unwind_x64.h.
//
// The data formats parsed here are the x64 PE exception tables:
// IMAGE_RUNTIME_FUNCTION_ENTRY (12 bytes, sorted by BeginAddress) in .pdata,
// each pointing at an UNWIND_INFO blob in .xdata. They are re-declared locally
// rather than pulled from <winnt.h> so this translation unit stays free of
// windows.h and can be unit-tested against a synthetic table on any host.

#include "host_unwind_x64.h"

#include <cstring>

namespace {

// One .pdata entry. All three fields are RVAs relative to the image base.
struct RuntimeFunction {
    uint32_t begin;
    uint32_t end;
    uint32_t unwind;
};

constexpr uint8_t kUnwFlagChainInfo = 0x4u;

// Unwind operation codes. Only the ones a real compiler emits are implemented;
// 6 (UWOP_EPILOG, UNWIND_INFO version 2 only) and 7 (UWOP_SPARE) are refused
// outright rather than guessed at, because both have a variable slot count and
// mis-sizing one desynchronises the whole remaining code array -- a silent
// wrong answer, which is worse than a counted BAD_INFO.
enum : uint8_t {
    UWOP_PUSH_NONVOL = 0,
    UWOP_ALLOC_LARGE = 1,
    UWOP_ALLOC_SMALL = 2,
    UWOP_SET_FPREG = 3,
    UWOP_SAVE_NONVOL = 4,
    UWOP_SAVE_NONVOL_FAR = 5,
    UWOP_EPILOG = 6,
    UWOP_SPARE = 7,
    UWOP_SAVE_XMM128 = 8,
    UWOP_SAVE_XMM128_FAR = 9,
    UWOP_PUSH_MACHFRAME = 10,
};

// Slots (2 bytes each) an operation occupies in the code array, including its
// own. 0 means "refuse this blob".
unsigned opcode_slots(uint8_t op, uint8_t info) {
    switch (op) {
        case UWOP_PUSH_NONVOL: return 1;
        case UWOP_ALLOC_LARGE: return info == 0 ? 2u : 3u;
        case UWOP_ALLOC_SMALL: return 1;
        case UWOP_SET_FPREG: return 1;
        case UWOP_SAVE_NONVOL: return 2;
        case UWOP_SAVE_NONVOL_FAR: return 3;
        case UWOP_SAVE_XMM128: return 2;
        case UWOP_SAVE_XMM128_FAR: return 3;
        case UWOP_PUSH_MACHFRAME: return 1;
        default: return 0;   // UWOP_EPILOG, UWOP_SPARE, anything unknown
    }
}

bool read64(const NdsHostUnwindEnv& env, uint64_t addr, uint64_t* out) {
    return env.read(env.ctx, addr, out, 8u);
}

enum class FindResult { Found, NoModule, NoEntry, ReadFailed };

// Binary search the module's .pdata for the entry covering `pc`. The array is
// sorted by BeginAddress and non-overlapping, which is a format guarantee the
// OS unwinder relies on too, so a 20-step search replaces a scan of the
// hundreds of thousands of entries a generated-code image carries.
FindResult find_function(const NdsHostUnwindEnv& env, uint64_t pc,
                         uint64_t* image_base, RuntimeFunction* out) {
    uint32_t pdata_rva = 0;
    uint32_t pdata_size = 0;
    if (!env.pdata(env.ctx, pc, image_base, &pdata_rva, &pdata_size))
        return FindResult::NoModule;
    if (pdata_size < sizeof(RuntimeFunction)) return FindResult::NoEntry;
    const uint64_t base = *image_base;
    if (pc < base) return FindResult::NoEntry;
    const uint32_t target = static_cast<uint32_t>(pc - base);
    uint32_t lo = 0;
    uint32_t hi = pdata_size / static_cast<uint32_t>(sizeof(RuntimeFunction));
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        RuntimeFunction rf{};
        if (!env.read(env.ctx,
                      base + pdata_rva + mid * sizeof(RuntimeFunction),
                      &rf, sizeof(rf)))
            return FindResult::ReadFailed;
        if (target < rf.begin) {
            hi = mid;
        } else if (target >= rf.end) {
            lo = mid + 1u;
        } else {
            *out = rf;
            return FindResult::Found;
        }
    }
    // A pc inside a module but outside every .pdata entry is the normal shape
    // of a true leaf function (nothing pushed, nothing to describe), so this is
    // not an error -- the caller decides, because "leaf" is only a legal
    // explanation for the innermost frame.
    return FindResult::NoEntry;
}

// Undo one function's prologue: on entry `regs` describes a point inside the
// function `rf` covers, on success it describes the same thread one frame out
// (rip = return address, rsp = caller's stack pointer, nonvolatile registers
// restored as far as the unwind data records them).
//
// `in_prologue_pc` is the sampled pc for the FIRST function of a chain and
// "past the prologue" for chained entries: a chain's parent entry describes
// frame setup that has, by construction, fully executed.
bool unwind_one(const NdsHostUnwindEnv& env, uint64_t image_base,
                RuntimeFunction rf, NdsHostUnwindRegs& regs,
                NdsHostUnwindStop* stop) {
    uint64_t prolog_offset = regs.rip - (image_base + rf.begin);
    uint32_t unwind_rva = rf.unwind;

    // Bounded because a corrupt or hostile chain is otherwise an infinite loop
    // inside an always-on Release code path. Real chains are 1-2 deep.
    for (unsigned chain = 0; chain < 32u; ++chain) {
        uint8_t header[4];
        if (!env.read(env.ctx, image_base + unwind_rva, header, sizeof(header))) {
            *stop = NDS_HOST_UNWIND_READ_FAILED;
            return false;
        }
        const uint8_t version = header[0] & 0x7u;
        const uint8_t flags = static_cast<uint8_t>(header[0] >> 3);
        const uint8_t count = header[2];
        const uint8_t frame_reg = header[3] & 0x0Fu;
        const uint8_t frame_off = static_cast<uint8_t>(header[3] >> 4);
        if (version != 1u && version != 2u) {
            *stop = NDS_HOST_UNWIND_BAD_INFO;
            return false;
        }

        // CountOfCodes is a byte, so the whole array is at most 510 bytes and
        // fits a fixed on-stack buffer -- the unwinder never allocates, which
        // is a hard requirement of the caller (see the header).
        uint8_t codes[510];
        const uint32_t codes_bytes = static_cast<uint32_t>(count) * 2u;
        if (codes_bytes &&
            !env.read(env.ctx, image_base + unwind_rva + 4u, codes,
                      codes_bytes)) {
            *stop = NDS_HOST_UNWIND_READ_FAILED;
            return false;
        }

        // Offsets recorded by UWOP_SAVE_NONVOL* are relative to the FIXED frame
        // base: the frame register's value minus its scaled offset when the
        // function established one, and plain RSP otherwise. Computed once,
        // before any code adjusts RSP, because that is what the format means by
        // "the" frame base.
        const uint64_t frame_base =
            frame_reg ? regs.gpr[frame_reg] - static_cast<uint64_t>(frame_off) * 16u
                      : nds_unwind_rsp(regs);

        unsigned i = 0;
        while (i < count) {
            const uint8_t code_off = codes[i * 2u];
            const uint8_t op = codes[i * 2u + 1u] & 0x0Fu;
            const uint8_t info = static_cast<uint8_t>(codes[i * 2u + 1u] >> 4);
            const unsigned slots = opcode_slots(op, info);
            if (slots == 0u || i + slots > count) {
                *stop = NDS_HOST_UNWIND_BAD_INFO;
                return false;
            }
            // Not executed yet at the sampled pc: the prologue is only
            // partially done, so this operation must not be undone. Skipping
            // still consumes the operation's extra slots.
            if (static_cast<uint64_t>(code_off) > prolog_offset) {
                i += slots;
                continue;
            }
            switch (op) {
                case UWOP_PUSH_NONVOL: {
                    uint64_t v = 0;
                    if (!read64(env, nds_unwind_rsp(regs), &v)) {
                        *stop = NDS_HOST_UNWIND_READ_FAILED;
                        return false;
                    }
                    regs.gpr[info] = v;
                    nds_unwind_rsp(regs) += 8u;
                    break;
                }
                case UWOP_ALLOC_LARGE: {
                    uint64_t size = 0;
                    if (info == 0u) {
                        uint16_t scaled = 0;
                        std::memcpy(&scaled, &codes[(i + 1u) * 2u], 2);
                        size = static_cast<uint64_t>(scaled) * 8u;
                    } else {
                        uint32_t raw = 0;
                        std::memcpy(&raw, &codes[(i + 1u) * 2u], 4);
                        size = raw;
                    }
                    nds_unwind_rsp(regs) += size;
                    break;
                }
                case UWOP_ALLOC_SMALL:
                    nds_unwind_rsp(regs) +=
                        (static_cast<uint64_t>(info) + 1u) * 8u;
                    break;
                case UWOP_SET_FPREG:
                    nds_unwind_rsp(regs) = frame_base;
                    break;
                case UWOP_SAVE_NONVOL:
                case UWOP_SAVE_NONVOL_FAR: {
                    uint64_t off = 0;
                    if (op == UWOP_SAVE_NONVOL) {
                        uint16_t scaled = 0;
                        std::memcpy(&scaled, &codes[(i + 1u) * 2u], 2);
                        off = static_cast<uint64_t>(scaled) * 8u;
                    } else {
                        uint32_t raw = 0;
                        std::memcpy(&raw, &codes[(i + 1u) * 2u], 4);
                        off = raw;
                    }
                    uint64_t v = 0;
                    if (!read64(env, frame_base + off, &v)) {
                        *stop = NDS_HOST_UNWIND_READ_FAILED;
                        return false;
                    }
                    regs.gpr[info] = v;
                    break;
                }
                case UWOP_SAVE_XMM128:
                case UWOP_SAVE_XMM128_FAR:
                    // XMM state is irrelevant to a call-chain walk; the slots
                    // still have to be stepped over.
                    break;
                case UWOP_PUSH_MACHFRAME: {
                    // A hardware trap frame: the kernel pushed SS/RSP/RFLAGS/
                    // CS/RIP (and optionally an error code). This IS the
                    // caller's state, so the walk finishes here rather than
                    // popping a return address afterwards.
                    uint64_t sp = nds_unwind_rsp(regs);
                    if (info) sp += 8u;   // skip the error code
                    uint64_t trap_rip = 0;
                    uint64_t trap_rsp = 0;
                    if (!read64(env, sp, &trap_rip) ||
                        !read64(env, sp + 24u, &trap_rsp)) {
                        *stop = NDS_HOST_UNWIND_READ_FAILED;
                        return false;
                    }
                    regs.rip = trap_rip;
                    nds_unwind_rsp(regs) = trap_rsp;
                    return true;
                }
                default:
                    *stop = NDS_HOST_UNWIND_BAD_INFO;
                    return false;
            }
            i += slots;
        }

        if (!(flags & kUnwFlagChainInfo)) break;

        // The chained RUNTIME_FUNCTION sits after the code array, 4-byte
        // aligned: 4 header bytes + an even number of code slots.
        const uint32_t aligned_codes =
            (static_cast<uint32_t>(count) + 1u) / 2u * 4u;
        RuntimeFunction parent{};
        if (!env.read(env.ctx, image_base + unwind_rva + 4u + aligned_codes,
                      &parent, sizeof(parent))) {
            *stop = NDS_HOST_UNWIND_READ_FAILED;
            return false;
        }
        unwind_rva = parent.unwind;
        // The parent entry's prologue is complete by definition at this point,
        // so every one of its codes applies.
        prolog_offset = ~static_cast<uint64_t>(0);
    }

    uint64_t ret = 0;
    if (!read64(env, nds_unwind_rsp(regs), &ret)) {
        *stop = NDS_HOST_UNWIND_READ_FAILED;
        return false;
    }
    nds_unwind_rsp(regs) += 8u;
    regs.rip = ret;
    return true;
}

}  // namespace

const char* nds_host_unwind_stop_name(NdsHostUnwindStop stop) {
    switch (stop) {
        case NDS_HOST_UNWIND_DEPTH: return "depth";
        case NDS_HOST_UNWIND_ROOT: return "root";
        case NDS_HOST_UNWIND_NO_MODULE: return "no_module";
        case NDS_HOST_UNWIND_NO_PDATA: return "no_pdata";
        case NDS_HOST_UNWIND_READ_FAILED: return "read_failed";
        case NDS_HOST_UNWIND_BAD_INFO: return "bad_info";
        default: return "unknown";
    }
}

unsigned nds_host_unwind(const NdsHostUnwindEnv& env,
                         NdsHostUnwindRegs regs,
                         uint64_t* out_frames, unsigned max,
                         NdsHostUnwindStop* out_stop) {
    NdsHostUnwindStop stop = NDS_HOST_UNWIND_DEPTH;
    unsigned depth = 0;
    if (!out_frames || max == 0u) {
        if (out_stop) *out_stop = stop;
        return 0u;
    }
    if (max > kNdsHostUnwindMaxFrames) max = kNdsHostUnwindMaxFrames;

    // Frame 0 is the raw pc and is recorded unconditionally. This is the whole
    // reason self-time attribution is trustworthy even when the walk fails
    // immediately: every stop reason below costs inclusive detail, never the
    // leaf.
    out_frames[depth++] = regs.rip;

    while (depth < max) {
        uint64_t image_base = 0;
        RuntimeFunction rf{};
        const FindResult found =
            find_function(env, regs.rip, &image_base, &rf);
        if (found == FindResult::NoModule) {
            stop = NDS_HOST_UNWIND_NO_MODULE;
            break;
        }
        if (found == FindResult::ReadFailed) {
            stop = NDS_HOST_UNWIND_READ_FAILED;
            break;
        }

        const uint64_t rsp_before = nds_unwind_rsp(regs);
        if (found == FindResult::NoEntry) {
            // Legal only for the innermost frame: a leaf function has pushed
            // nothing, so [RSP] is its return address. At any greater depth a
            // missing entry means the image has no usable unwind data (a
            // TinyCC-built live shard, say) and the honest answer is to stop
            // and say so.
            if (depth != 1u) {
                stop = NDS_HOST_UNWIND_NO_PDATA;
                break;
            }
            uint64_t ret = 0;
            if (!read64(env, rsp_before, &ret)) {
                stop = NDS_HOST_UNWIND_READ_FAILED;
                break;
            }
            nds_unwind_rsp(regs) = rsp_before + 8u;
            regs.rip = ret;
        } else if (!unwind_one(env, image_base, rf, regs, &stop)) {
            break;
        }

        if (regs.rip == 0u) {
            stop = NDS_HOST_UNWIND_ROOT;
            break;
        }
        // A frame that did not move the stack pointer outward is either a
        // misparse or a cycle; either way continuing would emit fiction.
        if (nds_unwind_rsp(regs) <= rsp_before) {
            stop = NDS_HOST_UNWIND_BAD_INFO;
            break;
        }
        out_frames[depth++] = regs.rip;
    }

    if (out_stop) *out_stop = stop;
    return depth;
}
