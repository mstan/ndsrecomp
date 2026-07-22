#include "config.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

bool write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    return stream.good();
}

bool parses(const std::filesystem::path& path, const std::string& text) {
    if (!write_text(path, text)) return false;
    ndsrecomp::HleProfileManifest manifest;
    return ndsrecomp::load_hle_profile_manifest(path.string(), manifest);
}

bool loads(const std::filesystem::path& path, const std::string& text,
           ndsrecomp::HleProfileManifest& manifest) {
    return write_text(path, text) &&
           ndsrecomp::load_hle_profile_manifest(path.string(), manifest);
}

}  // namespace

int main() {
    const auto path = std::filesystem::temp_directory_path() /
                      "ndsrecomp_hle_manifest_test.toml";
    const std::string valid =
        "version = 1\n"
        "[program]\n"
        "bank = \"sm64ds_arm9\"\n"
        "sha1 = \"16e6c9168e34f267cb427bacb388e4783dadc584\"\n"
        "[[routine]]\n"
        "id = \"sm64ds.mul_vec3_mat4x3\"\n"
        "address = 0x02052858\n"
        "end_address = 0x02052914\n"
        "mode = \"arm\"\n";

    int failures = 0;
    if (!parses(path, valid)) {
        std::fprintf(stderr, "valid manifest was rejected\n");
        ++failures;
    }
    ndsrecomp::HleProfileManifest replacement_manifest;
    std::string replacement = valid;
    replacement += "handler = \"sm64ds_mul_vec3_mat4x3\"\n";
    if (!loads(path, replacement, replacement_manifest)) {
        std::fprintf(stderr, "valid replacement manifest was rejected\n");
        ++failures;
    } else if (replacement_manifest.version != 1u ||
               replacement_manifest.routines.size() != 1u ||
               replacement_manifest.routines[0].handler !=
                   "sm64ds_mul_vec3_mat4x3") {
        std::fprintf(stderr, "replacement handler was not preserved\n");
        ++failures;
    }
    std::string unsafe_handler = replacement;
    const std::string handler_symbol = "sm64ds_mul_vec3_mat4x3";
    unsafe_handler.replace(unsafe_handler.rfind(handler_symbol),
                           handler_symbol.size(), "sm64ds_handler();");
    if (parses(path, unsafe_handler)) {
        std::fprintf(stderr, "unsafe replacement handler was accepted\n");
        ++failures;
    }
    std::string keyword_handler = replacement;
    keyword_handler.replace(keyword_handler.rfind(handler_symbol),
                            handler_symbol.size(), "for");
    if (parses(path, keyword_handler)) {
        std::fprintf(stderr, "C keyword replacement handler was accepted\n");
        ++failures;
    }
    std::string unsafe_bank = valid;
    unsafe_bank.replace(unsafe_bank.find("sm64ds_arm9"), 11u,
                        "sm64ds-arm9");
    if (parses(path, unsafe_bank)) {
        std::fprintf(stderr, "non-C-identifier bank was accepted\n");
        ++failures;
    }
    if (parses(path, valid + "unknown = 1\n")) {
        std::fprintf(stderr, "unknown routine key was accepted\n");
        ++failures;
    }
    if (parses(path, valid +
        "[[routine]]\n"
        "id = \"sm64ds.mul_vec3_mat4x3\"\n"
        "address = 0x02052858\n"
        "end_address = 0x02052914\n"
        "mode = \"arm\"\n")) {
        std::fprintf(stderr, "duplicate routine id was accepted\n");
        ++failures;
    }
    if (parses(path, valid +
        "[[routine]]\n"
        "id = \"sm64ds.same_selector\"\n"
        "address = 0x02052858\n"
        "end_address = 0x02052914\n"
        "mode = \"arm\"\n")) {
        std::fprintf(stderr, "duplicate routine selector was accepted\n");
        ++failures;
    }
    std::string bad_sha = valid;
    bad_sha.replace(bad_sha.find("16e6c9"), 6u, "nothex");
    if (parses(path, bad_sha)) {
        std::fprintf(stderr, "malformed program SHA-1 was accepted\n");
        ++failures;
    }
    std::string bad_range = valid;
    bad_range.replace(bad_range.find("0x02052914"), 10u, "0x02052858");
    if (parses(path, bad_range)) {
        std::fprintf(stderr, "empty routine range was accepted\n");
        ++failures;
    }
    std::string negative = valid;
    negative.replace(negative.find("0x02052858"), 10u, "-1");
    if (parses(path, negative)) {
        std::fprintf(stderr, "negative routine address was accepted\n");
        ++failures;
    }
    std::string overflow = valid;
    overflow.replace(overflow.find("0x02052858"), 10u, "0x100000000");
    if (parses(path, overflow)) {
        std::fprintf(stderr, "overflowing routine address was accepted\n");
        ++failures;
    }
    std::string string_address = valid;
    string_address.replace(string_address.find("0x02052858"), 10u,
                           "\"0x02052858\"");
    if (parses(path, string_address)) {
        std::fprintf(stderr, "string routine address was accepted\n");
        ++failures;
    }

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return failures == 0 ? 0 : 1;
}
