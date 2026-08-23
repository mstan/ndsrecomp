// reloc_scan.cpp — implementation. See reloc_scan.h for the model.

#include "reloc_scan.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "arm_decode.h"
#include "thumb_decode.h"
#include "arm_ir.h"
#include "bus.h"
#include "cpu_state.h"
#include "interpreter.h"

namespace ndsrecomp {

namespace {

// Bytes an anchor window must span before a content match is trusted.
// 64 bytes of real code is unique in practice; anything shorter risks
// matching a coincidental byte run.
constexpr std::size_t kMinAnchor = 64;

// Instruction budget. The MPH ARM7 autoload copies ~0x28000 bytes at
// roughly six instructions per word (~250k steps); the cap is far above
// any real startup path while still bounding a tainted runaway loop.
constexpr std::size_t kMaxSteps = 4u * 1024u * 1024u;

// std::search calls allowed per region, so a pathological shadow region
// cannot turn matching into quadratic work.
constexpr int kMaxAnchorTries = 512;

// Sealed bus: reads come from the scan's own writes first, then the
// module image; everything else reads as zero (counted — a tainted value
// can only ever produce a candidate region, never an emitted copy,
// because emission requires the content match). Writes never leave the
// shadow store.
class RelocBus final : public armv4t::Bus {
public:
    RelocBus(const uint8_t* image, std::size_t size, uint32_t base)
        : image_(image), size_(size), base_(base) {}

    uint8_t read8(uint32_t addr) override { return load(addr); }
    uint16_t read16(uint32_t addr) override {
        return static_cast<uint16_t>(load(addr)) |
               (static_cast<uint16_t>(load(addr + 1)) << 8);
    }
    uint32_t read32(uint32_t addr) override {
        return  static_cast<uint32_t>(load(addr))
             | (static_cast<uint32_t>(load(addr + 1)) << 8)
             | (static_cast<uint32_t>(load(addr + 2)) << 16)
             | (static_cast<uint32_t>(load(addr + 3)) << 24);
    }

    void write8(uint32_t addr, uint8_t v) override { shadow_[addr] = v; }
    void write16(uint32_t addr, uint16_t v) override {
        shadow_[addr] = static_cast<uint8_t>(v);
        shadow_[addr + 1] = static_cast<uint8_t>(v >> 8);
    }
    void write32(uint32_t addr, uint32_t v) override {
        shadow_[addr] = static_cast<uint8_t>(v);
        shadow_[addr + 1] = static_cast<uint8_t>(v >> 8);
        shadow_[addr + 2] = static_cast<uint8_t>(v >> 16);
        shadow_[addr + 3] = static_cast<uint8_t>(v >> 24);
    }

    const std::map<uint32_t, uint8_t>& shadow() const { return shadow_; }
    std::size_t unmapped_reads() const { return unmapped_reads_; }

private:
    uint8_t load(uint32_t addr) {
        auto it = shadow_.find(addr);
        if (it != shadow_.end()) return it->second;
        if (addr >= base_ && addr - base_ < size_) return image_[addr - base_];
        ++unmapped_reads_;
        return 0;
    }

    const uint8_t* image_;
    std::size_t size_;
    uint32_t base_;
    std::map<uint32_t, uint8_t> shadow_;
    std::size_t unmapped_reads_ = 0;
};

// True when the window is one repeated byte — such an anchor matches a
// zero-fill or padding run anywhere and proves nothing.
bool degenerate_window(const uint8_t* p, std::size_t n) {
    for (std::size_t i = 1; i < n; ++i)
        if (p[i] != p[0]) return false;
    return true;
}

}  // namespace

std::vector<RelocCopy> scan_relocations(const uint8_t* image,
                                        std::size_t image_size,
                                        uint32_t image_base,
                                        uint32_t entry_pc,
                                        RelocScanStats* stats_out) {
    RelocScanStats stats;
    std::vector<RelocCopy> copies;

    RelocBus bus(image, image_size, image_base);
    armv4t::CPUState cpu{};
    std::memset(&cpu, 0, sizeof cpu);
    cpu.cpsr.mode = static_cast<uint8_t>(armv4t::Mode::Supervisor);
    cpu.cpsr.i = true;
    cpu.cpsr.f = true;
    cpu.cpsr.t = (entry_pc & 1u) != 0;
    cpu.thumb = cpu.cpsr.t;
    cpu.R[15] = entry_pc & ~uint32_t{1};

    auto in_image = [&](uint32_t addr, uint32_t len) {
        return addr >= image_base &&
               (static_cast<uint64_t>(addr) - image_base) + len <= image_size;
    };

    // ── Execute ──────────────────────────────────────────────────────
    while (stats.steps < kMaxSteps) {
        const bool thumb = cpu.cpsr.t;
        const uint32_t pc = cpu.R[15] & (thumb ? ~uint32_t{1} : ~uint32_t{3});
        if (!in_image(pc, thumb ? 2u : 4u)) {
            // The startup path jumped into its own copy (or returned to
            // the BIOS). This is the expected, successful end of a scan.
            stats.stop_reason = "pc-left-module";
            break;
        }
        armv4t::Instr ins;
        if (thumb) {
            ins = armv4t::ThumbDecoder::decode(bus.read16(pc), pc);
        } else {
            ins = armv4t::ArmDecoder::decode(bus.read32(pc), pc);
        }
        if (ins.is_undefined) {
            stats.stop_reason = "undefined-instruction";
            break;
        }
        ++stats.steps;
        const auto r = armv4t::Interpreter::step(cpu, bus, ins, nullptr);
        if (r == armv4t::Interpreter::Result::Swi) {
            // A BIOS call from startup code: its effects are unmodeled,
            // so everything after it would be speculation.
            stats.stop_reason = "swi";
            break;
        }
        if (r == armv4t::Interpreter::Result::Undefined) {
            stats.stop_reason = "undefined-instruction";
            break;
        }
        if (r == armv4t::Interpreter::Result::NotImplemented) {
            stats.stop_reason = "unimplemented-op";
            break;
        }
    }
    if (stats.stop_reason.empty()) stats.stop_reason = "step-cap";
    stats.unmapped_reads = bus.unmapped_reads();
    stats.shadow_bytes = bus.shadow().size();

    // ── Coalesce shadow writes outside the module span ───────────────
    struct Region { uint32_t start; std::vector<uint8_t> bytes; };
    std::vector<Region> regions;
    for (const auto& [addr, byte] : bus.shadow()) {
        if (addr >= image_base && addr - image_base < image_size)
            continue;  // self-modification inside the span: not an alias
        if (!regions.empty() &&
            addr == regions.back().start + regions.back().bytes.size()) {
            regions.back().bytes.push_back(byte);
        } else {
            regions.push_back(Region{addr, {byte}});
        }
    }
    stats.regions_total = regions.size();

    // ── Content-match each region against the module image ───────────
    // A copy is emitted only where the region's bytes occur at exactly
    // one image offset and stay byte-identical from there — the module's
    // own content is the proof. Anchors slide forward past unmatched
    // spans (zero-fill, stack traffic, tainted copies), so one region can
    // yield several distinct copies.
    for (const auto& region : regions) {
        const std::size_t len = region.bytes.size();
        std::size_t pos = 0;
        int tries = 0;
        bool matched_any = false;
        bool ambiguous = false;
        while (pos + kMinAnchor <= len && tries < kMaxAnchorTries) {
            const uint8_t* anchor = region.bytes.data() + pos;
            if (degenerate_window(anchor, kMinAnchor)) {
                pos += 4;
                continue;
            }
            ++tries;
            const uint8_t* first = std::search(
                image, image + image_size, anchor, anchor + kMinAnchor);
            if (first == image + image_size) {
                pos += 4;
                continue;
            }
            const uint8_t* second = std::search(
                first + 1, image + image_size, anchor, anchor + kMinAnchor);
            if (second != image + image_size) {
                // The anchor is not unique in the image; a match here
                // could bind the copy to the wrong source offset.
                ambiguous = true;
                pos += 4;
                continue;
            }
            std::size_t off = static_cast<std::size_t>(first - image);
            std::size_t n = 0;
            while (pos + n < len && off + n < image_size &&
                   region.bytes[pos + n] == image[off + n]) {
                ++n;
            }
            copies.push_back(RelocCopy{
                region.start + static_cast<uint32_t>(pos),
                image_base + static_cast<uint32_t>(off),
                static_cast<uint32_t>(n)});
            matched_any = true;
            pos += n;
        }
        if (matched_any)        ++stats.regions_matched;
        else if (ambiguous)     ++stats.regions_ambiguous;
        else                    ++stats.regions_unmatched;
    }

    if (stats_out) *stats_out = stats;
    return copies;
}

}  // namespace ndsrecomp
