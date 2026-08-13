#include "net/wfc_provider.h"

#include <cstring>

namespace {

bool require(bool condition) {
    return condition;
}

bool provider_is(const char* name, const char* dns) {
    const NdsWfcProviderInfo* info = nds_wfc_provider_lookup(name);
    return info && std::strcmp(info->dns_server, dns) == 0;
}

}  // namespace

int main() {
    if (!require(provider_is("kaeru", "178.62.43.212")) ||
        !require(provider_is("wiimmfi", "178.62.43.212")) ||
        !require(provider_is("wiimmfi-direct", "95.217.77.181")) ||
        !require(provider_is("WIIMMFI", "178.62.43.212")) ||
        !require(nds_wfc_provider_lookup("unknown") == nullptr)) {
        return 1;
    }
    return 0;
}
