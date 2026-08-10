#pragma once

#include <cstdint>

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
