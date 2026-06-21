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

#include "sonicare_i.h"
#include "sonicare_pwd.h"
#include "sonicare_poller.h"

// Philips Sonicare BrushSync (NTAG213) brush-head wear-counter tool.
// Counter = brushing seconds at page 0x24 (LE16); fresh = 0. Per-head write password is
// computed from UID + MFG (handle-independent, no checksum on the counter).

#define SONICARE_DIR EXT_PATH("apps_data/sonicare")
#define SONICARE_EXT ".sonicare"

typedef enum {
    SonicareViewSubmenu,
    SonicareViewAdvanced,
    SonicareViewWork,
    SonicareViewNumber,
    SonicareViewByteInput,
    SonicareViewTextInput,
} SonicareViewId;

typedef enum {
    SonicareMenuRead,
    SonicareMenuReset,
    SonicareMenuSave,
    SonicareMenuRestore,
    SonicareMenuAdvanced,
    SonicareMenuAbout,
} SonicareMenuItem;

typedef enum {
    SonicareAdvSetPct,
    SonicareAdvSetMin,
    SonicareAdvPwdCalc,
} SonicareAdvItem;

typedef enum {
    SonicareScreenScanning, // talking to the tag
    SonicareScreenResult, // NFC outcome
    SonicareScreenPwdCalc, // manual password calculator result
    SonicareScreenAbout,
} SonicareScreen;

typedef enum {
    SonicareCustomEventDone = 100,
} SonicareCustomEvent;

// Set-usage entry is in minutes; the LE16 counter caps at 65535 s = 1092 min.
#define SONICARE_MAX_MIN 1092

typedef struct {
    SonicareScreen screen;
    SonicareOp op;
    uint16_t write_seconds; // for write ops (drawing the target)
    bool saved; // report saved to SD
    SonicareResult res;
    // password calculator inputs/result
    uint8_t calc_uid[7];
    char calc_mfg[11];
    uint8_t calc_pwd[4];
    uint8_t anim;
} SonicareWorkModel;

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
    SonicarePoller* poller;
    FuriTimer* timer;
    SonicareResult result; // filled by the poller thread
    SonicareOp op;
    uint16_t write_seconds;
    bool save_after_read; // menu "Save": auto-save the read record
    bool usage_by_percent; // Set usage: true = % entry, false = minutes entry
    // password calculator scratch
    uint8_t calc_uid[7];
    char calc_mfg[11];
} SonicareApp;

/* --------------------------------- drawing ---------------------------------- */

static void sonicare_draw_spinner(Canvas* canvas, uint8_t x, uint8_t y, uint8_t frame) {
    static const char* f[] = {"|", "/", "-", "\\"};
    canvas_draw_str(canvas, x, y, f[frame & 3]);
}

static void sonicare_work_draw(Canvas* canvas, void* model) {
    SonicareWorkModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    char line[28];

    if(m->screen == SonicareScreenAbout) {
        canvas_draw_str(canvas, 2, 11, "Brush Head Reset");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 24, "NTAG213 wear-counter");
        canvas_draw_str(canvas, 2, 34, "tool for your own brush");
        canvas_draw_str(canvas, 2, 44, "heads. Not affiliated");
        canvas_draw_str(canvas, 2, 54, "with Philips.");
        canvas_draw_str(canvas, 110, 63, "Back");
        return;
    }

    if(m->screen == SonicareScreenPwdCalc) {
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

    if(m->screen == SonicareScreenScanning) {
        canvas_draw_str(canvas, 2, 11, m->op == SonicareOpWrite ? "Writing head" : "Reading head");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 28, "Brush base flat on the");
        canvas_draw_str(canvas, 2, 38, "Flipper back. Hold");
        canvas_draw_str(canvas, 2, 48, "still until done.");
        sonicare_draw_spinner(canvas, 118, 33, m->anim);
        canvas_draw_str(canvas, 2, 63, "Back: cancel");
        return;
    }

    // SonicareScreenResult
    SonicareResult* r = &m->res;
    canvas_draw_str(canvas, 2, 11, m->op == SonicareOpWrite ? "Write result" : "Brush head");
    canvas_set_font(canvas, FontSecondary);

    if(!r->present) {
        canvas_draw_str(canvas, 2, 30, r->message ? r->message : "No tag found");
        canvas_draw_str(canvas, 2, 63, "Back");
        return;
    }
    if(!r->valid) {
        canvas_draw_str(canvas, 2, 30, r->message ? r->message : "Not a Sonicare head");
        canvas_draw_str(canvas, 2, 42, "(NTAG213 BrushSync only)");
        canvas_draw_str(canvas, 2, 63, "Back");
        return;
    }

    snprintf(line, sizeof(line), "Model: %s", r->mfg);
    canvas_draw_str(canvas, 2, 22, line);
    unsigned pct = (100u * r->seconds) / SONICARE_LIFE_SECONDS;
    snprintf(line, sizeof(line), "Used: %u%%  (%u min)", pct, r->seconds / 60u);
    canvas_draw_str(canvas, 2, 32, line);
    snprintf(
        line, sizeof(line), "PWD: %02X%02X%02X%02X", r->pwd[0], r->pwd[1], r->pwd[2], r->pwd[3]);
    canvas_draw_str(canvas, 2, 42, line);

    if(m->op == SonicareOpWrite) {
        const char* st;
        if(r->verify_ok) {
            st = (m->write_seconds == 0) ? "RESET OK -> 0%" : "Set OK";
        } else if(r->write_ok) {
            st = "Written (verify?)";
        } else if(r->auth_ok) {
            st = "Write failed";
        } else {
            st = "Auth failed";
        }
        canvas_draw_str(canvas, 2, 54, st);
        canvas_draw_str(canvas, 104, 63, "Back");
    } else {
        canvas_draw_str(canvas, 2, 54, m->saved ? "Saved to SD" : "OK: save");
        canvas_draw_str(canvas, 104, 63, "Back");
    }
}

/* --------------------------------- save / restore --------------------------- */

static void sonicare_save_record(SonicareApp* app, const SonicareResult* r) {
    storage_simply_mkdir(app->storage, SONICARE_DIR);
    FuriString* path = furi_string_alloc_printf(
        "%s/sonicare_%02X%02X%02X%02X%02X%02X%02X%s",
        SONICARE_DIR,
        r->uid[0],
        r->uid[1],
        r->uid[2],
        r->uid[3],
        r->uid[4],
        r->uid[5],
        r->uid[6],
        SONICARE_EXT);
    FuriString* body = furi_string_alloc();
    unsigned pct = (100u * r->seconds) / SONICARE_LIFE_SECONDS;
    furi_string_printf(
        body,
        "Filetype: Sonicare head\nUID: %02X%02X%02X%02X%02X%02X%02X\nMFG: %s\n"
        "PWD: %02X%02X%02X%02X\nSeconds: %u\nUsed: %u min (%u%%)\n",
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
        pct);
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

static bool sonicare_parse_seconds(SonicareApp* app, const char* path, uint16_t* out) {
    File* f = storage_file_alloc(app->storage);
    bool ok = false;
    if(storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[256];
        size_t n = storage_file_read(f, buf, sizeof(buf) - 1);
        buf[n] = '\0';
        char* p = strstr(buf, "Seconds:");
        if(p) {
            *out = (uint16_t)atoi(p + 8);
            ok = true;
        }
    }
    storage_file_close(f);
    storage_file_free(f);
    return ok;
}

/* --------------------------------- worker glue ------------------------------ */

static void sonicare_on_done(void* context) {
    SonicareApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, SonicareCustomEventDone);
}

static void sonicare_timer_cb(void* context) {
    SonicareApp* app = context;
    with_view_model(app->work, SonicareWorkModel * m, { m->anim++; }, true);
}

static void sonicare_start_scan(SonicareApp* app, SonicareOp op, uint16_t write_seconds) {
    app->op = op;
    app->write_seconds = write_seconds;
    with_view_model(
        app->work,
        SonicareWorkModel * m,
        {
            m->screen = SonicareScreenScanning;
            m->op = op;
            m->write_seconds = write_seconds;
            m->saved = false;
            m->anim = 0;
            memset(&m->res, 0, sizeof(m->res));
        },
        true);
    view_dispatcher_switch_to_view(app->view_dispatcher, SonicareViewWork);
    sonicare_poller_start(app->poller, op, write_seconds, &app->result, sonicare_on_done, app);
    furi_timer_start(app->timer, furi_ms_to_ticks(150));
}

static bool sonicare_custom_event_cb(void* context, uint32_t event) {
    SonicareApp* app = context;
    if(event != SonicareCustomEventDone) return false;

    furi_timer_stop(app->timer);
    sonicare_poller_stop(app->poller); // joins the poller thread -> result is stable

    SonicareResult* r = &app->result;
    bool good = r->present && r->valid &&
                (app->op == SonicareOpRead || (r->did_write && r->verify_ok));
    notification_message(app->notifications, good ? &sequence_success : &sequence_error);

    bool saved = false;
    if(app->save_after_read && r->present && r->valid) {
        sonicare_save_record(app, r);
        saved = true;
    }
    app->save_after_read = false;

    with_view_model(
        app->work,
        SonicareWorkModel * m,
        {
            m->screen = SonicareScreenResult;
            m->res = *r;
            m->saved = saved;
        },
        true);
    return true;
}

/* --------------------------------- set-usage (% or minutes) ----------------- */

static void sonicare_number_done(void* context, int32_t number) {
    SonicareApp* app = context;
    int32_t s = app->usage_by_percent ?
                    (int32_t)((int64_t)number * SONICARE_LIFE_SECONDS / 100) : // % -> seconds
                    number * 60; // minutes -> seconds
    if(s < 0) s = 0;
    if(s > 65535) s = 65535;
    // Single placement: read identity (gets pwd) -> auth -> write, all in one field
    // session. The poller only authenticates after a valid read, with the computed
    // password, exactly once -> the 3-attempt lockout can never be reached.
    sonicare_start_scan(app, SonicareOpWrite, (uint16_t)s);
}

/* --------------------------------- password calc ---------------------------- */

static void sonicare_text_input_done(void* context);

static void sonicare_byte_input_done(void* context) {
    SonicareApp* app = context;
    // got UID -> ask for MFG, then compute the password
    memset(app->calc_mfg, 0, sizeof(app->calc_mfg));
    text_input_set_header_text(app->text_input, "MFG (e.g. 250625 51T)");
    text_input_set_result_callback(
        app->text_input,
        sonicare_text_input_done,
        app,
        app->calc_mfg,
        sizeof(app->calc_mfg),
        true);
    view_dispatcher_switch_to_view(app->view_dispatcher, SonicareViewTextInput);
}

static void sonicare_text_input_done(void* context) {
    SonicareApp* app = context;
    uint8_t pwd[4];
    size_t mfg_len = strlen(app->calc_mfg);
    sonicare_pwd_compute(app->calc_uid, (const uint8_t*)app->calc_mfg, mfg_len, pwd);
    with_view_model(
        app->work,
        SonicareWorkModel * m,
        {
            m->screen = SonicareScreenPwdCalc;
            memcpy(m->calc_uid, app->calc_uid, 7);
            memcpy(m->calc_mfg, app->calc_mfg, sizeof(m->calc_mfg));
            memcpy(m->calc_pwd, pwd, 4);
        },
        true);
    view_dispatcher_switch_to_view(app->view_dispatcher, SonicareViewWork);
}

/* --------------------------------- restore ---------------------------------- */

static void sonicare_handle_restore(SonicareApp* app) {
    storage_simply_mkdir(app->storage, SONICARE_DIR);
    FuriString* path = furi_string_alloc_set_str(SONICARE_DIR);
    DialogsFileBrowserOptions opts;
    dialog_file_browser_set_basic_options(&opts, SONICARE_EXT, NULL);
    opts.base_path = SONICARE_DIR;
    uint16_t seconds = 0;
    if(dialog_file_browser_show(app->dialogs, path, path, &opts) &&
       sonicare_parse_seconds(app, furi_string_get_cstr(path), &seconds)) {
        sonicare_start_scan(app, SonicareOpWrite, seconds);
    }
    furi_string_free(path);
}

/* --------------------------------- input / menu ----------------------------- */

static bool sonicare_work_input(InputEvent* event, void* context) {
    SonicareApp* app = context;
    if(event->type != InputTypeShort) return false;

    SonicareScreen screen = SonicareScreenResult;
    bool valid_read = false;
    with_view_model(
        app->work,
        SonicareWorkModel * m,
        {
            screen = m->screen;
            valid_read = (m->op == SonicareOpRead) && m->res.present && m->res.valid && !m->saved;
        },
        false);

    if(event->key == InputKeyBack) {
        if(screen == SonicareScreenScanning) {
            furi_timer_stop(app->timer);
            sonicare_poller_stop(app->poller);
        }
        view_dispatcher_switch_to_view(app->view_dispatcher, SonicareViewSubmenu);
        return true;
    }
    if(event->key == InputKeyOk && screen == SonicareScreenResult && valid_read) {
        // a plain Read result: OK saves the record to SD
        sonicare_save_record(app, &app->result);
        with_view_model(app->work, SonicareWorkModel * m, { m->saved = true; }, true);
        return true;
    }
    return false;
}

static void sonicare_submenu_cb(void* context, uint32_t index) {
    SonicareApp* app = context;
    switch(index) {
    case SonicareMenuRead:
        app->save_after_read = false;
        sonicare_start_scan(app, SonicareOpRead, 0);
        break;
    case SonicareMenuReset:
        // Single placement: read -> auth -> write 0 (one field session, one auth)
        app->save_after_read = false;
        sonicare_start_scan(app, SonicareOpWrite, 0);
        break;
    case SonicareMenuSave:
        app->save_after_read = true; // read, then auto-save the record
        sonicare_start_scan(app, SonicareOpRead, 0);
        break;
    case SonicareMenuRestore:
        sonicare_handle_restore(app);
        break;
    case SonicareMenuAdvanced:
        view_dispatcher_switch_to_view(app->view_dispatcher, SonicareViewAdvanced);
        break;
    case SonicareMenuAbout:
        with_view_model(
            app->work, SonicareWorkModel * m, { m->screen = SonicareScreenAbout; }, true);
        view_dispatcher_switch_to_view(app->view_dispatcher, SonicareViewWork);
        break;
    default:
        break;
    }
}

static void sonicare_advanced_cb(void* context, uint32_t index) {
    SonicareApp* app = context;
    switch(index) {
    case SonicareAdvSetPct:
        app->usage_by_percent = true;
        number_input_set_header_text(app->number, "Usage % (0-100)");
        number_input_set_result_callback(app->number, sonicare_number_done, app, 0, 0, 100);
        view_dispatcher_switch_to_view(app->view_dispatcher, SonicareViewNumber);
        break;
    case SonicareAdvSetMin:
        app->usage_by_percent = false;
        number_input_set_header_text(app->number, "Usage minutes (0-1092)");
        number_input_set_result_callback(
            app->number, sonicare_number_done, app, 0, 0, SONICARE_MAX_MIN);
        view_dispatcher_switch_to_view(app->view_dispatcher, SonicareViewNumber);
        break;
    case SonicareAdvPwdCalc:
        memset(app->calc_uid, 0, sizeof(app->calc_uid));
        byte_input_set_header_text(app->byte_input, "Tag UID (7 bytes)");
        byte_input_set_result_callback(
            app->byte_input, sonicare_byte_input_done, NULL, app, app->calc_uid, 7);
        view_dispatcher_switch_to_view(app->view_dispatcher, SonicareViewByteInput);
        break;
    default:
        break;
    }
}

static uint32_t sonicare_prev_submenu(void* context) {
    UNUSED(context);
    return SonicareViewSubmenu;
}

// inputs reached via Advanced return there on Back
static uint32_t sonicare_prev_advanced(void* context) {
    UNUSED(context);
    return SonicareViewAdvanced;
}

static bool sonicare_nav_cb(void* context) {
    SonicareApp* app = context;
    view_dispatcher_stop(app->view_dispatcher);
    return true;
}

/* --------------------------------- app lifecycle ---------------------------- */

int32_t sonicare_app(void* p) {
    UNUSED(p);
    SonicareApp* app = malloc(sizeof(SonicareApp));
    memset(app, 0, sizeof(SonicareApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->poller = sonicare_poller_alloc();
    app->timer = furi_timer_alloc(sonicare_timer_cb, FuriTimerTypePeriodic, app);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, sonicare_custom_event_cb);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, sonicare_nav_cb);

    app->submenu = submenu_alloc();
    submenu_add_item(app->submenu, "Read brush head", SonicareMenuRead, sonicare_submenu_cb, app);
    submenu_add_item(app->submenu, "Reset to new", SonicareMenuReset, sonicare_submenu_cb, app);
    submenu_add_item(app->submenu, "Save", SonicareMenuSave, sonicare_submenu_cb, app);
    submenu_add_item(app->submenu, "Restore", SonicareMenuRestore, sonicare_submenu_cb, app);
    submenu_add_item(app->submenu, "Advanced", SonicareMenuAdvanced, sonicare_submenu_cb, app);
    submenu_add_item(app->submenu, "About", SonicareMenuAbout, sonicare_submenu_cb, app);
    view_dispatcher_add_view(
        app->view_dispatcher, SonicareViewSubmenu, submenu_get_view(app->submenu));

    app->advanced = submenu_alloc();
    submenu_add_item(app->advanced, "Set usage %", SonicareAdvSetPct, sonicare_advanced_cb, app);
    submenu_add_item(app->advanced, "Set usage min", SonicareAdvSetMin, sonicare_advanced_cb, app);
    submenu_add_item(
        app->advanced, "Password calc", SonicareAdvPwdCalc, sonicare_advanced_cb, app);
    view_set_previous_callback(submenu_get_view(app->advanced), sonicare_prev_submenu);
    view_dispatcher_add_view(
        app->view_dispatcher, SonicareViewAdvanced, submenu_get_view(app->advanced));

    app->work = view_alloc();
    view_allocate_model(app->work, ViewModelTypeLocking, sizeof(SonicareWorkModel));
    view_set_context(app->work, app);
    view_set_draw_callback(app->work, sonicare_work_draw);
    view_set_input_callback(app->work, sonicare_work_input);
    view_dispatcher_add_view(app->view_dispatcher, SonicareViewWork, app->work);

    app->number = number_input_alloc();
    view_set_previous_callback(number_input_get_view(app->number), sonicare_prev_advanced);
    view_dispatcher_add_view(
        app->view_dispatcher, SonicareViewNumber, number_input_get_view(app->number));

    app->byte_input = byte_input_alloc();
    view_set_previous_callback(byte_input_get_view(app->byte_input), sonicare_prev_advanced);
    view_dispatcher_add_view(
        app->view_dispatcher, SonicareViewByteInput, byte_input_get_view(app->byte_input));

    app->text_input = text_input_alloc();
    view_set_previous_callback(text_input_get_view(app->text_input), sonicare_prev_advanced);
    view_dispatcher_add_view(
        app->view_dispatcher, SonicareViewTextInput, text_input_get_view(app->text_input));

    view_dispatcher_switch_to_view(app->view_dispatcher, SonicareViewSubmenu);
    view_dispatcher_run(app->view_dispatcher);

    // cleanup
    furi_timer_stop(app->timer);
    sonicare_poller_stop(app->poller);
    view_dispatcher_remove_view(app->view_dispatcher, SonicareViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, SonicareViewAdvanced);
    view_dispatcher_remove_view(app->view_dispatcher, SonicareViewWork);
    view_dispatcher_remove_view(app->view_dispatcher, SonicareViewNumber);
    view_dispatcher_remove_view(app->view_dispatcher, SonicareViewByteInput);
    view_dispatcher_remove_view(app->view_dispatcher, SonicareViewTextInput);
    submenu_free(app->submenu);
    submenu_free(app->advanced);
    view_free(app->work);
    number_input_free(app->number);
    byte_input_free(app->byte_input);
    text_input_free(app->text_input);
    view_dispatcher_free(app->view_dispatcher);
    furi_timer_free(app->timer);
    sonicare_poller_free(app->poller);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
