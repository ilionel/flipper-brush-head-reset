#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/number_input.h>
#include <gui/modules/byte_input.h>
#include <gui/modules/text_input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>
#include <furi_hal_rtc.h>

#include "soniclear_i.h"
#include "soniclear_pwd.h"
#include "soniclear_poller.h"

// NTAG213 smart-toothbrush-head wear-counter tool.
//
// Counter = brushing seconds at page 0x24 (LE16); fresh = 0. The per-head write
// password is computed from UID + MFG (handle-independent, no checksum on the
// counter) in soniclear_pwd.c. NFC I/O is done by the async worker in
// soniclear_poller.c; this file is the GUI: a ViewDispatcher with a main submenu,
// an Advanced submenu and one custom "work" view that renders every transient
// screen (confirm / scanning / result / detail / password-calc / about) from a
// single locking view-model.

#define SONICLEAR_DIR EXT_PATH("apps_data/soniclear")
#define SONICLEAR_EXT ".soniclear"

typedef enum {
    SoniclearViewSubmenu,
    SoniclearViewAdvanced,
    SoniclearViewWork,
    SoniclearViewNumber,
    SoniclearViewByteInput,
    SoniclearViewTextInput,
} SoniclearViewId;

typedef enum {
    SoniclearMenuRead,
    SoniclearMenuReset,
    SoniclearMenuSave,
    SoniclearMenuRestore,
    SoniclearMenuAdvanced,
    SoniclearMenuAbout,
} SoniclearMenuItem;

typedef enum {
    SoniclearAdvSetPct,
    SoniclearAdvSetMin,
    SoniclearAdvPwdCalc,
} SoniclearAdvItem;

typedef enum {
    SoniclearScreenConfirm, // confirm a write before placing the head
    SoniclearScreenScanning, // talking to the tag
    SoniclearScreenResult, // NFC outcome
    SoniclearScreenPwdCalc, // manual password calculator result
    SoniclearScreenAbout,
} SoniclearScreen;

typedef enum {
    SoniclearCustomEventDone = 100,
} SoniclearCustomEvent;

// Set-usage entry is in minutes; the LE16 counter caps at 65535 s = 1092 min.
#define SONICLEAR_MAX_MIN 1092

typedef struct {
    SoniclearScreen screen;
    SoniclearOp op;
    uint16_t write_seconds; // for write ops (drawing the target)
    uint16_t confirm_seconds; // pending write target (confirm screen)
    char confirm_title[20]; // pending write label (confirm screen)
    bool saved; // report saved to SD
    bool details; // result screen: show the full per-field detail page
    SoniclearResult res;
    // password calculator inputs/result
    uint8_t calc_uid[7];
    char calc_mfg[11];
    uint8_t calc_pwd[4];
    uint8_t anim;
} SoniclearWorkModel;

typedef struct {
    Gui* gui;
    Storage* storage;
    DialogsApp* dialogs;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Submenu* advanced;
    View* work;
    NumberInput* number;
    ByteInput* byte_input;
    TextInput* text_input;
    NotificationApp* notifications;
    SoniclearPoller* poller;
    FuriTimer* timer;
    SoniclearResult result; // filled by the poller thread
    SoniclearOp op;
    uint16_t write_seconds;
    bool save_after_read; // menu "Save": auto-save the read record
    uint16_t pending_seconds; // write target awaiting confirmation
    bool usage_by_percent; // Set usage: true = % entry, false = minutes entry
    // password calculator scratch
    uint8_t calc_uid[7];
    char calc_mfg[11];
} SoniclearApp;

/* --------------------------------- drawing ---------------------------------- */

static void soniclear_draw_spinner(Canvas* canvas, uint8_t x, uint8_t y, uint8_t frame) {
    static const char* f[] = {"|", "/", "-", "\\"};
    canvas_draw_str(canvas, x, y, f[frame & 3]);
}

// Horizontal usage gauge: an outlined bar filled proportionally to `pct` (0-100).
static void soniclear_draw_bar(Canvas* canvas, int x, int y, int w, int h, unsigned pct) {
    if(pct > 100) pct = 100;
    canvas_draw_frame(canvas, x, y, w, h);
    int fill = ((w - 2) * (int)pct) / 100;
    if(fill > 0) canvas_draw_box(canvas, x + 1, y + 1, fill, h - 2);
}

// Seconds -> percent of rated life (capped at 100).
static unsigned soniclear_pct(uint16_t seconds) {
    unsigned p = (100u * seconds) / SONICLEAR_LIFE_SECONDS;
    return p > 100u ? 100u : p;
}

static void soniclear_work_draw(Canvas* canvas, void* model) {
    SoniclearWorkModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    char line[28];

    if(m->screen == SoniclearScreenAbout) {
        canvas_draw_str(canvas, 2, 11, "Brush Head Reset");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 23, "v" SONICLEAR_VERSION "  NTAG213 wear");
        canvas_draw_str(canvas, 2, 33, "counter tool for your");
        canvas_draw_str(canvas, 2, 43, "own brush heads.");
        canvas_draw_str(canvas, 2, 53, "Independent project.");
        canvas_draw_str(canvas, 110, 63, "Back");
        return;
    }

    if(m->screen == SoniclearScreenPwdCalc) {
        canvas_draw_str(canvas, 2, 11, "Password calc");
        canvas_set_font(canvas, FontSecondary);
        snprintf(
            line,
            sizeof(line),
            "UID:%02X%02X%02X%02X%02X%02X%02X",
            m->calc_uid[0],
            m->calc_uid[1],
            m->calc_uid[2],
            m->calc_uid[3],
            m->calc_uid[4],
            m->calc_uid[5],
            m->calc_uid[6]);
        canvas_draw_str(canvas, 2, 28, line);
        snprintf(line, sizeof(line), "MFG: %s", m->calc_mfg);
        canvas_draw_str(canvas, 2, 40, line);
        canvas_set_font(canvas, FontPrimary);
        snprintf(
            line,
            sizeof(line),
            "PWD: %02X%02X%02X%02X",
            m->calc_pwd[0],
            m->calc_pwd[1],
            m->calc_pwd[2],
            m->calc_pwd[3]);
        canvas_draw_str(canvas, 2, 54, line);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 110, 63, "Back");
        return;
    }

    if(m->screen == SoniclearScreenConfirm) {
        canvas_draw_str(canvas, 2, 12, m->confirm_title);
        canvas_set_font(canvas, FontSecondary);
        if(m->confirm_seconds == 0) {
            canvas_draw_str(canvas, 2, 26, "New counter: 0% (fresh)");
        } else {
            snprintf(
                line,
                sizeof(line),
                "New counter: %u%% (%umin)",
                soniclear_pct(m->confirm_seconds),
                m->confirm_seconds / 60u);
            canvas_draw_str(canvas, 2, 26, line);
        }
        canvas_draw_str(canvas, 2, 38, "Place head flat, hold");
        canvas_draw_str(canvas, 2, 48, "still. Lockout-safe.");
        canvas_draw_str(canvas, 2, 62, "OK: write");
        canvas_draw_str(canvas, 100, 62, "Back");
        return;
    }

    if(m->screen == SoniclearScreenScanning) {
        canvas_draw_str(canvas, 2, 11, m->op == SoniclearOpWrite ? "Writing head" : "Reading head");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 26, "Brush base flat on");
        canvas_draw_str(canvas, 2, 36, "Flipper, hold still.");
        if(m->op == SoniclearOpWrite) {
            if(m->write_seconds == 0) {
                canvas_draw_str(canvas, 2, 48, "Target: 0% (fresh)");
            } else {
                snprintf(
                    line,
                    sizeof(line),
                    "Target: %u%% (%umin)",
                    soniclear_pct(m->write_seconds),
                    m->write_seconds / 60u);
                canvas_draw_str(canvas, 2, 48, line);
            }
        }
        soniclear_draw_spinner(canvas, 118, 31, m->anim);
        canvas_draw_str(canvas, 2, 62, "Back: cancel");
        return;
    }

    // SoniclearScreenResult
    SoniclearResult* r = &m->res;
    canvas_draw_str(canvas, 2, 11, m->op == SoniclearOpWrite ? "Write result" : "Brush head");
    canvas_set_font(canvas, FontSecondary);

    if(!r->present) {
        canvas_draw_str(canvas, 2, 30, r->message ? r->message : "No tag found");
        canvas_draw_str(canvas, 2, 63, "Back");
        return;
    }
    if(!r->valid) {
        canvas_draw_str(canvas, 2, 28, r->message ? r->message : "Unknown tag layout");
        // still show the UID we read, so the user can use Password calc manually
        snprintf(
            line,
            sizeof(line),
            "UID %02X%02X%02X%02X%02X%02X%02X",
            r->uid[0],
            r->uid[1],
            r->uid[2],
            r->uid[3],
            r->uid[4],
            r->uid[5],
            r->uid[6]);
        canvas_draw_str(canvas, 2, 40, line);
        canvas_draw_str(canvas, 2, 50, "(NTAG213 head only)");
        canvas_draw_str(canvas, 2, 63, "Back");
        return;
    }

    // detail page: every field, reachable with Right from the summary
    if(m->details) {
        snprintf(
            line,
            sizeof(line),
            "UID %02X%02X%02X%02X%02X%02X%02X",
            r->uid[0],
            r->uid[1],
            r->uid[2],
            r->uid[3],
            r->uid[4],
            r->uid[5],
            r->uid[6]);
        canvas_draw_str(canvas, 2, 22, line);
        snprintf(line, sizeof(line), "MFG %s", r->mfg);
        canvas_draw_str(canvas, 2, 32, line);
        snprintf(
            line,
            sizeof(line),
            "Sec %u (%u%%)",
            r->seconds,
            (100u * r->seconds) / SONICLEAR_LIFE_SECONDS);
        canvas_draw_str(canvas, 2, 42, line);
        snprintf(
            line,
            sizeof(line),
            "PWD %02X%02X%02X%02X",
            r->pwd[0],
            r->pwd[1],
            r->pwd[2],
            r->pwd[3]);
        canvas_draw_str(canvas, 2, 52, line);
        canvas_draw_str(canvas, 2, 63, "Left: back");
        canvas_draw_str(canvas, 104, 63, "Back");
        return;
    }

    unsigned pct = soniclear_pct(r->seconds); // capped, for the bar fill
    unsigned real_pct = (100u * r->seconds) / SONICLEAR_LIFE_SECONDS; // uncapped, for text
    snprintf(line, sizeof(line), "%s  %u min", r->mfg, r->seconds / 60u);
    canvas_draw_str(canvas, 2, 21, line);
    // usage gauge with the (true) percentage printed at the right
    soniclear_draw_bar(canvas, 2, 25, 100, 7, pct);
    snprintf(line, sizeof(line), "%u%%", real_pct);
    canvas_draw_str(canvas, 107, 32, line);
    snprintf(
        line, sizeof(line), "PWD %02X%02X%02X%02X", r->pwd[0], r->pwd[1], r->pwd[2], r->pwd[3]);
    canvas_draw_str(canvas, 2, 44, line);

    if(m->op == SoniclearOpWrite) {
        const char* st;
        const char* hint = NULL;
        if(r->verify_ok) {
            st = (m->write_seconds == 0) ? "Reset OK -> 0%" : "Write OK";
        } else if(r->write_ok) {
            st = "Written, verify failed";
            hint = "Re-read to check";
        } else if(r->auth_ok) {
            st = "Write failed";
            hint = "Hold still, retry";
        } else {
            // auth failed: r->message tells coupling-loss apart from key rejection
            st = r->message ? r->message : "Auth failed";
            hint = (r->message && strcmp(r->message, "Password rejected") == 0) ?
                       "Check UID/MFG" :
                       "Hold still, retry";
        }
        canvas_draw_str(canvas, 2, 55, st);
        if(hint) canvas_draw_str(canvas, 2, 63, hint);
        canvas_draw_str(canvas, 104, 63, "Back");
    } else {
        canvas_draw_str(canvas, 2, 55, m->saved ? "Saved to SD" : "OK: save to SD");
        canvas_draw_str(canvas, 2, 63, "Right: details");
        canvas_draw_str(canvas, 104, 63, "Back");
    }
}

/* --------------------------------- save / restore --------------------------- */

static void soniclear_save_record(SoniclearApp* app, const SoniclearResult* r) {
    storage_simply_mkdir(app->storage, SONICLEAR_DIR);
    FuriString* path = furi_string_alloc_printf(
        "%s/soniclear_%02X%02X%02X%02X%02X%02X%02X%s",
        SONICLEAR_DIR,
        r->uid[0],
        r->uid[1],
        r->uid[2],
        r->uid[3],
        r->uid[4],
        r->uid[5],
        r->uid[6],
        SONICLEAR_EXT);
    FuriString* body = furi_string_alloc();
    unsigned pct = (100u * r->seconds) / SONICLEAR_LIFE_SECONDS;
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    furi_string_printf(
        body,
        "Filetype: Soniclear head\nUID: %02X%02X%02X%02X%02X%02X%02X\nMFG: %s\n"
        "PWD: %02X%02X%02X%02X\nSeconds: %u\nUsed: %u min (%u%%)\n"
        "Date: %04u-%02u-%02u %02u:%02u\n",
        r->uid[0],
        r->uid[1],
        r->uid[2],
        r->uid[3],
        r->uid[4],
        r->uid[5],
        r->uid[6],
        r->mfg,
        r->pwd[0],
        r->pwd[1],
        r->pwd[2],
        r->pwd[3],
        r->seconds,
        r->seconds / 60u,
        pct,
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute);
    File* f = storage_file_alloc(app->storage);
    bool ok = false;
    if(storage_file_open(f, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        ok = storage_file_write(f, furi_string_get_cstr(body), furi_string_size(body)) > 0;
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_string_free(body);
    furi_string_free(path);
    notification_message(app->notifications, ok ? &sequence_success : &sequence_error);
}

static bool soniclear_parse_seconds(SoniclearApp* app, const char* path, uint16_t* out) {
    File* f = storage_file_alloc(app->storage);
    bool ok = false;
    if(storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[256];
        size_t n = storage_file_read(f, buf, sizeof(buf) - 1);
        buf[n] = '\0';
        char* p = strstr(buf, "Seconds:");
        if(p) {
            int v = atoi(p + 8);
            if(v < 0) v = 0;
            if(v > 65535) v = 65535;
            *out = (uint16_t)v;
            ok = true;
        }
    }
    storage_file_close(f);
    storage_file_free(f);
    return ok;
}

/* --------------------------------- worker glue ------------------------------ */

static void soniclear_on_done(void* context) {
    SoniclearApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, SoniclearCustomEventDone);
}

static void soniclear_timer_cb(void* context) {
    SoniclearApp* app = context;
    with_view_model(app->work, SoniclearWorkModel * m, { m->anim++; }, true);
}

static void soniclear_start_scan(SoniclearApp* app, SoniclearOp op, uint16_t write_seconds) {
    app->op = op;
    app->write_seconds = write_seconds;
    with_view_model(
        app->work,
        SoniclearWorkModel * m,
        {
            m->screen = SoniclearScreenScanning;
            m->op = op;
            m->write_seconds = write_seconds;
            m->saved = false;
            m->details = false;
            m->anim = 0;
            memset(&m->res, 0, sizeof(m->res));
        },
        true);
    view_dispatcher_switch_to_view(app->view_dispatcher, SoniclearViewWork);
    soniclear_poller_start(app->poller, op, write_seconds, &app->result, soniclear_on_done, app);
    furi_timer_start(app->timer, furi_ms_to_ticks(150));
}

// Stage a write and show a confirmation screen before any tag is touched. The user
// then places the head once and presses OK -> a single read+auth+write session runs.
// Confirming up front (not between read and write) avoids re-positioning the head.
static void soniclear_confirm_write(SoniclearApp* app, uint16_t seconds, const char* title) {
    app->pending_seconds = seconds;
    app->save_after_read = false;
    with_view_model(
        app->work,
        SoniclearWorkModel * m,
        {
            m->screen = SoniclearScreenConfirm;
            m->op = SoniclearOpWrite;
            m->confirm_seconds = seconds;
            strncpy(m->confirm_title, title, sizeof(m->confirm_title) - 1);
            m->confirm_title[sizeof(m->confirm_title) - 1] = '\0';
        },
        true);
    view_dispatcher_switch_to_view(app->view_dispatcher, SoniclearViewWork);
}

static bool soniclear_custom_event_cb(void* context, uint32_t event) {
    SoniclearApp* app = context;
    if(event != SoniclearCustomEventDone) return false;

    furi_timer_stop(app->timer);
    soniclear_poller_stop(app->poller); // joins the poller thread -> result is stable

    SoniclearResult* r = &app->result;
    bool good = r->present && r->valid &&
                (app->op == SoniclearOpRead || (r->did_write && r->verify_ok));
    notification_message(app->notifications, good ? &sequence_success : &sequence_error);

    bool saved = false;
    if(app->save_after_read && r->present && r->valid) {
        soniclear_save_record(app, r);
        saved = true;
    }
    app->save_after_read = false;

    with_view_model(
        app->work,
        SoniclearWorkModel * m,
        {
            m->screen = SoniclearScreenResult;
            m->res = *r;
            m->saved = saved;
        },
        true);
    return true;
}

/* --------------------------------- set-usage (% or minutes) ----------------- */

static void soniclear_number_done(void* context, int32_t number) {
    SoniclearApp* app = context;
    int32_t s = app->usage_by_percent ?
                    (int32_t)((int64_t)number * SONICLEAR_LIFE_SECONDS / 100) : // % -> seconds
                    number * 60; // minutes -> seconds
    if(s < 0) s = 0;
    if(s > 65535) s = 65535;
    // Confirm, then a single read -> auth -> write session (see soniclear_confirm_write).
    soniclear_confirm_write(app, (uint16_t)s, "Set usage");
}

/* --------------------------------- password calc ---------------------------- */

static void soniclear_text_input_done(void* context);

static void soniclear_byte_input_done(void* context) {
    SoniclearApp* app = context;
    // got UID -> ask for MFG (prefilled from the last valid read), then compute
    if(app->result.valid && app->result.mfg[0]) {
        strncpy(app->calc_mfg, app->result.mfg, sizeof(app->calc_mfg) - 1);
        app->calc_mfg[sizeof(app->calc_mfg) - 1] = '\0';
    } else {
        memset(app->calc_mfg, 0, sizeof(app->calc_mfg));
    }
    text_input_set_header_text(app->text_input, "MFG (e.g. 250625 51T)");
    text_input_set_result_callback(
        app->text_input,
        soniclear_text_input_done,
        app,
        app->calc_mfg,
        sizeof(app->calc_mfg),
        true);
    view_dispatcher_switch_to_view(app->view_dispatcher, SoniclearViewTextInput);
}

static void soniclear_text_input_done(void* context) {
    SoniclearApp* app = context;
    uint8_t pwd[4];
    size_t mfg_len = strlen(app->calc_mfg);
    soniclear_pwd_compute(app->calc_uid, (const uint8_t*)app->calc_mfg, mfg_len, pwd);
    with_view_model(
        app->work,
        SoniclearWorkModel * m,
        {
            m->screen = SoniclearScreenPwdCalc;
            memcpy(m->calc_uid, app->calc_uid, 7);
            memcpy(m->calc_mfg, app->calc_mfg, sizeof(m->calc_mfg));
            memcpy(m->calc_pwd, pwd, 4);
        },
        true);
    view_dispatcher_switch_to_view(app->view_dispatcher, SoniclearViewWork);
}

/* --------------------------------- restore ---------------------------------- */

static void soniclear_handle_restore(SoniclearApp* app) {
    storage_simply_mkdir(app->storage, SONICLEAR_DIR);
    FuriString* path = furi_string_alloc_set_str(SONICLEAR_DIR);
    DialogsFileBrowserOptions opts;
    dialog_file_browser_set_basic_options(&opts, SONICLEAR_EXT, NULL);
    opts.base_path = SONICLEAR_DIR;
    uint16_t seconds = 0;
    if(dialog_file_browser_show(app->dialogs, path, path, &opts) &&
       soniclear_parse_seconds(app, furi_string_get_cstr(path), &seconds)) {
        soniclear_confirm_write(app, seconds, "Restore");
    }
    furi_string_free(path);
}

/* --------------------------------- input / menu ----------------------------- */

static bool soniclear_work_input(InputEvent* event, void* context) {
    SoniclearApp* app = context;
    if(event->type != InputTypeShort) return false;

    SoniclearScreen screen = SoniclearScreenResult;
    bool valid_read = false; // a Read result that can still be saved (OK)
    bool read_ok = false; // a valid Read result (regardless of saved)
    with_view_model(
        app->work,
        SoniclearWorkModel * m,
        {
            screen = m->screen;
            read_ok = (m->op == SoniclearOpRead) && m->res.present && m->res.valid;
            valid_read = read_ok && !m->saved;
        },
        false);

    if(event->key == InputKeyBack) {
        if(screen == SoniclearScreenScanning) {
            furi_timer_stop(app->timer);
            soniclear_poller_stop(app->poller);
        }
        view_dispatcher_switch_to_view(app->view_dispatcher, SoniclearViewSubmenu);
        return true;
    }
    if(event->key == InputKeyOk) {
        if(screen == SoniclearScreenConfirm) {
            // user confirmed -> start the single read+auth+write session
            soniclear_start_scan(app, SoniclearOpWrite, app->pending_seconds);
            return true;
        }
        if(screen == SoniclearScreenResult && valid_read) {
            // a plain Read result: OK saves the record to SD
            soniclear_save_record(app, &app->result);
            with_view_model(app->work, SoniclearWorkModel * m, { m->saved = true; }, true);
            return true;
        }
    }
    // Right/Left toggle the per-field detail page on a valid Read result
    if(screen == SoniclearScreenResult && read_ok &&
       (event->key == InputKeyRight || event->key == InputKeyLeft)) {
        bool det = (event->key == InputKeyRight);
        with_view_model(app->work, SoniclearWorkModel * m, { m->details = det; }, true);
        return true;
    }
    return false;
}

static void soniclear_submenu_cb(void* context, uint32_t index) {
    SoniclearApp* app = context;
    switch(index) {
    case SoniclearMenuRead:
        app->save_after_read = false;
        soniclear_start_scan(app, SoniclearOpRead, 0);
        break;
    case SoniclearMenuReset:
        soniclear_confirm_write(app, 0, "Reset to new");
        break;
    case SoniclearMenuSave:
        app->save_after_read = true; // read, then auto-save the record
        soniclear_start_scan(app, SoniclearOpRead, 0);
        break;
    case SoniclearMenuRestore:
        soniclear_handle_restore(app);
        break;
    case SoniclearMenuAdvanced:
        view_dispatcher_switch_to_view(app->view_dispatcher, SoniclearViewAdvanced);
        break;
    case SoniclearMenuAbout:
        with_view_model(
            app->work, SoniclearWorkModel * m, { m->screen = SoniclearScreenAbout; }, true);
        view_dispatcher_switch_to_view(app->view_dispatcher, SoniclearViewWork);
        break;
    default:
        break;
    }
}

static void soniclear_advanced_cb(void* context, uint32_t index) {
    SoniclearApp* app = context;
    switch(index) {
    case SoniclearAdvSetPct:
        app->usage_by_percent = true;
        number_input_set_header_text(app->number, "Usage % (0-100)");
        number_input_set_result_callback(app->number, soniclear_number_done, app, 0, 0, 100);
        view_dispatcher_switch_to_view(app->view_dispatcher, SoniclearViewNumber);
        break;
    case SoniclearAdvSetMin:
        app->usage_by_percent = false;
        number_input_set_header_text(app->number, "Usage minutes (0-1092)");
        number_input_set_result_callback(
            app->number, soniclear_number_done, app, 0, 0, SONICLEAR_MAX_MIN);
        view_dispatcher_switch_to_view(app->view_dispatcher, SoniclearViewNumber);
        break;
    case SoniclearAdvPwdCalc:
        // prefill the UID from the last read (incl. unknown-layout tags) for convenience
        if(app->result.present) {
            memcpy(app->calc_uid, app->result.uid, sizeof(app->calc_uid));
        } else {
            memset(app->calc_uid, 0, sizeof(app->calc_uid));
        }
        byte_input_set_header_text(app->byte_input, "Tag UID (7 bytes)");
        byte_input_set_result_callback(
            app->byte_input, soniclear_byte_input_done, NULL, app, app->calc_uid, 7);
        view_dispatcher_switch_to_view(app->view_dispatcher, SoniclearViewByteInput);
        break;
    default:
        break;
    }
}

static uint32_t soniclear_prev_submenu(void* context) {
    UNUSED(context);
    return SoniclearViewSubmenu;
}

// inputs reached via Advanced return there on Back
static uint32_t soniclear_prev_advanced(void* context) {
    UNUSED(context);
    return SoniclearViewAdvanced;
}

static bool soniclear_nav_cb(void* context) {
    SoniclearApp* app = context;
    view_dispatcher_stop(app->view_dispatcher);
    return true;
}

/* --------------------------------- app lifecycle ---------------------------- */

int32_t soniclear_app(void* p) {
    UNUSED(p);
    SoniclearApp* app = malloc(sizeof(SoniclearApp));
    memset(app, 0, sizeof(SoniclearApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->poller = soniclear_poller_alloc();
    app->timer = furi_timer_alloc(soniclear_timer_cb, FuriTimerTypePeriodic, app);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, soniclear_custom_event_cb);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, soniclear_nav_cb);

    app->submenu = submenu_alloc();
    submenu_add_item(app->submenu, "Read brush head", SoniclearMenuRead, soniclear_submenu_cb, app);
    submenu_add_item(app->submenu, "Reset to new", SoniclearMenuReset, soniclear_submenu_cb, app);
    submenu_add_item(app->submenu, "Save", SoniclearMenuSave, soniclear_submenu_cb, app);
    submenu_add_item(app->submenu, "Restore", SoniclearMenuRestore, soniclear_submenu_cb, app);
    submenu_add_item(app->submenu, "Advanced", SoniclearMenuAdvanced, soniclear_submenu_cb, app);
    submenu_add_item(app->submenu, "About", SoniclearMenuAbout, soniclear_submenu_cb, app);
    view_dispatcher_add_view(
        app->view_dispatcher, SoniclearViewSubmenu, submenu_get_view(app->submenu));

    app->advanced = submenu_alloc();
    submenu_add_item(app->advanced, "Set usage %", SoniclearAdvSetPct, soniclear_advanced_cb, app);
    submenu_add_item(app->advanced, "Set usage min", SoniclearAdvSetMin, soniclear_advanced_cb, app);
    submenu_add_item(
        app->advanced, "Password calc", SoniclearAdvPwdCalc, soniclear_advanced_cb, app);
    view_set_previous_callback(submenu_get_view(app->advanced), soniclear_prev_submenu);
    view_dispatcher_add_view(
        app->view_dispatcher, SoniclearViewAdvanced, submenu_get_view(app->advanced));

    app->work = view_alloc();
    view_allocate_model(app->work, ViewModelTypeLocking, sizeof(SoniclearWorkModel));
    view_set_context(app->work, app);
    view_set_draw_callback(app->work, soniclear_work_draw);
    view_set_input_callback(app->work, soniclear_work_input);
    view_dispatcher_add_view(app->view_dispatcher, SoniclearViewWork, app->work);

    app->number = number_input_alloc();
    view_set_previous_callback(number_input_get_view(app->number), soniclear_prev_advanced);
    view_dispatcher_add_view(
        app->view_dispatcher, SoniclearViewNumber, number_input_get_view(app->number));

    app->byte_input = byte_input_alloc();
    view_set_previous_callback(byte_input_get_view(app->byte_input), soniclear_prev_advanced);
    view_dispatcher_add_view(
        app->view_dispatcher, SoniclearViewByteInput, byte_input_get_view(app->byte_input));

    app->text_input = text_input_alloc();
    view_set_previous_callback(text_input_get_view(app->text_input), soniclear_prev_advanced);
    view_dispatcher_add_view(
        app->view_dispatcher, SoniclearViewTextInput, text_input_get_view(app->text_input));

    view_dispatcher_switch_to_view(app->view_dispatcher, SoniclearViewSubmenu);
    view_dispatcher_run(app->view_dispatcher);

    // cleanup
    furi_timer_stop(app->timer);
    soniclear_poller_stop(app->poller);
    view_dispatcher_remove_view(app->view_dispatcher, SoniclearViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, SoniclearViewAdvanced);
    view_dispatcher_remove_view(app->view_dispatcher, SoniclearViewWork);
    view_dispatcher_remove_view(app->view_dispatcher, SoniclearViewNumber);
    view_dispatcher_remove_view(app->view_dispatcher, SoniclearViewByteInput);
    view_dispatcher_remove_view(app->view_dispatcher, SoniclearViewTextInput);
    submenu_free(app->submenu);
    submenu_free(app->advanced);
    view_free(app->work);
    number_input_free(app->number);
    byte_input_free(app->byte_input);
    text_input_free(app->text_input);
    view_dispatcher_free(app->view_dispatcher);
    furi_timer_free(app->timer);
    soniclear_poller_free(app->poller);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
