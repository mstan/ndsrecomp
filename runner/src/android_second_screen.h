#pragma once
#include <cstdint>

// Present the DS bottom-screen framebuffer onto the AYN Thor's second physical
// display. The Android Surface is supplied from Java (MyGame) via
// nativeSetSecondSurface; the frontend calls _present() once per frame with the
// bottom framebuffer (RGBX8888). No-ops until a surface is attached, and on
// non-Android builds these are not compiled at all.
#if defined(__ANDROID__)
void android_second_screen_present(const uint32_t* pixels, int width,
                                   int height);
bool android_second_screen_active();
// Current DS-bottom touch state forwarded from the second display's touch
// surface. Coordinates are in DS space (0..255, 0..191). Always returns true.
bool android_second_screen_touch(int* x, int* y, bool* down);
#endif
