#if defined(__ANDROID__)
#include "android_second_screen.h"

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace {
std::mutex g_mtx;
ANativeWindow* g_win = nullptr;
int g_geom_w = 0;
int g_geom_h = 0;

// DS bottom-screen touch state forwarded from the second display's SurfaceView.
std::atomic<int> g_touch_x{0};
std::atomic<int> g_touch_y{0};
std::atomic<bool> g_touch_down{false};
}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_thor_mph_MyGame_nativeSetSecondSurface(JNIEnv* env, jclass,
                                                jobject surface) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_win) {
        ANativeWindow_release(g_win);
        g_win = nullptr;
        g_geom_w = g_geom_h = 0;
    }
    if (surface) {
        g_win = ANativeWindow_fromSurface(env, surface);
        __android_log_write(ANDROID_LOG_INFO, "ThorMPHrun",
                            g_win ? "[second-screen] surface attached"
                                  : "[second-screen] fromSurface failed");
    } else {
        __android_log_write(ANDROID_LOG_INFO, "ThorMPHrun",
                            "[second-screen] surface detached");
    }
}

// Normalized touch (nx,ny in [0,1] across the SurfaceView) -> DS 256x192.
extern "C" JNIEXPORT void JNICALL
Java_com_thor_mph_MyGame_nativeSecondScreenTouch(JNIEnv*, jclass, jfloat nx,
                                                 jfloat ny, jboolean down) {
    int x = static_cast<int>(nx * 255.0f + 0.5f);
    int y = static_cast<int>(ny * 191.0f + 0.5f);
    if (x < 0) x = 0; else if (x > 255) x = 255;
    if (y < 0) y = 0; else if (y > 191) y = 191;
    g_touch_x.store(x, std::memory_order_relaxed);
    g_touch_y.store(y, std::memory_order_relaxed);
    g_touch_down.store(down != JNI_FALSE, std::memory_order_relaxed);
}

bool android_second_screen_active() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_win != nullptr;
}

bool android_second_screen_touch(int* x, int* y, bool* down) {
    *x = g_touch_x.load(std::memory_order_relaxed);
    *y = g_touch_y.load(std::memory_order_relaxed);
    *down = g_touch_down.load(std::memory_order_relaxed);
    return true;
}

void android_second_screen_present(const uint32_t* pixels, int width,
                                   int height) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_win || !pixels || width <= 0 || height <= 0) return;
    if (width != g_geom_w || height != g_geom_h) {
        ANativeWindow_setBuffersGeometry(g_win, width, height,
                                         WINDOW_FORMAT_RGBA_8888);
        g_geom_w = width;
        g_geom_h = height;
    }
    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(g_win, &buf, nullptr) != 0) return;
    const int copy_h = height < buf.height ? height : buf.height;
    const int copy_w = width < buf.width ? width : buf.width;
    uint32_t* dst = static_cast<uint32_t*>(buf.bits);
    // Framebuffer is ARGB8888 (0xAARRGGBB); Android RGBA_8888 wants memory
    // [R,G,B,A] i.e. 0xAABBGGRR. Swap red and blue, keep alpha and green.
    for (int y = 0; y < copy_h; ++y) {
        const uint32_t* src_row = pixels + static_cast<size_t>(y) * width;
        uint32_t* dst_row = dst + static_cast<size_t>(y) * buf.stride;
        for (int x = 0; x < copy_w; ++x) {
            const uint32_t p = src_row[x];
            dst_row[x] = (p & 0xFF00FF00u) |
                         ((p & 0x00FF0000u) >> 16) |
                         ((p & 0x000000FFu) << 16);
        }
    }
    ANativeWindow_unlockAndPost(g_win);
}
#endif  // __ANDROID__
