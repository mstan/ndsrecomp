// Pins the in-memory firmware user-settings patches (beads-yjp.16 and the
// two patches that predate it, hoisted out of main.cpp for exactly this
// reason). The properties that matter and that nothing else checks:
//
//   * BOTH redundant user-settings copies are patched. The console picks the
//     copy with the higher Version counter, so patching one is a coin flip.
//   * Each copy's CRC16 at +0x72 is resealed with the 32-BIT-accumulator
//     firmware CRC. A uint16_t accumulator computes a different, wrong value
//     on most inputs (beads-yjp.15) and the guest would reject the block.
//   * A name is never silently truncated or transliterated -- an over-long
//     or unrepresentable one is a hard validation failure.
//   * The 10-slot name field is zero-padded, so a shorter name cannot leave
//     code units of a longer previous one behind.
//   * An empty name is a total no-op: a retail dump keeps its console's real
//     nickname and a generated image keeps "ndsrecomp".

#include "firmware_user_settings.h"
#include "generated_firmware.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

std::vector<uint8_t> make_firmware() {
    const uint8_t mac[6] = {0x00, 0x09, 0xBF, 0x11, 0x22, 0x33};
    return nds_generate_firmware(mac);
}

size_t user_base(const std::vector<uint8_t>& fw, unsigned copy) {
    return (size_t{fw[0x20]} << 3u | size_t{fw[0x21]} << 11u) +
           size_t{copy} * 0x100u;
}

uint16_t read16(const std::vector<uint8_t>& fw, size_t off) {
    return static_cast<uint16_t>(uint16_t{fw[off]} |
                                 (uint16_t{fw[off + 1u]} << 8u));
}

// Every copy must always satisfy the console's own check.
bool checksum_valid(const std::vector<uint8_t>& fw, unsigned copy) {
    const size_t base = user_base(fw, copy);
    return read16(fw, base + 0x72u) ==
           nds_firmware_crc16(fw.data() + base, 0x70u, 0xFFFFu);
}

void test_validation() {
    std::string error;
    check(nds_validate_player_name("Samus", &error), "plain name accepted");
    check(nds_validate_player_name("0123456789", &error),
          "exactly 10 characters accepted");
    check(!nds_validate_player_name("01234567890", &error),
          "11 characters rejected, never truncated");
    check(!nds_validate_player_name("", &error), "empty rejected by validator");
    check(!nds_validate_player_name(" lead", &error), "leading space rejected");
    check(!nds_validate_player_name("trail ", &error),
          "trailing space rejected");
    check(nds_validate_player_name("Mr. O'Neil", &error),
          "common punctuation accepted");
    check(!nds_validate_player_name("a\"b", &error), "double quote rejected");
    check(!nds_validate_player_name("a\\b", &error), "backslash rejected");
    check(!nds_validate_player_name("a\tb", &error), "control char rejected");
    check(!nds_validate_player_name("\xC3\xA9", &error), "non-ASCII rejected");
    error.clear();
    (void)nds_validate_player_name("way too long a name", &error);
    check(!error.empty(), "validator reports a reason");
}

void test_apply_both_copies() {
    std::vector<uint8_t> fw = make_firmware();
    check(nds_apply_player_name(fw, "Samus"), "apply succeeds");
    for (unsigned copy = 0; copy < 2; ++copy) {
        const size_t base = user_base(fw, copy);
        check(read16(fw, base + 0x1Au) == 5u, "NameLength is in characters");
        const char16_t expect[5] = {u'S', u'a', u'm', u'u', u's'};
        bool text_ok = true;
        for (unsigned i = 0; i < 5; ++i)
            text_ok = text_ok && read16(fw, base + 0x06u + i * 2u) == expect[i];
        check(text_ok, "UTF-16LE name bytes at +0x06");
        check(checksum_valid(fw, copy), "checksum resealed for this copy");
        std::string got;
        check(nds_read_player_name(fw, copy, &got) && got == "Samus",
              "name reads back from this copy");
    }
}

void test_zero_padding() {
    std::vector<uint8_t> fw = make_firmware();
    check(nds_apply_player_name(fw, "0123456789"), "10-char apply succeeds");
    check(nds_apply_player_name(fw, "Ann"), "shorter apply succeeds");
    for (unsigned copy = 0; copy < 2; ++copy) {
        const size_t base = user_base(fw, copy);
        bool padded = true;
        for (unsigned i = 3; i < 10; ++i)
            padded = padded && read16(fw, base + 0x06u + i * 2u) == 0u;
        check(padded, "unused name slots zeroed, no stale tail");
        check(read16(fw, base + 0x1Au) == 3u, "length follows the new name");
        check(checksum_valid(fw, copy), "checksum resealed after re-apply");
    }
}

void test_empty_is_noop() {
    std::vector<uint8_t> fw = make_firmware();
    const std::vector<uint8_t> before = fw;
    check(nds_apply_player_name(fw, ""), "empty name succeeds");
    check(fw == before, "empty name leaves the image byte-identical");
    std::string got;
    check(nds_read_player_name(fw, 0, &got) && got == "ndsrecomp",
          "generated image keeps its default nickname when unset");
}

void test_invalid_name_rejected_at_apply() {
    std::vector<uint8_t> fw = make_firmware();
    const std::vector<uint8_t> before = fw;
    check(!nds_apply_player_name(fw, "much too long"),
          "over-long name refused at apply");
    check(fw == before, "refused apply left the image untouched");
}

void test_malformed_layout() {
    std::vector<uint8_t> fw = make_firmware();
    fw[0x20] = 0xFF;
    fw[0x21] = 0xFF;  // user-settings offset now points past the image
    check(!nds_apply_player_name(fw, "Samus"),
          "malformed user-settings offset refused");
    check(!nds_normalize_touch_calibration(fw), "touch calibration refused");
    check(!nds_apply_startup_mode(fw, NdsStartupMode::Automatic),
          "startup mode refused");
    std::vector<uint8_t> tiny(4, 0);
    check(!nds_apply_player_name(tiny, "Samus"), "undersized image refused");
}

// The two patches hoisted out of main.cpp alongside the new one: same
// both-copies + reseal contract, previously covered by nothing.
void test_hoisted_patches() {
    std::vector<uint8_t> fw = make_firmware();
    check(nds_normalize_touch_calibration(fw), "touch calibration applies");
    for (unsigned copy = 0; copy < 2; ++copy) {
        const size_t base = user_base(fw, copy);
        check(read16(fw, base + 0x5Eu) == (255u << 4u), "ADC2 X calibrated");
        check(fw[base + 0x63u] == 191u, "pixel2 Y calibrated");
        check(checksum_valid(fw, copy), "calibration resealed checksum");
    }

    check(nds_apply_startup_mode(fw, NdsStartupMode::Automatic),
          "automatic startup applies");
    for (unsigned copy = 0; copy < 2; ++copy) {
        const size_t base = user_base(fw, copy);
        check((read16(fw, base + 0x64u) & (1u << 6u)) != 0u,
              "automatic sets the startup flag");
        check(checksum_valid(fw, copy), "startup mode resealed checksum");
    }
    check(nds_apply_startup_mode(fw, NdsStartupMode::Manual),
          "manual startup applies");
    for (unsigned copy = 0; copy < 2; ++copy) {
        const size_t base = user_base(fw, copy);
        check((read16(fw, base + 0x64u) & (1u << 6u)) == 0u,
              "manual clears the startup flag");
        check(checksum_valid(fw, copy), "manual resealed checksum");
    }

    const std::vector<uint8_t> before = fw;
    check(nds_apply_startup_mode(fw, NdsStartupMode::Preserve),
          "preserve succeeds");
    check(fw == before, "preserve is a byte-exact no-op");

    // Instance 0 is LLE-faithful: no MAC byte moves.
    check(nds_apply_instance_mac(fw, 0), "instance 0 succeeds");
    check(fw == before, "instance 0 is a byte-exact no-op");
    check(nds_apply_instance_mac(fw, 1), "instance 1 succeeds");
    check(fw[0x36u + 3u] == 0x12u, "MAC byte 3 perturbed (+1)");
    check(fw[0x36u + 4u] == 0x66u, "MAC byte 4 perturbed (+0x44)");
    check(fw[0x36u + 5u] == 0x43u, "MAC byte 5 perturbed (+0x10)");
    check((fw[0x36u] & 0x01u) == 0u, "perturbed MAC stays unicast");
    check(read16(fw, 0x2Au) ==
              nds_firmware_crc16(fw.data() + 0x2Cu, 0x138u, 0u),
          "Wi-Fi config checksum resealed");
}

// The nickname patch must not disturb the touch calibration or startup flag
// the other two patches wrote into the same 0x70-byte checksummed block.
void test_patches_compose() {
    std::vector<uint8_t> fw = make_firmware();
    check(nds_normalize_touch_calibration(fw), "calibration applies");
    check(nds_apply_startup_mode(fw, NdsStartupMode::Automatic),
          "startup applies");
    check(nds_apply_player_name(fw, "Samus"), "name applies last");
    for (unsigned copy = 0; copy < 2; ++copy) {
        const size_t base = user_base(fw, copy);
        check(read16(fw, base + 0x5Eu) == (255u << 4u),
              "calibration survives the name patch");
        check((read16(fw, base + 0x64u) & (1u << 6u)) != 0u,
              "startup flag survives the name patch");
        check(checksum_valid(fw, copy), "final checksum valid");
    }
}

}  // namespace

int main() {
    test_validation();
    test_apply_both_copies();
    test_zero_padding();
    test_empty_is_noop();
    test_invalid_name_rejected_at_apply();
    test_malformed_layout();
    test_hoisted_patches();
    test_patches_compose();
    if (g_failures != 0) {
        std::fprintf(stderr, "firmware_user_settings_test: %d failure(s)\n",
                     g_failures);
        return 1;
    }
    std::printf("firmware_user_settings_test: all checks passed\n");
    return 0;
}
