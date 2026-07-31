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

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "ndsrecomp_frontend_config_test.toml";
    {
        std::ofstream file(path);
        file << "[display]\n"
                "screen_layout = \"separate\"\n"
                "adaptive_widescreen = \"bottom\"\n";
    }
    NdsFrontendOptions options{};
    std::string error;
    if (!require(nds_load_frontend_config(path.string(), &options, &error)) ||
        !require(options.screen_layout == NdsScreenLayout::Separate) ||
        !require(options.adaptive_screens == NDS_ADAPTIVE_BOTTOM))
        return 3;

    {
        std::ofstream file(path);
        file << "[display]\n"
                "screen_layout = \"invalid\"\n";
    }
    if (!require(
            !nds_load_frontend_config(path.string(), &options, &error)))
        return 4;
    std::filesystem::remove(path);
    return 0;
}
