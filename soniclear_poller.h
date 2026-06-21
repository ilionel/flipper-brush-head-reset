#pragma once

#include "soniclear_i.h"

typedef struct SoniclearPoller SoniclearPoller;

// Called from the poller thread when the operation has finished (result is filled).
typedef void (*SoniclearPollerDone)(void* context);

SoniclearPoller* soniclear_poller_alloc(void);
void soniclear_poller_free(SoniclearPoller* instance);

/**
 * Start an operation asynchronously. For SoniclearOpWrite, the counter is set to
 * `write_seconds` (0 = brand-new). The result is written into *result; `done` is
 * invoked (from the poller thread) once finished. Call soniclear_poller_stop() afterwards.
 */
void soniclear_poller_start(
    SoniclearPoller* instance,
    SoniclearOp op,
    uint16_t write_seconds,
    SoniclearResult* result,
    SoniclearPollerDone done,
    void* context);

// Stop and join the running operation (safe to call once per start).
void soniclear_poller_stop(SoniclearPoller* instance);
