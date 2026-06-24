#pragma once

#include "soniclear_i.h"

typedef struct SoniclearPoller SoniclearPoller;

// Called from the poller thread when the operation has finished (result is filled).
typedef void (*SoniclearPollerDone)(void* context);

SoniclearPoller* soniclear_poller_alloc(void);
void soniclear_poller_free(SoniclearPoller* instance);

/**
 * Start an operation asynchronously. For SoniclearOpWrite the counter is set to
 * `write_seconds` (0 = brand-new); for SoniclearOpWriteLife the rated-life field is set
 * to `write_seconds`. For SoniclearOpRestore the counter is set to `write_seconds` and,
 * when `write_life` is non-zero, the rated-life field is restored too (same auth session).
 * The result is written into *result; `done` is invoked (from the poller thread) once
 * finished. Call soniclear_poller_stop() afterwards.
 */
void soniclear_poller_start(
    SoniclearPoller* instance,
    SoniclearOp op,
    uint16_t write_seconds,
    uint16_t write_life,
    SoniclearResult* result,
    SoniclearPollerDone done,
    void* context);

// Seed the next lock probe with a prior per-page map (length SONICLEAR_PAGE_COUNT) for the
// given UID. Already-resolved (writable/locked) pages are skipped, so repeated probes of the
// same head accumulate into a complete map across short coupling windows. The seed is applied
// only if the freshly read UID matches `uid`; call before soniclear_poller_start.
void soniclear_poller_seed_probe(SoniclearPoller* instance, const uint8_t* seed, const uint8_t* uid);

// Stop and join the running operation (safe to call once per start).
void soniclear_poller_stop(SoniclearPoller* instance);
