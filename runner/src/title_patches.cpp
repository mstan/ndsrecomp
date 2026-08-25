#include "title_patches.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

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
// AMHE0's room traversal clips candidate door/portal polygons against the
// native 256x192 view before marking adjacent room parts visible. Adaptive
// top-screen output can expose portals outside that native frustum, producing
// black doorway voids even though the host renderer can draw the wider scene.
constexpr uint32_t kMphUs10PortalClipFunc = 0x021174D4u;
constexpr uint32_t kMphUs10PortalClipReturn = 0x021118B4u;
constexpr uint32_t kMphUs10PortalRejectTarget = 0x02111C40u;
constexpr uint32_t kMphUs10PortalMinXReject = 0x02111B60u;
constexpr uint32_t kMphUs10PortalMinXFallthrough = 0x02111B64u;
constexpr uint32_t kMphUs10ViewportPtr = 0x020BCA70u;
constexpr uint32_t kMphViewportMaxY = 0x664u;
constexpr uint32_t kMphViewportMinY = 0x668u;
constexpr uint32_t kMphViewportMinX = 0x66Cu;
constexpr uint32_t kMphViewportMaxX = 0x670u;
constexpr int32_t kMphAdaptiveViewportMinX = -96;
constexpr int32_t kMphAdaptiveViewportMaxX = 351;
constexpr uint32_t kMphPortalPointStride = 12u;
constexpr uint32_t kMphPortalMaxPoints = 16u;

bool g_sm64ds_adaptive = false;
bool g_mph_mouse_aim = false;
bool g_mph_adaptive_room_culling = false;
bool g_logged_sm64ds_clipper = false;
bool g_logged_mph_portal_culling = false;
bool g_logged_mph_portal_min_x = false;
bool g_logged_mph_viewport = false;
uint64_t g_mph_portal_clip_calls = 0;
uint64_t g_mph_portal_min_x_relaxed = 0;
uint64_t g_mph_viewport_patches = 0;
int32_t g_mph_last_viewport_min_x = 0;
int32_t g_mph_last_viewport_max_x = 0;
int32_t g_mph_last_viewport_min_y = 0;
int32_t g_mph_last_viewport_max_y = 0;

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

void patch_mph_adaptive_viewport() {
    int32_t viewport_ptr_signed = 0;
    if (!read_main_ram32(kMphUs10ViewportPtr, &viewport_ptr_signed))
        return;
    const uint32_t viewport =
        static_cast<uint32_t>(viewport_ptr_signed);
    if (viewport < kMainRamBase) return;

    int32_t min_x = 0;
    int32_t max_x = 0;
    int32_t min_y = 0;
    int32_t max_y = 0;
    if (!read_main_ram32(viewport + kMphViewportMinX, &min_x) ||
        !read_main_ram32(viewport + kMphViewportMaxX, &max_x) ||
        !read_main_ram32(viewport + kMphViewportMinY, &min_y) ||
        !read_main_ram32(viewport + kMphViewportMaxY, &max_y)) {
        return;
    }
    g_mph_last_viewport_min_x = min_x;
    g_mph_last_viewport_max_x = max_x;
    g_mph_last_viewport_min_y = min_y;
    g_mph_last_viewport_max_y = max_y;

    if (min_y != 0 || max_y != 191) return;
    if (!((min_x == 0 && max_x == 255) ||
          (min_x == kMphAdaptiveViewportMinX &&
           max_x == kMphAdaptiveViewportMaxX))) {
        return;
    }

    bus_write_u32_slow(
        viewport + kMphViewportMinX,
        static_cast<uint32_t>(kMphAdaptiveViewportMinX));
    bus_write_u32_slow(
        viewport + kMphViewportMaxX,
        static_cast<uint32_t>(kMphAdaptiveViewportMaxX));
    ++g_mph_viewport_patches;
    g_mph_last_viewport_min_x = kMphAdaptiveViewportMinX;
    g_mph_last_viewport_max_x = kMphAdaptiveViewportMaxX;

    if (!g_logged_mph_viewport) {
        std::fprintf(stderr,
                     "[mph] adaptive viewport culling bounds widened\n");
        g_logged_mph_viewport = true;
    }
}

}  // namespace

void nds_title_patches_set_sm64ds_adaptive(bool enabled) {
    g_sm64ds_adaptive = enabled;
}

void nds_title_patches_set_mph_mouse_aim(bool enabled) {
    g_mph_mouse_aim = enabled;
}

void nds_title_patches_set_mph_adaptive_room_culling(bool enabled) {
    g_mph_adaptive_room_culling = enabled;
}

bool nds_title_patches_apply_mph_mouse_delta(int32_t dx, int32_t dy) {
    if (!g_mph_mouse_aim || (dx == 0 && dy == 0)) return false;
    if (dx != 0)
        bus_write_u32_slow(kMphUs10AimX, static_cast<uint32_t>(dx));
    if (dy != 0)
        bus_write_u32_slow(kMphUs10AimY, static_cast<uint32_t>(dy));
    return true;
}

bool nds_title_patches_handle_literal_branch(uint32_t source_pc,
                                             uint32_t target_pc) {
    if (!g_mph_adaptive_room_culling ||
        g_nds_active != NDS_ARM9 ||
        source_pc != kMphUs10PortalMinXReject ||
        target_pc != kMphUs10PortalRejectTarget) {
        return false;
    }

    const int32_t min_x = static_cast<int32_t>(g_cpu.R[11]);
    if (min_x < kMphAdaptiveViewportMinX * 4096) return false;

    ++g_mph_portal_min_x_relaxed;
    g_cpu.R[15] = kMphUs10PortalMinXFallthrough;
    g_cpu.cpsr &= ~CPSR_T_BIT;
    runtime_dispatch(kMphUs10PortalMinXFallthrough);

    if (!g_logged_mph_portal_min_x) {
        std::fprintf(stderr,
                     "[mph] adaptive room portal min-x culling relaxed\n");
        g_logged_mph_portal_min_x = true;
    }
    return true;
}

bool nds_title_patches_handle_literal_call(uint32_t target_pc) {
    if (!g_mph_adaptive_room_culling ||
        g_nds_active != NDS_ARM9 ||
        target_pc != kMphUs10PortalClipFunc ||
        (g_cpu.R[14] & ~3u) != kMphUs10PortalClipReturn) {
        return false;
    }

    const uint32_t src = g_cpu.R[1];
    const uint32_t count = g_cpu.R[2];
    const uint32_t dst = g_cpu.R[3];
    if (count > kMphPortalMaxPoints) return false;
    ++g_mph_portal_clip_calls;

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t src_point = src + i * kMphPortalPointStride;
        const uint32_t dst_point = dst + i * kMphPortalPointStride;
        bus_write_u32_slow(dst_point + 0u, bus_read_u32_slow(src_point + 0u));
        bus_write_u32_slow(dst_point + 4u, bus_read_u32_slow(src_point + 4u));
        bus_write_u32_slow(dst_point + 8u, bus_read_u32_slow(src_point + 8u));
    }

    g_cpu.R[0] = count;
    g_cpu.R[15] = kMphUs10PortalClipReturn;
    g_cpu.cpsr &= ~CPSR_T_BIT;
    runtime_call_should_return(kMphUs10PortalClipReturn);

    if (!g_logged_mph_portal_culling) {
        std::fprintf(stderr,
                     "[mph] adaptive room portal culling relaxed\n");
        g_logged_mph_portal_culling = true;
    }
    return true;
}

NdsTitlePatchDebugState nds_title_patches_debug_state() {
    return {
        g_mph_adaptive_room_culling,
        g_mph_portal_clip_calls,
        g_mph_portal_min_x_relaxed,
        g_mph_viewport_patches,
        g_mph_last_viewport_min_x,
        g_mph_last_viewport_max_x,
        g_mph_last_viewport_min_y,
        g_mph_last_viewport_max_y,
    };
}

void nds_title_patches_start_frame() {
    if (g_sm64ds_adaptive) patch_sm64ds_clipper();
    if (g_mph_adaptive_room_culling) patch_mph_adaptive_viewport();
}
