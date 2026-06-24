#pragma once

#include <stdint.h>
#include <stdbool.h>

#define SONICLEAR_VERSION "1.12"

// ~3-month rated life: 0x5460 = 21600 s = 180 x 2-min sessions, the wear total at
// which a smart toothbrush handle starts asking for a replacement head.
#define SONICLEAR_LIFE_SECONDS 21600u

// NTAG213 page holding the wear counter (bytes 0-1 = brushing seconds LE16; 2-3 = 02 00).
#define SONICLEAR_COUNTER_PAGE 36u

// NTAG213 page holding the rated-life field. Bytes 0-1 = rated brushing seconds LE16;
// bytes 2-3 are the first two ASCII chars of the MFG code and MUST be preserved on write.
#define SONICLEAR_LIFE_PAGE 33u

// NTAG213 page holding the brush-head type. Byte 2 = type (0x01-0x16); bytes 0,1,3 are other
// fields that MUST be preserved on write. This page is writable (verified by the lock probe).
#define SONICLEAR_TYPE_PAGE 31u
#define SONICLEAR_TYPE_MIN 0x01u
#define SONICLEAR_TYPE_MAX 0x16u

// Lock probe: read each page then write the SAME bytes back, classify the tag's answer.
// Only the user region 4-39 is write-probed; pages 0-3 (UID/lock/OTP), 40 (dynamic lock)
// and 41-44 (config / PWD / PACK) are NEVER written (writing the PWD page would zero the
// password and brick the head). 45 = page count (statuses indexed by page number).
#define SONICLEAR_PROBE_FIRST 4u
#define SONICLEAR_PROBE_LAST 39u
#define SONICLEAR_PAGE_COUNT 45u
// Per coupling window, the probe sweeps the pages up to this many times (only re-attempting
// still-unresolved ones). The probe loops over many windows automatically until the map is
// complete or SONICLEAR_PROBE_MAX_WINDOWS is reached.
#define SONICLEAR_PROBE_PASSES 4
#define SONICLEAR_PROBE_MAX_WINDOWS 50

typedef enum {
    SoniclearProbeNone = 0, // not tested
    SoniclearProbeWritable, // write accepted (page is reprogrammable)
    SoniclearProbeLocked, // write NAK'd (page is fixed / locked)
    SoniclearProbeNoAnswer, // read or write timed out (coupling lost)
} SoniclearProbeStatus;

// How many times to retry the identity READ if it misses on transient coupling.
// Reads never touch the AUTHLIM lockout counter, so retrying them is always safe.
// The PWD_AUTH itself is never retried (it is sent exactly once).
#define SONICLEAR_READ_RETRIES 6

typedef enum {
    SoniclearOpRead, // read identity + counter, compute password (no write)
    SoniclearOpWrite, // read, authenticate, write counter = target, verify
    SoniclearOpWriteLife, // read, authenticate, write rated-life field = target, verify
    SoniclearOpRestore, // read, authenticate, write rated-life (if given) then counter
    SoniclearOpProbe, // read+write-back each user page (unchanged) to map locked vs writable
    SoniclearOpWriteType, // read, authenticate, write the brush-head type byte (page 31)
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
    // Extra on-tag fields decoded by the "Head fields" view (full read path only).
    bool extra_ok; // pages 37-42 were read
    uint32_t stamp; // page 38: LE32 timestamp written by the handle (epoch-like)
    uint8_t field37; // page 37 byte 2: secondary counter/flag
    uint8_t auth0; // page 41 (CFG0) byte 3: first PWD-protected page
    uint8_t access; // page 42 (CFG1) byte 0: PROT | CFGLCK | AUTHLIM
    uint16_t tag_life; // page 33 bytes 0-1: rated life declared on the tag (LE16, verified)
    uint8_t type_byte; // page 31 (0x1F) byte 2: brush-head type (0x01-0x16, mbirth RE)
    uint8_t intensity; // page 36 (0x24) byte 2: last-session intensity (0=Low 1=Med 2=High)
    uint8_t mode; // page 36 (0x24) byte 3: last brush mode (0..4, preserved on write)
    // Lock-probe results (Advanced -> Lock probe): per-page SoniclearProbeStatus.
    bool probe_ok; // the probe ran (auth succeeded)
    uint8_t probe[SONICLEAR_PAGE_COUNT];
    bool did_write; // a write (reset/set) was attempted
    bool auth_ok;
    bool write_ok;
    bool verify_ok;
    const char* message; // optional status / error text
} SoniclearResult;
