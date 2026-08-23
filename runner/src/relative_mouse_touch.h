#pragma once

#include <cstdint>

struct NdsRelativeMouseDelta {
    int32_t x = 0;
    int32_t y = 0;
};

struct NdsLogicalRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

enum class NdsStackedRelativeMouseRoute : uint8_t {
    None,
    AcquireRelative,
    Touchscreen,
    CapturedButton,
};

// Pure shared-window left-button routing. Before capture, only the rendered
// top rectangle may acquire relative mode; the native bottom touch rectangle
// remains touchscreen input. Once captured, absolute cursor coordinates are
// intentionally ignored.
NdsStackedRelativeMouseRoute nds_route_stacked_relative_mouse_button(
    bool relative_mouse_enabled, bool relative_mouse_captured,
    const NdsLogicalRect& top_screen, const NdsLogicalRect& bottom_touch,
    int x, int y);

// Scale a frame's unbounded host delta for a title-owned direct-aim path.
// The Y scale accounts for games whose authored touch-look axes differ.
NdsRelativeMouseDelta nds_scale_relative_mouse_delta(
    int64_t dx, int64_t dy, uint16_t sensitivity_percent, bool invert_y,
    uint16_t y_scale_percent = 100);

// Pure, SDL-independent relative-mouse to DS-stylus transform. The frontend
// owns capture/focus policy; this helper owns deterministic fixed-point
// sensitivity, inversion, and native touchscreen bounds.
class NdsRelativeMouseTouch {
public:
    void capture(uint16_t sensitivity_percent, bool invert_y);
    void release();
    bool move(int32_t dx, int32_t dy);

    bool captured() const { return captured_; }
    uint16_t x() const;
    uint16_t y() const;

private:
    static constexpr int32_t kFraction = 256;
    static constexpr int32_t kCenterX = 128;
    static constexpr int32_t kCenterY = 96;
    static constexpr int32_t kMaxX = 255;
    static constexpr int32_t kMaxY = 191;

    bool captured_ = false;
    bool invert_y_ = false;
    uint16_t sensitivity_percent_ = 100;
    int32_t x_fixed_ = kCenterX * kFraction;
    int32_t y_fixed_ = kCenterY * kFraction;
};
