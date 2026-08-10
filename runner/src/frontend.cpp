#include "frontend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include "debug_server.h"
#include "gpu2d.h"
#include "gpu3d.h"
#include "io.h"
#include "profile_report.h"
#include "relative_mouse_touch.h"
#include "scheduler.h"
#include "spu.h"
#if defined(NDS_HAVE_COMPUTE_RENDERER)
#include "melonds_compute/ComputeHost.h"
#endif

namespace {
// Written per presented frame by the frontend loop, read from the same
// thread by the `frontend_stats` debug command at the debug_pump() safe
// point. Stays all-zero (active=0) when no frontend is running.
NdsFrontendLiveStats g_live_stats{};
NdsFrontendBlackBandCapture g_black_band{};

void observe_top_black_bands(const uint32_t* pixels, uint64_t frame) {
    if (!g_black_band.enabled || !pixels) return;
    ++g_black_band.scanned_frames;

    uint32_t current_start = 0;
    uint32_t current_rows = 0;
    uint32_t longest_start = 0;
    uint32_t longest_rows = 0;
    for (uint32_t y = 0; y < 192; ++y) {
        uint32_t black_pixels = 0;
        for (uint32_t x = 0; x < 256; ++x) {
            if ((pixels[y * 256 + x] & 0x00FFFFFFu) == 0)
                ++black_pixels;
        }
        if (black_pixels >= 252) {
            if (current_rows == 0) current_start = y;
            ++current_rows;
            if (current_rows > longest_rows) {
                longest_start = current_start;
                longest_rows = current_rows;
            }
        } else {
            current_rows = 0;
        }
    }

    // Ignore an intentional full-screen fade. The reported artifact is a
    // partial band surrounded by otherwise-published image rows.
    if (longest_rows < 8 || longest_rows >= 192) return;
    ++g_black_band.band_frames;
    if (longest_rows <= g_black_band.worst_row_count) return;
    g_black_band.has_capture = 1;
    g_black_band.worst_frame = frame;
    g_black_band.worst_system_timestamp = scheduler_system_timestamp();
    g_black_band.worst_start_row = longest_start;
    g_black_band.worst_row_count = longest_rows;
    std::memcpy(g_black_band.top_pixels, pixels,
                sizeof(g_black_band.top_pixels));
}
}  // namespace

#if defined(NDS_HAVE_SDL2)
#define SDL_MAIN_HANDLED
#include <SDL.h>

namespace {

constexpr int kScreenWidth = 256;
constexpr int kScreenHeight = 192;
constexpr int kWindowScale = 2;
constexpr uint64_t kSystemCyclesPerFrame = 2130ull * 263ull;
constexpr int kAudioFrequency = 33513982 / 1024;
constexpr uint32_t kAudioQueueFrames = 2048;
// Playback starts only once kAudioStartFrames (~1.5 s) are queued. The cold
// boot's frames ~5-131 emulate below real time with a measured cumulative
// production deficit of up to ~1.15 s; prebuffering more than that rides the
// whole window out with zero gaps — the stream stays bit-exact, only the
// initial latency is higher. The bounded drain (see drain_audio) then glides
// the queue back down to the ~63 ms steady-state target over a couple of
// seconds without ever freezing video, well before the Health & Safety
// screen needs interactive input.
constexpr uint32_t kAudioStartFrames = 57344;
constexpr uint32_t kAudioFrameBytes = 2u * sizeof(int16_t);
constexpr uint32_t kAudioCapacityFrames = 65536;

struct AudioQueue {
    std::array<int16_t, kAudioCapacityFrames * 2> samples{};
    uint32_t read = 0;
    uint32_t write = 0;
    uint32_t count = 0;
    std::atomic<uint64_t> underruns{0};
    std::atomic<bool> started{false};
};

void SDLCALL audio_callback(void* userdata, Uint8* stream, int len) {
    auto* queue = static_cast<AudioQueue*>(userdata);
    std::memset(stream, 0, static_cast<size_t>(len));
    if (!queue || len <= 0) return;
    const uint32_t requested = static_cast<uint32_t>(len) / kAudioFrameBytes;
    const uint32_t take = std::min(requested, queue->count);
    auto* output = reinterpret_cast<int16_t*>(stream);
    const uint32_t first = std::min(take, kAudioCapacityFrames - queue->read);
    std::memcpy(output, queue->samples.data() + queue->read * 2u,
                first * kAudioFrameBytes);
    if (first < take)
        std::memcpy(output + first * 2u, queue->samples.data(),
                    (take - first) * kAudioFrameBytes);
    queue->read = (queue->read + take) % kAudioCapacityFrames;
    queue->count -= take;
    if (take < requested && queue->started.load(std::memory_order_relaxed))
        queue->underruns.fetch_add(1, std::memory_order_relaxed);
}

uint16_t key_bit(SDL_Scancode key) {
    // KEYINPUT/EXTKEYIN are active-low. This layout follows the common DS
    // emulator convention: Z/X = A/B, A/S = Y/X, Q/W = L/R.
    switch (key) {
        case SDL_SCANCODE_Z:         return 1u << 0;  // A
        case SDL_SCANCODE_X:         return 1u << 1;  // B
        case SDL_SCANCODE_BACKSPACE: return 1u << 2;  // Select
        case SDL_SCANCODE_RETURN:    return 1u << 3;  // Start
        case SDL_SCANCODE_RIGHT:     return 1u << 4;
        case SDL_SCANCODE_LEFT:      return 1u << 5;
        case SDL_SCANCODE_UP:        return 1u << 6;
        case SDL_SCANCODE_DOWN:      return 1u << 7;
        case SDL_SCANCODE_W:         return 1u << 8;  // R
        case SDL_SCANCODE_Q:         return 1u << 9;  // L
        case SDL_SCANCODE_S:         return 1u << 10; // X
        case SDL_SCANCODE_A:         return 1u << 11; // Y
        default:                     return 0;
    }
}

uint16_t controller_bit(SDL_GameControllerButton button) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_A:             return 1u << 0;
        case SDL_CONTROLLER_BUTTON_B:             return 1u << 1;
        case SDL_CONTROLLER_BUTTON_BACK:          return 1u << 2;
        case SDL_CONTROLLER_BUTTON_START:         return 1u << 3;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return 1u << 4;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return 1u << 5;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:       return 1u << 6;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return 1u << 7;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 1u << 8;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return 1u << 9;
        case SDL_CONTROLLER_BUTTON_X:             return 1u << 10;
        case SDL_CONTROLLER_BUTTON_Y:             return 1u << 11;
        default:                                  return 0;
    }
}

SDL_GameController* open_first_controller() {
    for (int index = 0; index < SDL_NumJoysticks(); ++index) {
        if (!SDL_IsGameController(index)) continue;
        if (SDL_GameController* controller = SDL_GameControllerOpen(index)) {
            std::fprintf(stderr, "[sdl] Player 1 controller: %s\n",
                         SDL_GameControllerName(controller));
            return controller;
        }
    }
    return nullptr;
}

void set_touch_from_mouse(int window_x, int window_y, bool down,
                          NdsScreenLayout layout, int logical_width) {
    // SDL_RenderSetLogicalSize also maps absolute mouse events into the
    // renderer's logical coordinate system. Calling RenderWindowToLogical a
    // second time halves coordinates at 2x scale (and turns bottom-screen
    // clicks into top-screen clicks), so consume the event coordinates as-is.
    const float x = static_cast<float>(window_x);
    const float y = static_cast<float>(window_y);
    const float bottom_origin =
        layout == NdsScreenLayout::Separate ? 0.0f : kScreenHeight;
    const float left =
        static_cast<float>((logical_width - kScreenWidth) / 2);
    if (!down || x < left || x >= left + kScreenWidth ||
        y < bottom_origin || y >= bottom_origin + kScreenHeight) {
        nds_set_touch(0, 0, false);
        return;
    }
    const auto touch_x = static_cast<uint16_t>(std::clamp<int>(
        static_cast<int>(x - left), 0, kScreenWidth - 1));
    const auto touch_y = static_cast<uint16_t>(std::clamp<int>(
        static_cast<int>(y - bottom_origin), 0, kScreenHeight - 1));
    nds_set_touch(touch_x, touch_y, true);
}

uint32_t audio_queue_count(SDL_AudioDeviceID device, AudioQueue& queue) {
    if (!device) return 0;
    SDL_LockAudioDevice(device);
    const uint32_t count = queue.count;
    SDL_UnlockAudioDevice(device);
    return count;
}

uint32_t drain_audio(SDL_AudioDeviceID device, AudioQueue& queue,
                     bool throttle, uint32_t pace_floor, bool& queue_error) {
    if (!device) return 0;
    std::array<int16_t, 2048> samples{};
    for (;;) {
        const uint32_t frames = nds_spu_read_output(samples.data(), 1024);
        if (!frames) break;
        bool pushed = false;
        while (!pushed) {
            SDL_LockAudioDevice(device);
            if (kAudioCapacityFrames - queue.count >= frames) {
                const uint32_t first = std::min(
                    frames, kAudioCapacityFrames - queue.write);
                std::memcpy(queue.samples.data() + queue.write * 2u,
                            samples.data(), first * kAudioFrameBytes);
                if (first < frames)
                    std::memcpy(queue.samples.data(),
                                samples.data() + first * 2u,
                                (frames - first) * kAudioFrameBytes);
                queue.write = (queue.write + frames) % kAudioCapacityFrames;
                queue.count += frames;
                pushed = true;
            }
            SDL_UnlockAudioDevice(device);
            if (!pushed) {
                if (!throttle) {
                    queue_error = true;
                    return audio_queue_count(device, queue);
                }
                SDL_Delay(1);
            }
        }
    }
    // Audio is the host's real-time clock. Never drop a produced block: if the
    // emulator is faster than the DS cadence, let SDL consume the backlog
    // before emulating another frame. pace_floor is the current allowance:
    // kAudioQueueFrames in steady state, temporarily higher right after the
    // boot prebuffer (the caller decays it a fixed step per frame). The sleep
    // only stops the queue RISING above the floor — it never forces the queue
    // down while the emulator is running behind, so a slow stretch spends the
    // buffered runway instead of having it slept away.
    uint32_t queued = audio_queue_count(device, queue);
    while (throttle && queued > pace_floor) {
        SDL_Delay(1);
        queued = audio_queue_count(device, queue);
    }
    return queued;
}

uint64_t environment_u64(const char* name) {
    const char* value = std::getenv(name);
    if (!value || !*value) return 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    return end == value ? 0 : static_cast<uint64_t>(parsed);
}

uint64_t framebuffer_rgb_fnv(int screen) {
    const uint32_t* framebuffer = nds_gpu2d_framebuffer(screen);
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < 256u * 192u; ++i) {
        const uint32_t pixel = framebuffer[i];
        const uint8_t rgb[3] = {
            static_cast<uint8_t>(pixel >> 16),
            static_cast<uint8_t>(pixel >> 8),
            static_cast<uint8_t>(pixel),
        };
        for (const uint8_t byte : rgb) {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

struct FrontendPresentation {
    bool separate = false;
    SDL_Window* windows[2]{};
    SDL_Renderer* renderers[2]{};
    SDL_Texture* textures[2]{};
    SDL_Texture* sample_targets[2]{};
    uint32_t window_ids[2]{};
    int screen_widths[2]{kScreenWidth, kScreenWidth};
    int canvas_width = kScreenWidth;
    int sample_scale = 1;
};

void destroy_presentation(FrontendPresentation& presentation) {
    for (SDL_Texture*& texture : presentation.sample_targets) {
        if (texture) SDL_DestroyTexture(texture);
        texture = nullptr;
    }
    for (SDL_Texture*& texture : presentation.textures) {
        if (texture) SDL_DestroyTexture(texture);
        texture = nullptr;
    }
    if (presentation.separate && presentation.renderers[1])
        SDL_DestroyRenderer(presentation.renderers[1]);
    if (presentation.renderers[0])
        SDL_DestroyRenderer(presentation.renderers[0]);
    presentation.renderers[0] = nullptr;
    presentation.renderers[1] = nullptr;
    if (presentation.separate && presentation.windows[1])
        SDL_DestroyWindow(presentation.windows[1]);
    if (presentation.windows[0])
        SDL_DestroyWindow(presentation.windows[0]);
    presentation.windows[0] = nullptr;
    presentation.windows[1] = nullptr;
}

SDL_Renderer* create_renderer(SDL_Window* window) {
    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    return renderer;
}

bool create_presentation(const NdsFrontendOptions& options,
                         FrontendPresentation& presentation) {
    presentation.separate =
        options.screen_layout == NdsScreenLayout::Separate;
    const int aa_scale = options.antialiasing >= 8 ? 4 :
                         options.antialiasing >= 4 ? 3 :
                         options.antialiasing >= 2 ? 2 : 1;
    presentation.sample_scale =
        std::max<int>(options.supersampling, aa_scale);
    for (int screen = 0; screen < 2; ++screen) {
        const uint8_t bit = static_cast<uint8_t>(1u << screen);
        if ((options.adaptive_screens & bit) &&
            (options.adaptive_supported & bit)) {
            presentation.screen_widths[screen] =
                options.adaptive_max_width[screen];
        }
    }
    presentation.canvas_width = std::max(
        presentation.screen_widths[0],
        presentation.screen_widths[1]);
    const int first_height = presentation.separate
        ? kScreenHeight * kWindowScale
        : kScreenHeight * 2 * kWindowScale;
    presentation.windows[0] = SDL_CreateWindow(
        presentation.separate ? "ndsrecomp - Top Screen"
                              : "ndsrecomp firmware preview",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        (presentation.separate ? presentation.screen_widths[0]
                               : presentation.canvas_width) * kWindowScale,
        first_height,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!presentation.windows[0]) {
        std::fprintf(stderr, "[sdl] window failed: %s\n", SDL_GetError());
        return false;
    }
    presentation.renderers[0] = create_renderer(presentation.windows[0]);
    if (!presentation.renderers[0]) {
        std::fprintf(stderr, "[sdl] renderer failed: %s\n", SDL_GetError());
        destroy_presentation(presentation);
        return false;
    }

    if (presentation.separate) {
        int top_x = 0;
        int top_y = 0;
        SDL_GetWindowPosition(presentation.windows[0], &top_x, &top_y);
        presentation.windows[1] = SDL_CreateWindow(
            "ndsrecomp - Bottom Screen",
            top_x + presentation.screen_widths[0] * kWindowScale + 32,
            top_y,
            presentation.screen_widths[1] * kWindowScale,
            kScreenHeight * kWindowScale,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!presentation.windows[1]) {
            std::fprintf(stderr, "[sdl] bottom window failed: %s\n",
                         SDL_GetError());
            destroy_presentation(presentation);
            return false;
        }
        presentation.renderers[1] =
            create_renderer(presentation.windows[1]);
        if (!presentation.renderers[1]) {
            std::fprintf(stderr, "[sdl] bottom renderer failed: %s\n",
                         SDL_GetError());
            destroy_presentation(presentation);
            return false;
        }
    } else {
        presentation.windows[1] = presentation.windows[0];
        presentation.renderers[1] = presentation.renderers[0];
    }

    for (int screen = 0; screen < 2; ++screen) {
        const int logical_height =
            !presentation.separate && screen == 0
                ? kScreenHeight * 2 : kScreenHeight;
        if (screen == 0 || presentation.separate) {
            SDL_RenderSetLogicalSize(presentation.renderers[screen],
                presentation.separate
                    ? presentation.screen_widths[screen]
                    : presentation.canvas_width,
                logical_height);
            SDL_RenderSetIntegerScale(presentation.renderers[screen],
                                      SDL_TRUE);
        }
        presentation.textures[screen] = SDL_CreateTexture(
            presentation.renderers[screen], SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            presentation.screen_widths[screen], kScreenHeight);
        if (!presentation.textures[screen]) {
            std::fprintf(stderr, "[sdl] texture failed: %s\n",
                         SDL_GetError());
            destroy_presentation(presentation);
            return false;
        }
        if (presentation.sample_scale > 1) {
            presentation.sample_targets[screen] = SDL_CreateTexture(
                presentation.renderers[screen], SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_TARGET,
                presentation.screen_widths[screen] *
                    presentation.sample_scale,
                kScreenHeight * presentation.sample_scale);
            if (!presentation.sample_targets[screen]) {
                std::fprintf(stderr,
                             "[sdl] supersample target failed: %s\n",
                             SDL_GetError());
                destroy_presentation(presentation);
                return false;
            }
        }
        presentation.window_ids[screen] =
            SDL_GetWindowID(presentation.windows[screen]);
    }
    return true;
}

void render_screen(FrontendPresentation& presentation, int screen,
                   const SDL_Rect& destination) {
    SDL_Renderer* renderer = presentation.renderers[screen];
    SDL_Texture* source = presentation.textures[screen];
    if (presentation.sample_targets[screen]) {
        SDL_SetRenderTarget(renderer, presentation.sample_targets[screen]);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, source, nullptr, nullptr);
        SDL_SetRenderTarget(renderer, nullptr);
        source = presentation.sample_targets[screen];
    }
    SDL_RenderCopy(renderer, source, nullptr, &destination);
}

struct PresentationTicks {
    uint64_t upload = 0;
    uint64_t draw = 0;
    uint64_t swap = 0;
};

PresentationTicks present_screens(FrontendPresentation& presentation,
                                  const uint32_t* top_pixels,
                                  int top_width,
                                  const uint32_t* bottom_pixels,
                                  int bottom_width) {
    PresentationTicks ticks{};
    uint64_t start = SDL_GetPerformanceCounter();
    SDL_UpdateTexture(presentation.textures[0], nullptr, top_pixels,
                      top_width * sizeof(uint32_t));
    SDL_UpdateTexture(presentation.textures[1], nullptr, bottom_pixels,
                      bottom_width * sizeof(uint32_t));
    ticks.upload += SDL_GetPerformanceCounter() - start;
    if (!presentation.separate) {
        SDL_Renderer* renderer = presentation.renderers[0];
        start = SDL_GetPerformanceCounter();
        SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
        SDL_RenderClear(renderer);
        const SDL_Rect top_rect{
            (presentation.canvas_width -
             presentation.screen_widths[0]) / 2,
            0, presentation.screen_widths[0], kScreenHeight};
        const SDL_Rect bottom_rect{
            (presentation.canvas_width -
             presentation.screen_widths[1]) / 2,
            kScreenHeight, presentation.screen_widths[1], kScreenHeight};
        render_screen(presentation, 0, top_rect);
        render_screen(presentation, 1, bottom_rect);
        ticks.draw += SDL_GetPerformanceCounter() - start;
        start = SDL_GetPerformanceCounter();
        SDL_RenderPresent(renderer);
        ticks.swap += SDL_GetPerformanceCounter() - start;
        return ticks;
    }

    for (int screen = 0; screen < 2; ++screen) {
        const SDL_Rect screen_rect{
            0, 0, presentation.screen_widths[screen], kScreenHeight};
        SDL_Renderer* renderer = presentation.renderers[screen];
        start = SDL_GetPerformanceCounter();
        SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
        SDL_RenderClear(renderer);
        render_screen(presentation, screen, screen_rect);
        ticks.draw += SDL_GetPerformanceCounter() - start;
        start = SDL_GetPerformanceCounter();
        SDL_RenderPresent(renderer);
        ticks.swap += SDL_GetPerformanceCounter() - start;
    }
    return ticks;
}

} // namespace

int nds_run_interactive_frontend(const NdsFrontendOptions& options) {
    SDL_SetMainReady();
    // SDL_INIT_TIMER matters on Windows: it raises the OS timer resolution
    // to 1 ms (SDL_HINT_TIMER_RESOLUTION default). Without it SDL_Delay(1)
    // sleeps a full ~15.6 ms scheduler quantum, so the audio-queue throttle
    // overshoots every frame, pinning the loop at ~57 FPS and cyclically
    // starving the audio queue (the audible boot crackle).
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS |
                 SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "[sdl] init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) != 0)
        std::fprintf(stderr, "[sdl] thread priority unchanged: %s\n",
                     SDL_GetError());

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
                (options.supersampling > 1 || options.antialiasing > 0)
                    ? "1" : "0");
    FrontendPresentation presentation{};
    if (!create_presentation(options, presentation)) {
        SDL_Quit();
        return 1;
    }
    const int bottom_logical_width = presentation.separate
        ? presentation.screen_widths[1]
        : presentation.canvas_width;
    const int bottom_content_left =
        (bottom_logical_width - kScreenWidth) / 2;
    std::fprintf(stderr,
        "[sdl] layout=%s adaptive=%s supersampling=%ux aa=%ux\n",
        nds_screen_layout_name(options.screen_layout),
        nds_adaptive_screens_name(options.adaptive_screens),
        static_cast<unsigned>(options.supersampling),
        static_cast<unsigned>(options.antialiasing));
    const uint16_t output_width = static_cast<uint16_t>(std::max(
        presentation.screen_widths[0],
        presentation.screen_widths[1]));
    if (!nds_gpu3d_set_output_width(output_width)) {
        std::fprintf(stderr,
                     "[sdl] adaptive 3D width %u is unavailable\n",
                     output_width);
        destroy_presentation(presentation);
        SDL_Quit();
        return 1;
    }

#if defined(NDS_HAVE_COMPUTE_RENDERER)
    // Activate only after every fallible visible-frontend allocation. From
    // here onward teardown always destroys the compute renderer while this
    // context is current.
    if (!nds_compute_host_start()) {
        destroy_presentation(presentation);
        SDL_Quit();
        return 1;
    }
#else
    if (const char* selection = std::getenv("NDS_3D_RENDERER")) {
        if (std::strcmp(selection, "compute") == 0)
            std::fprintf(stderr,
                         "[gpu3d] compute renderer not built; using soft\n");
    }
#endif

    AudioQueue audio_queue{};
    SDL_AudioSpec want{};
    // The mixer runs once per 1024 DS system cycles. Request its integer host
    // rate directly; the sub-sample remainder is absorbed by the bounded queue
    // instead of producing a roughly once-per-second underrun at 32768 Hz.
    want.freq = kAudioFrequency;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;
    want.userdata = &audio_queue;
    SDL_AudioSpec got{};
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(
        nullptr, 0, &want, &got, 0);
    if (audio && (got.freq != want.freq || got.format != want.format ||
                  got.channels != want.channels)) {
        std::fprintf(stderr,
            "[sdl] refusing mismatched audio format: want=%d/%u/%u "
            "got=%d/%u/%u\n",
            want.freq, want.format, want.channels,
            got.freq, got.format, got.channels);
        SDL_CloseAudioDevice(audio);
        audio = 0;
    }
    if (!audio)
        std::fprintf(stderr, "[sdl] audio unavailable: %s\n", SDL_GetError());

    std::fprintf(stderr,
        "[sdl] controls: gamepad=Player 1 | bottom mouse=touch | "
        "arrows=D-pad | Z=A X=B | A=Y S=X | Q=L W=R | "
        "Enter=Start Backspace=Select | Esc=quit\n");
    if (options.relative_mouse_touch) {
        std::fprintf(stderr,
            "[sdl] relative mouse: click top screen to capture; "
            "Esc or focus loss releases; sensitivity=%u%% invert-y=%s\n",
            static_cast<unsigned>(options.relative_mouse_sensitivity),
            options.relative_mouse_invert_y ? "on" : "off");
    }

    SDL_GameController* controller = open_first_controller();
    SDL_JoystickID controller_id = controller
        ? SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller))
        : -1;
    uint16_t keyboard_pressed = 0;
    uint16_t controller_pressed = 0;
    uint16_t mouse_pressed = 0;
    auto publish_keys = [&]() {
        nds_set_key_mask(static_cast<uint16_t>(
            0x0FFFu &
            ~(keyboard_pressed | controller_pressed | mouse_pressed)));
    };
    publish_keys();
    nds_set_touch(0, 0, false);
    bool running = true;
    bool compute_failed = false;
    bool mouse_down = false;
    bool touch_release_pending = false;
    uint32_t touch_frames_held = 0;
    NdsRelativeMouseTouch relative_mouse;
    auto release_relative_mouse = [&]() {
        if (relative_mouse.captured()) {
            SDL_SetRelativeMouseMode(SDL_FALSE);
            SDL_CaptureMouse(SDL_FALSE);
            relative_mouse.release();
            nds_set_touch(0, 0, false);
            std::fprintf(stderr, "[sdl] relative mouse released\n");
        }
        if (mouse_pressed != 0) {
            mouse_pressed = 0;
            publish_keys();
        }
    };
    auto capture_relative_mouse = [&]() {
        if (!options.relative_mouse_touch || relative_mouse.captured())
            return;
        if (SDL_SetRelativeMouseMode(SDL_TRUE) != 0) {
            std::fprintf(stderr,
                         "[sdl] relative mouse capture failed: %s\n",
                         SDL_GetError());
            return;
        }
        SDL_CaptureMouse(SDL_TRUE);
        SDL_GetRelativeMouseState(nullptr, nullptr);
        relative_mouse.capture(options.relative_mouse_sensitivity,
                               options.relative_mouse_invert_y);
        nds_set_touch(relative_mouse.x(), relative_mouse.y(), true);
        std::fprintf(stderr, "[sdl] relative mouse captured\n");
    };
    uint64_t shown_frames = 0;
    uint64_t fps_frames = 0;
    uint64_t fps_start = SDL_GetPerformanceCounter();
    const uint64_t frequency = SDL_GetPerformanceFrequency();
    const uint64_t soak_frames = environment_u64("NDS_FRONTEND_MAX_FRAMES");
    const bool print_stats = std::getenv("NDS_FRONTEND_STATS") != nullptr;
    const bool require_audio =
        std::getenv("NDS_FRONTEND_REQUIRE_AUDIO") != nullptr;
    const bool selftest_menu =
        std::getenv("NDS_FRONTEND_SELFTEST_MENU") != nullptr;
    const bool selftest_relative_mouse =
        std::getenv("NDS_FRONTEND_SELFTEST_RELATIVE_MOUSE") != nullptr;
    bool audio_started = false;
    bool audio_queue_error = false;
    uint32_t audio_pace_floor = kAudioQueueFrames;
    uint32_t audio_min_queue = std::numeric_limits<uint32_t>::max();
    uint32_t audio_max_queue = 0;
    uint64_t host_key_presses = 0;
    uint64_t host_touch_presses = 0;
    int last_touch_event_x = -1;
    int last_touch_event_y = -1;
    bool selftest_key_down = false;
    bool selftest_key_up = false;
    bool selftest_touch_down = false;
    bool selftest_touch_up = false;
    bool selftest_event_error = false;
    unsigned relative_mouse_selftest_stage = 0;
    bool relative_mouse_selftest_error = false;
    uint64_t phase_emu_ticks = 0;
    uint64_t phase_present_ticks = 0;
    uint64_t phase_adaptive_ticks = 0;
    uint64_t phase_upload_ticks = 0;
    uint64_t phase_draw_ticks = 0;
    uint64_t phase_swap_ticks = 0;
    uint64_t phase_drain_ticks = 0;
    g_live_stats = {};
    g_live_stats.active = 1;
    g_live_stats.freq = frequency;
    g_black_band = {};
    uint64_t max_emu_ticks = 0;
    uint64_t max_emu_frame = 0;
    uint64_t slow_frames_32ms = 0;
    uint64_t last_underruns_seen = 0;
    uint64_t first_underrun_frame = 0;
    uint64_t last_underrun_frame = 0;
    const uint64_t soak_start = SDL_GetPerformanceCounter();

    while (running) {
#if defined(NDS_HAVE_COMPUTE_RENDERER)
        if (!nds_compute_host_make_current()) {
            std::fprintf(stderr, "[gpu3d] lost compute GL context: %s\n",
                         SDL_GetError());
            compute_failed = true;
            running = false;
            break;
        }
#endif
        // Play-mode debug surface: execute any pending TCP command at this
        // between-frames safe point (no-op when no pump was started or no
        // client is connected). See debug_server.h.
        debug_pump();
        if (selftest_menu) {
            const NdsEventCounts& counts = nds_event_counts();
            SDL_Event injected{};
            if (!selftest_key_down && counts.vblank9 >= 10) {
                injected.type = SDL_KEYDOWN;
                injected.key.keysym.scancode = SDL_SCANCODE_Q;
                injected.key.repeat = 0;
                selftest_event_error |= SDL_PushEvent(&injected) < 0;
                selftest_key_down = true;
            } else if (selftest_key_down && !selftest_key_up &&
                       counts.vblank9 >= 12) {
                injected.type = SDL_KEYUP;
                injected.key.keysym.scancode = SDL_SCANCODE_Q;
                injected.key.repeat = 0;
                selftest_event_error |= SDL_PushEvent(&injected) < 0;
                selftest_key_up = true;
            }
            if (!selftest_touch_down && g_insn_count[0] >= 42300000) {
                injected = {};
                injected.type = SDL_MOUSEBUTTONDOWN;
                injected.button.windowID = presentation.window_ids[1];
                injected.button.button = SDL_BUTTON_LEFT;
                // SDL transforms window-tagged mouse events from physical
                // window pixels into the renderer's logical coordinates.
                injected.button.x =
                    (bottom_content_left + 127) * kWindowScale;
                injected.button.y = (presentation.separate
                    ? 180 : 192 + 180) * kWindowScale;
                selftest_event_error |= SDL_PushEvent(&injected) < 0;
                selftest_touch_down = true;
            } else if (selftest_touch_down && !selftest_touch_up &&
                       counts.vblank9 >= 116) {
                injected = {};
                injected.type = SDL_MOUSEBUTTONUP;
                injected.button.windowID = presentation.window_ids[1];
                injected.button.button = SDL_BUTTON_LEFT;
                injected.button.x =
                    (bottom_content_left + 127) * kWindowScale;
                injected.button.y = (presentation.separate
                    ? 180 : 192 + 180) * kWindowScale;
                selftest_event_error |= SDL_PushEvent(&injected) < 0;
                selftest_touch_up = true;
            }
        }
        if (selftest_relative_mouse) {
            SDL_Event injected{};
            if (relative_mouse_selftest_stage == 0 && shown_frames >= 2) {
                relative_mouse_selftest_error |=
                    !options.relative_mouse_touch || !presentation.separate ||
                    options.relative_mouse_fire_mask == 0;
                SDL_RaiseWindow(presentation.windows[0]);
                injected.type = SDL_MOUSEBUTTONDOWN;
                injected.button.windowID = presentation.window_ids[0];
                injected.button.button = SDL_BUTTON_LEFT;
                relative_mouse_selftest_error |= SDL_PushEvent(&injected) < 0;
                relative_mouse_selftest_stage = 1;
            } else if (relative_mouse_selftest_stage == 1 &&
                       shown_frames >= 3) {
                relative_mouse_selftest_error |= !relative_mouse.captured();
                injected.type = SDL_MOUSEMOTION;
                injected.motion.windowID = presentation.window_ids[0];
                injected.motion.xrel = 20;
                injected.motion.yrel = -10;
                relative_mouse_selftest_error |= SDL_PushEvent(&injected) < 0;
                relative_mouse_selftest_stage = 2;
            } else if (relative_mouse_selftest_stage == 2 &&
                       shown_frames >= 4) {
                relative_mouse_selftest_error |=
                    relative_mouse.x() == 128 && relative_mouse.y() == 96;
                injected.type = SDL_MOUSEBUTTONDOWN;
                injected.button.windowID = presentation.window_ids[0];
                injected.button.button = SDL_BUTTON_LEFT;
                relative_mouse_selftest_error |= SDL_PushEvent(&injected) < 0;
                relative_mouse_selftest_stage = 3;
            } else if (relative_mouse_selftest_stage == 3 &&
                       shown_frames >= 5) {
                relative_mouse_selftest_error |=
                    mouse_pressed != options.relative_mouse_fire_mask;
                injected.type = SDL_MOUSEBUTTONUP;
                injected.button.windowID = presentation.window_ids[0];
                injected.button.button = SDL_BUTTON_LEFT;
                relative_mouse_selftest_error |= SDL_PushEvent(&injected) < 0;
                relative_mouse_selftest_stage = 4;
            } else if (relative_mouse_selftest_stage == 4 &&
                       shown_frames >= 6) {
                relative_mouse_selftest_error |= mouse_pressed != 0;
                injected.type = SDL_WINDOWEVENT;
                injected.window.windowID = presentation.window_ids[0];
                injected.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
                relative_mouse_selftest_error |= SDL_PushEvent(&injected) < 0;
                relative_mouse_selftest_stage = 5;
            } else if (relative_mouse_selftest_stage == 5 &&
                       shown_frames >= 7) {
                relative_mouse_selftest_error |=
                    relative_mouse.captured() || mouse_pressed != 0;
                relative_mouse_selftest_stage = 6;
            }
        }
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                release_relative_mouse();
                running = false;
            }
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE) {
                release_relative_mouse();
                running = false;
            }
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_FOCUS_LOST &&
                event.window.windowID == presentation.window_ids[0]) {
                release_relative_mouse();
            }
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                    if (relative_mouse.captured())
                        release_relative_mouse();
                    else
                        running = false;
                } else if (const uint16_t bit = key_bit(event.key.keysym.scancode)) {
                    ++host_key_presses;
                    keyboard_pressed |= bit;
                    publish_keys();
                }
            }
            if (event.type == SDL_KEYUP && !event.key.repeat) {
                if (const uint16_t bit = key_bit(event.key.keysym.scancode)) {
                    keyboard_pressed &= static_cast<uint16_t>(~bit);
                    publish_keys();
                }
            }
            if (event.type == SDL_CONTROLLERDEVICEADDED && !controller) {
                controller = SDL_GameControllerOpen(event.cdevice.which);
                if (controller) {
                    controller_id = SDL_JoystickInstanceID(
                        SDL_GameControllerGetJoystick(controller));
                    std::fprintf(stderr,
                                 "[sdl] Player 1 controller: %s\n",
                                 SDL_GameControllerName(controller));
                }
            }
            if (event.type == SDL_CONTROLLERDEVICEREMOVED &&
                controller && event.cdevice.which == controller_id) {
                SDL_GameControllerClose(controller);
                controller = nullptr;
                controller_id = -1;
                controller_pressed = 0;
                publish_keys();
            }
            if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                if (const uint16_t bit = controller_bit(
                        static_cast<SDL_GameControllerButton>(
                            event.cbutton.button))) {
                    controller_pressed |= bit;
                    publish_keys();
                }
            }
            if (event.type == SDL_CONTROLLERBUTTONUP) {
                if (const uint16_t bit = controller_bit(
                        static_cast<SDL_GameControllerButton>(
                            event.cbutton.button))) {
                    controller_pressed &= static_cast<uint16_t>(~bit);
                    publish_keys();
                }
            }
            if (event.type == SDL_MOUSEBUTTONDOWN &&
                event.button.button == SDL_BUTTON_LEFT &&
                event.button.windowID == presentation.window_ids[0] &&
                options.relative_mouse_touch) {
                if (!relative_mouse.captured()) {
                    // The acquisition click only captures; the next click is
                    // the first guest fire press, avoiding an accidental shot.
                    capture_relative_mouse();
                } else if (options.relative_mouse_fire_mask != 0) {
                    mouse_pressed |= options.relative_mouse_fire_mask;
                    publish_keys();
                }
            }
            if (event.type == SDL_MOUSEBUTTONUP &&
                event.button.button == SDL_BUTTON_LEFT &&
                relative_mouse.captured() && mouse_pressed != 0) {
                mouse_pressed &= static_cast<uint16_t>(
                    ~options.relative_mouse_fire_mask);
                publish_keys();
            }
            if (event.type == SDL_MOUSEBUTTONDOWN &&
                event.button.button == SDL_BUTTON_LEFT &&
                event.button.windowID == presentation.window_ids[1] &&
                event.button.x >= bottom_content_left &&
                event.button.x < bottom_content_left + kScreenWidth &&
                (presentation.separate ||
                 event.button.y >= kScreenHeight)) {
                mouse_down = true;
                ++host_touch_presses;
                last_touch_event_x = event.button.x;
                last_touch_event_y = event.button.y;
                touch_release_pending = false;
                touch_frames_held = 0;
                set_touch_from_mouse(event.button.x, event.button.y, true,
                                     options.screen_layout,
                                     bottom_logical_width);
            }
            if (event.type == SDL_MOUSEBUTTONUP &&
                event.button.button == SDL_BUTTON_LEFT &&
                event.button.windowID == presentation.window_ids[1] &&
                mouse_down) {
                mouse_down = false;
                // A host click can begin and end while the emulation thread is
                // rendering one slow frame. Keep such a click asserted long
                // enough for the ARM7 touchscreen polling path to observe it.
                if (touch_frames_held < 2)
                    touch_release_pending = true;
                else
                    set_touch_from_mouse(event.button.x, event.button.y, false,
                                         options.screen_layout,
                                         bottom_logical_width);
            }
            if (event.type == SDL_MOUSEMOTION && mouse_down &&
                event.motion.windowID == presentation.window_ids[1])
                set_touch_from_mouse(event.motion.x, event.motion.y, true,
                                     options.screen_layout,
                                     bottom_logical_width);
            if (event.type == SDL_MOUSEMOTION &&
                relative_mouse.captured() &&
                (event.motion.windowID == presentation.window_ids[0] ||
                 event.motion.windowID == 0) &&
                relative_mouse.move(event.motion.xrel, event.motion.yrel)) {
                nds_set_touch(relative_mouse.x(), relative_mouse.y(), true);
            }
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_LEAVE && mouse_down &&
                event.window.windowID == presentation.window_ids[1]) {
                mouse_down = false;
                if (touch_frames_held < 2)
                    touch_release_pending = true;
                else
                    nds_set_touch(0, 0, false);
            }
        }

        const uint64_t phase0 = SDL_GetPerformanceCounter();
        const uint64_t now = scheduler_system_timestamp();
        const uint64_t next_frame =
            (now / kSystemCyclesPerFrame + 1u) * kSystemCyclesPerFrame;
        while (running && scheduler_system_timestamp() < next_frame &&
               !(scheduler_cpu_terminal_halted(0) &&
                 scheduler_cpu_terminal_halted(1))) {
            scheduler_run_round();
        }
        {
            const uint64_t emu_ticks = SDL_GetPerformanceCounter() - phase0;
            phase_emu_ticks += emu_ticks;
            if (emu_ticks > max_emu_ticks) {
                max_emu_ticks = emu_ticks;
                max_emu_frame = shown_frames;
            }
            if (emu_ticks * 1000u > frequency * 32u) ++slow_frames_32ms;
        }
#if defined(NDS_HAVE_COMPUTE_RENDERER)
        if (nds_gpu3d_compute_runtime_failed()) {
            compute_failed = true;
            running = false;
            break;
        }
#endif
        {
            const uint64_t seen =
                audio_queue.underruns.load(std::memory_order_relaxed);
            if (seen != last_underruns_seen) {
                if (!last_underruns_seen) first_underrun_frame = shown_frames;
                last_underrun_frame = shown_frames;
                last_underruns_seen = seen;
            }
        }

        if (mouse_down || touch_release_pending)
            ++touch_frames_held;
        if (touch_release_pending && touch_frames_held >= 2) {
            nds_set_touch(0, 0, false);
            touch_release_pending = false;
        }

        const uint64_t phase1 = SDL_GetPerformanceCounter();
        const uint32_t* const native_top = nds_gpu2d_framebuffer(0);
        const uint32_t* top_pixels = native_top;
        const uint32_t* bottom_pixels = nds_gpu2d_framebuffer(1);
        uint16_t top_width = 256;
        uint16_t bottom_width = 256;
        const uint64_t adaptive_start = SDL_GetPerformanceCounter();
        if (options.adaptive_screens & NDS_ADAPTIVE_TOP)
            top_pixels =
                nds_gpu2d_adaptive_framebuffer(0, &top_width);
        if (options.adaptive_screens & NDS_ADAPTIVE_BOTTOM)
            bottom_pixels =
                nds_gpu2d_adaptive_framebuffer(1, &bottom_width);
        phase_adaptive_ticks +=
            SDL_GetPerformanceCounter() - adaptive_start;
        observe_top_black_bands(native_top, shown_frames);
        const PresentationTicks presentation_ticks = present_screens(
            presentation, top_pixels, top_width,
            bottom_pixels, bottom_width);
        phase_upload_ticks += presentation_ticks.upload;
        phase_draw_ticks += presentation_ticks.draw;
        phase_swap_ticks += presentation_ticks.swap;
        phase_present_ticks += SDL_GetPerformanceCounter() - phase1;
        if (audio && audio_started) {
            const uint32_t queued = audio_queue_count(audio, audio_queue);
            audio_min_queue = std::min(audio_min_queue, queued);
        }
        const uint64_t phase2 = SDL_GetPerformanceCounter();
        // Glide: after the prebuffered start, the pacing allowance decays a
        // fixed step per frame from the prebuffer level down to the steady
        // 63 ms target, shedding the extra startup latency over a couple of
        // seconds without ever forcing the queue down during a slow stretch.
        constexpr uint32_t kGlideStepFrames = 300;
        if (audio_pace_floor > kAudioQueueFrames + kGlideStepFrames)
            audio_pace_floor -= kGlideStepFrames;
        else
            audio_pace_floor = kAudioQueueFrames;
        const uint32_t queued = drain_audio(
            audio, audio_queue, audio_started, audio_pace_floor,
            audio_queue_error);
        phase_drain_ticks += SDL_GetPerformanceCounter() - phase2;
        audio_max_queue = std::max(audio_max_queue, queued);
        if (audio && !audio_started && queued >= kAudioStartFrames) {
            // Opening paused and prebuffering avoids the guaranteed startup
            // underrun produced by unpausing an empty SDL queue.
            audio_queue.started.store(true, std::memory_order_relaxed);
            SDL_PauseAudioDevice(audio, 0);
            audio_started = true;
            audio_min_queue = queued;
            audio_pace_floor = kAudioStartFrames;
        }

        ++shown_frames;
        ++fps_frames;
        g_live_stats.frames = shown_frames;
        g_live_stats.emu_ticks = phase_emu_ticks;
        g_live_stats.present_ticks = phase_present_ticks;
        g_live_stats.adaptive_ticks = phase_adaptive_ticks;
        g_live_stats.upload_ticks = phase_upload_ticks;
        g_live_stats.draw_ticks = phase_draw_ticks;
        g_live_stats.swap_ticks = phase_swap_ticks;
        g_live_stats.drain_ticks = phase_drain_ticks;
        g_live_stats.underruns =
            audio_queue.underruns.load(std::memory_order_relaxed);
        const uint64_t counter = SDL_GetPerformanceCounter();
        if (counter - fps_start >= frequency) {
            const double seconds = static_cast<double>(counter - fps_start) /
                                   static_cast<double>(frequency);
            const double fps = static_cast<double>(fps_frames) / seconds;
            const std::string fps_text =
                std::to_string(fps).substr(0, 4) + " FPS";
            const std::string top_title = presentation.separate
                ? "ndsrecomp - Top Screen - " + fps_text
                : "ndsrecomp firmware preview - " + fps_text;
            SDL_SetWindowTitle(presentation.windows[0],
                               top_title.c_str());
            if (presentation.separate) {
                const std::string bottom_title =
                    "ndsrecomp - Bottom Screen - " + fps_text;
                SDL_SetWindowTitle(presentation.windows[1],
                                   bottom_title.c_str());
            }
            fps_frames = 0;
            fps_start = counter;
        }

        if (soak_frames && shown_frames >= soak_frames)
            running = false;
        if (selftest_menu && selftest_touch_up &&
            nds_event_counts().vblank9 >= 600)
            running = false;
        if (selftest_relative_mouse && relative_mouse_selftest_stage == 6)
            running = false;

        if (scheduler_cpu_terminal_halted(0) &&
            scheduler_cpu_terminal_halted(1)) {
            SDL_Delay(8);
        }
    }

    release_relative_mouse();
    nds_set_touch(0, 0, false);
    nds_set_key_mask(0x0FFFu);
    g_live_stats.active = 0;
    const double soak_seconds = static_cast<double>(
        SDL_GetPerformanceCounter() - soak_start) /
        static_cast<double>(frequency);
    const uint64_t top_hash = framebuffer_rgb_fnv(0);
    const uint64_t bottom_hash = framebuffer_rgb_fnv(1);
    if (audio) SDL_PauseAudioDevice(audio, 1);
    const uint64_t audio_underruns =
        audio_queue.underruns.load(std::memory_order_relaxed);
    if (audio) SDL_CloseAudioDevice(audio);
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    nds_compute_host_stop();
#endif
    if (controller) SDL_GameControllerClose(controller);
    destroy_presentation(presentation);
    SDL_Quit();
    std::fprintf(stderr, "[sdl] closed after %llu presented frames\n",
                 static_cast<unsigned long long>(shown_frames));
    if (print_stats || soak_frames || selftest_menu ||
        selftest_relative_mouse) {
        std::fprintf(stderr,
            "[sdl] soak: frames=%llu seconds=%.3f fps=%.3f "
            "audio_started=%u queue_errors=%u underruns=%llu "
            "min_queue_frames=%u max_queue_frames=%u "
            "key_presses=%llu touch_presses=%llu last_touch=(%d,%d) "
            "frame_fnv=(%016llx,%016llx)\n",
            static_cast<unsigned long long>(shown_frames), soak_seconds,
            soak_seconds > 0.0 ? shown_frames / soak_seconds : 0.0,
            audio_started ? 1u : 0u, audio_queue_error ? 1u : 0u,
            static_cast<unsigned long long>(audio_underruns),
            audio_min_queue == std::numeric_limits<uint32_t>::max()
                ? 0u : audio_min_queue,
            audio_max_queue,
            static_cast<unsigned long long>(host_key_presses),
            static_cast<unsigned long long>(host_touch_presses),
            last_touch_event_x, last_touch_event_y,
            static_cast<unsigned long long>(top_hash),
            static_cast<unsigned long long>(bottom_hash));
        const double tick_seconds = 1.0 / static_cast<double>(frequency);
        std::fprintf(stderr,
            "[sdl] phases: emu=%.3fs present=%.3fs drain=%.3fs other=%.3fs "
            "max_emu_ms=%.1f@f%llu slow32ms=%llu underrun_frames=[%llu,%llu]\n",
            phase_emu_ticks * tick_seconds,
            phase_present_ticks * tick_seconds,
            phase_drain_ticks * tick_seconds,
            soak_seconds - (phase_emu_ticks + phase_present_ticks +
                            phase_drain_ticks) * tick_seconds,
            max_emu_ticks * tick_seconds * 1000.0,
            static_cast<unsigned long long>(max_emu_frame),
            static_cast<unsigned long long>(slow_frames_32ms),
            static_cast<unsigned long long>(first_underrun_frame),
            static_cast<unsigned long long>(last_underrun_frame));
        std::fprintf(stderr,
            "[sdl] present detail: adaptive=%.3fs upload=%.3fs "
            "draw=%.3fs swap=%.3fs\n",
            phase_adaptive_ticks * tick_seconds,
            phase_upload_ticks * tick_seconds,
            phase_draw_ticks * tick_seconds,
            phase_swap_ticks * tick_seconds);
        nds_profile_report(stderr);
    }
    const bool audio_failed = audio_queue_error ||
        (require_audio && (audio_underruns != 0 || !audio || !audio_started));
    const bool menu_selftest_failed = selftest_menu &&
        (selftest_event_error || !selftest_key_up || !selftest_touch_up ||
         host_key_presses != 1 || host_touch_presses != 1 ||
         last_touch_event_x != bottom_content_left + 127 ||
         last_touch_event_y != (presentation.separate ? 180 : 372) ||
         top_hash != 0xa0f41b93e4eefa55ull ||
         bottom_hash != 0x6c43b370e9cda730ull);
    const bool relative_mouse_selftest_failed = selftest_relative_mouse &&
        (relative_mouse_selftest_error || relative_mouse_selftest_stage != 6);
    if (selftest_menu)
        std::fprintf(stderr, "[sdl] menu self-test: %s\n",
                     menu_selftest_failed ? "FAIL" : "PASS");
    if (selftest_relative_mouse)
        std::fprintf(stderr, "[sdl] relative mouse self-test: %s\n",
                     relative_mouse_selftest_failed ? "FAIL" : "PASS");
    return (audio_failed || menu_selftest_failed ||
            relative_mouse_selftest_failed || compute_failed) ? 1 : 0;
}

#else

int nds_run_interactive_frontend(const NdsFrontendOptions&) {
    std::fprintf(stderr,
        "[sdl] this runner was built without SDL2; install SDL2 and reconfigure\n");
    return 1;
}

#endif

void nds_frontend_live_stats(NdsFrontendLiveStats* out) {
    if (!out) return;
    *out = g_live_stats;
#if defined(NDS_HAVE_SDL2)
    if (g_live_stats.active)
        out->now_ticks = SDL_GetPerformanceCounter();
#endif
}

void nds_frontend_black_band_scan(bool enabled, bool reset) {
    if (reset) g_black_band = {};
    g_black_band.enabled = enabled ? 1 : 0;
}

void nds_frontend_black_band_capture(NdsFrontendBlackBandCapture* out) {
    if (out) *out = g_black_band;
}
