#pragma once

#include "sonicare_i.h"

typedef struct SonicarePoller SonicarePoller;

// Called from the poller thread when the operation has finished (result is filled).
typedef void (*SonicarePollerDone)(void* context);

SonicarePoller* sonicare_poller_alloc(void);
void sonicare_poller_free(SonicarePoller* instance);

/**
 * Start an operation asynchronously. The result is written into *result; `done` is
 * invoked (from the poller thread) once finished. Call sonicare_poller_stop() afterwards.
 */
void sonicare_poller_start(
    SonicarePoller* instance,
    SonicareOp op,
    SonicareResult* result,
    SonicarePollerDone done,
    void* context);

// Stop and join the running operation (safe to call once per start).
void sonicare_poller_stop(SonicarePoller* instance);
