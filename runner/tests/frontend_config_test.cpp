#include "frontend.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

bool require(bool condition) {
    return condition;
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

    NdsStartupMode startup = NdsStartupMode::Preserve;
    if (!require(nds_parse_startup_mode("automatic", &startup)) ||
        !require(startup == NdsStartupMode::Automatic) ||
        !require(nds_parse_startup_mode("menu", &startup)) ||
        !require(startup == NdsStartupMode::Manual) ||
        !require(nds_parse_startup_mode("firmware", &startup)) ||
        !require(startup == NdsStartupMode::Preserve) ||
        !require(!nds_parse_startup_mode("fast", &startup)))
        return 4;

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "ndsrecomp_frontend_config_test.toml";
    {
        std::ofstream file(path);
        file << "[system]\n"
                "startup_mode = \"automatic\"\n"
                "[display]\n"
                "screen_layout = \"separate\"\n"
                "adaptive_widescreen = \"bottom\"\n"
                "supersampling = 3\n"
                "antialiasing = 4\n";
    }
    NdsFrontendOptions options{};
    std::string error;
    if (!require(nds_load_frontend_config(path.string(), &options, &error)) ||
        !require(options.screen_layout == NdsScreenLayout::Separate) ||
        !require(options.startup_mode == NdsStartupMode::Automatic) ||
        !require(options.adaptive_screens == NDS_ADAPTIVE_BOTTOM) ||
        !require(options.supersampling == 3) ||
        !require(options.antialiasing == 4))
        return 5;

    {
        std::ofstream file(path);
        file << "[display]\n"
                "screen_layout = \"invalid\"\n";
    }
    if (!require(
            !nds_load_frontend_config(path.string(), &options, &error)))
        return 6;
    std::filesystem::remove(path);
    return 0;
}
