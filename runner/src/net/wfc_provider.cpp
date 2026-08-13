// wfc_provider.cpp -- see wfc_provider.h for the design note.

#include "wfc_provider.h"

#include <cctype>

namespace {

bool iequals(const char* a, const char* b) {
    while (*a && *b) {
        const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(*a)));
        const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(*b)));
        if (ca != cb) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

}  // namespace

const NdsWfcProviderInfo kNdsWfcProviders[] = {
    // Verified DS route: Kaeru WFC, DNS-only, no-ROM-patch, primary and
    // secondary both this same address. This fronts the Wiimmfi ecosystem
    // for stock DS/DSi clients.
    {"kaeru", "178.62.43.212"},
    // User-facing Wiimmfi provider name: DS guides often call the Kaeru
    // route "Wiimmfi", and live validation shows the raw Wiimmfi DNS path
    // reaches DNS/TCP but returns Error Code 20100 for stock MKDS.
    {"wiimmfi", "178.62.43.212"},
    // Raw official Wiimmfi DNS endpoint. Kept for diagnostics and future
    // patched-client compatibility; not the stock-DS route.
    {"wiimmfi-direct", "95.217.77.181"},
    // Guest-visible Slirp host alias. Packets sent here are translated by
    // libslirp to host loopback, so the local dwc-docker/wfc_dns.py stack can
    // stay bound to 127.0.0.1 without handing the guest its own loopback.
    {"local", "10.64.0.1"},
    {"local-oracle", "10.64.0.1"},
};
const size_t kNdsWfcProviderCount =
    sizeof(kNdsWfcProviders) / sizeof(kNdsWfcProviders[0]);

const NdsWfcProviderInfo* nds_wfc_provider_lookup(const char* name) {
    if (!name) return nullptr;
    for (size_t i = 0; i < kNdsWfcProviderCount; ++i) {
        if (iequals(kNdsWfcProviders[i].name, name)) return &kNdsWfcProviders[i];
    }
    return nullptr;
}
