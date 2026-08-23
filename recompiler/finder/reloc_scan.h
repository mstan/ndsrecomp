// reloc_scan.h — Static relocation discovery (beads-yjp.35 item 2).
//
// NDS modules routinely copy parts of themselves to other addresses at
// startup (the Nitro SDK "autoload": ITCM mirrors on the ARM9, the WRAM +
// main-RAM driver segments on the ARM7) and then jump into the copy. A
// CFG walk of the module at its declared base cannot follow those jumps —
// the targets are outside the readable image — so everything past the
// first such jump goes dark. On MPH that was 0.21% ARM7 discovery and
// two-thirds of all Tier-3 interpreter entries (the alias class measured
// by the first player coverage ingest).
//
// Instead of pattern-matching copy-loop idioms, this scanner EXECUTES the
// module's own startup code with the reference IR interpreter, in a
// sealed sandbox:
//
//   * instruction fetch only from the module image — the scan ends the
//     moment PC leaves it;
//   * data reads served from the module image, then from a shadow store
//     of the scan's own writes; anything else reads as 0 (counted);
//   * writes land only in the shadow store — nothing real is touched;
//   * a hard step cap bounds runaway loops.
//
// Afterwards, every contiguous shadow region OUTSIDE the module span is
// content-matched against the module image: a region whose leading bytes
// occur at exactly ONE image offset, extended byte-for-byte as far as it
// stays equal, is a proven relocation and becomes a code_copy range. This
// matching is the correctness gate — the execution above only has to be
// right often enough to produce candidates, because a candidate is only
// emitted when the module's own bytes confirm it. Zero-fill (bss) and
// garbage-sourced writes never match uniquely and fall away.
//
// The output feeds FunctionFinder::add_code_copy, after which discovery
// walks straight through the relocation jump and the recompiler emits the
// aliased code from ROM content — no captured image required.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ndsrecomp {

struct RelocCopy {
    uint32_t runtime_start;  // where the copy executes (guest address)
    uint32_t source_start;   // where the bytes live in the module (guest)
    uint32_t size;           // bytes proven byte-identical
};

struct RelocScanStats {
    std::size_t steps = 0;             // instructions executed
    std::size_t unmapped_reads = 0;    // data reads served as 0
    std::size_t shadow_bytes = 0;      // bytes written during the scan
    std::size_t regions_total = 0;     // contiguous out-of-module regions
    std::size_t regions_matched = 0;   // regions emitted as copies
    std::size_t regions_ambiguous = 0; // anchor matched >1 image offset
    std::size_t regions_unmatched = 0; // anchor matched nothing / too short
    std::string stop_reason;           // why execution ended
};

// Execute the module's startup code from `entry_pc` and return every
// content-proven relocation. `image`/`image_size` are the module bytes as
// loaded at `image_base`. Deterministic, side-effect free, bounded.
std::vector<RelocCopy> scan_relocations(const uint8_t* image,
                                        std::size_t image_size,
                                        uint32_t image_base,
                                        uint32_t entry_pc,
                                        RelocScanStats* stats = nullptr);

}  // namespace ndsrecomp
