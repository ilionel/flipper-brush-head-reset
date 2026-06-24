#pragma once

#include <stdint.h>
#include <stddef.h>

// Registry of known smart-toothbrush brush-head families.
//
// Each NTAG213 head carries an NDEF URL that identifies its ecosystem (e.g. the
// Philips Sonicare BrushSync "tap" URL). We match a small, unique ASCII substring
// of that URL to recognise the brand and pick the correct rated brushing life so
// the usage percentage is accurate per family. The table is intentionally easy to
// extend: add a row with a unique NDEF needle, a friendly name and the life.

typedef struct {
    const char* name; // friendly brand / family name
    const char* ndef_needle; // unique ASCII substring of the head's NDEF URL
    uint16_t life_seconds; // rated brushing life for this family
} SoniclearBrand;

// Identify a head family from its (possibly noisy) NDEF ASCII bytes.
// Returns the matched brand, or NULL if unrecognised.
const SoniclearBrand* soniclear_identify_brand(const char* ndef_ascii, size_t len);

// Map the brush-head type byte (page 0x1F byte 2, values 0x01-0x16) to a model name,
// or NULL if out of range. Table from mbirth's 2026-03 reverse-engineering.
const char* soniclear_head_type_name(uint8_t type);

// Enumeration helpers for the on-device "Models" list.
size_t soniclear_brand_count(void);
const SoniclearBrand* soniclear_brand_at(size_t i);
