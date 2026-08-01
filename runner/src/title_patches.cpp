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

bool g_sm64ds_adaptive = false;
bool g_logged_sm64ds_clipper = false;

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

}  // namespace

void nds_title_patches_set_sm64ds_adaptive(bool enabled) {
    g_sm64ds_adaptive = enabled;
}

void nds_title_patches_start_frame() {
    if (g_sm64ds_adaptive) patch_sm64ds_clipper();
}
