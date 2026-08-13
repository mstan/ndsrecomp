// wfc_provider.h -- built-in named WFC DNS providers (Wiimmfi M4).
//
// Configuration, not compiled-in constants: `game.toml`'s
// `[network.wfc] provider = "..."` / the `--wfc-provider` CLI flag select
// a NAME from this table; the resulting DNS address flows through
// runner/src/main.cpp -> nds_wifi_configure_network()
// (runner/src/wifi_net.h) -> the Net_Slirp constructor's
// `nameserver_ipv4_host_order` override
// (runner/vendor/melonds/patches/0006-net-slirp-configurable-nameserver.patch)
// -- nowhere is a provider address hardcoded into the network path itself,
// only into this one small, documented table, and any entry can be
// overridden per-endpoint via an explicit `dns_server` (see
// NdsWfcProvider in runner/src/frontend.h).
//
// Default provider is "kaeru" (178.62.43.212): the DNS-only, no-ROM-patch
// DS/DSi entry point into the Wiimmfi ecosystem, verified appropriate for a
// stock DS game. "wiimmfi" intentionally aliases that same DS-compatible
// route because that is what DS setup guides often call the Wiimmfi option.
// "wiimmfi-direct" remains available for diagnostics against Wiimmfi's raw
// official DNS endpoint, but that path is expected to require client-side
// patch support and is not the no-ROM-patch DS default.

#pragma once

#include <cstddef>

struct NdsWfcProviderInfo {
    const char* name;
    const char* dns_server;  // dotted-quad IPv4, primary == secondary for
                              // every currently-listed provider
};

// Case-insensitive lookup by name. Returns nullptr for an unrecognized
// name -- callers (main.cpp CLI/config validation) must reject rather
// than silently falling back to some default.
const NdsWfcProviderInfo* nds_wfc_provider_lookup(const char* name);

// Every built-in provider, for --help / error-message enumeration.
extern const NdsWfcProviderInfo kNdsWfcProviders[];
extern const size_t kNdsWfcProviderCount;
