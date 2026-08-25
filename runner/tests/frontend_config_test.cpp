#include "frontend.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

bool require(bool condition) {
    return condition;
}

bool set_fullscreen_environment(const char* value) {
#if defined(_WIN32)
    return _putenv_s("NDS_FULLSCREEN", value ? value : "") == 0;
#else
    return value ? setenv("NDS_FULLSCREEN", value, 1) == 0
                 : unsetenv("NDS_FULLSCREEN") == 0;
#endif
}

}  // namespace

int main() {
    NdsScreenLayout layout = NdsScreenLayout::Stacked;
    if (!require(nds_parse_screen_layout("separate", &layout)) ||
        !require(layout == NdsScreenLayout::Separate) ||
        !require(nds_parse_screen_layout("combined", &layout)) ||
        !require(layout == NdsScreenLayout::Stacked) ||
        !require(!nds_parse_screen_layout("sideways", &layout)))
        return 1;

    NdsFullscreenMode fullscreen = NdsFullscreenMode::Off;
    if (!require(nds_parse_fullscreen_mode("off", &fullscreen)) ||
        !require(fullscreen == NdsFullscreenMode::Off) ||
        !require(nds_parse_fullscreen_mode("Borderless", &fullscreen)) ||
        !require(fullscreen == NdsFullscreenMode::Borderless) ||
        !require(nds_parse_fullscreen_mode("exclusive", &fullscreen)) ||
        !require(fullscreen == NdsFullscreenMode::Exclusive) ||
        !require(!nds_parse_fullscreen_mode("windowed", &fullscreen)))
        return 18;

    uint8_t adaptive = NDS_ADAPTIVE_NONE;
    if (!require(nds_parse_adaptive_screens("top", &adaptive)) ||
        !require(adaptive == NDS_ADAPTIVE_TOP) ||
        !require(nds_parse_adaptive_screens("BOTH", &adaptive)) ||
        !require(adaptive == NDS_ADAPTIVE_BOTH) ||
        !require(!nds_parse_adaptive_screens("automatic", &adaptive)))
        return 2;

    uint8_t quality = 0;
    if (!require(nds_parse_supersampling("4", &quality)) ||
        !require(quality == 4) ||
        !require(!nds_parse_supersampling("0", &quality)) ||
        !require(nds_parse_antialiasing("8", &quality)) ||
        !require(quality == 8) ||
        !require(!nds_parse_antialiasing("3", &quality)))
        return 3;

    bool toggle = false;
    uint16_t mouse_value = 0;
    uint16_t local_wireless_port = 0;
    if (!require(nds_parse_on_off("on", &toggle)) || !require(toggle) ||
        !require(nds_parse_on_off("false", &toggle)) || require(toggle) ||
        !require(!nds_parse_on_off("maybe", &toggle)) ||
        !require(nds_parse_mouse_sensitivity("125", &mouse_value)) ||
        !require(mouse_value == 125) ||
        !require(!nds_parse_mouse_sensitivity("9", &mouse_value)) ||
        !require(!nds_parse_mouse_sensitivity("401", &mouse_value)) ||
        !require(nds_parse_mouse_fire_key("L", &mouse_value)) ||
        !require(mouse_value == (1u << 9)) ||
        !require(nds_parse_mouse_fire_key("none", &mouse_value)) ||
        !require(mouse_value == 0) ||
        !require(!nds_parse_mouse_fire_key("start", &mouse_value)) ||
        !require(nds_parse_local_wireless_base_port(
            "26710", &local_wireless_port)) ||
        !require(local_wireless_port == 26710) ||
        !require(!nds_parse_local_wireless_base_port(
            "1023", &local_wireless_port)) ||
        !require(!nds_parse_local_wireless_base_port(
            "65521", &local_wireless_port)))
        return 13;

    NdsStartupMode startup = NdsStartupMode::Preserve;
    if (!require(nds_parse_startup_mode("automatic", &startup)) ||
        !require(startup == NdsStartupMode::Automatic) ||
        !require(nds_parse_startup_mode("menu", &startup)) ||
        !require(startup == NdsStartupMode::Manual) ||
        !require(nds_parse_startup_mode("firmware", &startup)) ||
        !require(startup == NdsStartupMode::Preserve) ||
        !require(!nds_parse_startup_mode("fast", &startup)))
        return 4;

    {
        NdsFrontendOptions defaults{};
        if (!require(defaults.mph_virtual_stylus_sensitivity == 20) ||
            !require(defaults.fullscreen == NdsFullscreenMode::Off))
            return 17;
    }

    NdsCartridgeSaveType save_type = NdsCartridgeSaveType::Eeprom;
    if (!require(nds_parse_cartridge_save_type("flash", &save_type)) ||
        !require(save_type == NdsCartridgeSaveType::Flash) ||
        !require(nds_parse_cartridge_save_type("eeprom-tiny", &save_type)) ||
        !require(save_type == NdsCartridgeSaveType::EepromTiny) ||
        !require(!nds_parse_cartridge_save_type("sram", &save_type)))
        return 5;

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "ndsrecomp_frontend_config_test.toml";
    {
        std::ofstream file(path);
        file << "[game]\n"
                "sha1 = \"90164d1ac127ee5f9815ea4ae7de798c7b5fc629\"\n"
                "[system]\n"
                "startup_mode = \"automatic\"\n"
                "[display]\n"
                "screen_layout = \"separate\"\n"
                "fullscreen = \"borderless\"\n"
                "adaptive_widescreen = \"top\"\n"
                "adaptive_capability = \"top\"\n"
                "adaptive_width = 448\n"
                "adaptive_skybox_fill = true\n"
                "adaptive_hud_anchor = true\n"
                "adaptive_center_native = true\n"
                "adaptive_hud_center_width = 128\n"
                "supersampling = 3\n"
                "antialiasing = 4\n"
                "[cartridge]\n"
                "save_type = \"flash\"\n"
                "save_size = 262144\n"
                "[local_wireless]\n"
                "enabled = true\n"
                "base_port = 27000\n"
                "[controls.prime]\n"
                "enabled = true\n"
                "unified_window_focus = true\n"
                "virtual_stylus_sensitivity = 125\n"
                "[controls.prime.bindings]\n"
                "move-forward = \"Up\"\n"
                "shoot = \"Mouse Left\"\n";
    }
    NdsFrontendOptions options{};
    std::string error;
    if (!require(nds_load_frontend_config(path.string(), &options, &error)) ||
        !require(options.expected_rom_sha1 ==
                 "90164d1ac127ee5f9815ea4ae7de798c7b5fc629") ||
        !require(options.screen_layout == NdsScreenLayout::Separate) ||
        !require(options.fullscreen == NdsFullscreenMode::Borderless) ||
        !require(options.startup_mode == NdsStartupMode::Automatic) ||
        !require(options.adaptive_screens == NDS_ADAPTIVE_TOP) ||
        !require(options.adaptive_supported == NDS_ADAPTIVE_TOP) ||
        !require(options.adaptive_max_width[0] == 448) ||
        !require(options.adaptive_max_width[1] == 256) ||
        !require(options.adaptive_skybox_fill) ||
        !require(options.adaptive_hud_anchor) ||
        !require(options.adaptive_center_native) ||
        !require(options.adaptive_hud_center_width == 128) ||
        !require(options.supersampling == 3) ||
        !require(options.antialiasing == 4) ||
        !require(options.cartridge_save.type ==
                 NdsCartridgeSaveType::Flash) ||
        !require(options.cartridge_save.size == 262144) ||
        !require(options.local_wireless.enabled) ||
        !require(options.local_wireless.base_port == 27000) ||
        !require(options.mph_prime_controls) ||
        !require(options.mph_prime_unified_window_focus) ||
        !require(options.mph_virtual_stylus_sensitivity == 125) ||
        !require(options.mph_bindings.move_forward == "Up") ||
        !require(options.mph_bindings.shoot == "Mouse Left"))
        return 6;

    {
        std::ofstream file(path);
        file << "[network]\n"
                "enabled = true\n"
                "backend = \"pcap\"\n"
                "pcap_adapter = \"Ethernet\"\n";
    }
    options = {};
    if (!require(nds_load_frontend_config(path.string(), &options, &error)) ||
        !require(options.network.enabled) ||
        !require(options.network.backend == "pcap") ||
        !require(options.network.pcap_adapter == "Ethernet"))
        return 14;

    {
        std::ofstream file(path);
        file << "[display]\n"
                "screen_layout = \"invalid\"\n";
    }
    if (!require(
            !nds_load_frontend_config(path.string(), &options, &error)))
        return 7;

    {
        std::ofstream file(path);
        file << "[display]\n"
                "fullscreen = \"invalid\"\n";
    }
    options = {};
    if (!require(!nds_load_frontend_config(path.string(), &options, &error)))
        return 19;

    // Exercise the production source-application helper: TOML has already
    // populated options, then NDS_FULLSCREEN, then --fullscreen.
    {
        std::ofstream file(path);
        file << "[display]\n"
                "fullscreen = \"borderless\"\n";
    }
    options = {};
    NdsFullscreenOverrideError fullscreen_error =
        NdsFullscreenOverrideError::None;
    if (!require(nds_load_frontend_config(path.string(), &options, &error)) ||
        !require(options.fullscreen == NdsFullscreenMode::Borderless) ||
        !require(set_fullscreen_environment("exclusive")) ||
        !require(nds_apply_fullscreen_overrides(
            &options, "off", &fullscreen_error)) ||
        !require(fullscreen_error == NdsFullscreenOverrideError::None) ||
        !require(options.fullscreen == NdsFullscreenMode::Off))
        return 20;
    if (!require(set_fullscreen_environment(nullptr))) return 21;

    options = {};
    fullscreen_error = NdsFullscreenOverrideError::None;
    if (!require(nds_apply_fullscreen_overrides(
            &options, "", &fullscreen_error)) ||
        !require(options.fullscreen == NdsFullscreenMode::Off) ||
        !require(fullscreen_error == NdsFullscreenOverrideError::None))
        return 22;

    options = {};
    if (!require(set_fullscreen_environment("invalid")) ||
        !require(!nds_apply_fullscreen_overrides(
            &options, "", &fullscreen_error)) ||
        !require(fullscreen_error == NdsFullscreenOverrideError::Environment) ||
        !require(set_fullscreen_environment(nullptr)))
        return 23;

    options = {};
    if (!require(!nds_apply_fullscreen_overrides(
            &options, "invalid", &fullscreen_error)) ||
        !require(fullscreen_error == NdsFullscreenOverrideError::CommandLine))
        return 24;

    {
        std::ofstream file(path);
        file << "[cartridge]\n"
                "save_type = \"flash\"\n"
                "save_size = 1000\n";
    }
    if (!require(
            !nds_load_frontend_config(path.string(), &options, &error)))
        return 8;

    {
        std::ofstream file(path);
        file << "[game]\n"
                "sha1 = \"90164D1AC127EE5F9815EA4AE7DE798C7B5FC629\"\n";
    }
    if (!require(
            !nds_load_frontend_config(path.string(), &options, &error)))
        return 9;

    {
        std::ofstream file(path);
        file << "[display]\n"
                "adaptive_capability = \"top\"\n";
    }
    options = {};
    if (!require(
            !nds_load_frontend_config(path.string(), &options, &error)))
        return 10;

    {
        std::ofstream file(path);
        file << "[game]\n"
                "sha1 = \"90164d1ac127ee5f9815ea4ae7de798c7b5fc629\"\n"
                "[display]\n"
                "adaptive_capability = \"top\"\n"
                "adaptive_width = 447\n";
    }
    options = {};
    if (!require(
            !nds_load_frontend_config(path.string(), &options, &error)))
        return 11;

    {
        std::ofstream file(path);
        file << "[display]\n"
                "adaptive_hud_center_width = 70\n";
    }
    options = {};
    if (!require(
            !nds_load_frontend_config(path.string(), &options, &error)))
        return 12;
    {
        std::ofstream file(path);
        file << "[controls.prime]\n"
                "virtual_stylus_sensitivity = 401\n";
    }
    options = {};
    if (!require(
            !nds_load_frontend_config(path.string(), &options, &error)))
        return 15;

    {
        std::ofstream file(path);
        file << "[controls.prime.bindings]\n"
                "bad-action = \"W\"\n";
    }
    options = {};
    if (!require(
            !nds_load_frontend_config(path.string(), &options, &error)))
        return 16;
    std::filesystem::remove(path);
    return 0;
}
