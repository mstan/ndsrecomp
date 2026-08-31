#include "gpu2d.h"
#include "gpu3d.h"
#include "savestate.h"
#include "vram.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

namespace {

bool expect(bool value, const char* message) {
    if (!value) std::fprintf(stderr, "FAIL: %s\n", message);
    return value;
}

bool same_vram(const NdsVramSaveState& a, const NdsVramSaveState& b) {
    return a.vram == b.vram && a.written == b.written &&
        a.exec_generation == b.exec_generation && a.palette == b.palette &&
        a.oam == b.oam &&
        std::memcmp(a.vramcnt, b.vramcnt, sizeof(a.vramcnt)) == 0 &&
        a.texture_generation == b.texture_generation;
}

bool same_gpu2d_unit(const NdsGpu2dUnitSaveState& a,
                     const NdsGpu2dUnitSaveState& b) {
    return a.dispcnt == b.dispcnt &&
        std::equal(std::begin(a.bgcnt), std::end(a.bgcnt), std::begin(b.bgcnt)) &&
        std::equal(std::begin(a.bgx), std::end(a.bgx), std::begin(b.bgx)) &&
        std::equal(std::begin(a.bgy), std::end(a.bgy), std::begin(b.bgy)) &&
        std::equal(std::begin(a.pa), std::end(a.pa), std::begin(b.pa)) &&
        std::equal(std::begin(a.pb), std::end(a.pb), std::begin(b.pb)) &&
        std::equal(std::begin(a.pc), std::end(a.pc), std::begin(b.pc)) &&
        std::equal(std::begin(a.pd), std::end(a.pd), std::begin(b.pd)) &&
        std::equal(std::begin(a.refx), std::end(a.refx), std::begin(b.refx)) &&
        std::equal(std::begin(a.refy), std::end(a.refy), std::begin(b.refy)) &&
        std::equal(std::begin(a.win), std::end(a.win), std::begin(b.win)) &&
        a.bg_mosaic_x == b.bg_mosaic_x && a.bg_mosaic_y == b.bg_mosaic_y &&
        a.obj_mosaic_x == b.obj_mosaic_x && a.obj_mosaic_y == b.obj_mosaic_y &&
        a.bldcnt == b.bldcnt && a.bldalpha == b.bldalpha &&
        a.eva == b.eva && a.evb == b.evb && a.evy == b.evy &&
        std::equal(std::begin(a.refx_internal), std::end(a.refx_internal),
                   std::begin(b.refx_internal)) &&
        std::equal(std::begin(a.refy_internal), std::end(a.refy_internal),
                   std::begin(b.refy_internal)) &&
        a.capture == b.capture && a.master_bright == b.master_bright &&
        a.capture_latch == b.capture_latch;
}

bool same_gpu2d(const NdsGpu2dSaveState& a, const NdsGpu2dSaveState& b) {
    return same_gpu2d_unit(a.unit[0], b.unit[0]) &&
        same_gpu2d_unit(a.unit[1], b.unit[1]) &&
        a.framebuffers == b.framebuffers && a.front == b.front &&
        a.frame_capture_active == b.frame_capture_active &&
        a.present_capture_active == b.present_capture_active;
}

bool vram_and_midframe_capture_roundtrip() {
    nds_vram_reset();
    nds_gpu3d_reset();
    nds_gpu2d_reset();
    nds_vram_map(0, 0x80u);  // bank A, LCDC for display capture
    nds_vram_map(2, 0x82u);  // bank C, ARM7 executable window
    nds_video_write(9, 0x05000000u, 0x1234u, 2u);
    nds_video_write(9, 0x07000000u, 0x5678u, 2u);
    nds_video_write(7, 0x06000000u, 0xA55Au, 2u);

    nds_gpu2d_write(0x04000000u, 0x00010000u, 4u);
    nds_gpu2d_write(0x04000008u, 0x23450123u, 4u);
    nds_gpu2d_write(0x04000010u, 0x01AB0123u, 4u);
    nds_gpu2d_write(0x04000020u, 0xFF000100u, 4u);
    nds_gpu2d_write(0x04000024u, 0x01000020u, 4u);
    nds_gpu2d_write(0x04000028u, 0x00123456u, 4u);
    nds_gpu2d_write(0x0400003Cu, 0x00654321u, 4u);
    nds_gpu2d_write(0x04000040u, 0x70908020u, 4u);
    nds_gpu2d_write(0x04000048u, 0x3F1F2F0Fu, 4u);
    nds_gpu2d_write(0x0400004Cu, 0x4321u, 2u);
    nds_gpu2d_write(0x04000050u, 0x08000441u, 4u);
    nds_gpu2d_write(0x04000054u, 0x0000000Cu, 4u);
    nds_gpu2d_write(0x04000064u, 0x80000000u, 4u);
    nds_gpu2d_write(0x0400006Cu, 0x4008u, 2u);
    nds_gpu2d_write(0x04001000u, 0x00010000u, 4u);
    nds_gpu2d_write(0x04001028u, 0x00765432u, 4u);
    nds_gpu2d_set_threaded(true, 2u);
    const uint64_t generation_before = nds_vram_texture_generation();
    nds_gpu2d_render_scanline(0);

    NdsGpu2dSaveState saved_gpu2d{};
    NdsVramSaveState saved_vram{};
    std::string error;
    bool ok = expect(gpu2d_savestate_export(&saved_gpu2d),
                     "export threaded mid-frame GPU2D state") &&
        expect(vram_savestate_export(&saved_vram),
               "export VRAM after staged capture") &&
        expect(nds_gpu2d_jobs_outstanding.load() == 0u &&
               nds_gpu2d_staged_captures.load() == 0u,
               "video export drains workers and applies staged capture") &&
        expect(saved_gpu2d.unit[0].capture_latch == 1u,
               "mid-frame display capture latch is serialized") &&
        expect(saved_vram.texture_generation > generation_before,
               "capture write advances texture coherence generation") &&
        expect((saved_vram.vram[1] & 0x80u) != 0u,
               "captured source-A alpha reaches physical VRAM");

    nds_vram_map(0, 0u);
    nds_vram_map(2, 0u);
    nds_gpu2d_write(0x04000064u, 0u, 4u);
    nds_gpu2d_finish_frame();
    ok &= expect(vram_savestate_import(saved_vram, &error), error.c_str()) &&
        expect(gpu2d_savestate_import(saved_gpu2d, &error), error.c_str());

    NdsVramSaveState loaded_vram{};
    NdsGpu2dSaveState loaded_gpu2d{};
    ok &= expect(vram_savestate_export(&loaded_vram),
                 "re-export restored VRAM") &&
        expect(gpu2d_savestate_export(&loaded_gpu2d),
               "re-export restored GPU2D") &&
        expect(same_vram(saved_vram, loaded_vram),
               "VRAM banks, maps, palette/OAM, provenance and generations roundtrip") &&
        expect(same_gpu2d(saved_gpu2d, loaded_gpu2d),
               "GPU2D registers, affine phase, capture and framebuffers roundtrip");

    // A second application must be stable and must not retain stale ring jobs.
    ok &= expect(vram_savestate_import(saved_vram, &error), error.c_str()) &&
        expect(gpu2d_savestate_import(saved_gpu2d, &error), error.c_str()) &&
        expect(nds_gpu2d_jobs_outstanding.load() == 0u,
               "repeated GPU2D load remains quiescent");
    nds_gpu2d_shutdown_workers();
    return ok;
}

bool gpu3d_device_roundtrip() {
    nds_vram_reset();
    nds_gpu3d_reset();
    nds_gpu3d_set_threaded(true);
    nds_gpu3d_set_render_xpos(0x42u);
    nds_gpu3d_write(0x04000350u, 0x3F1F001Fu, 4u);
    NdsGpu3dSaveState saved{};
    std::string error;
    bool ok = expect(gpu3d_savestate_export(&saved, &error), error.c_str()) &&
        expect(gpu3d_savestate_validate(saved, &error), error.c_str());
    nds_gpu3d_set_render_xpos(7u);
    ok &= expect(gpu3d_savestate_import(saved, &error), error.c_str()) &&
        expect(nds_gpu3d_render_xpos() == 0x42u,
               "GPU3D register state restores from pointer-safe vendored stream");
    NdsGpu3dSaveState repeated{};
    ok &= expect(gpu3d_savestate_export(&repeated, &error), error.c_str()) &&
        expect(saved.device == repeated.device &&
               saved.arm9_timestamp == repeated.arm9_timestamp,
               "GPU3D device stream is deterministic after renderer rebuild");
    NdsGpu3dSaveState corrupt = saved;
    corrupt.device[0] ^= 0xFFu;
    ok &= expect(!gpu3d_savestate_validate(corrupt, &error),
                 "corrupt GPU3D stream is rejected before live apply");
    // MELN header + GP3D section header is 32 bytes. The first device field is
    // the 256-entry command FIFO occupancy; pin the vendored decode guard so a
    // checksum-recomputed section cannot install an out-of-range FIFO state.
    corrupt = saved;
    corrupt.device[32] = 0x01u;
    corrupt.device[33] = 0x01u;
    corrupt.device[34] = 0u;
    corrupt.device[35] = 0u;
    ok &= expect(!gpu3d_savestate_validate(corrupt, &error),
                 "out-of-range GPU3D FIFO is rejected during detached decode") &&
        expect(nds_gpu3d_render_xpos() == 0x42u,
               "GPU3D prevalidation does not mutate the live device");
    nds_gpu3d_use_soft_renderer(false);
    return ok;
}

}  // namespace

int main() {
    bool ok = vram_and_midframe_capture_roundtrip();
    ok &= gpu3d_device_roundtrip();
    return ok ? 0 : 1;
}
