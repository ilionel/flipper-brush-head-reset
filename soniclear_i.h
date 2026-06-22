#pragma once

#include <stdint.h>
#include <stdbool.h>

#define SONICLEAR_VERSION "1.4"

// ~3-month rated life: 0x5460 = 21600 s = 180 x 2-min sessions, the wear total at
// which a smart toothbrush handle starts asking for a replacement head.
#define SONICLEAR_LIFE_SECONDS 21600u

// NTAG213 page holding the wear counter (bytes 0-1 = brushing seconds LE16; 2-3 = 02 00).
#define SONICLEAR_COUNTER_PAGE 36u

// How many times to retry the identity READ if it misses on transient coupling.
// Reads never touch the AUTHLIM lockout counter, so retrying them is always safe.
// The PWD_AUTH itself is never retried (it is sent exactly once).
#define SONICLEAR_READ_RETRIES 6

typedef enum {
    SoniclearOpRead, // read identity + counter, compute password (no write)
    SoniclearOpWrite, // read, authenticate, write counter = target, verify
} SoniclearOp;

// Outcome of one NFC operation (filled by the poller worker, drawn by the UI).
typedef struct {
    bool present; // an ISO14443-3A tag answered
    bool valid; // structure looks like a known smart-toothbrush NTAG213
    uint8_t uid[7];
    char mfg[11]; // 10 ASCII chars + NUL
    uint8_t pwd[4]; // computed PWD_AUTH key
    const char* brand; // recognised family name (from the model registry), or NULL
    uint16_t life_seconds; // rated brushing life for this head (family-specific)
    uint16_t seconds; // brushing seconds used (after a write, the written value)
    bool did_write; // a write (reset/set) was attempted
    bool auth_ok;
    bool write_ok;
    bool verify_ok;
    const char* message; // optional status / error text
} SoniclearResult;
