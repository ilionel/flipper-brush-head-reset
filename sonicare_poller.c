#include "sonicare_poller.h"
#include "sonicare_pwd.h"

#include <furi.h>
#include <string.h>
#include <nfc/nfc.h>
#include <nfc/nfc_poller.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a_poller.h>

struct SonicarePoller {
    Nfc* nfc;
    NfcPoller* poller;
    SonicareOp op;
    SonicareResult* result;
    SonicarePollerDone done;
    void* context;
    bool running;
};

SonicarePoller* sonicare_poller_alloc(void) {
    SonicarePoller* instance = malloc(sizeof(SonicarePoller));
    memset(instance, 0, sizeof(SonicarePoller));
    instance->nfc = nfc_alloc();
    return instance;
}

void sonicare_poller_free(SonicarePoller* instance) {
    furi_assert(instance);
    if(instance->running) sonicare_poller_stop(instance);
    nfc_free(instance->nfc);
    free(instance);
}

// Read identity (UID, MFG), the wear counter, and compute the password.
static bool sonicare_read_identity(MfUltralightPoller* poller, SonicareResult* r) {
    MfUltralightPageReadCommandData d;

    // pages 0-3 -> UID (page0 bytes 0-2 + page1 bytes 0-3)
    if(mf_ultralight_poller_read_page(poller, 0, &d) != MfUltralightErrorNone) return false;
    r->uid[0] = d.page[0].data[0];
    r->uid[1] = d.page[0].data[1];
    r->uid[2] = d.page[0].data[2];
    r->uid[3] = d.page[1].data[0];
    r->uid[4] = d.page[1].data[1];
    r->uid[5] = d.page[1].data[2];
    r->uid[6] = d.page[1].data[3];
    r->present = true;

    // pages 33-36 -> MFG (pages 33-35, offset 2, 10 bytes) + counter (page 36)
    if(mf_ultralight_poller_read_page(poller, 33, &d) != MfUltralightErrorNone) return false;
    uint8_t mfg[10];
    mfg[0] = d.page[0].data[2];
    mfg[1] = d.page[0].data[3];
    memcpy(mfg + 2, d.page[1].data, 4);
    memcpy(mfg + 6, d.page[2].data, 4);
    memcpy(r->mfg, mfg, 10);
    r->mfg[10] = '\0';

    r->seconds = (uint16_t)(d.page[3].data[0] | (d.page[3].data[1] << 8));
    // Sonicare signature: the counter page's frame bytes are always 02 00.
    r->valid = (d.page[3].data[2] == 0x02) && (d.page[3].data[3] == 0x00);

    sonicare_pwd_compute(r->uid, mfg, sizeof(mfg), r->pwd);
    return true;
}

// Authenticate with the computed password and write the counter to the "new" value.
// Only ONE auth attempt is made (the password is computed, hence correct) so the
// 3-attempt permanent lockout can never be triggered.
static void sonicare_reset_counter(MfUltralightPoller* poller, SonicareResult* r) {
    r->did_reset = true;

    MfUltralightPollerAuthContext auth;
    memset(&auth, 0, sizeof(auth));
    memcpy(auth.password.data, r->pwd, 4);

    MfUltralightError err = mf_ultralight_poller_auth_pwd(poller, &auth);
    if(err != MfUltralightErrorNone || !auth.auth_success) {
        r->message = "Auth failed";
        return;
    }
    r->auth_ok = true;

    MfUltralightPage page = {.data = {0x00, 0x00, 0x02, 0x00}}; // counter 0, frame preserved
    if(mf_ultralight_poller_write_page(poller, SONICARE_COUNTER_PAGE, &page) !=
       MfUltralightErrorNone) {
        r->message = "Write failed";
        return;
    }
    r->write_ok = true;

    // read back to confirm
    MfUltralightPageReadCommandData d;
    if(mf_ultralight_poller_read_page(poller, SONICARE_COUNTER_PAGE, &d) == MfUltralightErrorNone) {
        r->verify_ok =
            (d.page[0].data[0] == 0x00 && d.page[0].data[1] == 0x00 && d.page[0].data[2] == 0x02 &&
             d.page[0].data[3] == 0x00);
    }
    if(r->verify_ok) r->seconds = 0;
}

static NfcCommand sonicare_poller_callback(NfcGenericEventEx event, void* context) {
    SonicarePoller* instance = context;
    MfUltralightPoller* poller = (MfUltralightPoller*)event.poller;
    Iso14443_3aPollerEvent* iso = (Iso14443_3aPollerEvent*)event.parent_event_data;
    SonicareResult* r = instance->result;

    if(iso->type == Iso14443_3aPollerEventTypeError) {
        r->message = "Tag activation error";
        if(instance->done) instance->done(instance->context);
        return NfcCommandStop;
    }
    if(iso->type != Iso14443_3aPollerEventTypeReady) return NfcCommandContinue;

    if(!sonicare_read_identity(poller, r)) {
        if(!r->message) r->message = r->present ? "Read error" : "Not an NTAG";
    } else if(!r->valid) {
        r->message = "Not a Sonicare head";
    } else if(instance->op == SonicareOpReset) {
        sonicare_reset_counter(poller, r);
    }

    if(instance->done) instance->done(instance->context);
    return NfcCommandStop;
}

void sonicare_poller_start(
    SonicarePoller* instance,
    SonicareOp op,
    SonicareResult* result,
    SonicarePollerDone done,
    void* context) {
    furi_assert(instance);
    furi_assert(!instance->running);
    memset(result, 0, sizeof(SonicareResult));
    instance->op = op;
    instance->result = result;
    instance->done = done;
    instance->context = context;
    instance->poller = nfc_poller_alloc(instance->nfc, NfcProtocolMfUltralight);
    instance->running = true;
    nfc_poller_start_ex(instance->poller, sonicare_poller_callback, instance);
}

void sonicare_poller_stop(SonicarePoller* instance) {
    furi_assert(instance);
    if(!instance->running) return;
    nfc_poller_stop(instance->poller);
    nfc_poller_free(instance->poller);
    instance->poller = NULL;
    instance->running = false;
}
