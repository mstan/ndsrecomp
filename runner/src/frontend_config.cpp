#include "frontend.h"

#include <algorithm>
#include <cctype>
#include <exception>

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
    }
    return true;
}
