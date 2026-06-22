#include "soniclear_models.h"
#include "soniclear_i.h"

#include <string.h>

// Known brush-head families. Verified entries only: the NDEF needle is a literal
// substring of the head's on-tag URL, which is what the brush handle reads.
//
// Philips Sonicare BrushSync heads carry "https://philips.com/nfcbrushheadtap".
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

size_t soniclear_brand_count(void) {
    return SONICLEAR_BRAND_COUNT;
}

const SoniclearBrand* soniclear_brand_at(size_t i) {
    return (i < SONICLEAR_BRAND_COUNT) ? &k_brands[i] : NULL;
}
