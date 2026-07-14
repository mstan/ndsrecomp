#include "frontend.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

#include "gpu2d.h"
#include "io.h"
#include "scheduler.h"
#include "spu.h"

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

void set_touch_from_mouse(int window_x, int window_y, bool down) {
    // SDL_RenderSetLogicalSize also maps absolute mouse events into the
    // renderer's logical coordinate system. Calling RenderWindowToLogical a
    // second time halves coordinates at 2x scale (and turns bottom-screen
    // clicks into top-screen clicks), so consume the event coordinates as-is.
    const float x = static_cast<float>(window_x);
    const float y = static_cast<float>(window_y);
    if (!down || x < 0.0f || x >= kScreenWidth ||
        y < kScreenHeight || y >= kScreenHeight * 2) {
        nds_set_touch(0, 0, false);
        return;
    }
    const auto touch_x = static_cast<uint16_t>(std::clamp<int>(
        static_cast<int>(x), 0, kScreenWidth - 1));
    const auto touch_y = static_cast<uint16_t>(std::clamp<int>(
        static_cast<int>(y) - kScreenHeight, 0, kScreenHeight - 1));
    nds_set_touch(touch_x, touch_y, true);
}

void drain_audio(SDL_AudioDeviceID device) {
    if (!device) return;
    std::array<int16_t, 2048> samples{};
    for (;;) {
        const uint32_t frames = nds_spu_read_output(samples.data(), 1024);
        if (!frames) break;
        SDL_QueueAudio(device, samples.data(), frames * 2u * sizeof(int16_t));
    }
    // Audio is a second real-time clock alongside display VSync. Never drop a
    // queued block: if the host is faster than the DS cadence, let SDL consume
    // the small bounded backlog before emulating another frame.
    const uint32_t high_water =
        kAudioQueueFrames * 2u * sizeof(int16_t);
    while (SDL_GetQueuedAudioSize(device) > high_water)
        SDL_Delay(1);
}

} // namespace

int nds_run_interactive_frontend() {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "[sdl] init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_Window* window = SDL_CreateWindow(
        "ndsrecomp firmware preview",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        kScreenWidth * kWindowScale, kScreenHeight * 2 * kWindowScale,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        std::fprintf(stderr, "[sdl] window failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        std::fprintf(stderr, "[sdl] renderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_RenderSetLogicalSize(renderer, kScreenWidth, kScreenHeight * 2);
    SDL_RenderSetIntegerScale(renderer, SDL_TRUE);

    SDL_Texture* top = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, kScreenWidth, kScreenHeight);
    SDL_Texture* bottom = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, kScreenWidth, kScreenHeight);
    if (!top || !bottom) {
        std::fprintf(stderr, "[sdl] texture failed: %s\n", SDL_GetError());
        if (top) SDL_DestroyTexture(top);
        if (bottom) SDL_DestroyTexture(bottom);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_AudioSpec want{};
    // The mixer runs once per 1024 DS system cycles. Request its integer host
    // rate directly; the sub-sample remainder is absorbed by the bounded queue
    // instead of producing a roughly once-per-second underrun at 32768 Hz.
    want.freq = kAudioFrequency;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    const SDL_AudioDeviceID audio = SDL_OpenAudioDevice(
        nullptr, 0, &want, nullptr, 0);
    if (audio) SDL_PauseAudioDevice(audio, 0);
    else std::fprintf(stderr, "[sdl] audio unavailable: %s\n", SDL_GetError());

    std::fprintf(stderr,
        "[sdl] controls: mouse=touch | arrows=D-pad | Z=A X=B | "
        "A=Y S=X | Q=L W=R | Enter=Start Backspace=Select | Esc=quit\n");

    uint16_t keys = 0x0FFFu;
    nds_set_key_mask(keys);
    nds_set_touch(0, 0, false);
    bool running = true;
    bool mouse_down = false;
    bool touch_release_pending = false;
    uint32_t touch_frames_held = 0;
    uint64_t shown_frames = 0;
    uint64_t fps_frames = 0;
    uint64_t fps_start = SDL_GetPerformanceCounter();
    const uint64_t frequency = SDL_GetPerformanceFrequency();

    while (running) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                    running = false;
                } else if (const uint16_t bit = key_bit(event.key.keysym.scancode)) {
                    keys &= static_cast<uint16_t>(~bit);
                    nds_set_key_mask(keys);
                }
            }
            if (event.type == SDL_KEYUP && !event.key.repeat) {
                if (const uint16_t bit = key_bit(event.key.keysym.scancode)) {
                    keys |= bit;
                    nds_set_key_mask(keys);
                }
            }
            if (event.type == SDL_MOUSEBUTTONDOWN &&
                event.button.button == SDL_BUTTON_LEFT) {
                mouse_down = true;
                touch_release_pending = false;
                touch_frames_held = 0;
                set_touch_from_mouse(event.button.x, event.button.y, true);
            }
            if (event.type == SDL_MOUSEBUTTONUP &&
                event.button.button == SDL_BUTTON_LEFT) {
                mouse_down = false;
                // A host click can begin and end while the emulation thread is
                // rendering one slow frame. Keep such a click asserted long
                // enough for the ARM7 touchscreen polling path to observe it.
                if (touch_frames_held < 2)
                    touch_release_pending = true;
                else
                    set_touch_from_mouse(event.button.x, event.button.y, false);
            }
            if (event.type == SDL_MOUSEMOTION && mouse_down)
                set_touch_from_mouse(event.motion.x, event.motion.y, true);
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_LEAVE && mouse_down) {
                mouse_down = false;
                if (touch_frames_held < 2)
                    touch_release_pending = true;
                else
                    nds_set_touch(0, 0, false);
            }
        }

        const uint64_t now = scheduler_system_timestamp();
        const uint64_t next_frame =
            (now / kSystemCyclesPerFrame + 1u) * kSystemCyclesPerFrame;
        while (running && scheduler_system_timestamp() < next_frame &&
               !(scheduler_cpu_terminal_halted(0) &&
                 scheduler_cpu_terminal_halted(1))) {
            scheduler_run_round();
        }

        if (mouse_down || touch_release_pending)
            ++touch_frames_held;
        if (touch_release_pending && touch_frames_held >= 2) {
            nds_set_touch(0, 0, false);
            touch_release_pending = false;
        }

        SDL_UpdateTexture(top, nullptr, nds_gpu2d_framebuffer(0),
                          kScreenWidth * sizeof(uint32_t));
        SDL_UpdateTexture(bottom, nullptr, nds_gpu2d_framebuffer(1),
                          kScreenWidth * sizeof(uint32_t));
        SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
        SDL_RenderClear(renderer);
        const SDL_Rect top_rect{0, 0, kScreenWidth, kScreenHeight};
        const SDL_Rect bottom_rect{0, kScreenHeight, kScreenWidth, kScreenHeight};
        SDL_RenderCopy(renderer, top, nullptr, &top_rect);
        SDL_RenderCopy(renderer, bottom, nullptr, &bottom_rect);
        SDL_RenderPresent(renderer);
        drain_audio(audio);

        ++shown_frames;
        ++fps_frames;
        const uint64_t counter = SDL_GetPerformanceCounter();
        if (counter - fps_start >= frequency) {
            const double seconds = static_cast<double>(counter - fps_start) /
                                   static_cast<double>(frequency);
            const double fps = static_cast<double>(fps_frames) / seconds;
            const std::string title = "ndsrecomp firmware preview - " +
                std::to_string(fps).substr(0, 4) + " FPS";
            SDL_SetWindowTitle(window, title.c_str());
            fps_frames = 0;
            fps_start = counter;
        }

        if (scheduler_cpu_terminal_halted(0) &&
            scheduler_cpu_terminal_halted(1)) {
            SDL_Delay(8);
        }
    }

    nds_set_touch(0, 0, false);
    nds_set_key_mask(0x0FFFu);
    if (audio) SDL_CloseAudioDevice(audio);
    SDL_DestroyTexture(bottom);
    SDL_DestroyTexture(top);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::fprintf(stderr, "[sdl] closed after %llu presented frames\n",
                 static_cast<unsigned long long>(shown_frames));
    return 0;
}

#else

int nds_run_interactive_frontend() {
    std::fprintf(stderr,
        "[sdl] this runner was built without SDL2; install SDL2 and reconfigure\n");
    return 1;
}

#endif
