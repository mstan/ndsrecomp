#include "frontend.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <cstdlib>

#include "toml.hpp"

namespace {

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

}  // namespace

bool nds_parse_screen_layout(const std::string& value,
                             NdsScreenLayout* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "stacked" || normalized == "combined" ||
        normalized == "single") {
        *out = NdsScreenLayout::Stacked;
        return true;
    }
    if (normalized == "separate" || normalized == "two-window" ||
        normalized == "two_windows") {
        *out = NdsScreenLayout::Separate;
        return true;
    }
    return false;
}

bool nds_parse_fullscreen_mode(const std::string& value,
                               NdsFullscreenMode* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "off") {
        *out = NdsFullscreenMode::Off;
        return true;
    }
    if (normalized == "borderless") {
        *out = NdsFullscreenMode::Borderless;
        return true;
    }
    if (normalized == "exclusive") {
        *out = NdsFullscreenMode::Exclusive;
        return true;
    }
    return false;
}

bool nds_apply_fullscreen_overrides(
    NdsFrontendOptions* options, const std::string& cli_fullscreen,
    NdsFullscreenOverrideError* error) {
    if (error) *error = NdsFullscreenOverrideError::None;
    if (!options) return false;
    if (const char* value = std::getenv("NDS_FULLSCREEN")) {
        if (!nds_parse_fullscreen_mode(value, &options->fullscreen)) {
            if (error) *error = NdsFullscreenOverrideError::Environment;
            return false;
        }
    }
    if (!cli_fullscreen.empty() &&
        !nds_parse_fullscreen_mode(cli_fullscreen, &options->fullscreen)) {
        if (error) *error = NdsFullscreenOverrideError::CommandLine;
        return false;
    }
    return true;
}

bool nds_parse_adaptive_screens(const std::string& value, uint8_t* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "none" || normalized == "off") {
        *out = NDS_ADAPTIVE_NONE;
        return true;
    }
    if (normalized == "top") {
        *out = NDS_ADAPTIVE_TOP;
        return true;
    }
    if (normalized == "bottom") {
        *out = NDS_ADAPTIVE_BOTTOM;
        return true;
    }
    if (normalized == "both") {
        *out = NDS_ADAPTIVE_BOTH;
        return true;
    }
    return false;
}

bool nds_parse_widescreen_width(const std::string& value, uint16_t* out) {
    if (!out || value.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' ||
        (parsed != 256 && parsed != 320 &&
         parsed != 384 && parsed != 448)) {
        return false;
    }
    *out = static_cast<uint16_t>(parsed);
    return true;
}

bool nds_parse_supersampling(const std::string& value, uint8_t* out) {
    if (!out || value.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 1 || parsed > 4) return false;
    *out = static_cast<uint8_t>(parsed);
    return true;
}

bool nds_parse_internal_resolution(const std::string& value, uint8_t* out) {
    if (!out || value.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 1 || parsed > 4) return false;
    *out = static_cast<uint8_t>(parsed);
    return true;
}

bool nds_parse_texture_upscale(const std::string& value, uint8_t* out) {
    if (!out || value.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' ||
        (parsed != 1 && parsed != 2 && parsed != 4)) {
        return false;
    }
    *out = static_cast<uint8_t>(parsed);
    return true;
}

bool nds_parse_frame_interpolation(const std::string& value,
                                   NdsFrameInterpolation* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "off" || normalized == "none" || normalized == "0") {
        *out = NdsFrameInterpolation::Off;
        return true;
    }
    if (normalized == "blend") {
        *out = NdsFrameInterpolation::Blend;
        return true;
    }
    return false;
}

bool nds_parse_antialiasing(const std::string& value, uint8_t* out) {
    if (!out || value.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' ||
        (parsed != 0 && parsed != 2 && parsed != 4 && parsed != 8)) {
        return false;
    }
    *out = static_cast<uint8_t>(parsed);
    return true;
}

bool nds_parse_on_off(const std::string& value, bool* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "on" || normalized == "true" || normalized == "1" ||
        normalized == "yes") {
        *out = true;
        return true;
    }
    if (normalized == "off" || normalized == "false" || normalized == "0" ||
        normalized == "no") {
        *out = false;
        return true;
    }
    return false;
}

bool nds_parse_mouse_sensitivity(const std::string& value, uint16_t* out) {
    if (!out || value.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 10 || parsed > 400) return false;
    *out = static_cast<uint16_t>(parsed);
    return true;
}

bool nds_parse_mouse_fire_key(const std::string& value, uint16_t* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "none" || normalized == "off") {
        *out = 0;
        return true;
    }
    if (normalized == "a") *out = 1u << 0;
    else if (normalized == "b") *out = 1u << 1;
    else if (normalized == "r") *out = 1u << 8;
    else if (normalized == "l") *out = 1u << 9;
    else if (normalized == "x") *out = 1u << 10;
    else if (normalized == "y") *out = 1u << 11;
    else return false;
    return true;
}

namespace {

std::string NdsMphPrimeControlBindings::* mph_binding_member(
    const std::string& action) {
    using B = NdsMphPrimeControlBindings;
    if (action == "move-forward") return &B::move_forward;
    if (action == "move-back") return &B::move_back;
    if (action == "move-left") return &B::move_left;
    if (action == "move-right") return &B::move_right;
    if (action == "jump") return &B::jump;
    if (action == "morph-ball") return &B::morph_ball;
    if (action == "boost-zoom") return &B::boost_zoom;
    if (action == "scan-visor") return &B::scan_visor;
    if (action == "ui-left") return &B::ui_left;
    if (action == "ui-right") return &B::ui_right;
    if (action == "ui-ok") return &B::ui_ok;
    if (action == "shoot") return &B::shoot;
    if (action == "scan-shoot") return &B::scan_shoot;
    if (action == "beam") return &B::beam;
    if (action == "missile") return &B::missile;
    if (action == "weapon1") return &B::weapon1;
    if (action == "weapon2") return &B::weapon2;
    if (action == "weapon3") return &B::weapon3;
    if (action == "weapon4") return &B::weapon4;
    if (action == "weapon5") return &B::weapon5;
    if (action == "weapon6") return &B::weapon6;
    if (action == "virtual-stylus") return &B::virtual_stylus;
    if (action == "menu") return &B::menu;
    return nullptr;
}

}  // namespace

NdsMphPrimeControlBindings nds_default_mph_pad_bindings() {
    NdsMphPrimeControlBindings pad;
    // Movement comes from the left stick / D-pad natively; keep the four
    // move actions unbound so the D-pad stays a plain D-pad.
    pad.move_forward = "None";
    pad.move_back = "None";
    pad.move_left = "None";
    pad.move_right = "None";
    pad.jump = "Pad A";
    pad.morph_ball = "Pad B";
    pad.boost_zoom = "Pad RB";
    pad.scan_visor = "Pad R3";
    pad.ui_left = "Pad Left";
    pad.ui_right = "Pad Right";
    pad.ui_ok = "Pad Y";
    pad.shoot = "Pad RT";
    pad.scan_shoot = "Pad LT";
    pad.beam = "Pad LB";
    pad.missile = "Pad X";
    pad.weapon1 = "None";
    pad.weapon2 = "None";
    pad.weapon3 = "None";
    pad.weapon4 = "None";
    pad.weapon5 = "None";
    pad.weapon6 = "None";
    // Keep the virtual stylus on a keyboard binding by default; reserving a
    // controller button for it would reduce the already limited pad bindings.
    pad.virtual_stylus = "None";
    pad.menu = "Pad Start";
    return pad;
}

bool nds_set_mph_prime_binding(NdsFrontendOptions* options,
                               const std::string& action,
                               const std::string& value) {
    if (!options || value.size() >= 64u) return false;
    const auto member = mph_binding_member(action);
    if (!member) return false;
    options->mph_bindings.*member = value;
    return true;
}

bool nds_set_mph_prime_pad_binding(NdsFrontendOptions* options,
                                   const std::string& action,
                                   const std::string& value) {
    if (!options || value.size() >= 64u) return false;
    const auto member = mph_binding_member(action);
    if (!member) return false;
    options->mph_pad_bindings.*member = value;
    return true;
}

bool nds_parse_cartridge_save_type(const std::string& value,
                                   NdsCartridgeSaveType* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "none") {
        *out = NdsCartridgeSaveType::None;
        return true;
    }
    if (normalized == "eeprom-tiny" || normalized == "eeprom_tiny") {
        *out = NdsCartridgeSaveType::EepromTiny;
        return true;
    }
    if (normalized == "eeprom") {
        *out = NdsCartridgeSaveType::Eeprom;
        return true;
    }
    if (normalized == "flash") {
        *out = NdsCartridgeSaveType::Flash;
        return true;
    }
    return false;
}

bool nds_parse_ipv4(const std::string& value, uint32_t* out) {
    if (!out || value.empty()) return false;
    uint32_t octets[4] = {0, 0, 0, 0};
    size_t pos = 0;
    for (int i = 0; i < 4; ++i) {
        if (pos >= value.size() || !std::isdigit(static_cast<unsigned char>(value[pos])))
            return false;
        size_t start = pos;
        while (pos < value.size() && std::isdigit(static_cast<unsigned char>(value[pos])))
            ++pos;
        const std::string digits = value.substr(start, pos - start);
        if (digits.size() > 1 && digits[0] == '0') return false;  // no leading zeros
        if (digits.size() > 3) return false;
        const long v = std::strtol(digits.c_str(), nullptr, 10);
        if (v < 0 || v > 255) return false;
        octets[i] = static_cast<uint32_t>(v);
        if (i < 3) {
            if (pos >= value.size() || value[pos] != '.') return false;
            ++pos;
        }
    }
    if (pos != value.size()) return false;  // trailing garbage
    *out = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    return true;
}

bool nds_parse_network_backend(const std::string& value, std::string* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    // Wiimmfi M8 added "replay"; M7 wires "pcap" when the runner is built
    // with NDS_ENABLE_PCAP_BACKEND. Builds without that flag reject pcap at
    // main.cpp's bridge-construction validation.
    if (normalized == "slirp" || normalized == "pcap" ||
        normalized == "replay") {
        *out = normalized;
        return true;
    }
    return false;
}

bool nds_parse_startup_mode(const std::string& value,
                            NdsStartupMode* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "preserve" || normalized == "firmware" ||
        normalized == "default") {
        *out = NdsStartupMode::Preserve;
        return true;
    }
    if (normalized == "manual" || normalized == "menu") {
        *out = NdsStartupMode::Manual;
        return true;
    }
    if (normalized == "automatic" || normalized == "auto" ||
        normalized == "slot-1" || normalized == "slot1") {
        *out = NdsStartupMode::Automatic;
        return true;
    }
    return false;
}

bool nds_parse_boot_mode(const std::string& value, NdsBootMode* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "lle" || normalized == "firmware" ||
        normalized == "default") {
        *out = NdsBootMode::Lle;
        return true;
    }
    if (normalized == "direct") {
        *out = NdsBootMode::Direct;
        return true;
    }
    return false;
}

bool nds_parse_instance_index(const std::string& value, uint32_t* out) {
    if (!out || value.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 0 || parsed > 255) return false;
    *out = static_cast<uint32_t>(parsed);
    return true;
}

bool nds_parse_local_wireless_base_port(const std::string& value,
                                        uint16_t* out) {
    if (!out || value.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 1024 || parsed > 65520)
        return false;
    *out = static_cast<uint16_t>(parsed);
    return true;
}

const char* nds_screen_layout_name(NdsScreenLayout value) {
    return value == NdsScreenLayout::Separate ? "separate" : "stacked";
}

const char* nds_frame_interpolation_name(NdsFrameInterpolation value) {
    return value == NdsFrameInterpolation::Blend ? "blend" : "off";
}

const char* nds_fullscreen_mode_name(NdsFullscreenMode value) {
    switch (value) {
        case NdsFullscreenMode::Borderless: return "borderless";
        case NdsFullscreenMode::Exclusive: return "exclusive";
        default: return "off";
    }
}

const char* nds_startup_mode_name(NdsStartupMode value) {
    switch (value) {
        case NdsStartupMode::Manual: return "manual";
        case NdsStartupMode::Automatic: return "automatic";
        default: return "preserve";
    }
}

const char* nds_boot_mode_name(NdsBootMode value) {
    return value == NdsBootMode::Direct ? "direct" : "lle";
}

const char* nds_adaptive_screens_name(uint8_t value) {
    switch (value & NDS_ADAPTIVE_BOTH) {
        case NDS_ADAPTIVE_TOP: return "top";
        case NDS_ADAPTIVE_BOTTOM: return "bottom";
        case NDS_ADAPTIVE_BOTH: return "both";
        default: return "none";
    }
}

bool nds_load_frontend_config(const std::string& path,
                              NdsFrontendOptions* options,
                              std::string* error) {
    if (!options) return false;
    toml::table root;
    try {
        root = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        if (error) *error = e.description();
        return false;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }

    if (const toml::table* game = root["game"].as_table()) {
        if (const auto value = (*game)["sha1"].value<std::string>()) {
            const bool invalid_length = value->size() != 40u;
            const bool invalid_character = std::any_of(
                value->begin(), value->end(), [](unsigned char c) {
                    return !(c >= static_cast<unsigned char>('0') &&
                             c <= static_cast<unsigned char>('9')) &&
                           !(c >= static_cast<unsigned char>('a') &&
                             c <= static_cast<unsigned char>('f'));
                });
            if (invalid_length || invalid_character) {
                if (error) {
                    *error =
                        "game.sha1 must be exactly 40 lowercase hex digits";
                }
                return false;
            }
            options->expected_rom_sha1 = *value;
        }
    }

    if (const toml::table* system = root["system"].as_table()) {
        if (const auto value =
                (*system)["startup_mode"].value<std::string>()) {
            if (!nds_parse_startup_mode(*value, &options->startup_mode)) {
                if (error) {
                    *error =
                        "system.startup_mode must be preserve, manual, or "
                        "automatic";
                }
                return false;
            }
        }
        if (const auto value = (*system)["boot"].value<std::string>()) {
            if (!nds_parse_boot_mode(*value, &options->boot_mode)) {
                if (error) {
                    *error = "system.boot must be lle or direct";
                }
                return false;
            }
        }
        if (const auto value = (*system)["generated_firmware"].value<bool>()) {
            options->generated_firmware = *value;
        }
        if (const auto value = (*system)["freebios"].value<bool>()) {
            options->freebios = *value;
        }
        // beads-yjp.16: the firmware console nickname. Stored raw here and
        // validated once in main.cpp, together with NDS_PLAYER_NAME and
        // --player-name, so one rule set covers all three sources and the
        // error message can name the value that actually won.
        if (const auto value = (*system)["player_name"].value<std::string>()) {
            options->player_name = *value;
        }
        if (const auto value = (*system)["instance_index"].value<int64_t>()) {
            if (!nds_parse_instance_index(std::to_string(*value),
                                          &options->instance_index)) {
                if (error) {
                    *error = "system.instance_index must be 0..255";
                }
                return false;
            }
        }
    }

    if (const toml::table* display = root["display"].as_table()) {
        if (const auto value = (*display)["fullscreen"].value<std::string>()) {
            if (!nds_parse_fullscreen_mode(*value, &options->fullscreen)) {
                if (error) {
                    *error =
                        "display.fullscreen must be off, borderless, or exclusive";
                }
                return false;
            }
        }
        if (const auto value =
                (*display)["screen_layout"].value<std::string>()) {
            if (!nds_parse_screen_layout(*value,
                                         &options->screen_layout)) {
                if (error) {
                    *error =
                        "display.screen_layout must be stacked or separate";
                }
                return false;
            }
        }
        if (const auto value =
                (*display)["adaptive_widescreen"].value<std::string>()) {
            if (!nds_parse_adaptive_screens(*value,
                                            &options->adaptive_screens)) {
                if (error) {
                    *error =
                        "display.adaptive_widescreen must be none, top, "
                        "bottom, or both";
                }
                return false;
            }
        }
        if (const auto value =
                (*display)["adaptive_capability"].value<std::string>()) {
            if (!nds_parse_adaptive_screens(*value,
                                            &options->adaptive_supported)) {
                if (error) {
                    *error =
                        "display.adaptive_capability must be none, top, "
                        "bottom, or both";
                }
                return false;
            }
        }
        if (const auto value =
                (*display)["adaptive_width"].value<int64_t>()) {
            uint16_t width = 0;
            if (!nds_parse_widescreen_width(std::to_string(*value), &width)) {
                if (error) {
                    *error =
                        "display.adaptive_width must be 256, 320, 384, "
                        "or 448";
                }
                return false;
            }
            if (options->adaptive_supported & NDS_ADAPTIVE_TOP)
                options->adaptive_max_width[0] = width;
            if (options->adaptive_supported & NDS_ADAPTIVE_BOTTOM)
                options->adaptive_max_width[1] = width;
        } else {
            if (options->adaptive_supported & NDS_ADAPTIVE_TOP)
                options->adaptive_max_width[0] = 448;
            if (options->adaptive_supported & NDS_ADAPTIVE_BOTTOM)
                options->adaptive_max_width[1] = 448;
        }
        if (const auto value =
                (*display)["adaptive_skybox_fill"].value<bool>()) {
            options->adaptive_skybox_fill = *value;
        }
        if (const auto value =
                (*display)["adaptive_hud_anchor"].value<bool>()) {
            options->adaptive_hud_anchor = *value;
        }
        if (const auto value =
                (*display)["adaptive_center_native"].value<bool>()) {
            options->adaptive_center_native = *value;
        }
        if (const auto value =
                (*display)["adaptive_guest_culling"].value<bool>()) {
            options->adaptive_guest_culling = *value;
        }
        if (const auto value =
                (*display)["adaptive_center_max_polygons"].value<int64_t>()) {
            if (*value < 0 || *value > 2048) {
                if (error) {
                    *error =
                        "display.adaptive_center_max_polygons must be from "
                        "0 through 2048";
                }
                return false;
            }
            options->adaptive_center_max_polygons =
                static_cast<uint32_t>(*value);
        }
        if (const auto value =
                (*display)["adaptive_hud_center_width"].value<int64_t>()) {
            if (*value < 8 || *value > 256 || (*value & 7) != 0) {
                if (error) {
                    *error =
                        "display.adaptive_hud_center_width must be a "
                        "multiple of 8 from 8 through 256";
                }
                return false;
            }
            options->adaptive_hud_center_width =
                static_cast<uint16_t>(*value);
        }
        if (options->adaptive_supported != NDS_ADAPTIVE_NONE &&
            options->expected_rom_sha1.empty()) {
            if (error) {
                *error =
                    "display.adaptive_capability requires an exact game.sha1";
            }
            return false;
        }
        if (const auto value =
                (*display)["supersampling"].value<int64_t>()) {
            if (!nds_parse_supersampling(std::to_string(*value),
                                         &options->supersampling)) {
                if (error) *error = "display.supersampling must be 1..4";
                return false;
            }
        }
        if (const auto value =
                (*display)["antialiasing"].value<int64_t>()) {
            if (!nds_parse_antialiasing(std::to_string(*value),
                                        &options->antialiasing)) {
                if (error) {
                    *error = "display.antialiasing must be 0, 2, 4, or 8";
                }
                return false;
            }
        }
        if (const auto value =
                (*display)["texture_upscale"].value<int64_t>()) {
            if (!nds_parse_texture_upscale(std::to_string(*value),
                                           &options->texture_upscale)) {
                if (error) *error = "display.texture_upscale must be 1, 2, or 4";
                return false;
            }
        }
        if (const auto value =
                (*display)["frame_interpolation"].value<std::string>()) {
            if (!nds_parse_frame_interpolation(
                    *value, &options->frame_interpolation)) {
                if (error) {
                    *error = "display.frame_interpolation must be off or blend";
                }
                return false;
            }
        }
        if (const auto value =
                (*display)["internal_resolution"].value<int64_t>()) {
            if (!nds_parse_internal_resolution(
                    std::to_string(*value), &options->internal_resolution)) {
                if (error) {
                    *error = "display.internal_resolution must be 1..4";
                }
                return false;
            }
        }
    }

    if (const toml::table* cartridge = root["cartridge"].as_table()) {
        if (const auto value =
                (*cartridge)["save_type"].value<std::string>()) {
            if (!nds_parse_cartridge_save_type(
                    *value, &options->cartridge_save.type)) {
                if (error) {
                    *error =
                        "cartridge.save_type must be none, eeprom-tiny, "
                        "eeprom, or flash";
                }
                return false;
            }
        }
        if (const auto value = (*cartridge)["save_size"].value<int64_t>()) {
            if (*value < 0 || *value > (64ll * 1024ll * 1024ll)) {
                if (error) {
                    *error =
                        "cartridge.save_size must be between 0 and 67108864";
                }
                return false;
            }
            options->cartridge_save.size = static_cast<uint32_t>(*value);
        }
        const uint32_t size = options->cartridge_save.size;
        const bool power_of_two = size != 0u && (size & (size - 1u)) == 0u;
        if (options->cartridge_save.type == NdsCartridgeSaveType::None) {
            if (size != 0u) {
                if (error) {
                    *error =
                        "cartridge.save_size must be 0 when save_type is none";
                }
                return false;
            }
        } else if (!power_of_two) {
            if (error) {
                *error =
                    "cartridge.save_size must be a nonzero power of two";
            }
            return false;
        } else if (options->cartridge_save.type ==
                       NdsCartridgeSaveType::EepromTiny &&
                   size != 512u) {
            if (error) {
                *error =
                    "cartridge.save_size must be 512 for eeprom-tiny";
            }
            return false;
        }
    }

    if (const toml::table* network = root["network"].as_table()) {
        if (const auto value = (*network)["enabled"].value<bool>()) {
            options->network.enabled = *value;
        }
        if (const auto value = (*network)["backend"].value<std::string>()) {
            if (!nds_parse_network_backend(*value, &options->network.backend)) {
                if (error) *error = "network.backend must be slirp, replay, or pcap";
                return false;
            }
        }
        if (const auto value = (*network)["pcap_adapter"].value<std::string>()) {
            options->network.pcap_adapter = *value;
        }
        // Wiimmfi M8: capture/replay knobs, TOML mirror of the
        // --net-capture-* CLI flags (see frontend.h's NdsNetworkOptions).
        if (const toml::table* capture = (*network)["capture"].as_table()) {
            if (const auto value = (*capture)["out"].value<std::string>()) {
                options->network.capture_out = *value;
            }
            if (const auto value = (*capture)["in"].value<std::string>()) {
                options->network.capture_in = *value;
            }
            if (const auto value = (*capture)["raw"].value<bool>()) {
                options->network.capture_raw = *value;
            }
            if (const auto value = (*capture)["no_pcap"].value<bool>()) {
                options->network.capture_no_pcap = *value;
            }
            if (const auto value = (*capture)["scenario"].value<std::string>()) {
                options->network.capture_scenario = *value;
            }
        }
        if (const toml::table* wfc = (*network)["wfc"].as_table()) {
            if (const auto value = (*wfc)["enabled"].value<bool>()) {
                options->network.wfc_enabled = *value;
            }
            if (const auto value = (*wfc)["provider"].value<std::string>()) {
                options->network.wfc_provider.name = *value;
            }
            if (const auto value =
                    (*wfc)["clear_crt_errno_addr"].value<int64_t>()) {
                if (*value < 0 || *value > 0xFFFFFFFFll) {
                    if (error) {
                        *error = "network.wfc.clear_crt_errno_addr must be "
                                 "a 32-bit guest address";
                    }
                    return false;
                }
                options->network.wfc_clear_crt_errno_addr =
                    static_cast<uint32_t>(*value);
            }
            if (const auto value = (*wfc)["dns_server"].value<std::string>()) {
                uint32_t probe = 0;
                if (!nds_parse_ipv4(*value, &probe)) {
                    if (error) {
                        *error =
                            "network.wfc.dns_server must be a dotted-quad "
                            "IPv4 address";
                    }
                    return false;
                }
                options->network.wfc_provider.dns_server = *value;
            }
        }
    }
    if (const toml::table* local =
            root["local_wireless"].as_table()) {
        if (const auto value = (*local)["enabled"].value<bool>()) {
            options->local_wireless.enabled = *value;
        }
        if (const auto value = (*local)["base_port"].value<int64_t>()) {
            if (!nds_parse_local_wireless_base_port(
                    std::to_string(*value),
                    &options->local_wireless.base_port)) {
                if (error) {
                    *error =
                        "local_wireless.base_port must be 1024..65520";
                }
                return false;
            }
        }
    }

    if (const toml::table* controls = root["controls"].as_table()) {
        if (const toml::table* prime = (*controls)["prime"].as_table()) {
            if (const auto value = (*prime)["enabled"].value<bool>()) {
                options->mph_prime_controls = *value;
            }
            if (const auto value =
                    (*prime)["unified_window_focus"].value<bool>()) {
                options->mph_prime_unified_window_focus = *value;
            }
            if (const auto value =
                    (*prime)["virtual_stylus_sensitivity"].value<int64_t>()) {
                if (!nds_parse_mouse_sensitivity(std::to_string(*value),
                                                 &options->mph_virtual_stylus_sensitivity)) {
                    if (error) {
                        *error =
                            "controls.prime.virtual_stylus_sensitivity "
                            "must be 10..400";
                    }
                    return false;
                }
            }
            if (const auto value =
                    (*prime)["pad_aim_sensitivity"].value<int64_t>()) {
                if (!nds_parse_mouse_sensitivity(std::to_string(*value),
                                                 &options->mph_pad_aim_sensitivity)) {
                    if (error) {
                        *error =
                            "controls.prime.pad_aim_sensitivity "
                            "must be 10..400";
                    }
                    return false;
                }
            }
            if (const toml::table* bindings = (*prime)["bindings"].as_table()) {
                for (const auto& item : *bindings) {
                    const auto value = item.second.value<std::string>();
                    if (!value) continue;
                    if (!nds_set_mph_prime_binding(
                            options, std::string(item.first.str()), *value)) {
                        if (error) {
                            *error =
                                "controls.prime.bindings contains an "
                                "unknown binding name or too-long value";
                        }
                        return false;
                    }
                }
            }
            if (const toml::table* pad_bindings =
                    (*prime)["pad_bindings"].as_table()) {
                for (const auto& item : *pad_bindings) {
                    const auto value = item.second.value<std::string>();
                    if (!value) continue;
                    if (!nds_set_mph_prime_pad_binding(
                            options, std::string(item.first.str()), *value)) {
                        if (error) {
                            *error =
                                "controls.prime.pad_bindings contains an "
                                "unknown binding name or too-long value";
                        }
                        return false;
                    }
                }
            }
        }
    }
    return true;
}
