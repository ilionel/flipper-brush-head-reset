#pragma once

#include <stdint.h>
#include <stdbool.h>

// ~3-month rated life: 0x5460 = 21600 s = 180 x 2-min sessions (BrushSync threshold).
#define SONICARE_LIFE_SECONDS 21600u

// NTAG213 page holding the wear counter (bytes 0-1 = brushing seconds LE16; 2-3 = 02 00).
#define SONICARE_COUNTER_PAGE 36u

typedef enum {
    SonicareOpRead, // read identity + counter, compute password (no write)
    SonicareOpWrite, // read, authenticate, write counter = target, verify
} SonicareOp;

// Outcome of one NFC operation (filled by the poller worker, drawn by the UI).
typedef struct {
    bool present; // an ISO14443-3A tag answered
    bool valid; // structure looks like a Sonicare BrushSync NTAG213
    uint8_t uid[7];
    char mfg[11]; // 10 ASCII chars + NUL
    uint8_t pwd[4]; // computed PWD_AUTH key
    uint16_t seconds; // brushing seconds used (after a write, the written value)
    bool did_write; // a write (reset/set) was attempted
    bool auth_ok;
    bool write_ok;
    bool verify_ok;
    const char* message; // optional status / error text
} SonicareResult;
