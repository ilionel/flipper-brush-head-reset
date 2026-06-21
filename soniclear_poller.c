#include "soniclear_poller.h"
#include "soniclear_pwd.h"

#include <furi.h>
#include <string.h>
#include <nfc/nfc.h>
#include <nfc/nfc_poller.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a_poller.h>

struct SoniclearPoller {
    Nfc* nfc;
    NfcPoller* poller;
    SoniclearOp op;
    uint16_t write_seconds;
    SoniclearResult* result;
    SoniclearPollerDone done;
    void* context;
    bool running;
};

SoniclearPoller* soniclear_poller_alloc(void) {
    SoniclearPoller* instance = malloc(sizeof(SoniclearPoller));
    memset(instance, 0, sizeof(SoniclearPoller));
    instance->nfc = nfc_alloc();
    return instance;
}

void soniclear_poller_free(SoniclearPoller* instance) {
    furi_assert(instance);
    if(instance->running) soniclear_poller_stop(instance);
    nfc_free(instance->nfc);
    free(instance);
}

// Read identity (UID, MFG), the wear counter, and compute the password.
static bool soniclear_read_identity(MfUltralightPoller* poller, SoniclearResult* r) {
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
    // Soniclear signature: the counter page's frame bytes are always 02 00.
    r->valid = (d.page[3].data[2] == 0x02) && (d.page[3].data[3] == 0x00);

    soniclear_pwd_compute(r->uid, mfg, sizeof(mfg), r->pwd);
    return true;
}

// Authenticate with the computed password and write the counter to `seconds`
// (0 = brand-new). Only ONE auth attempt is made (the password is computed, hence
// correct) so the 3-attempt permanent lockout can never be triggered.
static void soniclear_write_counter(MfUltralightPoller* poller, SoniclearResult* r, uint16_t seconds) {
    r->did_write = true;

    MfUltralightPollerAuthContext auth;
    memset(&auth, 0, sizeof(auth));
    memcpy(auth.password.data, r->pwd, 4);

    MfUltralightError err = mf_ultralight_poller_auth_pwd(poller, &auth);
    if(err != MfUltralightErrorNone) {
        // No clean PWD_AUTH exchange (timeout / lost coupling). The tag did not
        // register a wrong-password attempt, so the AUTHLIM lockout counter is
        // unaffected -> the user can safely reposition and retry.
        r->message = "No tag answer";
        return;
    }
    if(!auth.auth_success) {
        // The tag answered but rejected the key (this would count toward AUTHLIM).
        // With a correctly computed password this should not happen unless the
        // head's MFG layout differs from the known one.
        r->message = "Password rejected";
        return;
    }
    r->auth_ok = true;

    // counter = seconds (LE16), frame bytes 02 00 preserved
    MfUltralightPage page = {
        .data = {(uint8_t)(seconds & 0xFF), (uint8_t)(seconds >> 8), 0x02, 0x00}};
    if(mf_ultralight_poller_write_page(poller, SONICLEAR_COUNTER_PAGE, &page) !=
       MfUltralightErrorNone) {
        r->message = "Write failed";
        return;
    }
    r->write_ok = true;

    // read back to confirm
    MfUltralightPageReadCommandData d;
    if(mf_ultralight_poller_read_page(poller, SONICLEAR_COUNTER_PAGE, &d) == MfUltralightErrorNone) {
        r->verify_ok =
            (d.page[0].data[0] == page.data[0] && d.page[0].data[1] == page.data[1] &&
             d.page[0].data[2] == 0x02 && d.page[0].data[3] == 0x00);
    }
    if(r->verify_ok) r->seconds = seconds;
}

static NfcCommand soniclear_poller_callback(NfcGenericEventEx event, void* context) {
    SoniclearPoller* instance = context;
    MfUltralightPoller* poller = (MfUltralightPoller*)event.poller;
    Iso14443_3aPollerEvent* iso = (Iso14443_3aPollerEvent*)event.parent_event_data;
    SoniclearResult* r = instance->result;

    if(iso->type == Iso14443_3aPollerEventTypeError) {
        r->message = "Tag activation error";
        if(instance->done) instance->done(instance->context);
        return NfcCommandStop;
    }
    if(iso->type != Iso14443_3aPollerEventTypeReady) return NfcCommandContinue;

    if(!soniclear_read_identity(poller, r)) {
        if(!r->message) r->message = r->present ? "Read error" : "Not an NTAG";
    } else if(!r->valid) {
        r->message = "Not a Soniclear head";
    } else if(instance->op == SoniclearOpWrite) {
        soniclear_write_counter(poller, r, instance->write_seconds);
    }

    if(instance->done) instance->done(instance->context);
    return NfcCommandStop;
}

void soniclear_poller_start(
    SoniclearPoller* instance,
    SoniclearOp op,
    uint16_t write_seconds,
    SoniclearResult* result,
    SoniclearPollerDone done,
    void* context) {
    furi_assert(instance);
    furi_assert(!instance->running);
    memset(result, 0, sizeof(SoniclearResult));
    instance->op = op;
    instance->write_seconds = write_seconds;
    instance->result = result;
    instance->done = done;
    instance->context = context;
    instance->poller = nfc_poller_alloc(instance->nfc, NfcProtocolMfUltralight);
    instance->running = true;
    nfc_poller_start_ex(instance->poller, soniclear_poller_callback, instance);
}

void soniclear_poller_stop(SoniclearPoller* instance) {
    furi_assert(instance);
    if(!instance->running) return;
    nfc_poller_stop(instance->poller);
    nfc_poller_free(instance->poller);
    instance->poller = NULL;
    instance->running = false;
}
