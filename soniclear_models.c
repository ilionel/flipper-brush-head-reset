#include "soniclear_models.h"
#include "soniclear_i.h"

#include <string.h>

// Known brush-head families. Verified entries only: the NDEF needle is a literal
// substring of the head's on-tag URL, which is what the brush handle reads.
//
// Philips Sonicare BrushSync heads carry "https://philips.com/nfcbrushheadtap".
// "Philips", "Sonicare" and "BrushSync" are trademarks of their respective owners,
// used here only nominatively to describe compatibility (see README disclaimer).
// To add another brand: capture a head's NDEF URL, pick a unique substring, and
// add a row with a friendly name and the rated brushing life (in seconds).
static const SoniclearBrand k_brands[] = {
    {"Sonicare BrushSync", "nfcbrushhead", SONICLEAR_LIFE_SECONDS},
};

#define SONICLEAR_BRAND_COUNT (sizeof(k_brands) / sizeof(k_brands[0]))

// Search for a NUL-terminated needle inside a byte buffer that may itself contain
// NULs or non-printable noise (so plain strstr cannot be used).
static bool soniclear_buf_contains(const char* hay, size_t hay_len, const char* needle) {
    size_t n = strlen(needle);
    if(n == 0 || n > hay_len) return false;
    for(size_t i = 0; i + n <= hay_len; i++) {
        if(memcmp(hay + i, needle, n) == 0) return true;
    }
    return false;
}

const SoniclearBrand* soniclear_identify_brand(const char* ndef_ascii, size_t len) {
    if(!ndef_ascii) return NULL;
    for(size_t i = 0; i < SONICLEAR_BRAND_COUNT; i++) {
        if(soniclear_buf_contains(ndef_ascii, len, k_brands[i].ndef_needle)) {
            return &k_brands[i];
        }
    }
    return NULL;
}

// Brush-head type byte (page 0x1F byte 2) -> model name. Index = type value (0x01-0x16).
// Names condensed to fit the Flipper screen (W = White, B = Black). Source: mbirth RE.
static const char* const k_head_types[] = {
    [0x01] = "Prem Plaque Def W",
    [0x02] = "Prem Plaque Def B",
    [0x03] = "Prem Gum Care W",
    [0x04] = "Prem Gum Care B",
    [0x05] = "Prem White W",
    [0x06] = "Prem White B",
    [0x07] = "Opt Plaque Def W",
    [0x08] = "Opt Gum Care W",
    [0x09] = "Opt White W",
    [0x0A] = "Opt White B",
    [0x0B] = "Opt White sm W",
    [0x0C] = "InterCare W",
    [0x0D] = "InterCare sm W",
    [0x0E] = "TongueCare+ W",
    [0x0F] = "TongueCare+ B",
    [0x10] = "Prem All-in-One W",
    [0x11] = "Prem All-in-One B",
    [0x12] = "SimplyClean W",
    [0x13] = "ProResults W",
    [0x14] = "Sensitive W",
    [0x15] = "Sensitive B",
    [0x16] = "Gentle Clean W",
};

const char* soniclear_head_type_name(uint8_t type) {
    if(type >= (sizeof(k_head_types) / sizeof(k_head_types[0]))) return NULL;
    return k_head_types[type]; // NULL for 0x00 and any gap
}

size_t soniclear_brand_count(void) {
    return SONICLEAR_BRAND_COUNT;
}

const SoniclearBrand* soniclear_brand_at(size_t i) {
    return (i < SONICLEAR_BRAND_COUNT) ? &k_brands[i] : NULL;
}
