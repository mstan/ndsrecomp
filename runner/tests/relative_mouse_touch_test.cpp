#include "relative_mouse_touch.h"

namespace {

bool require(bool value) {
    return value;
}

}  // namespace

int main() {
    NdsRelativeMouseTouch mouse;
    if (!require(!mouse.captured()) || !require(!mouse.move(10, 10))) return 1;

    mouse.capture(100, false);
    if (!require(mouse.captured()) || !require(mouse.x() == 128) ||
        !require(mouse.y() == 96)) return 2;
    if (!require(mouse.move(12, -7)) || !require(mouse.x() == 140) ||
        !require(mouse.y() == 89)) return 3;

    mouse.capture(50, false);
    if (!require(mouse.move(3, 3)) || !require(mouse.x() == 129) ||
        !require(mouse.y() == 97)) return 4;
    if (!require(mouse.move(1, 1)) || !require(mouse.x() == 130) ||
        !require(mouse.y() == 98)) return 5;

    mouse.capture(200, true);
    if (!require(mouse.move(5, -4)) || !require(mouse.x() == 138) ||
        !require(mouse.y() == 104)) return 6;

    if (!require(mouse.move(10000, -10000)) || !require(mouse.x() == 255) ||
        !require(mouse.y() == 191)) return 7;
    if (!require(mouse.move(-10000, 10000)) || !require(mouse.x() == 0) ||
        !require(mouse.y() == 0)) return 8;

    mouse.release();
    if (!require(!mouse.captured()) || !require(!mouse.move(1, 1))) return 9;

    NdsRelativeMouseDelta delta =
        nds_scale_relative_mouse_delta(20, -10, 100, false, 150);
    if (!require(delta.x == 20) || !require(delta.y == -15)) return 10;
    delta = nds_scale_relative_mouse_delta(3, -3, 50, true, 150);
    if (!require(delta.x == 1) || !require(delta.y == 2)) return 11;
    delta = nds_scale_relative_mouse_delta(
        static_cast<int64_t>(1) << 40, -(static_cast<int64_t>(1) << 40),
        400, false, 150);
    if (!require(delta.x == 2147483647) ||
        !require(delta.y == static_cast<int32_t>(0x80000000u)))
        return 12;

    const NdsLogicalRect adaptive_top{0, 0, 448, 192};
    const NdsLogicalRect centered_bottom{96, 192, 256, 192};
    if (!require(nds_route_stacked_relative_mouse_button(
            true, false, adaptive_top, centered_bottom, 20, 100) ==
                 NdsStackedRelativeMouseRoute::AcquireRelative))
        return 13;
    if (!require(nds_route_stacked_relative_mouse_button(
            true, false, adaptive_top, centered_bottom, 223, 300) ==
                 NdsStackedRelativeMouseRoute::Touchscreen) ||
        !require(nds_route_stacked_relative_mouse_button(
            true, false, adaptive_top, centered_bottom, 223, 192) ==
                 NdsStackedRelativeMouseRoute::Touchscreen) ||
        !require(nds_route_stacked_relative_mouse_button(
            true, false, adaptive_top, centered_bottom, 20, 192) ==
                 NdsStackedRelativeMouseRoute::None))
        return 14;
    if (!require(nds_route_stacked_relative_mouse_button(
            true, true, adaptive_top, centered_bottom, -20, 500) ==
                 NdsStackedRelativeMouseRoute::CapturedButton) ||
        !require(nds_route_stacked_relative_mouse_button(
            true, true, adaptive_top, centered_bottom, 223, 300) ==
                 NdsStackedRelativeMouseRoute::CapturedButton))
        return 15;
    return 0;
}
