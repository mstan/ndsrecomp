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

bool nds_parse_supersampling(const std::string& value, uint8_t* out) {
    if (!out || value.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 1 || parsed > 4) return false;
    *out = static_cast<uint8_t>(parsed);
    return true;
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

bool nds_set_mph_prime_binding(NdsFrontendOptions* options,
                               const std::string& action,
                               const std::string& value) {
    if (!options || value.size() >= 64u) return false;
    NdsMphPrimeControlBindings& b = options->mph_bindings;
    if (action == "move-forward") b.move_forward = value;
    else if (action == "move-back") b.move_back = value;
    else if (action == "move-left") b.move_left = value;
    else if (action == "move-right") b.move_right = value;
    else if (action == "jump") b.jump = value;
    else if (action == "morph-ball") b.morph_ball = value;
    else if (action == "boost-zoom") b.boost_zoom = value;
    else if (action == "scan-visor") b.scan_visor = value;
    else if (action == "ui-left") b.ui_left = value;
    else if (action == "ui-right") b.ui_right = value;
    else if (action == "ui-ok") b.ui_ok = value;
    else if (action == "shoot") b.shoot = value;
    else if (action == "scan-shoot") b.scan_shoot = value;
    else if (action == "beam") b.beam = value;
    else if (action == "missile") b.missile = value;
    else if (action == "weapon1") b.weapon1 = value;
    else if (action == "weapon2") b.weapon2 = value;
    else if (action == "weapon3") b.weapon3 = value;
    else if (action == "weapon4") b.weapon4 = value;
    else if (action == "weapon5") b.weapon5 = value;
    else if (action == "weapon6") b.weapon6 = value;
    else if (action == "virtual-stylus") b.virtual_stylus = value;
    else if (action == "menu") b.menu = value;
    else return false;
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

const char* nds_startup_mode_name(NdsStartupMode value) {
    switch (value) {
        case NdsStartupMode::Manual: return "manual";
        case NdsStartupMode::Automatic: return "automatic";
        default: return "preserve";
    }
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
            if (*value < 256 || *value > 448 || (*value & 1) != 0) {
                if (error) {
                    *error =
                        "display.adaptive_width must be an even value from "
                        "256 through 448";
                }
                return false;
            }
            const uint16_t width = static_cast<uint16_t>(*value);
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
        }
    }
    return true;
}
