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

} // namespace

int nds_run_interactive_frontend() {
    SDL_SetMainReady();
    // SDL_INIT_TIMER matters on Windows: it raises the OS timer resolution
    // to 1 ms (SDL_HINT_TIMER_RESOLUTION default). Without it SDL_Delay(1)
    // sleeps a full ~15.6 ms scheduler quantum, so the audio-queue throttle
    // overshoots every frame, pinning the loop at ~57 FPS and cyclically
    // starving the audio queue (the audible boot crackle).
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS |
                 SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "[sdl] init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) != 0)
        std::fprintf(stderr, "[sdl] thread priority unchanged: %s\n",
                     SDL_GetError());

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
        window, -1, SDL_RENDERER_ACCELERATED);
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

#if defined(NDS_HAVE_COMPUTE_RENDERER)
    // Activate only after every fallible visible-frontend allocation. From
    // here onward teardown always destroys the compute renderer while this
    // context is current.
    if (!nds_compute_host_start()) {
        SDL_DestroyTexture(bottom);
        SDL_DestroyTexture(top);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
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
        "[sdl] controls: mouse=touch | arrows=D-pad | Z=A X=B | "
        "A=Y S=X | Q=L W=R | Enter=Start Backspace=Select | Esc=quit\n");

    uint16_t keys = 0x0FFFu;
    nds_set_key_mask(keys);
    nds_set_touch(0, 0, false);
    bool running = true;
    bool compute_failed = false;
    bool mouse_down = false;
    bool touch_release_pending = false;
    uint32_t touch_frames_held = 0;
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
    uint64_t phase_emu_ticks = 0;
    uint64_t phase_present_ticks = 0;
    uint64_t phase_drain_ticks = 0;
    g_live_stats = {};
    g_live_stats.active = 1;
    g_live_stats.freq = frequency;
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
                injected.button.button = SDL_BUTTON_LEFT;
                injected.button.x = 127;
                injected.button.y = 192 + 180;
                selftest_event_error |= SDL_PushEvent(&injected) < 0;
                selftest_touch_down = true;
            } else if (selftest_touch_down && !selftest_touch_up &&
                       counts.vblank9 >= 116) {
                injected = {};
                injected.type = SDL_MOUSEBUTTONUP;
                injected.button.button = SDL_BUTTON_LEFT;
                injected.button.x = 127;
                injected.button.y = 192 + 180;
                selftest_event_error |= SDL_PushEvent(&injected) < 0;
                selftest_touch_up = true;
            }
        }
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                    running = false;
                } else if (const uint16_t bit = key_bit(event.key.keysym.scancode)) {
                    ++host_key_presses;
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
                ++host_touch_presses;
                last_touch_event_x = event.button.x;
                last_touch_event_y = event.button.y;
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
        g_live_stats.drain_ticks = phase_drain_ticks;
        g_live_stats.underruns =
            audio_queue.underruns.load(std::memory_order_relaxed);
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

        if (soak_frames && shown_frames >= soak_frames)
            running = false;
        if (selftest_menu && selftest_touch_up &&
            nds_event_counts().vblank9 >= 600)
            running = false;

        if (scheduler_cpu_terminal_halted(0) &&
            scheduler_cpu_terminal_halted(1)) {
            SDL_Delay(8);
        }
    }

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
    SDL_DestroyTexture(bottom);
    SDL_DestroyTexture(top);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::fprintf(stderr, "[sdl] closed after %llu presented frames\n",
                 static_cast<unsigned long long>(shown_frames));
    if (print_stats || soak_frames || selftest_menu) {
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
        nds_profile_report(stderr);
    }
    const bool audio_failed = audio_queue_error ||
        (require_audio && (audio_underruns != 0 || !audio || !audio_started));
    const bool selftest_failed = selftest_menu &&
        (selftest_event_error || !selftest_key_up || !selftest_touch_up ||
         host_key_presses != 1 || host_touch_presses != 1 ||
         last_touch_event_x != 127 || last_touch_event_y != 372 ||
         top_hash != 0xa0f41b93e4eefa55ull ||
         bottom_hash != 0x6c43b370e9cda730ull);
    if (selftest_menu)
        std::fprintf(stderr, "[sdl] menu self-test: %s\n",
                     selftest_failed ? "FAIL" : "PASS");
    return (audio_failed || selftest_failed || compute_failed) ? 1 : 0;
}

#else

int nds_run_interactive_frontend() {
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
