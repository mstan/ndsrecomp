#include "title_patches.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "gpu3d.h"
#include "runtime_arm.h"
#include "state.h"

namespace {

constexpr uint32_t kMainRamBase = 0x02000000u;
constexpr uint32_t kSm64dsClipper = 0x0209F43Cu;
constexpr uint32_t kPlane0 = kSm64dsClipper + 0x04u;
constexpr uint32_t kPlane2 = kSm64dsClipper + 0x1Cu;
constexpr uint32_t kAspect = kSm64dsClipper + 0x4Cu;
constexpr int32_t kNativeAspect = 0x1555;
constexpr int32_t kWideAspect = 0x2555;  // round(0x1555 * 448 / 256)
constexpr double kWideScale = 448.0 / 256.0;
constexpr double kFix12One = 4096.0;
// AMHE0's native touch-look routine consumes these signed, per-frame fields.
// Feeding deltas here while holding the stylus at center preserves the game
// path but removes the finite physical touchscreen edge.
constexpr uint32_t kMphUs10AimX = 0x020DE526u;
constexpr uint32_t kMphUs10AimY = 0x020DE52Eu;

// AMHE0 adventure mode builds every frustum from a 4:3 literal and from two
// aspect numerators baked into Camera_SetupProjection and Rooms_TraverseAndDraw.
// Widening those three words in place makes the guest itself produce band-wide
// frusta, sub-frusta and portal screen bboxes; the neighbours are residency
// guards, so multiplayer overlays and menus simply fail the match.
constexpr uint32_t kMphWideSiteCount = 3u;
struct MphWideSite {
    uint32_t addr;
    uint32_t native;
    uint32_t neighbor_addr;
    uint32_t neighbor;
};
constexpr MphWideSite kMphWideSites[kMphWideSiteCount] = {
    {0x02110820u, 0x00001555u, 0x0211081Cu, 0x021230CCu},
    {0x0211C620u, 0xE2810001u, 0x0211C618u, 0xE59A1670u},
    {0x02110FF8u, 0xE5990670u, 0x02110FFCu, 0xE5991664u},
};

bool g_sm64ds_adaptive = false;
bool g_mph_mouse_aim = false;
bool g_logged_sm64ds_clipper = false;

bool g_mph_adventure_wide = false;
uint16_t g_mph_adventure_width = 0;
uint32_t g_mph_wide_words[kMphWideSiteCount] = {};
bool g_mph_wide_active = false;
bool g_logged_mph_wide_active = false;
bool g_logged_mph_wide_lost = false;
bool g_logged_mph_wide_width = false;
uint64_t g_mph_wide_applied[kMphWideSiteCount] = {};
uint64_t g_mph_wide_frames_active = 0;
uint64_t g_mph_wide_frames_inactive = 0;

bool read_main_ram32(uint32_t addr, int32_t* out) {
    if (!out || addr < kMainRamBase) return false;
    BusRegion main_ram{};
    if (!bus_get_region("mainram", &main_ram)) return false;
    const uint32_t offset = addr - kMainRamBase;
    if (offset > main_ram.len || main_ram.len - offset < sizeof(*out))
        return false;
    std::memcpy(out, main_ram.ptr + offset, sizeof(*out));
    return true;
}

bool read_main_ram_word(uint32_t addr, uint32_t* out) {
    int32_t value = 0;
    if (!out || !read_main_ram32(addr, &value)) return false;
    *out = static_cast<uint32_t>(value);
    return true;
}

// ARM data-processing immediates are an 8-bit value rotated right by an even
// amount; not every width is representable, so the caller disables the feature
// rather than emitting a wrong constant.
bool encode_arm_mov_r0_imm(uint32_t value, uint32_t* out) {
    for (uint32_t rot = 0; rot < 16u; ++rot) {
        const uint32_t shift = rot * 2u;
        const uint32_t imm8 = shift == 0u
            ? value
            : ((value << shift) | (value >> (32u - shift)));
        if (imm8 <= 0xFFu) {
            *out = 0xE3A00000u | (rot << 8) | imm8;
            return true;
        }
    }
    return false;
}

void patch_mph_adventure_wide() {
    // The wide words bake the adaptive width into guest constants, so they
    // are only correct when the 3D engine actually renders at that width.
    // Headless/serve runs keep the native 256-wide surface; leave the guest
    // untouched there instead of widening its frusta against a native render.
    if (nds_gpu3d_output_width() != g_mph_adventure_width) {
        ++g_mph_wide_frames_inactive;
        if (!g_logged_mph_wide_width) {
            std::fprintf(stderr,
                         "[mph] adventure wide frustum idle (render width %u, "
                         "configured %u)\n",
                         static_cast<unsigned>(nds_gpu3d_output_width()),
                         static_cast<unsigned>(g_mph_adventure_width));
            g_logged_mph_wide_width = true;
        }
        g_mph_wide_active = false;
        nds_gpu3d_set_guest_wide_projection(false);
        return;
    }

    bool active = true;
    for (uint32_t i = 0; i < kMphWideSiteCount; ++i) {
        const MphWideSite& site = kMphWideSites[i];
        uint32_t word = 0;
        uint32_t neighbor = 0;
        if (!read_main_ram_word(site.addr, &word) ||
            !read_main_ram_word(site.neighbor_addr, &neighbor)) {
            active = false;
            continue;
        }
        if (word == site.native && neighbor == site.neighbor) {
            bus_write_u32_slow(site.addr, g_mph_wide_words[i]);
            ++g_mph_wide_applied[i];
            if (!read_main_ram_word(site.addr, &word)) {
                active = false;
                continue;
            }
        }
        active &= (word == g_mph_wide_words[i]);
    }

    if (active) ++g_mph_wide_frames_active;
    else ++g_mph_wide_frames_inactive;

    if (active && !g_logged_mph_wide_active) {
        std::fprintf(stderr,
                     "[mph] adventure wide frustum enabled (%u px)\n",
                     static_cast<unsigned>(g_mph_adventure_width));
        g_logged_mph_wide_active = true;
    } else if (!active && g_logged_mph_wide_active &&
               !g_logged_mph_wide_lost) {
        std::fprintf(stderr,
                     "[mph] adventure wide frustum inactive "
                     "(overlay not resident)\n");
        g_logged_mph_wide_lost = true;
    }

    g_mph_wide_active = active;
    nds_gpu3d_set_guest_wide_projection(active);
}

void widen_horizontal_plane(uint32_t addr) {
    int32_t x = 0;
    int32_t z = 0;
    if (!read_main_ram32(addr, &x) ||
        !read_main_ram32(addr + 8u, &z))
        return;

    const double wide_z = static_cast<double>(z) * kWideScale;
    const double length = std::hypot(static_cast<double>(x), wide_z);
    if (length < 1.0) return;

    const int32_t new_x =
        static_cast<int32_t>(std::lround(x * kFix12One / length));
    const int32_t new_z =
        static_cast<int32_t>(std::lround(wide_z * kFix12One / length));
    bus_write_u32_slow(addr, static_cast<uint32_t>(new_x));
    bus_write_u32_slow(addr + 8u, static_cast<uint32_t>(new_z));
}

void patch_sm64ds_clipper() {
    int32_t aspect = 0;
    if (!read_main_ram32(kAspect, &aspect) || aspect != kNativeAspect)
        return;

    // SM64DS derives four fixed-point frustum planes from this aspect field.
    // The horizontal planes are 0 and 2. Scale their Z component by the host
    // presentation ratio and renormalize them to Fix12 unit length. Updating
    // the aspect field makes later game-side recomputations preserve the wide
    // frustum; if the game installs a fresh native camera, this runs again.
    widen_horizontal_plane(kPlane0);
    widen_horizontal_plane(kPlane2);
    bus_write_u32_slow(kAspect, static_cast<uint32_t>(kWideAspect));

    if (!g_logged_sm64ds_clipper) {
        std::fprintf(stderr,
                     "[sm64ds] adaptive 21:9 actor frustum enabled\n");
        g_logged_sm64ds_clipper = true;
    }
}

}  // namespace

void nds_title_patches_set_sm64ds_adaptive(bool enabled) {
    g_sm64ds_adaptive = enabled;
}

void nds_title_patches_set_mph_mouse_aim(bool enabled) {
    g_mph_mouse_aim = enabled;
}

void nds_title_patches_set_mph_adventure_wide(bool enabled,
                                              uint16_t adaptive_width) {
    g_mph_adventure_wide = false;
    g_mph_adventure_width = 0;
    if (enabled) {
        uint32_t mov_r0_width = 0;
        if (adaptive_width < 256u || adaptive_width > 510u) {
            std::fprintf(stderr,
                "[mph] adaptive width %u is out of range for the adventure "
                "wide frustum; leaving it disabled\n",
                static_cast<unsigned>(adaptive_width));
        } else if (!encode_arm_mov_r0_imm(adaptive_width, &mov_r0_width)) {
            std::fprintf(stderr,
                "[mph] adaptive width %u is not an ARM immediate; leaving the "
                "adventure wide frustum disabled\n",
                static_cast<unsigned>(adaptive_width));
        } else {
            g_mph_wide_words[0] = static_cast<uint32_t>(
                (kNativeAspect * adaptive_width + 128) / 256);
            g_mph_wide_words[1] = 0xE2810000u | (adaptive_width - 255u);
            g_mph_wide_words[2] = mov_r0_width;
            g_mph_adventure_wide = true;
            g_mph_adventure_width = adaptive_width;
        }
    }
    if (!g_mph_adventure_wide) {
        g_mph_wide_active = false;
        nds_gpu3d_set_guest_wide_projection(false);
    }
}

bool nds_title_patches_apply_mph_mouse_delta(int32_t dx, int32_t dy) {
    if (!g_mph_mouse_aim || (dx == 0 && dy == 0)) return false;
    if (dx != 0)
        bus_write_u32_slow(kMphUs10AimX, static_cast<uint32_t>(dx));
    if (dy != 0)
        bus_write_u32_slow(kMphUs10AimY, static_cast<uint32_t>(dy));
    return true;
}

static_assert(kMphWideSiteCount ==
              sizeof(NdsTitlePatchDebugState::mph_adventure_wide_site_applied) /
                  sizeof(uint64_t));

NdsTitlePatchDebugState nds_title_patches_debug_state() {
    NdsTitlePatchDebugState state{};
    state.mph_adventure_wide_enabled = g_mph_adventure_wide;
    state.mph_adventure_wide_active = g_mph_wide_active;
    state.mph_adventure_wide_width = g_mph_adventure_width;
    for (uint32_t i = 0; i < kMphWideSiteCount; ++i)
        state.mph_adventure_wide_site_applied[i] = g_mph_wide_applied[i];
    state.mph_adventure_wide_frames_active = g_mph_wide_frames_active;
    state.mph_adventure_wide_frames_inactive = g_mph_wide_frames_inactive;
    return state;
}

void nds_title_patches_start_frame() {
    if (g_sm64ds_adaptive) patch_sm64ds_clipper();
    if (g_mph_adventure_wide) patch_mph_adventure_wide();
}
