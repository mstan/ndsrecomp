#include "frontend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

#include "debug_server.h"
#include "diagnostics.h"
#include "gpu2d.h"
#include "gpu3d.h"
#include "melonds_compute/TextureUpscale.h"
#include "io.h"
#include "profile_report.h"
#include "relative_mouse_touch.h"
#include "runtime_arm.h"
#include "scheduler.h"
#include "spu.h"
#include "title_patches.h"
#if defined(NDS_HAVE_COMPUTE_RENDERER)
#include "melonds_compute/ComputeHost.h"
#endif

namespace {
// Written per presented frame by the frontend loop, read from the same
// thread by the `frontend_stats` debug command at the debug_pump() safe
// point. Stays all-zero (active=0) when no frontend is running.
NdsFrontendLiveStats g_live_stats{};
NdsFrontendBlackBandCapture g_black_band{};
NdsFrontendInputDebugState g_input_debug{};

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
constexpr uint32_t kMphUs10MorphState = 0x020DA818u;

enum class MphPrimeInputKind : uint8_t {
    None,
    Key,
    Mouse,
};

struct MphPrimeBinding {
    MphPrimeInputKind kind = MphPrimeInputKind::None;
    SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
    uint8_t mouse = 0;
};

enum class MphPrimeAction : uint8_t {
    MoveForward,
    MoveBack,
    MoveLeft,
    MoveRight,
    Jump,
    MorphBall,
    BoostZoom,
    ScanVisor,
    UiLeft,
    UiRight,
    UiOk,
    Shoot,
    ScanShoot,
    Beam,
    Missile,
    Weapon1,
    Weapon2,
    Weapon3,
    Weapon4,
    Weapon5,
    Weapon6,
    VirtualStylus,
    Menu,
    Count,
};

struct MphPrimeBindingSet {
    std::array<MphPrimeBinding,
               static_cast<size_t>(MphPrimeAction::Count)> bindings{};
    bool valid = true;
};

struct MphTouchStep {
    uint16_t x = 0;
    uint16_t y = 0;
    bool down = false;
    uint8_t frames = 0;
};

struct MphTouchSequence {
    std::array<MphTouchStep, 6> steps{};
    uint8_t count = 0;
    uint8_t index = 0;
    uint8_t remaining = 0;

    bool active() const { return index < count; }

    void start(std::initializer_list<MphTouchStep> source) {
        count = static_cast<uint8_t>(
            std::min(source.size(), steps.size()));
        std::copy_n(source.begin(), count, steps.begin());
        index = 0;
        remaining = count ? steps[0].frames : 0;
    }

    void tick() {
        if (!active()) return;
        const MphTouchStep& step = steps[index];
        nds_set_touch(step.x, step.y, step.down);
        if (remaining > 0) --remaining;
        if (remaining == 0) {
            ++index;
            if (active()) remaining = steps[index].frames;
        }
    }
};

std::string binding_name_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       if (c == '_' || c == '-') return ' ';
                       return static_cast<char>(std::tolower(c));
                   });
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    while (!value.empty() && value.back() == ' ') value.pop_back();
    return value;
}

SDL_Scancode scancode_from_binding_name(const std::string& value) {
    const std::string normalized = binding_name_lower(value);
    SDL_Scancode key = SDL_GetScancodeFromName(value.c_str());
    if (key != SDL_SCANCODE_UNKNOWN)
        return key;
    if (normalized.size() == 1) {
        const char c = normalized[0];
        if (c >= 'a' && c <= 'z') {
            return static_cast<SDL_Scancode>(
                SDL_SCANCODE_A + (c - 'a'));
        }
        if (c >= '1' && c <= '9') {
            return static_cast<SDL_Scancode>(
                SDL_SCANCODE_1 + (c - '1'));
        }
        if (c == '0')
            return SDL_SCANCODE_0;
    }
    if (normalized == "ctrl" || normalized == "control" ||
        normalized == "left ctrl" || normalized == "left control") {
        return SDL_SCANCODE_LCTRL;
    }
    if (normalized == "right ctrl" || normalized == "right control")
        return SDL_SCANCODE_RCTRL;
    if (normalized == "shift" || normalized == "left shift")
        return SDL_SCANCODE_LSHIFT;
    if (normalized == "right shift")
        return SDL_SCANCODE_RSHIFT;
    if (normalized == "space")
        return SDL_SCANCODE_SPACE;
    if (normalized == "tab")
        return SDL_SCANCODE_TAB;
    if (normalized == "enter" || normalized == "return")
        return SDL_SCANCODE_RETURN;
    if (normalized == "backspace")
        return SDL_SCANCODE_BACKSPACE;
    return SDL_SCANCODE_UNKNOWN;
}

MphPrimeBinding parse_mph_prime_binding(const std::string& value) {
    const std::string normalized = binding_name_lower(value);
    if (normalized.empty() || normalized == "none" ||
        normalized == "unbound") {
        return {};
    }
    if (normalized == "mouse left" || normalized == "left mouse")
        return {MphPrimeInputKind::Mouse, SDL_SCANCODE_UNKNOWN,
                SDL_BUTTON_LEFT};
    if (normalized == "mouse right" || normalized == "right mouse")
        return {MphPrimeInputKind::Mouse, SDL_SCANCODE_UNKNOWN,
                SDL_BUTTON_RIGHT};
    if (normalized == "mouse middle" || normalized == "middle mouse")
        return {MphPrimeInputKind::Mouse, SDL_SCANCODE_UNKNOWN,
                SDL_BUTTON_MIDDLE};
    if (normalized == "mouse 4")
        return {MphPrimeInputKind::Mouse, SDL_SCANCODE_UNKNOWN,
                SDL_BUTTON_X1};
    if (normalized == "mouse 5")
        return {MphPrimeInputKind::Mouse, SDL_SCANCODE_UNKNOWN,
                SDL_BUTTON_X2};

    SDL_Scancode key = scancode_from_binding_name(value);
    return {key == SDL_SCANCODE_UNKNOWN ? MphPrimeInputKind::None
                                        : MphPrimeInputKind::Key,
            key, 0};
}

// ── Gamepad bindings for Prime Controls actions ─────────────────────────
enum class MphPadInputKind : uint8_t {
    None,
    Button,
    TriggerLeft,
    TriggerRight,
};

struct MphPadBinding {
    MphPadInputKind kind = MphPadInputKind::None;
    SDL_GameControllerButton button = SDL_CONTROLLER_BUTTON_INVALID;
};

struct MphPadBindingSet {
    std::array<MphPadBinding,
               static_cast<size_t>(MphPrimeAction::Count)> bindings{};
    bool valid = true;
};

MphPadBinding parse_mph_pad_binding(const std::string& value,
                                    bool* recognized) {
    if (recognized) *recognized = true;
    const std::string normalized = binding_name_lower(value);
    if (normalized.empty() || normalized == "none" ||
        normalized == "unbound") {
        return {};
    }
    auto button = [](SDL_GameControllerButton b) {
        return MphPadBinding{MphPadInputKind::Button, b};
    };
    if (normalized == "pad a") return button(SDL_CONTROLLER_BUTTON_A);
    if (normalized == "pad b") return button(SDL_CONTROLLER_BUTTON_B);
    if (normalized == "pad x") return button(SDL_CONTROLLER_BUTTON_X);
    if (normalized == "pad y") return button(SDL_CONTROLLER_BUTTON_Y);
    if (normalized == "pad lb")
        return button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    if (normalized == "pad rb")
        return button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    if (normalized == "pad l3")
        return button(SDL_CONTROLLER_BUTTON_LEFTSTICK);
    if (normalized == "pad r3")
        return button(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    if (normalized == "pad up") return button(SDL_CONTROLLER_BUTTON_DPAD_UP);
    if (normalized == "pad down")
        return button(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    if (normalized == "pad left")
        return button(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    if (normalized == "pad right")
        return button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    if (normalized == "pad start")
        return button(SDL_CONTROLLER_BUTTON_START);
    if (normalized == "pad back" || normalized == "pad select")
        return button(SDL_CONTROLLER_BUTTON_BACK);
    if (normalized == "pad lt")
        return {MphPadInputKind::TriggerLeft, SDL_CONTROLLER_BUTTON_INVALID};
    if (normalized == "pad rt")
        return {MphPadInputKind::TriggerRight, SDL_CONTROLLER_BUTTON_INVALID};
    if (recognized) *recognized = false;
    return {};
}

MphPadBindingSet make_mph_pad_bindings(
    const NdsMphPrimeControlBindings& source) {
    MphPadBindingSet set{};
    auto put = [&](MphPrimeAction action, const std::string& value) {
        bool recognized = true;
        set.bindings[static_cast<size_t>(action)] =
            parse_mph_pad_binding(value, &recognized);
        if (!recognized) {
            std::fprintf(stderr,
                         "[sdl] invalid MPH Prime Controls pad binding: %s\n",
                         value.c_str());
            set.valid = false;
        }
    };
    put(MphPrimeAction::MoveForward, source.move_forward);
    put(MphPrimeAction::MoveBack, source.move_back);
    put(MphPrimeAction::MoveLeft, source.move_left);
    put(MphPrimeAction::MoveRight, source.move_right);
    put(MphPrimeAction::Jump, source.jump);
    put(MphPrimeAction::MorphBall, source.morph_ball);
    put(MphPrimeAction::BoostZoom, source.boost_zoom);
    put(MphPrimeAction::ScanVisor, source.scan_visor);
    put(MphPrimeAction::UiLeft, source.ui_left);
    put(MphPrimeAction::UiRight, source.ui_right);
    put(MphPrimeAction::UiOk, source.ui_ok);
    put(MphPrimeAction::Shoot, source.shoot);
    put(MphPrimeAction::ScanShoot, source.scan_shoot);
    put(MphPrimeAction::Beam, source.beam);
    put(MphPrimeAction::Missile, source.missile);
    put(MphPrimeAction::Weapon1, source.weapon1);
    put(MphPrimeAction::Weapon2, source.weapon2);
    put(MphPrimeAction::Weapon3, source.weapon3);
    put(MphPrimeAction::Weapon4, source.weapon4);
    put(MphPrimeAction::Weapon5, source.weapon5);
    put(MphPrimeAction::Weapon6, source.weapon6);
    put(MphPrimeAction::VirtualStylus, source.virtual_stylus);
    put(MphPrimeAction::Menu, source.menu);
    return set;
}

bool binding_matches_key(const MphPrimeBinding& binding, SDL_Scancode key) {
    return binding.kind == MphPrimeInputKind::Key && binding.key == key;
}

bool binding_matches_mouse(const MphPrimeBinding& binding, uint8_t button) {
    return binding.kind == MphPrimeInputKind::Mouse &&
           binding.mouse == button;
}

uint16_t mph_prime_hold_mask(MphPrimeAction action) {
    switch (action) {
        case MphPrimeAction::MoveForward: return 1u << 6;  // Up
        case MphPrimeAction::MoveBack:    return 1u << 7;  // Down
        case MphPrimeAction::MoveLeft:    return 1u << 5;  // Left
        case MphPrimeAction::MoveRight:   return 1u << 4;  // Right
        case MphPrimeAction::Jump:        return 1u << 1;  // B
        case MphPrimeAction::BoostZoom:   return 1u << 8;  // R
        case MphPrimeAction::Shoot:
        case MphPrimeAction::ScanShoot:   return 1u << 9;  // L
        case MphPrimeAction::Menu:        return 1u << 3;  // Start
        default:                          return 0;
    }
}

void start_mph_touch_action(MphPrimeAction action,
                            MphTouchSequence& sequence) {
    const MphTouchStep up{0, 0, false, 2};
    auto tap = [&](uint16_t x, uint16_t y, uint8_t touch_frames = 2,
                   uint8_t release_frames = 2) {
        sequence.start({up, {x, y, true, touch_frames},
                        {0, 0, false, release_frames}});
    };
    switch (action) {
        case MphPrimeAction::MorphBall:
            // melonPrimeDS releases late here; boost ball is unreliable if
            // the stylus is restored to aim-center immediately after morph.
            tap(231, 167, 2, 8);
            break;
        case MphPrimeAction::ScanVisor:
            // Hold the visor touch long enough for the transition path that
            // melonPrimeDS handled with a 30-frame loop.
            tap(128, 173, 30, 2);
            break;
        case MphPrimeAction::UiOk:
            tap(128, 142);
            break;
        case MphPrimeAction::UiLeft:
            tap(71, 141);
            break;
        case MphPrimeAction::UiRight:
            tap(185, 141);
            break;
        case MphPrimeAction::Beam:
            tap(85, 32);
            break;
        case MphPrimeAction::Missile:
            tap(125, 32);
            break;
        case MphPrimeAction::Weapon1:
        case MphPrimeAction::Weapon2:
        case MphPrimeAction::Weapon3:
        case MphPrimeAction::Weapon4:
        case MphPrimeAction::Weapon5:
        case MphPrimeAction::Weapon6: {
            const unsigned index =
                static_cast<unsigned>(action) -
                static_cast<unsigned>(MphPrimeAction::Weapon1);
            const uint16_t x = static_cast<uint16_t>(93 + 25 * index);
            const uint16_t y = static_cast<uint16_t>(48 + 25 * index);
            sequence.start({up, {232, 34, true, 2}, {x, y, true, 2},
                            {0, 0, false, 2}});
            break;
        }
        default:
            break;
    }
}

MphPrimeBindingSet make_mph_prime_bindings(
    const NdsMphPrimeControlBindings& source) {
    MphPrimeBindingSet set{};
    auto put = [&](MphPrimeAction action, const std::string& value) {
        MphPrimeBinding binding = parse_mph_prime_binding(value);
        if (binding.kind == MphPrimeInputKind::None &&
            binding_name_lower(value) != "none" &&
            binding_name_lower(value) != "unbound") {
            std::fprintf(stderr,
                         "[sdl] invalid MPH Prime Controls binding: %s\n",
                         value.c_str());
            set.valid = false;
        }
        set.bindings[static_cast<size_t>(action)] = binding;
    };
    put(MphPrimeAction::MoveForward, source.move_forward);
    put(MphPrimeAction::MoveBack, source.move_back);
    put(MphPrimeAction::MoveLeft, source.move_left);
    put(MphPrimeAction::MoveRight, source.move_right);
    put(MphPrimeAction::Jump, source.jump);
    put(MphPrimeAction::MorphBall, source.morph_ball);
    put(MphPrimeAction::BoostZoom, source.boost_zoom);
    put(MphPrimeAction::ScanVisor, source.scan_visor);
    put(MphPrimeAction::UiLeft, source.ui_left);
    put(MphPrimeAction::UiRight, source.ui_right);
    put(MphPrimeAction::UiOk, source.ui_ok);
    put(MphPrimeAction::Shoot, source.shoot);
    put(MphPrimeAction::ScanShoot, source.scan_shoot);
    put(MphPrimeAction::Beam, source.beam);
    put(MphPrimeAction::Missile, source.missile);
    put(MphPrimeAction::Weapon1, source.weapon1);
    put(MphPrimeAction::Weapon2, source.weapon2);
    put(MphPrimeAction::Weapon3, source.weapon3);
    put(MphPrimeAction::Weapon4, source.weapon4);
    put(MphPrimeAction::Weapon5, source.weapon5);
    put(MphPrimeAction::Weapon6, source.weapon6);
    put(MphPrimeAction::VirtualStylus, source.virtual_stylus);
    put(MphPrimeAction::Menu, source.menu);
    return set;
}
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

void clear_audio_queue(SDL_AudioDeviceID device, AudioQueue& queue) {
    if (!device) return;
    SDL_LockAudioDevice(device);
    queue.read = 0;
    queue.write = 0;
    queue.count = 0;
    SDL_UnlockAudioDevice(device);
}

void discard_spu_output() {
    std::array<int16_t, 2048> samples{};
    while (nds_spu_read_output(samples.data(), 1024) != 0) {}
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
    bool gl_top = false;
    SDL_Window* windows[2]{};
    SDL_Renderer* renderers[2]{};
    SDL_Texture* textures[2]{};
    SDL_Texture* sample_targets[2]{};
    uint32_t window_ids[2]{};
    int screen_widths[2]{kScreenWidth, kScreenWidth};
    int canvas_width = kScreenWidth;
    int sample_scale = 1;
};

// docs/frame_interpolation.md, MVP blend mode. Holds the previous DS frame's
// post-compositor ARGB pixels per screen plus the scratch the blend is built
// into. These are copies taken after composition, so nothing the guest can
// observe is involved and no scheduler work is attached to them.
struct FrameBlendCache {
    std::vector<uint32_t> previous[2];
    std::vector<uint32_t> blended[2];
    int widths[2]{0, 0};
    bool valid = false;
};

// 50/50 per-channel average, the alpha 0.5 midpoint the 120 Hz target wants.
// The low bit of each channel is dropped rather than rounded (invisible at 8
// bits, one pass of cheap word arithmetic over ~100k pixels), and the alpha
// byte is carried straight from the current frame so a blended upload is
// byte-compatible with the real uploads either side of it.
void blend_half(const uint32_t* previous, const uint32_t* current,
                uint32_t* out, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        out[i] = (current[i] & 0xFF000000u) |
                 (((previous[i] >> 1) & 0x007F7F7Fu) +
                  ((current[i] >> 1) & 0x007F7F7Fu));
    }
}

void cache_presented_frame(FrameBlendCache& cache, int screen,
                           const uint32_t* pixels, int width) {
    const size_t count = static_cast<size_t>(width) * kScreenHeight;
    if (cache.widths[screen] != width) {
        cache.widths[screen] = width;
        cache.previous[screen].assign(count, 0u);
        cache.blended[screen].assign(count, 0u);
    }
    std::memcpy(cache.previous[screen].data(), pixels,
                count * sizeof(uint32_t));
}

uint32_t fullscreen_flags(NdsFullscreenMode mode) {
    switch (mode) {
        case NdsFullscreenMode::Borderless:
            return SDL_WINDOW_FULLSCREEN_DESKTOP;
        case NdsFullscreenMode::Exclusive:
            return SDL_WINDOW_FULLSCREEN;
        default:
            return 0;
    }
}

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
                         FrontendPresentation& presentation,
                         bool allow_gl_top = true) {
    presentation.separate =
        options.screen_layout == NdsScreenLayout::Separate;
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    const char* direct_selection =
        std::getenv("NDS_COMPUTE_DIRECT_PRESENT");
    const bool direct_enabled =
        !direct_selection || !*direct_selection ||
        std::strcmp(direct_selection, "1") == 0;
    const bool direct_top_requested =
        (options.adaptive_screens & NDS_ADAPTIVE_TOP) != 0u ||
        options.internal_resolution > 1u;
    presentation.gl_top = presentation.separate &&
        allow_gl_top &&
        direct_top_requested &&
        nds_gpu3d_renderer_prefers_compute() &&
        direct_enabled;
#endif
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
    const uint32_t top_window_flags =
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
        (presentation.gl_top
             ? static_cast<uint32_t>(SDL_WINDOW_OPENGL) : 0u);
    presentation.windows[0] = SDL_CreateWindow(
        presentation.separate ? "ndsrecomp - Top Screen"
                              : "ndsrecomp firmware preview",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        (presentation.separate ? presentation.screen_widths[0]
                               : presentation.canvas_width) * kWindowScale,
        first_height,
        top_window_flags);
    if (!presentation.windows[0]) {
        std::fprintf(stderr, "[sdl] window failed: %s\n", SDL_GetError());
        return false;
    }
    if (!presentation.gl_top) {
        presentation.renderers[0] = create_renderer(presentation.windows[0]);
        if (!presentation.renderers[0]) {
            std::fprintf(stderr, "[sdl] renderer failed: %s\n", SDL_GetError());
            destroy_presentation(presentation);
            return false;
        }
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

    // Fullscreen is applied only after both separate-layout windows have their
    // final placement. The primary combined/top window is deliberately the
    // sole target so the separate touch window remains usable.
    if (SDL_SetWindowFullscreen(presentation.windows[0],
                                fullscreen_flags(options.fullscreen)) != 0) {
        std::fprintf(stderr, "[sdl] fullscreen (%s) failed: %s\n",
                     nds_fullscreen_mode_name(options.fullscreen),
                     SDL_GetError());
        destroy_presentation(presentation);
        return false;
    }

    for (int screen = 0; screen < 2; ++screen) {
        if (screen == 0 && presentation.gl_top) {
            presentation.window_ids[screen] =
                SDL_GetWindowID(presentation.windows[screen]);
            continue;
        }
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
        if (SDL_SetTextureScaleMode(presentation.textures[screen],
                                    SDL_ScaleModeNearest) != 0) {
            std::fprintf(stderr, "[sdl] texture scale mode failed: %s\n",
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
            if (SDL_SetTextureScaleMode(presentation.sample_targets[screen],
                                        SDL_ScaleModeNearest) != 0) {
                std::fprintf(
                    stderr,
                    "[sdl] supersample target scale mode failed: %s\n",
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
    bool ok = true;
};

void draw_virtual_stylus(SDL_Renderer* renderer, const SDL_Rect& destination,
                         float stylus_x, float stylus_y) {
    const int x = destination.x + static_cast<int>(
        std::lround(stylus_x * destination.w / 256.0f));
    const int y = destination.y + static_cast<int>(
        std::lround(stylus_y * destination.h / 192.0f));

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawLine(renderer, x - 5, y, x - 2, y);
    SDL_RenderDrawLine(renderer, x + 2, y, x + 5, y);
    SDL_RenderDrawLine(renderer, x, y - 5, x, y - 2);
    SDL_RenderDrawLine(renderer, x, y + 2, x, y + 5);
}

PresentationTicks present_screens(FrontendPresentation& presentation,
                                  const uint32_t* top_pixels,
                                  int top_width,
                                  const uint32_t* bottom_pixels,
                                  int bottom_width,
                                  bool virtual_stylus_visible,
                                  float virtual_stylus_x,
                                  float virtual_stylus_y) {
    PresentationTicks ticks{};
    if (presentation.gl_top) {
#if defined(NDS_HAVE_COMPUTE_RENDERER)
        NdsGpu2dDirectFrame direct_frame{};
        const bool direct = nds_gpu2d_direct_frame(&direct_frame);
        NdsComputePresentTicks gl_ticks{};
        if (!nds_compute_host_present_top(
                top_pixels, static_cast<uint16_t>(top_width),
                direct ? &direct_frame : nullptr, &gl_ticks)) {
            std::fprintf(stderr, "[gpu3d] direct top presentation failed\n");
            ticks.ok = false;
            return ticks;
        }
        ticks.upload += gl_ticks.upload;
        ticks.draw += gl_ticks.draw;
        ticks.swap += gl_ticks.swap;
        uint64_t start = SDL_GetPerformanceCounter();
        SDL_UpdateTexture(presentation.textures[1], nullptr, bottom_pixels,
                          bottom_width * sizeof(uint32_t));
        ticks.upload += SDL_GetPerformanceCounter() - start;
        const SDL_Rect screen_rect{
            0, 0, presentation.screen_widths[1], kScreenHeight};
        SDL_Renderer* renderer = presentation.renderers[1];
        start = SDL_GetPerformanceCounter();
        SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
        SDL_RenderClear(renderer);
        render_screen(presentation, 1, screen_rect);
        if (virtual_stylus_visible)
            draw_virtual_stylus(renderer, screen_rect,
                                virtual_stylus_x, virtual_stylus_y);
        ticks.draw += SDL_GetPerformanceCounter() - start;
        start = SDL_GetPerformanceCounter();
        SDL_RenderPresent(renderer);
        ticks.swap += SDL_GetPerformanceCounter() - start;
#endif
        return ticks;
    }
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
        if (virtual_stylus_visible)
            draw_virtual_stylus(renderer, bottom_rect,
                                virtual_stylus_x, virtual_stylus_y);
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
        if (screen == 1 && virtual_stylus_visible)
            draw_virtual_stylus(renderer, screen_rect,
                                virtual_stylus_x, virtual_stylus_y);
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

    if (!SDL_SetHintWithPriority(SDL_HINT_RENDER_SCALE_QUALITY, "0",
                                 SDL_HINT_OVERRIDE)) {
        std::fprintf(stderr,
                     "[sdl] render scale quality hint was not applied\n");
    }
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
    const int top_content_left = presentation.separate
        ? 0 : (presentation.canvas_width - presentation.screen_widths[0]) / 2;
    const NdsLogicalRect stacked_top_screen{
        top_content_left, 0, presentation.screen_widths[0], kScreenHeight};
    const NdsLogicalRect stacked_bottom_touch{
        bottom_content_left, kScreenHeight, kScreenWidth, kScreenHeight};
    std::fprintf(stderr,
        "[sdl] layout=%s fullscreen=%s adaptive=%s supersampling=%ux aa=%ux "
        "internal=%ux\n",
        nds_screen_layout_name(options.screen_layout),
        nds_fullscreen_mode_name(options.fullscreen),
        nds_adaptive_screens_name(options.adaptive_screens),
        static_cast<unsigned>(options.supersampling),
        static_cast<unsigned>(options.antialiasing),
        static_cast<unsigned>(options.internal_resolution));
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
    // Must precede nds_compute_host_start(): the scale is a baked shader
    // constant, so it has to be known before the renderer compiles anything.
    //
    // Deliberately NOT gated on the direct presenter. Rendering at scale and
    // presenting at scale are separate: the scaled raster still point-samples
    // the same native surface every faithful consumer reads, so a run without
    // the GPU presenter is a valid way to prove that invariance holds. Only
    // the visible benefit needs gl_top, so that is what the notice says.
    if (!nds_gpu3d_set_internal_scale(options.internal_resolution)) {
        std::fprintf(stderr,
                     "[sdl] internal resolution %ux is unavailable\n",
                     static_cast<unsigned>(options.internal_resolution));
        destroy_presentation(presentation);
        SDL_Quit();
        return 1;
    }
    if (options.internal_resolution > 1 && !presentation.gl_top) {
        std::fprintf(stderr,
            "[sdl] internal resolution %ux renders but is not presented: "
            "the extra sample density needs the direct OpenGL top-screen "
            "presenter\n",
            static_cast<unsigned>(options.internal_resolution));
    }
    // The adaptive compositor only pays for the extra per-pixel stores when
    // something can consume them.
    nds_gpu2d_set_hd_emit(options.internal_resolution > 1 &&
                          presentation.gl_top);
    nds_texture_upscale_set_factor(options.texture_upscale);

#if defined(NDS_HAVE_COMPUTE_RENDERER)
    // Activate only after every fallible visible-frontend allocation. From
    // here onward teardown always destroys the compute renderer while this
    // context is current.
    if (!nds_compute_host_start(
            presentation.gl_top ? presentation.windows[0] : nullptr)) {
        destroy_presentation(presentation);
        SDL_Quit();
        return 1;
    }
    // Auto is allowed to recover from missing/failed GL 4.3. A direct-top
    // presentation was created without an SDL renderer, so rebuild the same
    // windows on the faithful SDL/software path after that fallback.
    if (presentation.gl_top && !nds_compute_host_active()) {
        destroy_presentation(presentation);
        presentation = {};
        if (!create_presentation(options, presentation, false)) {
            SDL_Quit();
            return 1;
        }
    }
    nds_gpu2d_set_direct_present(
        nds_compute_host_has_visible_context());
#else
    if (nds_gpu3d_renderer_policy() == NdsGpu3dRendererPolicy::Auto) {
        std::fprintf(stderr,
            "[gpu3d] OpenGL renderer not built; "
            "automatic fallback to threaded soft\n");
    }
#endif

    // "Frame interpolation (experimental)" — docs/frame_interpolation.md.
    // Resolved here, after the compute-renderer fallback above, because that
    // can rebuild the presentation on the SDL path and clear gl_top.
    //
    // Diagnostic-only override: NDS_FRAME_INTERPOLATION_MIN_REFRESH=<hz>
    // lowers the refresh gate so the blend path can be exercised on a 60 Hz
    // panel during validation. Leave it unset in normal use — a synthetic
    // present has nowhere to land on a display that is not comfortably above
    // 60 Hz, and forcing one only spends present time for no visible frame.
    constexpr int kInterpolationMinRefreshHz = 100;
    // The audio queue is this frontend's real-time clock, so it is also the
    // budget for the extra present: a blend happens only while the queue
    // still holds a comfortable runway (half the steady-state target, ~31 ms).
    // At or below that the loop is not keeping up and the synthetic present is
    // skipped for that frame. Nothing in this path ever sleeps or busy-loops.
    constexpr uint32_t kInterpolationAudioFloorFrames = kAudioQueueFrames / 2;
    int interpolation_min_refresh_hz = kInterpolationMinRefreshHz;
    if (const uint64_t forced_min_refresh =
            environment_u64("NDS_FRAME_INTERPOLATION_MIN_REFRESH")) {
        interpolation_min_refresh_hz =
            static_cast<int>(std::min<uint64_t>(forced_min_refresh, 1000));
        std::fprintf(stderr,
            "[sdl] frame interpolation: diagnostic min-refresh override "
            "%d Hz\n", interpolation_min_refresh_hz);
    }
    const bool interpolation_requested =
        options.frame_interpolation == NdsFrameInterpolation::Blend;
    bool interpolation_active = false;
    int interpolation_refresh_hz = 0;
    if (interpolation_requested) {
        if (presentation.gl_top) {
            std::fprintf(stderr,
                "[sdl] frame interpolation (experimental) is disabled: the "
                "direct OpenGL top-screen presenter owns its own swap and "
                "needs an offscreen output texture before a blended frame "
                "can be inserted\n");
        } else {
            const int display_index =
                SDL_GetWindowDisplayIndex(presentation.windows[0]);
            SDL_DisplayMode display_mode{};
            if (display_index < 0 ||
                SDL_GetCurrentDisplayMode(display_index, &display_mode) != 0) {
                std::fprintf(stderr,
                    "[sdl] frame interpolation: display refresh unavailable "
                    "(%s)\n", SDL_GetError());
            } else {
                interpolation_refresh_hz = display_mode.refresh_rate;
            }
            interpolation_active =
                interpolation_refresh_hz >= interpolation_min_refresh_hz;
            std::fprintf(stderr,
                "[sdl] frame interpolation (experimental): mode=blend "
                "active=%s refresh=%dHz min_refresh=%dHz\n",
                interpolation_active ? "yes" : "no",
                interpolation_refresh_hz, interpolation_min_refresh_hz);
        }
    }

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
        "Enter=Start Backspace=Select | Esc=quit%s\n",
        options.tab_turbo ? " | hold Tab=turbo" : "");
    if (options.relative_mouse_touch) {
        std::fprintf(stderr,
            "[sdl] relative mouse: click game screen to capture; "
            "Esc or focus loss releases; sensitivity=%u%% invert-y=%s "
            "aim=%s\n",
            static_cast<unsigned>(options.relative_mouse_sensitivity),
            options.relative_mouse_invert_y ? "on" : "off",
            options.relative_mouse_direct_aim ? "direct-unbounded"
                                              : "virtual-touch");
    }
    const bool mph_prime_controls_available =
        options.mph_prime_controls && options.relative_mouse_direct_aim;
    const bool mph_prime_unified_window_focus =
        mph_prime_controls_available &&
        options.mph_prime_unified_window_focus;
    MphPrimeBindingSet mph_prime_bindings{};
    MphPadBindingSet mph_pad_bindings{};
    if (mph_prime_controls_available) {
        mph_prime_bindings = make_mph_prime_bindings(options.mph_bindings);
        mph_pad_bindings = make_mph_pad_bindings(options.mph_pad_bindings);
        if (!mph_prime_bindings.valid || !mph_pad_bindings.valid) {
            destroy_presentation(presentation);
            SDL_Quit();
            return 1;
        }
        std::fprintf(stderr,
            "[sdl] MPH Prime Controls: melonPrimeDS bindings enabled; "
            "virtual stylus sensitivity=%u%%\n",
            static_cast<unsigned>(
                options.mph_virtual_stylus_sensitivity));
    }

    SDL_GameController* controller = open_first_controller();
    SDL_JoystickID controller_id = controller
        ? SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller))
        : -1;
    uint16_t keyboard_pressed = 0;
    uint16_t controller_pressed = 0;
    uint16_t mouse_pressed = 0;
    uint16_t mph_prime_pressed = 0;
    // Left analog stick mapped to the D-pad (movement in MPH, menus
    // everywhere), kept separate from button-event state so a wobbling
    // stick never fights explicit D-pad presses.
    uint16_t stick_pressed = 0;
    auto publish_keys = [&]() {
        nds_set_key_mask(static_cast<uint16_t>(
            0x0FFFu &
            ~(keyboard_pressed | controller_pressed | mouse_pressed |
              mph_prime_pressed | stick_pressed)));
    };
    publish_keys();
    nds_set_touch(0, 0, false);
    bool running = true;
    bool compute_failed = false;
    bool mouse_down = false;
    bool touch_release_pending = false;
    uint32_t touch_frames_held = 0;
    int64_t relative_delta_x = 0;
    int64_t relative_delta_y = 0;
    uint64_t relative_direct_writes = 0;
    uint64_t mph_prime_key_downs = 0;
    uint64_t mph_prime_mouse_downs = 0;
    NdsRelativeMouseTouch relative_mouse;
    std::array<bool, static_cast<size_t>(MphPrimeAction::Count)>
        mph_prime_held{};
    MphTouchSequence mph_touch_sequence{};
    float mph_virtual_x = 128.0f;
    float mph_virtual_y = 96.0f;
    // ── Gamepad dual-stick state ──────────────────────────────────────────
    // Right stick + triggers drive Prime Controls camera aim and fire,
    // engaging while used and idling back out so the touchscreen and menus
    // keep working when the sticks rest.
    bool mph_prime_pad_engaged = false;
    int mph_pad_idle_frames = 0;
    float mph_pad_aim_rem_x = 0.0f;
    float mph_pad_aim_rem_y = 0.0f;
    int32_t mph_pad_frame_x = 0;
    int32_t mph_pad_frame_y = 0;
    bool mph_pad_trigger_left_held = false;
    bool mph_pad_trigger_right_held = false;
    uint64_t mph_pad_aim_writes = 0;
    auto mph_prime_active = [&]() {
        return mph_prime_controls_available &&
               (relative_mouse.captured() || mph_prime_pad_engaged);
    };
    auto update_mph_prime_pressed = [&]() {
        uint16_t mask = 0;
        const bool virtual_stylus =
            mph_prime_held[static_cast<size_t>(
                MphPrimeAction::VirtualStylus)];
        for (size_t i = 0; i < mph_prime_held.size(); ++i) {
            if (!mph_prime_held[i]) continue;
            const auto action = static_cast<MphPrimeAction>(i);
            if (virtual_stylus &&
                (action == MphPrimeAction::Shoot ||
                 action == MphPrimeAction::ScanShoot)) {
                continue;
            }
            mask |= mph_prime_hold_mask(action);
        }
        mph_prime_pressed = mask;
        publish_keys();
    };
    auto clear_mph_prime_controls = [&]() {
        mph_prime_held.fill(false);
        mph_prime_pressed = 0;
        mph_touch_sequence = {};
    };
    auto is_presentation_window = [&](uint32_t window_id) {
        return window_id == presentation.window_ids[0] ||
               window_id == presentation.window_ids[1];
    };
    auto presentation_window_focus_index = [&](uint32_t window_id) {
        if (window_id == presentation.window_ids[0]) return 0;
        if (presentation.separate &&
            window_id == presentation.window_ids[1]) {
            return 1;
        }
        return -1;
    };
    auto set_mph_prime_action = [&](MphPrimeAction action, bool down,
                                    bool repeat) {
        const size_t index = static_cast<size_t>(action);
        const uint16_t mask = mph_prime_hold_mask(action);
        if (mask != 0 ||
            action == MphPrimeAction::VirtualStylus) {
            mph_prime_held[index] = down;
            update_mph_prime_pressed();
            return true;
        }
        if (down && !repeat)
            start_mph_touch_action(action, mph_touch_sequence);
        return true;
    };
    auto process_mph_prime_key = [&](SDL_Scancode key, bool down,
                                     bool repeat) {
        if (!mph_prime_controls_available) return false;
        bool consumed = false;
        for (size_t i = 0; i < mph_prime_bindings.bindings.size(); ++i) {
            if (!binding_matches_key(mph_prime_bindings.bindings[i], key))
                continue;
            consumed = true;
            set_mph_prime_action(static_cast<MphPrimeAction>(i), down,
                                 repeat);
        }
        if (consumed && down && !repeat)
            ++mph_prime_key_downs;
        return consumed;
    };
    // Pad buttons bound to Prime actions are consumed here (never also sent
    // as raw DS buttons). Unlike the keyboard path this gates on
    // availability, not on active capture: pressing a bound button is
    // itself the pad's engage gesture.
    auto process_mph_prime_pad = [&](MphPadInputKind kind,
                                     SDL_GameControllerButton button,
                                     bool down) {
        if (!mph_prime_controls_available) return false;
        bool consumed = false;
        for (size_t i = 0; i < mph_pad_bindings.bindings.size(); ++i) {
            const MphPadBinding& binding = mph_pad_bindings.bindings[i];
            if (binding.kind != kind) continue;
            if (kind == MphPadInputKind::Button &&
                binding.button != button) {
                continue;
            }
            consumed = true;
            if (down) {
                mph_prime_pad_engaged = true;
                mph_pad_idle_frames = 0;
            }
            set_mph_prime_action(static_cast<MphPrimeAction>(i), down,
                                 false);
        }
        return consumed;
    };
    auto process_mph_prime_mouse = [&](uint8_t button, bool down,
                                       bool repeat) {
        if (!mph_prime_active()) return false;
        bool consumed = false;
        for (size_t i = 0; i < mph_prime_bindings.bindings.size(); ++i) {
            if (!binding_matches_mouse(mph_prime_bindings.bindings[i],
                                       button)) {
                continue;
            }
            consumed = true;
            set_mph_prime_action(static_cast<MphPrimeAction>(i), down,
                                 repeat);
        }
        if (consumed && down && !repeat)
            ++mph_prime_mouse_downs;
        return consumed;
    };
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
        clear_mph_prime_controls();
        publish_keys();
        relative_delta_x = 0;
        relative_delta_y = 0;
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
        if (mph_prime_controls_available && keyboard_pressed != 0) {
            keyboard_pressed = 0;
            publish_keys();
        }
        nds_set_touch(relative_mouse.x(), relative_mouse.y(), true);
        std::fprintf(stderr, "[sdl] relative mouse captured\n");
    };
    uint64_t shown_frames = 0;
    uint64_t synthetic_presents = 0;
    FrameBlendCache blend_cache{};
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
    const uint64_t relative_mouse_selftest_start_vblank =
        environment_u64("NDS_FRONTEND_SELFTEST_RELATIVE_MOUSE_VBLANK");
    bool audio_started = false;
    uint32_t audio_start_threshold = kAudioStartFrames;
    bool audio_queue_error = false;
    bool turbo_pressed = false;
    bool turbo_active = false;
    bool focus_release_pending = false;
    bool presentation_window_focused[2] = {
        (SDL_GetWindowFlags(presentation.windows[0]) &
         SDL_WINDOW_INPUT_FOCUS) != 0,
        presentation.separate &&
            (SDL_GetWindowFlags(presentation.windows[1]) &
             SDL_WINDOW_INPUT_FOCUS) != 0,
    };
    auto any_presentation_window_focused = [&]() {
        return presentation_window_focused[0] ||
               (presentation.separate && presentation_window_focused[1]);
    };
    auto clear_tab_turbo = [&]() {
        if (options.tab_turbo) turbo_pressed = false;
    };
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
    g_input_debug = {};
    nds_diagnostics_start_performance_log(options);
    auto publish_input_debug = [&]() {
        g_input_debug.active = 1;
        g_input_debug.mph_prime_controls_available =
            mph_prime_controls_available ? 1 : 0;
        g_input_debug.mph_prime_controls_active =
            mph_prime_active() ? 1 : 0;
        g_input_debug.relative_mouse_captured =
            relative_mouse.captured() ? 1 : 0;
        g_input_debug.keyboard_pressed = keyboard_pressed;
        g_input_debug.mouse_pressed = mouse_pressed;
        g_input_debug.mph_prime_pressed = mph_prime_pressed;
        g_input_debug.stick_pressed = stick_pressed;
        g_input_debug.pad_engaged = mph_prime_pad_engaged ? 1 : 0;
        g_input_debug.pad_aim_writes = mph_pad_aim_writes;
        g_input_debug.published_key_mask = static_cast<uint16_t>(
            0x0FFFu & ~(keyboard_pressed | controller_pressed |
                        mouse_pressed | mph_prime_pressed |
                        stick_pressed));
        g_input_debug.relative_direct_writes = relative_direct_writes;
        g_input_debug.mph_prime_key_downs = mph_prime_key_downs;
        g_input_debug.mph_prime_mouse_downs = mph_prime_mouse_downs;
        g_input_debug.virtual_stylus_x =
            static_cast<int>(std::lround(mph_virtual_x));
        g_input_debug.virtual_stylus_y =
            static_cast<int>(std::lround(mph_virtual_y));
        g_input_debug.top_window_id = presentation.window_ids[0];
        g_input_debug.bottom_window_id = presentation.window_ids[1];
        g_input_debug.bottom_content_left = bottom_content_left;
        g_input_debug.separate = presentation.separate ? 1 : 0;
    };
    publish_input_debug();
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
        publish_input_debug();
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
            if (relative_mouse_selftest_stage == 0 && shown_frames >= 2 &&
                (relative_mouse_selftest_start_vblank == 0 ||
                 nds_event_counts().vblank9 >=
                     relative_mouse_selftest_start_vblank)) {
                std::fprintf(stderr,
                    "[sdl] relative mouse self-test starting at VBlank %llu\n",
                    static_cast<unsigned long long>(
                        nds_event_counts().vblank9));
                relative_mouse_selftest_error |=
                    !options.relative_mouse_touch ||
                    options.relative_mouse_fire_mask == 0;
                SDL_RaiseWindow(presentation.windows[0]);
                injected.type = SDL_MOUSEBUTTONDOWN;
                injected.button.windowID = presentation.window_ids[0];
                injected.button.button = SDL_BUTTON_LEFT;
                // Stacked first verifies that the bottom logical screen
                // remains touch-only. Separate verifies the traditional
                // top-window capture path.
                injected.button.x = presentation.separate
                    ? 127 * kWindowScale
                    : (bottom_content_left + 127) * kWindowScale;
                injected.button.y = presentation.separate
                    ? 96 * kWindowScale
                    : (kScreenHeight + 96) * kWindowScale;
                relative_mouse_selftest_error |= SDL_PushEvent(&injected) < 0;
                relative_mouse_selftest_stage = 1;
            } else if (relative_mouse_selftest_stage == 1 &&
                       shown_frames >= 3) {
                if (presentation.separate) {
                    relative_mouse_selftest_error |= !relative_mouse.captured();
                    injected.type = SDL_MOUSEMOTION;
                    injected.motion.windowID = presentation.window_ids[0];
                    injected.motion.xrel = 20;
                    injected.motion.yrel = -10;
                    relative_mouse_selftest_stage = 3;
                } else {
                    relative_mouse_selftest_error |= relative_mouse.captured() ||
                        !mouse_down;
                    injected.type = SDL_MOUSEBUTTONUP;
                    injected.button.windowID = presentation.window_ids[0];
                    injected.button.button = SDL_BUTTON_LEFT;
                    injected.button.x =
                        (bottom_content_left + 127) * kWindowScale;
                    injected.button.y =
                        (kScreenHeight + 96) * kWindowScale;
                    relative_mouse_selftest_stage = 2;
                }
                relative_mouse_selftest_error |= SDL_PushEvent(&injected) < 0;
            } else if (relative_mouse_selftest_stage == 2 &&
                       shown_frames >= 4) {
                relative_mouse_selftest_error |= mouse_down;
                injected.type = SDL_MOUSEBUTTONDOWN;
                injected.button.windowID = presentation.window_ids[0];
                injected.button.button = SDL_BUTTON_LEFT;
                injected.button.x =
                    (top_content_left + 127) * kWindowScale;
                injected.button.y = 96 * kWindowScale;
                relative_mouse_selftest_error |= SDL_PushEvent(&injected) < 0;
                relative_mouse_selftest_stage = 3;
            } else if (relative_mouse_selftest_stage == 3 &&
                       shown_frames >= 5) {
                relative_mouse_selftest_error |= !relative_mouse.captured();
                injected.type = SDL_MOUSEMOTION;
                injected.motion.windowID = presentation.window_ids[0];
                injected.motion.xrel = 20;
                injected.motion.yrel = -10;
                relative_mouse_selftest_error |= SDL_PushEvent(&injected) < 0;
                relative_mouse_selftest_stage = 4;
            } else if (relative_mouse_selftest_stage == 4 &&
                       shown_frames >= 6) {
                relative_mouse_selftest_error |=
                    options.relative_mouse_direct_aim
                        ? relative_direct_writes == 0
                        : (relative_mouse.x() == 128 &&
                           relative_mouse.y() == 96);
                injected.type = SDL_MOUSEBUTTONDOWN;
                injected.button.windowID = presentation.window_ids[0];
                injected.button.button = SDL_BUTTON_LEFT;
                relative_mouse_selftest_error |= SDL_PushEvent(&injected) < 0;
                relative_mouse_selftest_stage = 5;
            } else if (relative_mouse_selftest_stage == 5 &&
                       shown_frames >= 7) {
                relative_mouse_selftest_error |=
                    mouse_pressed != options.relative_mouse_fire_mask;
                std::fprintf(stderr,
                    "[sdl] relative mouse fire asserted at VBlank %llu\n",
                    static_cast<unsigned long long>(
                        nds_event_counts().vblank9));
                injected.type = SDL_MOUSEBUTTONUP;
                injected.button.windowID = presentation.window_ids[0];
                injected.button.button = SDL_BUTTON_LEFT;
                relative_mouse_selftest_error |= SDL_PushEvent(&injected) < 0;
                relative_mouse_selftest_stage = 6;
            } else if (relative_mouse_selftest_stage == 6 &&
                       shown_frames >= 8) {
                relative_mouse_selftest_error |= mouse_pressed != 0;
                injected.type = SDL_KEYDOWN;
                injected.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
                relative_mouse_selftest_error |= SDL_PushEvent(&injected) < 0;
                relative_mouse_selftest_stage = 7;
            } else if (relative_mouse_selftest_stage == 7 &&
                       shown_frames >= 9) {
                relative_mouse_selftest_error |=
                    relative_mouse.captured() || mouse_pressed != 0;
                injected.type = SDL_MOUSEBUTTONDOWN;
                injected.button.windowID = presentation.window_ids[0];
                injected.button.button = SDL_BUTTON_LEFT;
                injected.button.x =
                    (top_content_left + 127) * kWindowScale;
                injected.button.y = 96 * kWindowScale;
                relative_mouse_selftest_error |= SDL_PushEvent(&injected) < 0;
                relative_mouse_selftest_stage = 8;
            } else if (relative_mouse_selftest_stage == 8 &&
                       shown_frames >= 10) {
                relative_mouse_selftest_error |= !relative_mouse.captured();
                injected.type = SDL_WINDOWEVENT;
                injected.window.windowID = presentation.window_ids[0];
                injected.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
                relative_mouse_selftest_error |= SDL_PushEvent(&injected) < 0;
                relative_mouse_selftest_stage = 9;
            } else if (relative_mouse_selftest_stage == 9 &&
                       shown_frames >= 11) {
                relative_mouse_selftest_error |=
                    relative_mouse.captured() || mouse_pressed != 0;
                relative_mouse_selftest_stage = 10;
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
                is_presentation_window(event.window.windowID)) {
                const int index =
                    presentation_window_focus_index(event.window.windowID);
                if (index >= 0)
                    presentation_window_focused[index] = false;
                if (mph_prime_unified_window_focus)
                    focus_release_pending = true;
                else {
                    release_relative_mouse();
                    clear_tab_turbo();
                }
            }
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED &&
                is_presentation_window(event.window.windowID)) {
                const int index =
                    presentation_window_focus_index(event.window.windowID);
                if (index >= 0)
                    presentation_window_focused[index] = true;
                focus_release_pending = false;
            }
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                    if (relative_mouse.captured())
                        release_relative_mouse();
                    else
                        running = false;
                } else if (process_mph_prime_key(
                               event.key.keysym.scancode, true, false)) {
                    // Consumed by the MPH-specific keyboard/mouse layer.
                } else if (options.tab_turbo &&
                           event.key.keysym.scancode == SDL_SCANCODE_TAB) {
                    turbo_pressed = true;
                } else if (mph_prime_active()) {
                    // Prime Controls replaces the normal keyboard keypad map;
                    // unbound keys must not leak through as DS buttons.
                } else if (const uint16_t bit = key_bit(event.key.keysym.scancode)) {
                    ++host_key_presses;
                    keyboard_pressed |= bit;
                    publish_keys();
                }
            }
            if (event.type == SDL_KEYUP && !event.key.repeat) {
                if (process_mph_prime_key(
                        event.key.keysym.scancode, false, false)) {
                    // Consumed by the MPH-specific keyboard/mouse layer.
                } else if (options.tab_turbo &&
                           event.key.keysym.scancode == SDL_SCANCODE_TAB) {
                    turbo_pressed = false;
                } else if (mph_prime_active()) {
                    // See keydown path: ignore generic keyboard bindings
                    // while the Prime Controls capture owns the keyboard.
                } else if (const uint16_t bit = key_bit(
                               event.key.keysym.scancode)) {
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
                // Pad-held Prime actions must not survive the device; the
                // keyboard/mouse can re-press theirs on the next event.
                clear_mph_prime_controls();
                publish_keys();
            }
            if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                const auto button = static_cast<SDL_GameControllerButton>(
                    event.cbutton.button);
                if (process_mph_prime_pad(MphPadInputKind::Button, button,
                                          true)) {
                    // Consumed by Prime Controls.
                } else if (const uint16_t bit = controller_bit(button)) {
                    controller_pressed |= bit;
                    publish_keys();
                }
            }
            if (event.type == SDL_CONTROLLERBUTTONUP) {
                const auto button = static_cast<SDL_GameControllerButton>(
                    event.cbutton.button);
                if (process_mph_prime_pad(MphPadInputKind::Button, button,
                                          false)) {
                    // Consumed by Prime Controls.
                } else if (const uint16_t bit = controller_bit(button)) {
                    controller_pressed &= static_cast<uint16_t>(~bit);
                    publish_keys();
                }
            }
            const bool primary_left_down =
                event.type == SDL_MOUSEBUTTONDOWN &&
                event.button.button == SDL_BUTTON_LEFT &&
                event.button.windowID == presentation.window_ids[0];
            const bool bottom_left_down =
                event.type == SDL_MOUSEBUTTONDOWN &&
                event.button.button == SDL_BUTTON_LEFT &&
                presentation.separate &&
                event.button.windowID == presentation.window_ids[1];
            const bool relative_left_down =
                primary_left_down ||
                (bottom_left_down && mph_prime_unified_window_focus);
            const NdsStackedRelativeMouseRoute stacked_left_route =
                primary_left_down && !presentation.separate
                    ? nds_route_stacked_relative_mouse_button(
                        options.relative_mouse_touch,
                        relative_mouse.captured(), stacked_top_screen,
                        stacked_bottom_touch, event.button.x, event.button.y)
                    : NdsStackedRelativeMouseRoute::None;
            if (relative_left_down && options.relative_mouse_touch &&
                (presentation.separate ||
                 stacked_left_route ==
                     NdsStackedRelativeMouseRoute::AcquireRelative ||
                 stacked_left_route ==
                     NdsStackedRelativeMouseRoute::CapturedButton)) {
                if (!relative_mouse.captured()) {
                    // The acquisition click only captures; the next click is
                    // the first guest fire press, avoiding an accidental shot.
                    capture_relative_mouse();
                } else if (process_mph_prime_mouse(
                               event.button.button, true, false)) {
                    // Consumed by Prime Controls.
                } else if (options.relative_mouse_fire_mask != 0) {
                    mouse_pressed |= options.relative_mouse_fire_mask;
                    publish_keys();
                }
            }
            if (event.type == SDL_MOUSEBUTTONDOWN &&
                event.button.button != SDL_BUTTON_LEFT &&
                (event.button.windowID == presentation.window_ids[0] ||
                 (mph_prime_unified_window_focus &&
                  is_presentation_window(event.button.windowID))) &&
                process_mph_prime_mouse(event.button.button, true, false)) {
                // Consumed by Prime Controls.
            }
            if (event.type == SDL_MOUSEBUTTONUP &&
                (event.button.windowID == presentation.window_ids[0] ||
                 (mph_prime_unified_window_focus &&
                  is_presentation_window(event.button.windowID))) &&
                relative_mouse.captured()) {
                if (process_mph_prime_mouse(
                        event.button.button, false, false)) {
                    // Consumed by Prime Controls.
                } else if (event.button.button == SDL_BUTTON_LEFT &&
                           mouse_pressed != 0) {
                    mouse_pressed &= static_cast<uint16_t>(
                        ~options.relative_mouse_fire_mask);
                    publish_keys();
                }
            }
            if (event.type == SDL_MOUSEBUTTONDOWN &&
                event.button.button == SDL_BUTTON_LEFT &&
                event.button.windowID == presentation.window_ids[1] &&
                !(mph_prime_unified_window_focus &&
                  relative_mouse.captured()) &&
                (presentation.separate
                    ? event.button.x >= bottom_content_left &&
                          event.button.x < bottom_content_left + kScreenWidth
                    : stacked_left_route ==
                          NdsStackedRelativeMouseRoute::Touchscreen)) {
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
                 (mph_prime_unified_window_focus &&
                  is_presentation_window(event.motion.windowID)) ||
                 event.motion.windowID == 0)) {
                const bool prime_virtual_stylus = mph_prime_active() &&
                    mph_prime_held[static_cast<size_t>(
                        MphPrimeAction::VirtualStylus)];
                if (prime_virtual_stylus) {
                    mph_virtual_x +=
                        static_cast<float>(event.motion.xrel) *
                        options.mph_virtual_stylus_sensitivity * 0.01f;
                    mph_virtual_y +=
                        static_cast<float>(event.motion.yrel) *
                        (256.0f / 192.0f) *
                        options.mph_virtual_stylus_sensitivity * 0.01f;
                    mph_virtual_x = std::clamp(mph_virtual_x, 0.0f, 255.0f);
                    mph_virtual_y = std::clamp(mph_virtual_y, 0.0f, 191.0f);
                } else if (options.relative_mouse_direct_aim) {
                    relative_delta_x += event.motion.xrel;
                    relative_delta_y += event.motion.yrel;
                } else if (relative_mouse.move(event.motion.xrel,
                                               event.motion.yrel)) {
                    nds_set_touch(relative_mouse.x(), relative_mouse.y(), true);
                }
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
        if (focus_release_pending) {
            if (!any_presentation_window_focused()) {
                release_relative_mouse();
                clear_tab_turbo();
            }
            focus_release_pending = false;
        }

        // ── Gamepad dual-stick poll (once per shown frame) ───────────────
        mph_pad_frame_x = 0;
        mph_pad_frame_y = 0;
        if (controller) {
            // Left stick -> D-pad, with hysteresis so a stick resting near
            // a threshold never flickers a direction.
            const float lx = SDL_GameControllerGetAxis(
                controller, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
            const float ly = SDL_GameControllerGetAxis(
                controller, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
            auto stick_dir = [&](float value, uint16_t bit, bool positive) {
                const float v = positive ? value : -value;
                const bool held = (stick_pressed & bit) != 0;
                const bool next = v > (held ? 0.35f : 0.5f);
                if (next != held) {
                    if (next) stick_pressed |= bit;
                    else stick_pressed &= static_cast<uint16_t>(~bit);
                    publish_keys();
                }
            };
            stick_dir(lx, 1u << 4, true);    // Right
            stick_dir(lx, 1u << 5, false);   // Left
            stick_dir(ly, 1u << 6, false);   // Up (SDL Y axis points down)
            stick_dir(ly, 1u << 7, true);    // Down

            if (mph_prime_controls_available) {
                // Right stick -> camera aim; triggers act as bindable
                // pseudo-buttons (defaults: RT shoot, LT scan-fire).
                const float rx = SDL_GameControllerGetAxis(
                    controller, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
                const float ry = SDL_GameControllerGetAxis(
                    controller, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
                const bool trigger_right = SDL_GameControllerGetAxis(
                    controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 9830;
                const bool trigger_left = SDL_GameControllerGetAxis(
                    controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 9830;
                if (trigger_right != mph_pad_trigger_right_held) {
                    mph_pad_trigger_right_held = trigger_right;
                    process_mph_prime_pad(MphPadInputKind::TriggerRight,
                                          SDL_CONTROLLER_BUTTON_INVALID,
                                          trigger_right);
                }
                if (trigger_left != mph_pad_trigger_left_held) {
                    mph_pad_trigger_left_held = trigger_left;
                    process_mph_prime_pad(MphPadInputKind::TriggerLeft,
                                          SDL_CONTROLLER_BUTTON_INVALID,
                                          trigger_left);
                }
                const float mag = std::sqrt(rx * rx + ry * ry);
                constexpr float kDeadzone = 0.25f;
                const bool aiming = mag > kDeadzone;
                if (aiming || trigger_right || trigger_left) {
                    mph_pad_idle_frames = 0;
                    if (!mph_prime_pad_engaged) {
                        mph_prime_pad_engaged = true;
                        std::fprintf(stderr,
                            "[sdl] MPH pad aim engaged (right stick / "
                            "triggers); idles out when released\n");
                    }
                } else if (mph_prime_pad_engaged &&
                           !relative_mouse.captured() &&
                           ++mph_pad_idle_frames > 45) {
                    mph_prime_pad_engaged = false;
                    nds_set_touch(0, 0, false);
                }
                if (aiming && mph_prime_pad_engaged) {
                    // Square-law response: fine aim near center, a full
                    // deflection turns at the built-in rate scaled by the
                    // pad sensitivity. Y keeps the mouse path's 150% scale
                    // and follows the same invert option.
                    const float curved = (mag - kDeadzone) / (1.0f - kDeadzone);
                    const float rate = curved * curved * 5.0f *
                        (options.mph_pad_aim_sensitivity / 100.0f) / mag;
                    mph_pad_aim_rem_x += rx * rate;
                    mph_pad_aim_rem_y += ry * rate * 1.5f *
                        (options.relative_mouse_invert_y ? -1.0f : 1.0f);
                    mph_pad_frame_x = static_cast<int32_t>(mph_pad_aim_rem_x);
                    mph_pad_frame_y = static_cast<int32_t>(mph_pad_aim_rem_y);
                    mph_pad_aim_rem_x -= static_cast<float>(mph_pad_frame_x);
                    mph_pad_aim_rem_y -= static_cast<float>(mph_pad_frame_y);
                }
            }
        } else {
            if (stick_pressed != 0) {
                stick_pressed = 0;
                publish_keys();
            }
            if (mph_pad_trigger_right_held) {
                mph_pad_trigger_right_held = false;
                process_mph_prime_pad(MphPadInputKind::TriggerRight,
                                      SDL_CONTROLLER_BUTTON_INVALID, false);
            }
            if (mph_pad_trigger_left_held) {
                mph_pad_trigger_left_held = false;
                process_mph_prime_pad(MphPadInputKind::TriggerLeft,
                                      SDL_CONTROLLER_BUTTON_INVALID, false);
            }
            if (mph_prime_pad_engaged) {
                mph_prime_pad_engaged = false;
                if (!relative_mouse.captured()) nds_set_touch(0, 0, false);
            }
        }

        const bool mph_prime_is_active = mph_prime_active();
        const bool mph_prime_virtual_stylus = mph_prime_is_active &&
            mph_prime_held[static_cast<size_t>(
                MphPrimeAction::VirtualStylus)];

        const bool turbo_want = turbo_pressed || nds_debug_turbo();
        if (turbo_want != turbo_active) {
            turbo_active = turbo_want;
            if (audio) {
                SDL_PauseAudioDevice(audio, 1);
                clear_audio_queue(audio, audio_queue);
                audio_queue.started.store(false, std::memory_order_relaxed);
            }
            audio_started = false;
            audio_start_threshold = kAudioQueueFrames;
            audio_pace_floor = audio_start_threshold;
            audio_min_queue = std::numeric_limits<uint32_t>::max();
            audio_max_queue = 0;
            std::fprintf(stderr, "[sdl] turbo %s\n",
                         turbo_active ? "on" : "off");
        }

        if ((relative_mouse.captured() || mph_prime_pad_engaged) &&
            options.relative_mouse_direct_aim &&
            !mph_prime_virtual_stylus && !mph_touch_sequence.active() &&
            (relative_delta_x != 0 || relative_delta_y != 0 ||
             mph_pad_frame_x != 0 || mph_pad_frame_y != 0)) {
            // AMHE0 consumes signed per-frame aim deltas. Keep the native
            // stylus held at center, but feed motion through those title-owned
            // fields so turning never stops at a virtual touchscreen edge.
            // The pad's right-stick contribution is pre-scaled and merges
            // with the mouse counts here because the title fields are
            // OVERWRITTEN per frame, not accumulated.
            const NdsRelativeMouseDelta delta =
                nds_scale_relative_mouse_delta(
                    relative_delta_x, relative_delta_y,
                    options.relative_mouse_sensitivity,
                    options.relative_mouse_invert_y, 150);
            const int32_t final_x = delta.x + mph_pad_frame_x;
            const int32_t final_y = delta.y + mph_pad_frame_y;
            if (nds_title_patches_apply_mph_mouse_delta(final_x, final_y)) {
                ++relative_direct_writes;
                if (mph_pad_frame_x != 0 || mph_pad_frame_y != 0)
                    ++mph_pad_aim_writes;
            }
            relative_delta_x = 0;
            relative_delta_y = 0;
        }
        if (mph_prime_is_active) {
            if (mph_touch_sequence.active()) {
                mph_touch_sequence.tick();
            } else if (mph_prime_virtual_stylus) {
                const bool touch_down =
                    mph_prime_held[static_cast<size_t>(
                        MphPrimeAction::Shoot)] ||
                    mph_prime_held[static_cast<size_t>(
                        MphPrimeAction::ScanShoot)];
                nds_set_touch(
                    static_cast<uint16_t>(std::lround(mph_virtual_x)),
                    static_cast<uint16_t>(std::lround(mph_virtual_y)),
                    touch_down);
            } else {
                const bool in_ball =
                    bus_read_u8_slow(kMphUs10MorphState) == 0x02u;
                if (in_ball)
                    nds_set_touch(0, 0, false);
                else
                    nds_set_touch(128, 96, true);
            }
        }
        publish_input_debug();

        const uint64_t phase0 = SDL_GetPerformanceCounter();
        const uint64_t now = scheduler_system_timestamp();
        const uint64_t next_frame =
            (now / kSystemCyclesPerFrame + 1u) * kSystemCyclesPerFrame;
        while (running && scheduler_system_timestamp() < next_frame &&
               !(scheduler_cpu_terminal_halted(0) &&
                 scheduler_cpu_terminal_halted(1))) {
            scheduler_run_round();
        }
        // A real DS power-off is an application lifecycle request, not a
        // debuggable terminal halt. Leave before presenting the powered-down
        // black framebuffer; main() performs durable state flushes.
        if (nds_powered_off()) {
            std::fprintf(stderr,
                         "[sdl] guest requested power-off; closing\n");
            running = false;
            break;
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
        // Direct-present frames skip the adaptive compositor, so the HD
        // surfaces must be invalidated here rather than inside it.
        nds_gpu2d_invalidate_hd_frame();
        if ((options.adaptive_screens & NDS_ADAPTIVE_TOP) &&
            !nds_gpu2d_direct_present_frame_active())
            top_pixels =
                nds_gpu2d_adaptive_framebuffer(0, &top_width);
        else if (nds_gpu2d_direct_present_frame_active())
            top_width = nds_gpu3d_output_width();
        if (options.adaptive_screens & NDS_ADAPTIVE_BOTTOM)
            bottom_pixels =
                nds_gpu2d_adaptive_framebuffer(1, &bottom_width);
        phase_adaptive_ticks +=
            SDL_GetPerformanceCounter() - adaptive_start;
        observe_top_black_bands(native_top, shown_frames);
        // At most ONE synthetic frame, presented ahead of the real one, so
        // the visible order stays real(N-1), blend(N-1,N), real(N). No
        // scheduler round, no input sampling, no audio production happens
        // here: this only re-presents pixels that already exist. The direct
        // presenter is excluded above; direct-present frames are excluded
        // again per frame because their top surface lives on the GPU.
        if (interpolation_active && blend_cache.valid && !turbo_active &&
            !nds_gpu2d_direct_present_frame_active() &&
            blend_cache.widths[0] == top_width &&
            blend_cache.widths[1] == bottom_width && audio_started &&
            audio_queue_count(audio, audio_queue) >
                kInterpolationAudioFloorFrames) {
            blend_half(blend_cache.previous[0].data(), top_pixels,
                       blend_cache.blended[0].data(),
                       blend_cache.previous[0].size());
            blend_half(blend_cache.previous[1].data(), bottom_pixels,
                       blend_cache.blended[1].data(),
                       blend_cache.previous[1].size());
            const PresentationTicks synthetic_ticks = present_screens(
                presentation, blend_cache.blended[0].data(), top_width,
                blend_cache.blended[1].data(), bottom_width,
                mph_prime_virtual_stylus,
                mph_virtual_x, mph_virtual_y);
            if (!synthetic_ticks.ok) {
                compute_failed = true;
                running = false;
                break;
            }
            phase_upload_ticks += synthetic_ticks.upload;
            phase_draw_ticks += synthetic_ticks.draw;
            phase_swap_ticks += synthetic_ticks.swap;
            // phase_present_ticks is measured from phase1, which already
            // encloses this block; only the sub-counters need folding in.
            ++synthetic_presents;
            // Events are still consumed at exactly one point, the top of the
            // loop, so input keeps its DS-frame sampling cadence. Pumping
            // here only keeps the OS message queue from backing up across the
            // extra present.
            SDL_PumpEvents();
        }
        const PresentationTicks presentation_ticks = present_screens(
            presentation, top_pixels, top_width,
            bottom_pixels, bottom_width,
            mph_prime_virtual_stylus,
            mph_virtual_x, mph_virtual_y);
        if (!presentation_ticks.ok) {
            compute_failed = true;
            running = false;
            break;
        }
        phase_upload_ticks += presentation_ticks.upload;
        phase_draw_ticks += presentation_ticks.draw;
        phase_swap_ticks += presentation_ticks.swap;
        if (interpolation_active &&
            !nds_gpu2d_direct_present_frame_active()) {
            cache_presented_frame(blend_cache, 0, top_pixels, top_width);
            cache_presented_frame(blend_cache, 1, bottom_pixels, bottom_width);
            blend_cache.valid = true;
        } else {
            blend_cache.valid = false;
        }
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
        uint32_t queued = audio_queue_count(audio, audio_queue);
        if (turbo_active) {
            discard_spu_output();
        } else {
            queued = drain_audio(
                audio, audio_queue, audio_started, audio_pace_floor,
                audio_queue_error);
        }
        phase_drain_ticks += SDL_GetPerformanceCounter() - phase2;
        audio_max_queue = std::max(audio_max_queue, queued);
        if (audio && !audio_started && !turbo_active &&
            queued >= audio_start_threshold) {
            // Opening paused and prebuffering avoids the guaranteed startup
            // underrun produced by unpausing an empty SDL queue.
            audio_queue.started.store(true, std::memory_order_relaxed);
            SDL_PauseAudioDevice(audio, 0);
            audio_started = true;
            audio_min_queue = queued;
            audio_pace_floor = audio_start_threshold;
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
        g_live_stats.real_presents = shown_frames;
        g_live_stats.synthetic_presents = synthetic_presents;
        const uint64_t counter = SDL_GetPerformanceCounter();
        g_live_stats.now_ticks = counter;
        g_live_stats.freq = frequency;
        nds_diagnostics_maybe_write_performance_sample(g_live_stats);
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
        if (selftest_relative_mouse && relative_mouse_selftest_stage == 10)
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
    g_input_debug.active = 0;
    nds_diagnostics_stop_performance_log();
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
    nds_gpu2d_set_direct_present(false);
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
        std::fprintf(stderr,
            "[sdl] frame interpolation: mode=%s active=%u refresh=%dHz "
            "min_refresh=%dHz real_presents=%llu synthetic_presents=%llu\n",
            nds_frame_interpolation_name(options.frame_interpolation),
            interpolation_active ? 1u : 0u,
            interpolation_refresh_hz, interpolation_min_refresh_hz,
            static_cast<unsigned long long>(shown_frames),
            static_cast<unsigned long long>(synthetic_presents));
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
        (relative_mouse_selftest_error || relative_mouse_selftest_stage != 10);
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

bool nds_frontend_request_exit() {
#if defined(NDS_HAVE_SDL2)
    if (!g_live_stats.active) return false;
    SDL_Event event{};
    event.type = SDL_QUIT;
    return SDL_PushEvent(&event) == 1;
#else
    return false;
#endif
}

bool nds_frontend_debug_key(const char* key_name, bool down) {
#if defined(NDS_HAVE_SDL2)
    if (!g_input_debug.active || !key_name || key_name[0] == '\0')
        return false;
    const SDL_Scancode key = scancode_from_binding_name(key_name);
    g_input_debug.debug_last_key_scancode = static_cast<uint32_t>(key);
    if (key == SDL_SCANCODE_UNKNOWN)
        return false;
    SDL_Event event{};
    event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.windowID = g_input_debug.top_window_id;
    event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.key.repeat = 0;
    event.key.keysym.scancode = key;
    event.key.keysym.sym = SDL_GetKeyFromScancode(key);
    const bool pushed = SDL_PushEvent(&event) == 1;
    if (pushed) ++g_input_debug.debug_key_events;
    else ++g_input_debug.debug_event_errors;
    return pushed;
#else
    (void)key_name;
    (void)down;
    return false;
#endif
}

bool nds_frontend_debug_mouse_button(uint8_t button, bool down) {
#if defined(NDS_HAVE_SDL2)
    if (!g_input_debug.active || button == 0)
        return false;
    SDL_Event event{};
    event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    event.button.windowID = g_input_debug.top_window_id;
    event.button.button = button;
    event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.button.clicks = 1;
    event.button.x = 128 * kWindowScale;
    event.button.y = 96 * kWindowScale;
    const bool pushed = SDL_PushEvent(&event) == 1;
    if (pushed) ++g_input_debug.debug_mouse_button_events;
    else ++g_input_debug.debug_event_errors;
    return pushed;
#else
    (void)button;
    (void)down;
    return false;
#endif
}

bool nds_frontend_debug_mouse_motion(int dx, int dy) {
#if defined(NDS_HAVE_SDL2)
    if (!g_input_debug.active)
        return false;
    SDL_Event event{};
    event.type = SDL_MOUSEMOTION;
    event.motion.windowID = g_input_debug.top_window_id;
    event.motion.x = 128 * kWindowScale;
    event.motion.y = 96 * kWindowScale;
    event.motion.xrel = dx;
    event.motion.yrel = dy;
    const bool pushed = SDL_PushEvent(&event) == 1;
    if (pushed) ++g_input_debug.debug_mouse_motion_events;
    else ++g_input_debug.debug_event_errors;
    return pushed;
#else
    (void)dx;
    (void)dy;
    return false;
#endif
}

bool nds_frontend_debug_touch(uint16_t x, uint16_t y, bool down) {
#if defined(NDS_HAVE_SDL2)
    if (!g_input_debug.active || x >= kScreenWidth || y >= kScreenHeight)
        return false;
    SDL_Event event{};
    event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    event.button.windowID = g_input_debug.bottom_window_id;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.button.clicks = 1;
    event.button.x = (g_input_debug.bottom_content_left +
                      static_cast<int>(x)) * kWindowScale;
    event.button.y = (g_input_debug.separate ? static_cast<int>(y)
                                             : kScreenHeight +
                                                   static_cast<int>(y)) *
                     kWindowScale;
    const bool pushed = SDL_PushEvent(&event) == 1;
    if (pushed) ++g_input_debug.debug_touch_events;
    else ++g_input_debug.debug_event_errors;
    return pushed;
#else
    (void)x;
    (void)y;
    (void)down;
    return false;
#endif
}

bool nds_frontend_debug_capture_mouse() {
#if defined(NDS_HAVE_SDL2)
    if (!g_input_debug.active)
        return false;
    if (g_input_debug.relative_mouse_captured)
        return true;
    const bool pushed = nds_frontend_debug_mouse_button(SDL_BUTTON_LEFT, true);
    if (pushed) ++g_input_debug.debug_capture_events;
    return pushed;
#else
    return false;
#endif
}

bool nds_frontend_debug_release_mouse() {
#if defined(NDS_HAVE_SDL2)
    if (!g_input_debug.active)
        return false;
    if (!g_input_debug.relative_mouse_captured)
        return true;
    SDL_Event event{};
    event.type = SDL_WINDOWEVENT;
    event.window.windowID = g_input_debug.top_window_id;
    event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    const bool pushed = SDL_PushEvent(&event) == 1;
    if (pushed) ++g_input_debug.debug_release_events;
    else ++g_input_debug.debug_event_errors;
    return pushed;
#else
    return false;
#endif
}

void nds_frontend_live_stats(NdsFrontendLiveStats* out) {
    if (!out) return;
    *out = g_live_stats;
#if defined(NDS_HAVE_SDL2)
    if (g_live_stats.active)
        out->now_ticks = SDL_GetPerformanceCounter();
#endif
}

void nds_frontend_input_debug_state(NdsFrontendInputDebugState* out) {
    if (out) *out = g_input_debug;
}

void nds_frontend_black_band_scan(bool enabled, bool reset) {
    if (reset) g_black_band = {};
    g_black_band.enabled = enabled ? 1 : 0;
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    nds_gpu2d_set_direct_present(
        !enabled && nds_compute_host_has_visible_context());
#endif
}

void nds_frontend_black_band_capture(NdsFrontendBlackBandCapture* out) {
    if (out) *out = g_black_band;
}
