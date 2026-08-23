#include "relative_mouse_touch.h"

#include <algorithm>
#include <limits>

namespace {

int32_t clamp_i32(int64_t value) {
    return static_cast<int32_t>(std::clamp<int64_t>(
        value, std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max()));
}

bool point_in_rect(const NdsLogicalRect& rect, int x, int y) {
    return x >= rect.x && x < rect.x + rect.width &&
           y >= rect.y && y < rect.y + rect.height;
}

}  // namespace

NdsStackedRelativeMouseRoute nds_route_stacked_relative_mouse_button(
    bool relative_mouse_enabled, bool relative_mouse_captured,
    const NdsLogicalRect& top_screen, const NdsLogicalRect& bottom_touch,
    int x, int y) {
    if (relative_mouse_captured)
        return NdsStackedRelativeMouseRoute::CapturedButton;
    if (relative_mouse_enabled && point_in_rect(top_screen, x, y))
        return NdsStackedRelativeMouseRoute::AcquireRelative;
    if (point_in_rect(bottom_touch, x, y))
        return NdsStackedRelativeMouseRoute::Touchscreen;
    return NdsStackedRelativeMouseRoute::None;
}

NdsRelativeMouseDelta nds_scale_relative_mouse_delta(
    int64_t dx, int64_t dy, uint16_t sensitivity_percent, bool invert_y,
    uint16_t y_scale_percent) {
    const int64_t scaled_x = dx * sensitivity_percent / 100;
    int64_t scaled_y =
        dy * sensitivity_percent * y_scale_percent / 10000;
    if (invert_y) scaled_y = -scaled_y;
    return {clamp_i32(scaled_x), clamp_i32(scaled_y)};
}

void NdsRelativeMouseTouch::capture(uint16_t sensitivity_percent,
                                    bool invert_y) {
    captured_ = true;
    invert_y_ = invert_y;
    sensitivity_percent_ = sensitivity_percent;
    x_fixed_ = kCenterX * kFraction;
    y_fixed_ = kCenterY * kFraction;
}

void NdsRelativeMouseTouch::release() {
    captured_ = false;
}

bool NdsRelativeMouseTouch::move(int32_t dx, int32_t dy) {
    if (!captured_ || (dx == 0 && dy == 0)) return false;
    const int64_t x_delta =
        static_cast<int64_t>(dx) * sensitivity_percent_ * kFraction / 100;
    int64_t y_delta =
        static_cast<int64_t>(dy) * sensitivity_percent_ * kFraction / 100;
    if (invert_y_) y_delta = -y_delta;
    const int64_t next_x = static_cast<int64_t>(x_fixed_) + x_delta;
    const int64_t next_y = static_cast<int64_t>(y_fixed_) + y_delta;
    x_fixed_ = static_cast<int32_t>(std::clamp<int64_t>(
        next_x, 0, static_cast<int64_t>(kMaxX) * kFraction));
    y_fixed_ = static_cast<int32_t>(std::clamp<int64_t>(
        next_y, 0, static_cast<int64_t>(kMaxY) * kFraction));
    return true;
}

uint16_t NdsRelativeMouseTouch::x() const {
    return static_cast<uint16_t>(x_fixed_ / kFraction);
}

uint16_t NdsRelativeMouseTouch::y() const {
    return static_cast<uint16_t>(y_fixed_ / kFraction);
}
