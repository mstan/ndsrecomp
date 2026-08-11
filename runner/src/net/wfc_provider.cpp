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
    // Verified default (task spec): Kaeru WFC, DNS-only, no-ROM-patch,
    // primary and secondary both this same address.
    {"kaeru", "178.62.43.212"},
    // Listed for completeness/selectability, NOT the default -- see the
    // header comment.
    {"wiimmfi", "95.217.77.181"},
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
