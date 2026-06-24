// Async NFC worker for the brush-head NTAG213.
//
// We drive an MfUltralight poller in "ex" mode: nfc_poller_start_ex() runs the
// poller on its own thread and calls soniclear_poller_callback() with the parent
// Iso14443_3a event. On the Ready event (a tag is activated) we issue raw page
// reads / auth / writes synchronously and then stop the poller. The result is
// written into a caller-owned SoniclearResult and the `done` callback fires once.
//
// Lockout safety lives here: see soniclear_write_counter().

#include "soniclear_poller.h"
#include "soniclear_pwd.h"
#include "soniclear_models.h"

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
    uint16_t write_life; // SoniclearOpRestore: rated-life to restore (0 = leave as-is)
    uint8_t probe_seed[SONICLEAR_PAGE_COUNT]; // prior probe map to resume (OpProbe)
    uint8_t probe_seed_uid[7];
    bool probe_seeded;
    bool probe_started; // OpProbe loop: identity has been read once, pwd known
    uint16_t probe_windows; // OpProbe loop: coupling windows consumed so far
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

// Read identity (UID, MFG), the wear counter, and compute the password. When
// `read_ndef` is set, also read the NDEF URL to recognise the head family; that is
// skipped on the write path to keep the read->auth window (and coupling time) short.
static bool soniclear_read_identity(MfUltralightPoller* poller, SoniclearResult* r, bool read_ndef) {
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
    // Page 36 bytes 2-3 are NOT a fixed frame (the old "always 02 00" was wrong): byte 2 =
    // last-session intensity (0=Low 1=Med 2=High), byte 3 = last brush mode (0..4) -- per
    // mbirth's 2026-03 RE. We capture them and PRESERVE them on every counter write.
    r->intensity = d.page[3].data[2];
    r->mode = d.page[3].data[3];

    // The head declares its own rated life at page 33 bytes 0-1 (LE16) -- verified on
    // every real head dumped (0x5460 = 21600 s).
    uint16_t tag_life = (uint16_t)(d.page[0].data[0] | (d.page[0].data[1] << 8));
    r->tag_life = tag_life;

    // Structural validity: the MFG date (mfg[0..5]) is 6 ASCII digits and the last
    // intensity/mode are in range. The old exact "02 00" check wrongly rejected any head
    // last used at a different intensity/mode (-> "Unknown tag layout", un-resettable).
    bool mfg_digits = true;
    for(int i = 0; i < 6; i++)
        if(mfg[i] < '0' || mfg[i] > '9') mfg_digits = false;
    r->valid = mfg_digits && (r->intensity <= 2) && (r->mode <= 4);
    bool tag_life_ok = r->valid && tag_life >= 600u && tag_life < 0xFFFFu;

    soniclear_pwd_compute(r->uid, mfg, sizeof(mfg), r->pwd);

    // Effective rated life: the tag's own value wins; the model registry may still
    // refine the *name* (and provides the life only as a fallback for odd reads).
    r->brand = NULL;
    r->life_seconds = tag_life_ok ? tag_life : SONICLEAR_LIFE_SECONDS;
    if(!read_ndef) return true;
    char ndef[32];
    memset(ndef, 0, sizeof(ndef));
    bool ndef_ok = true;
    if(mf_ultralight_poller_read_page(poller, 4, &d) == MfUltralightErrorNone) {
        for(int i = 0; i < 4; i++) memcpy(ndef + i * 4, d.page[i].data, 4);
    } else {
        ndef_ok = false;
    }
    if(ndef_ok && mf_ultralight_poller_read_page(poller, 8, &d) == MfUltralightErrorNone) {
        for(int i = 0; i < 4; i++) memcpy(ndef + 16 + i * 4, d.page[i].data, 4);
        const SoniclearBrand* b = soniclear_identify_brand(ndef, sizeof(ndef));
        if(b) {
            r->brand = b->name;
            if(!tag_life_ok) r->life_seconds = b->life_seconds;
        }
    }

    // Extra fields for the "Head fields" view. Best-effort; a miss never fails the read.
    // Page 31 (0x1F) byte 2 is the head type/colour code (open-source RE; not provable
    // from a single model in hand, so it is shown with a "?" in the UI). Page 37 returns
    // 37-40 (secondary per-head field + a handle-written timestamp). Page 41 returns the
    // config pages 41-44 (CFG0/CFG1 hold AUTH0 and the PROT/CFGLCK/AUTHLIM access bits).
    if(mf_ultralight_poller_read_page(poller, 31, &d) == MfUltralightErrorNone) {
        r->type_byte = d.page[0].data[2];
    }
    if(mf_ultralight_poller_read_page(poller, 37, &d) == MfUltralightErrorNone) {
        r->field37 = d.page[0].data[2];
        r->stamp = (uint32_t)d.page[1].data[0] | ((uint32_t)d.page[1].data[1] << 8) |
                   ((uint32_t)d.page[1].data[2] << 16) | ((uint32_t)d.page[1].data[3] << 24);
    }
    if(mf_ultralight_poller_read_page(poller, 41, &d) == MfUltralightErrorNone) {
        r->auth0 = d.page[0].data[3]; // page 41 (CFG0) byte 3
        r->access = d.page[1].data[0]; // page 42 (CFG1) byte 0
        r->extra_ok = true;
    }
    return true;
}

// Authenticate with the computed password. Exactly ONE PWD_AUTH is sent (the only
// lockout-sensitive step). Success is `err == MfUltralightErrorNone`: the SDK function
// returns None only when the tag answers with a 2-byte PACK, i.e. it accepted the password
// (it does NOT set auth_context.auth_success for direct callers -- that field is filled by
// the firmware's own poller wrapper, not by this function). A successful auth also clears
// the tag's failed-attempt (AUTHLIM) counter. We never retry the auth, so the 3-strikes
// permanent lock can never be reached. Returns true and sets r->auth_ok on success.
static bool soniclear_open_auth(MfUltralightPoller* poller, SoniclearResult* r) {
    MfUltralightPollerAuthContext auth;
    memset(&auth, 0, sizeof(auth));
    memcpy(auth.password.data, r->pwd, 4);

    MfUltralightError err = mf_ultralight_poller_auth_pwd(poller, &auth);
    if(err != MfUltralightErrorNone) {
        // The password is computed (correct), so a failure here is almost always a
        // coupling drop, not a real rejection. We do not retry the auth, so re-placing
        // and trying again is safe. Timeout/NotPresent == the tag never answered.
        r->message = (err == MfUltralightErrorTimeout || err == MfUltralightErrorNotPresent) ?
                         "No tag answer" :
                         "Auth failed";
        return false;
    }
    r->auth_ok = true;
    return true;
}

// Write the wear counter = `seconds` (LE16) at page 36, PRESERVING bytes 2-3 (the head's
// last-session intensity/mode, captured during the identity read) -- the handle overwrites
// them next brushing anyway, but we must not clobber them with a bogus fixed frame. Assumes
// auth is open. Returns true on verified write; sets r->write_ok / r->message / r->seconds.
static bool
    soniclear_put_counter(MfUltralightPoller* poller, SoniclearResult* r, uint16_t seconds) {
    MfUltralightPage page = {
        .data = {(uint8_t)(seconds & 0xFF), (uint8_t)(seconds >> 8), r->intensity, r->mode}};
    MfUltralightError werr = mf_ultralight_poller_write_page(poller, SONICLEAR_COUNTER_PAGE, &page);
    if(werr != MfUltralightErrorNone) {
        r->message = (werr == MfUltralightErrorTimeout || werr == MfUltralightErrorNotPresent) ?
                         "Write lost-hold still" :
                         "Write refused-locked?";
        return false;
    }
    r->write_ok = true;

    MfUltralightPageReadCommandData d;
    bool ok = false;
    if(mf_ultralight_poller_read_page(poller, SONICLEAR_COUNTER_PAGE, &d) == MfUltralightErrorNone) {
        ok = (d.page[0].data[0] == page.data[0] && d.page[0].data[1] == page.data[1] &&
              d.page[0].data[2] == page.data[2] && d.page[0].data[3] == page.data[3]);
    }
    if(ok) r->seconds = seconds;
    return ok;
}

// Write the rated-life field = `life` (page 33 bytes 0-1, LE16). Bytes 2-3 carry the first
// two ASCII chars of the MFG code, so we re-read the page (under the open auth) and write
// those two bytes back unchanged. Assumes auth is open. Returns true on verified write.
static bool soniclear_put_life(MfUltralightPoller* poller, SoniclearResult* r, uint16_t life) {
    MfUltralightPageReadCommandData d;
    if(mf_ultralight_poller_read_page(poller, SONICLEAR_LIFE_PAGE, &d) != MfUltralightErrorNone) {
        r->message = "Read failed";
        return false;
    }
    MfUltralightPage page = {
        .data = {
            (uint8_t)(life & 0xFF), (uint8_t)(life >> 8), d.page[0].data[2], d.page[0].data[3]}};
    MfUltralightError werr = mf_ultralight_poller_write_page(poller, SONICLEAR_LIFE_PAGE, &page);
    if(werr != MfUltralightErrorNone) {
        // Distinguish a tag-side refusal (NAK -> page is locked) from a lost coupling
        // (timeout / tag gone). A locked page answers; a dropped field does not.
        r->message = (werr == MfUltralightErrorTimeout || werr == MfUltralightErrorNotPresent) ?
                         "Write lost-hold still" :
                         "Write refused-locked?";
        return false;
    }
    r->write_ok = true;

    bool ok = false;
    if(mf_ultralight_poller_read_page(poller, SONICLEAR_LIFE_PAGE, &d) == MfUltralightErrorNone) {
        ok = (d.page[0].data[0] == page.data[0]) && (d.page[0].data[1] == page.data[1]);
    }
    if(ok) {
        // reflect the new life so the result screen shows the recomputed usage %
        r->tag_life = life;
        r->life_seconds = life;
    }
    return ok;
}

// Op wrappers: each opens exactly one auth session, then writes.
static void
    soniclear_write_counter(MfUltralightPoller* poller, SoniclearResult* r, uint16_t seconds) {
    r->did_write = true;
    if(!soniclear_open_auth(poller, r)) return;
    r->verify_ok = soniclear_put_counter(poller, r, seconds);
}

static void soniclear_write_life(MfUltralightPoller* poller, SoniclearResult* r, uint16_t life) {
    r->did_write = true;
    if(!soniclear_open_auth(poller, r)) return;
    r->verify_ok = soniclear_put_life(poller, r, life);
}

// Write the brush-head type (page 31 byte 2) to `type`, preserving bytes 0,1,3. Same one-shot
// auth discipline. Page 31 is writable (lock probe), so this is reversible (re-write the
// original type). On success r->type_byte reflects the new value.
static void soniclear_write_type(MfUltralightPoller* poller, SoniclearResult* r, uint8_t type) {
    r->did_write = true;
    if(!soniclear_open_auth(poller, r)) return;

    MfUltralightPageReadCommandData d;
    if(mf_ultralight_poller_read_page(poller, SONICLEAR_TYPE_PAGE, &d) != MfUltralightErrorNone) {
        r->message = "Read failed";
        return;
    }
    MfUltralightPage page = {
        .data = {d.page[0].data[0], d.page[0].data[1], type, d.page[0].data[3]}};
    MfUltralightError werr = mf_ultralight_poller_write_page(poller, SONICLEAR_TYPE_PAGE, &page);
    if(werr != MfUltralightErrorNone) {
        r->message = (werr == MfUltralightErrorTimeout || werr == MfUltralightErrorNotPresent) ?
                         "Write lost-hold still" :
                         "Write refused-locked?";
        return;
    }
    r->write_ok = true;
    if(mf_ultralight_poller_read_page(poller, SONICLEAR_TYPE_PAGE, &d) == MfUltralightErrorNone) {
        r->verify_ok = (d.page[0].data[2] == type);
    }
    if(r->verify_ok) r->type_byte = type;
}

// Restore a backed-up head: in one auth session, restore the rated life and then the wear
// counter. The life is only rewritten when the record carries one AND it differs from what
// is already on the tag (r->tag_life, from the identity read) -- so a normal restore never
// touches page 33, and a head whose life field happens to be locked is not failed by a
// no-op life write. The counter is the essential wear state and is always written; verify_ok
// requires both writes (when each is needed), so a partial restore is reported honestly.
static void soniclear_write_restore(
    MfUltralightPoller* poller, SoniclearResult* r, uint16_t seconds, uint16_t life) {
    r->did_write = true;
    if(!soniclear_open_auth(poller, r)) return;
    bool need_life = (life != 0) && (life != r->tag_life);
    bool life_ok = !need_life || soniclear_put_life(poller, r, life);
    bool counter_ok = soniclear_put_counter(poller, r, seconds);
    r->verify_ok = counter_ok && life_ok;
}

// One attempt at one page, like Set life: read it, then write the SAME bytes back, and
// classify -- using *auth_valid to know whether we currently hold a CONFIRMED auth. A NAK
// is trusted as LOCKED only while *auth_valid is true (we authenticated with a success, the
// read then succeeded so the card is coupled, and the write got a NAK from a present,
// authenticated card -> genuine write-protection). Any timeout clears *auth_valid so the
// next NAK is re-confirmed first. Returns Writable/Locked (conclusive) or NoAnswer (retry).
static uint8_t
    soniclear_probe_once(MfUltralightPoller* poller, SoniclearResult* r, uint8_t p, bool* auth_valid) {
    if(!*auth_valid) {
        *auth_valid = soniclear_open_auth(poller, r); // re-confirm after a drop
        if(!*auth_valid) return SoniclearProbeNoAnswer;
    }
    MfUltralightPageReadCommandData d;
    if(mf_ultralight_poller_read_page(poller, p, &d) != MfUltralightErrorNone) {
        *auth_valid = false; // coupling dropped -> our auth is no longer trustworthy
        return SoniclearProbeNoAnswer;
    }
    MfUltralightPage pg;
    memcpy(pg.data, d.page[0].data, sizeof(pg.data)); // identical bytes -> no change
    MfUltralightError werr = mf_ultralight_poller_write_page(poller, p, &pg);
    if(werr == MfUltralightErrorNone) return SoniclearProbeWritable;
    if(werr == MfUltralightErrorTimeout || werr == MfUltralightErrorNotPresent) {
        *auth_valid = false;
        return SoniclearProbeNoAnswer;
    }
    return SoniclearProbeLocked; // NAK from a coupled, confirmed-authenticated card
}

// The probe page order: high-value pages (33 = rated life, 36 = counter, identity cluster)
// FIRST so they land in the opening coupling window.
static const uint8_t k_probe_order[] = {33, 36, 31, 32, 34, 35, 37, 38, 39, 16, 17, 18, 19,
                                        20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 4,  5,
                                        6,  7,  8,  9,  10, 11, 12, 13, 14, 15};

// One coupling window's worth of probing: one confirmed auth, then sweep the still-unresolved
// pages (a few passes), accumulating Writable/Locked verdicts into r->probe. Returns the
// number of pages still unresolved. Resolved pages are never re-probed, so calling this once
// per coupling window across the probe loop fills the map in. Writing identical bytes changes
// nothing on the tag.
static unsigned soniclear_probe_session(MfUltralightPoller* poller, SoniclearResult* r) {
    bool auth_valid = soniclear_open_auth(poller, r);
    for(int pass = 0; pass < SONICLEAR_PROBE_PASSES; pass++) {
        bool all_done = true;
        for(size_t i = 0; i < sizeof(k_probe_order); i++) {
            uint8_t p = k_probe_order[i];
            if(r->probe[p] == SoniclearProbeWritable || r->probe[p] == SoniclearProbeLocked) {
                continue; // already resolved -> never re-probe it
            }
            uint8_t s = soniclear_probe_once(poller, r, p, &auth_valid);
            if(s == SoniclearProbeWritable || s == SoniclearProbeLocked) {
                r->probe[p] = s;
            } else {
                all_done = false;
            }
        }
        if(all_done) break;
        if(!auth_valid) break; // coupling gone for this window -> let the loop wait for the next
    }
    unsigned unresolved = 0;
    for(size_t i = 0; i < sizeof(k_probe_order); i++) {
        if(r->probe[k_probe_order[i]] != SoniclearProbeWritable &&
           r->probe[k_probe_order[i]] != SoniclearProbeLocked) {
            unresolved++;
        }
    }
    return unresolved;
}

// Finalise the probe map: anything still unresolved becomes NoAnswer, and mark it done.
static void soniclear_probe_finalize(SoniclearResult* r) {
    for(size_t i = 0; i < sizeof(k_probe_order); i++) {
        uint8_t p = k_probe_order[i];
        if(r->probe[p] != SoniclearProbeWritable && r->probe[p] != SoniclearProbeLocked) {
            r->probe[p] = SoniclearProbeNoAnswer;
        }
    }
    r->probe_ok = true;
}

static NfcCommand soniclear_poller_callback(NfcGenericEventEx event, void* context) {
    SoniclearPoller* instance = context;
    MfUltralightPoller* poller = (MfUltralightPoller*)event.poller;
    Iso14443_3aPollerEvent* iso = (Iso14443_3aPollerEvent*)event.parent_event_data;
    SoniclearResult* r = instance->result;

    bool is_probe = (instance->op == SoniclearOpProbe);

    if(iso->type == Iso14443_3aPollerEventTypeError) {
        // During the probe loop a transient activation error is just a missed window; wait
        // for the next one rather than aborting the whole map.
        if(is_probe && instance->probe_started) return NfcCommandContinue;
        r->message = "Tag activation error";
        if(instance->done) instance->done(instance->context);
        return NfcCommandStop;
    }
    if(iso->type != Iso14443_3aPollerEventTypeReady) return NfcCommandContinue;

    // PROBE LOOP: after the first window the password is known, so skip the identity read
    // and spend each coupling window purely on probing more pages. The poller keeps polling,
    // so every re-coupling of the head advances the map until it is complete (or capped).
    if(is_probe && instance->probe_started) {
        unsigned unresolved = soniclear_probe_session(poller, r);
        if(unresolved == 0 || ++instance->probe_windows >= SONICLEAR_PROBE_MAX_WINDOWS) {
            soniclear_probe_finalize(r);
            if(instance->done) instance->done(instance->context);
            return NfcCommandStop;
        }
        return NfcCommandContinue; // wait for the next coupling window
    }

    // Reads can also miss on transient coupling; retrying them is safe (no AUTHLIM
    // cost) and lets a write proceed once a clean identity read lands.
    bool got_id = false;
    bool read_ndef = (instance->op == SoniclearOpRead); // skip NDEF on the write path
    for(int attempt = 0; attempt < SONICLEAR_READ_RETRIES; attempt++) {
        if(soniclear_read_identity(poller, r, read_ndef)) {
            got_id = true;
            break;
        }
        furi_delay_ms(40);
    }

    if(!got_id) {
        // For the probe, a bad first window is not fatal -- keep polling for a clean one.
        if(is_probe) return NfcCommandContinue;
        if(!r->message) r->message = r->present ? "Read error" : "Not an NTAG";
    } else if(!r->valid) {
        r->message = "Unknown tag layout";
    } else if(instance->op == SoniclearOpWrite) {
        soniclear_write_counter(poller, r, instance->write_seconds);
    } else if(instance->op == SoniclearOpWriteLife) {
        soniclear_write_life(poller, r, instance->write_seconds);
    } else if(instance->op == SoniclearOpRestore) {
        soniclear_write_restore(poller, r, instance->write_seconds, instance->write_life);
    } else if(instance->op == SoniclearOpWriteType) {
        soniclear_write_type(poller, r, (uint8_t)instance->write_seconds);
    } else if(is_probe) {
        // First valid window: resume a prior map for the same head, then probe.
        if(instance->probe_seeded && memcmp(r->uid, instance->probe_seed_uid, 7) == 0) {
            memcpy(r->probe, instance->probe_seed, SONICLEAR_PAGE_COUNT);
        }
        r->did_write = true;
        instance->probe_started = true;
        unsigned unresolved = soniclear_probe_session(poller, r);
        if(unresolved == 0 || ++instance->probe_windows >= SONICLEAR_PROBE_MAX_WINDOWS) {
            soniclear_probe_finalize(r);
            if(instance->done) instance->done(instance->context);
            return NfcCommandStop;
        }
        return NfcCommandContinue;
    }

    if(instance->done) instance->done(instance->context);
    return NfcCommandStop;
}

void soniclear_poller_start(
    SoniclearPoller* instance,
    SoniclearOp op,
    uint16_t write_seconds,
    uint16_t write_life,
    SoniclearResult* result,
    SoniclearPollerDone done,
    void* context) {
    furi_assert(instance);
    furi_assert(!instance->running);
    memset(result, 0, sizeof(SoniclearResult));
    instance->op = op;
    instance->write_seconds = write_seconds;
    instance->write_life = write_life;
    instance->probe_started = false;
    instance->probe_windows = 0;
    instance->result = result;
    instance->done = done;
    instance->context = context;
    instance->poller = nfc_poller_alloc(instance->nfc, NfcProtocolMfUltralight);
    instance->running = true;
    nfc_poller_start_ex(instance->poller, soniclear_poller_callback, instance);
}

void soniclear_poller_seed_probe(
    SoniclearPoller* instance, const uint8_t* seed, const uint8_t* uid) {
    furi_assert(instance);
    if(seed && uid) {
        memcpy(instance->probe_seed, seed, SONICLEAR_PAGE_COUNT);
        memcpy(instance->probe_seed_uid, uid, 7);
        instance->probe_seeded = true;
    } else {
        instance->probe_seeded = false;
    }
}

void soniclear_poller_stop(SoniclearPoller* instance) {
    furi_assert(instance);
    if(!instance->running) return;
    nfc_poller_stop(instance->poller);
    nfc_poller_free(instance->poller);
    instance->poller = NULL;
    instance->running = false;
}
