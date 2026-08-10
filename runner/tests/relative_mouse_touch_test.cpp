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
    return 0;
}
