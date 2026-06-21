#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/byte_input.h>
#include <gui/modules/text_input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

#include "sonicare_i.h"
#include "sonicare_pwd.h"
#include "sonicare_poller.h"

// Philips Sonicare BrushSync (NTAG213) brush-head wear-counter tool.
// Counter = brushing seconds at page 0x24 (LE16); fresh = 0. Per-head write password is
// computed from UID + MFG (handle-independent, no checksum on the counter). v2 adds:
// custom usage set, on-device password calculator, and a saved report.

#define SONICARE_DIR EXT_PATH("apps_data/sonicare")

typedef enum {
    SonicareViewSubmenu,
    SonicareViewWork,
    SonicareViewTuning,
    SonicareViewByteInput,
    SonicareViewTextInput,
} SonicareViewId;

typedef enum {
    SonicareMenuRead,
    SonicareMenuReset,
    SonicareMenuSetUsage,
    SonicareMenuPwdCalc,
    SonicareMenuAbout,
} SonicareMenuItem;

typedef enum {
    SonicareScreenConfirm, // reset confirmation
    SonicareScreenScanning, // talking to the tag
    SonicareScreenResult, // NFC outcome
    SonicareScreenPwdCalc, // manual password calculator result
    SonicareScreenAbout,
} SonicareScreen;

typedef enum {
    SonicareCustomEventDone = 100,
} SonicareCustomEvent;

// percentage presets for "Set usage"
static const uint8_t sonicare_usage_pct[] = {0, 25, 50, 75, 90, 100};
#define SONICARE_USAGE_COUNT (sizeof(sonicare_usage_pct) / sizeof(sonicare_usage_pct[0]))

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
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    View* work;
    VariableItemList* tuning;
    ByteInput* byte_input;
    TextInput* text_input;
    NotificationApp* notifications;
    SonicarePoller* poller;
    FuriTimer* timer;
    SonicareResult result; // filled by the poller thread
    SonicareOp op;
    uint16_t write_seconds;
    uint8_t usage_idx; // selected "Set usage" preset
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
        canvas_draw_str(canvas, 2, 11, "Sonicare Reset");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 26, "BrushSync NTAG213 wear");
        canvas_draw_str(canvas, 2, 36, "counter tool. PWD from");
        canvas_draw_str(canvas, 2, 46, "UID+MFG (handle-indep).");
        canvas_draw_str(canvas, 2, 63, "Back");
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

    if(m->screen == SonicareScreenConfirm) {
        canvas_draw_str(canvas, 2, 11, "Reset to new");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 26, "Sets wear counter to 0%.");
        canvas_draw_str(canvas, 2, 36, "Password is computed");
        canvas_draw_str(canvas, 2, 46, "(safe, single auth).");
        canvas_draw_str(canvas, 2, 63, "OK: scan & reset   Back");
        return;
    }

    if(m->screen == SonicareScreenScanning) {
        canvas_draw_str(canvas, 2, 11, m->op == SonicareOpWrite ? "Write head" : "Read head");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 30, "Apply brush-head base");
        canvas_draw_str(canvas, 2, 40, "to the Flipper back");
        sonicare_draw_spinner(canvas, 118, 35, m->anim);
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
    } else {
        canvas_draw_str(canvas, 2, 54, m->saved ? "Saved to SD" : "OK: save report");
    }
    canvas_draw_str(canvas, 104, 63, "Back");
}

/* --------------------------------- save report ------------------------------ */

static void sonicare_save_report(SonicareApp* app, const SonicareResult* r) {
    storage_simply_mkdir(app->storage, SONICARE_DIR);
    FuriString* path = furi_string_alloc_printf(
        "%s/sonicare_%02X%02X%02X%02X%02X%02X%02X.txt",
        SONICARE_DIR,
        r->uid[0],
        r->uid[1],
        r->uid[2],
        r->uid[3],
        r->uid[4],
        r->uid[5],
        r->uid[6]);
    FuriString* body = furi_string_alloc();
    unsigned pct = (100u * r->seconds) / SONICARE_LIFE_SECONDS;
    furi_string_printf(
        body,
        "Sonicare BrushSync head\nUID: %02X%02X%02X%02X%02X%02X%02X\nMFG: %s\n"
        "PWD: %02X%02X%02X%02X\nUsed: %u s (%u min, %u%%)\n",
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

    with_view_model(
        app->work,
        SonicareWorkModel * m,
        {
            m->screen = SonicareScreenResult;
            m->res = *r;
        },
        true);
    return true;
}

/* --------------------------------- set-usage list --------------------------- */

static void sonicare_usage_change_cb(VariableItem* item) {
    SonicareApp* app = variable_item_get_context(item);
    app->usage_idx = variable_item_get_current_value_index(item);
    char b[8];
    snprintf(b, sizeof(b), "%u%%", sonicare_usage_pct[app->usage_idx]);
    variable_item_set_current_value_text(item, b);
}

static void sonicare_usage_enter_cb(void* context, uint32_t index) {
    UNUSED(index);
    SonicareApp* app = context;
    uint16_t seconds =
        (uint16_t)((uint32_t)sonicare_usage_pct[app->usage_idx] * SONICARE_LIFE_SECONDS / 100u);
    sonicare_start_scan(app, SonicareOpWrite, seconds);
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
    if(event->key == InputKeyOk) {
        if(screen == SonicareScreenConfirm) {
            sonicare_start_scan(app, SonicareOpWrite, 0);
            return true;
        }
        if(screen == SonicareScreenResult && valid_read) {
            sonicare_save_report(app, &app->result);
            with_view_model(app->work, SonicareWorkModel * m, { m->saved = true; }, true);
            return true;
        }
    }
    return false;
}

static void sonicare_submenu_cb(void* context, uint32_t index) {
    SonicareApp* app = context;
    switch(index) {
    case SonicareMenuRead:
        sonicare_start_scan(app, SonicareOpRead, 0);
        break;
    case SonicareMenuReset:
        with_view_model(
            app->work,
            SonicareWorkModel * m,
            {
                m->screen = SonicareScreenConfirm;
                m->op = SonicareOpWrite;
                m->write_seconds = 0;
            },
            true);
        view_dispatcher_switch_to_view(app->view_dispatcher, SonicareViewWork);
        break;
    case SonicareMenuSetUsage:
        view_dispatcher_switch_to_view(app->view_dispatcher, SonicareViewTuning);
        break;
    case SonicareMenuPwdCalc:
        memset(app->calc_uid, 0, sizeof(app->calc_uid));
        byte_input_set_header_text(app->byte_input, "Tag UID (7 bytes)");
        byte_input_set_result_callback(
            app->byte_input, sonicare_byte_input_done, NULL, app, app->calc_uid, 7);
        view_dispatcher_switch_to_view(app->view_dispatcher, SonicareViewByteInput);
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

static uint32_t sonicare_prev_submenu(void* context) {
    UNUSED(context);
    return SonicareViewSubmenu;
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
    submenu_add_item(app->submenu, "Set usage", SonicareMenuSetUsage, sonicare_submenu_cb, app);
    submenu_add_item(app->submenu, "Password calc", SonicareMenuPwdCalc, sonicare_submenu_cb, app);
    submenu_add_item(app->submenu, "About", SonicareMenuAbout, sonicare_submenu_cb, app);
    view_dispatcher_add_view(
        app->view_dispatcher, SonicareViewSubmenu, submenu_get_view(app->submenu));

    app->work = view_alloc();
    view_allocate_model(app->work, ViewModelTypeLocking, sizeof(SonicareWorkModel));
    view_set_context(app->work, app);
    view_set_draw_callback(app->work, sonicare_work_draw);
    view_set_input_callback(app->work, sonicare_work_input);
    view_dispatcher_add_view(app->view_dispatcher, SonicareViewWork, app->work);

    app->tuning = variable_item_list_alloc();
    VariableItem* it = variable_item_list_add(
        app->tuning, "Set to", SONICARE_USAGE_COUNT, sonicare_usage_change_cb, app);
    variable_item_set_current_value_index(it, 0);
    variable_item_set_current_value_text(it, "0%");
    variable_item_list_set_enter_callback(app->tuning, sonicare_usage_enter_cb, app);
    view_set_previous_callback(
        variable_item_list_get_view(app->tuning), sonicare_prev_submenu);
    view_dispatcher_add_view(
        app->view_dispatcher, SonicareViewTuning, variable_item_list_get_view(app->tuning));

    app->byte_input = byte_input_alloc();
    view_set_previous_callback(byte_input_get_view(app->byte_input), sonicare_prev_submenu);
    view_dispatcher_add_view(
        app->view_dispatcher, SonicareViewByteInput, byte_input_get_view(app->byte_input));

    app->text_input = text_input_alloc();
    text_input_set_result_callback(
        app->text_input,
        sonicare_text_input_done,
        app,
        app->calc_mfg,
        sizeof(app->calc_mfg),
        true);
    view_set_previous_callback(text_input_get_view(app->text_input), sonicare_prev_submenu);
    view_dispatcher_add_view(
        app->view_dispatcher, SonicareViewTextInput, text_input_get_view(app->text_input));

    view_dispatcher_switch_to_view(app->view_dispatcher, SonicareViewSubmenu);
    view_dispatcher_run(app->view_dispatcher);

    // cleanup
    furi_timer_stop(app->timer);
    sonicare_poller_stop(app->poller);
    view_dispatcher_remove_view(app->view_dispatcher, SonicareViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, SonicareViewWork);
    view_dispatcher_remove_view(app->view_dispatcher, SonicareViewTuning);
    view_dispatcher_remove_view(app->view_dispatcher, SonicareViewByteInput);
    view_dispatcher_remove_view(app->view_dispatcher, SonicareViewTextInput);
    submenu_free(app->submenu);
    view_free(app->work);
    variable_item_list_free(app->tuning);
    byte_input_free(app->byte_input);
    text_input_free(app->text_input);
    view_dispatcher_free(app->view_dispatcher);
    furi_timer_free(app->timer);
    sonicare_poller_free(app->poller);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
