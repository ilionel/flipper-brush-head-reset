#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <notification/notification_messages.h>

#include "sonicare_i.h"
#include "sonicare_poller.h"

// Philips Sonicare BrushSync (NTAG213) brush-head wear-counter reader/resetter.
// The wear counter is brushing-time-in-seconds at page 0x24 (bytes 0-1, LE16); a fresh
// head reads 0. The per-head write password is computed from UID + MFG (no checksum on
// the counter, password is handle-independent). Reset = authenticate + write 00 00 02 00.

typedef enum {
    SonicareViewSubmenu,
    SonicareViewWork,
} SonicareViewId;

typedef enum {
    SonicareMenuRead,
    SonicareMenuReset,
    SonicareMenuAbout,
} SonicareMenuItem;

typedef enum {
    SonicareScreenConfirm, // reset confirmation
    SonicareScreenScanning, // waiting for / talking to the tag
    SonicareScreenResult, // outcome
    SonicareScreenAbout,
} SonicareScreen;

typedef enum {
    SonicareCustomEventDone = 100,
} SonicareCustomEvent;

typedef struct {
    SonicareScreen screen;
    SonicareOp op;
    SonicareResult res;
    uint8_t anim;
} SonicareWorkModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    View* work;
    NotificationApp* notifications;
    SonicarePoller* poller;
    FuriTimer* timer;
    SonicareResult result; // filled by the poller thread
    SonicareOp op;
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

    if(m->screen == SonicareScreenAbout) {
        canvas_draw_str(canvas, 2, 11, "Sonicare Reset");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 26, "BrushSync NTAG213 wear");
        canvas_draw_str(canvas, 2, 36, "counter reader / reset.");
        canvas_draw_str(canvas, 2, 48, "Password from UID+MFG.");
        canvas_draw_str(canvas, 2, 63, "Back");
        return;
    }

    if(m->screen == SonicareScreenConfirm) {
        canvas_draw_str(canvas, 2, 11, "Reset to new");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 26, "Sets wear counter to 0%.");
        canvas_draw_str(canvas, 2, 36, "Computes the head's");
        canvas_draw_str(canvas, 2, 46, "password (safe, 1 auth).");
        canvas_draw_str(canvas, 2, 63, "OK: scan & reset   Back");
        return;
    }

    if(m->screen == SonicareScreenScanning) {
        canvas_draw_str(canvas, 2, 11, m->op == SonicareOpReset ? "Reset head" : "Read head");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 30, "Apply brush-head base");
        canvas_draw_str(canvas, 2, 40, "to the Flipper back");
        sonicare_draw_spinner(canvas, 118, 35, m->anim);
        canvas_draw_str(canvas, 2, 63, "Back: cancel");
        return;
    }

    // SonicareScreenResult
    SonicareResult* r = &m->res;
    canvas_draw_str(canvas, 2, 11, m->op == SonicareOpReset ? "Reset result" : "Brush head");
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

    char line[28];
    snprintf(line, sizeof(line), "Model: %s", r->mfg);
    canvas_draw_str(canvas, 2, 22, line);

    unsigned pct = (100u * r->seconds) / SONICARE_LIFE_SECONDS;
    snprintf(line, sizeof(line), "Used: %u%%  (%u min)", pct, r->seconds / 60u);
    canvas_draw_str(canvas, 2, 32, line);

    snprintf(
        line, sizeof(line), "PWD: %02X%02X%02X%02X", r->pwd[0], r->pwd[1], r->pwd[2], r->pwd[3]);
    canvas_draw_str(canvas, 2, 42, line);

    if(m->op == SonicareOpReset) {
        const char* st = r->verify_ok    ? "RESET OK -> 0%" :
                         r->write_ok      ? "Written (verify?)" :
                         r->auth_ok       ? "Write failed" :
                                            "Auth failed";
        canvas_draw_str(canvas, 2, 54, st);
    } else {
        snprintf(
            line,
            sizeof(line),
            "UID:%02X%02X%02X%02X%02X%02X%02X",
            r->uid[0],
            r->uid[1],
            r->uid[2],
            r->uid[3],
            r->uid[4],
            r->uid[5],
            r->uid[6]);
        canvas_draw_str(canvas, 2, 54, line);
    }
    canvas_draw_str(canvas, 104, 63, "Back");
}

/* --------------------------------- worker glue ------------------------------ */

// runs in the poller thread
static void sonicare_on_done(void* context) {
    SonicareApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, SonicareCustomEventDone);
}

static void sonicare_timer_cb(void* context) {
    SonicareApp* app = context;
    with_view_model(app->work, SonicareWorkModel * m, { m->anim++; }, true);
}

static void sonicare_start_scan(SonicareApp* app, SonicareOp op) {
    app->op = op;
    with_view_model(
        app->work,
        SonicareWorkModel * m,
        {
            m->screen = SonicareScreenScanning;
            m->op = op;
            m->anim = 0;
            memset(&m->res, 0, sizeof(m->res));
        },
        true);
    view_dispatcher_switch_to_view(app->view_dispatcher, SonicareViewWork);
    sonicare_poller_start(app->poller, op, &app->result, sonicare_on_done, app);
    furi_timer_start(app->timer, furi_ms_to_ticks(150));
}

static bool sonicare_custom_event_cb(void* context, uint32_t event) {
    SonicareApp* app = context;
    if(event != SonicareCustomEventDone) return false;

    furi_timer_stop(app->timer);
    sonicare_poller_stop(app->poller); // joins the poller thread -> result is stable

    SonicareResult* r = &app->result;
    bool good = r->present && r->valid &&
                (app->op == SonicareOpRead || (r->did_reset && r->verify_ok));
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

/* --------------------------------- input / menu ----------------------------- */

static bool sonicare_work_input(InputEvent* event, void* context) {
    SonicareApp* app = context;
    if(event->type != InputTypeShort) return false;

    SonicareScreen screen = SonicareScreenResult;
    with_view_model(
        app->work, SonicareWorkModel * m, { screen = m->screen; }, false);

    if(event->key == InputKeyBack) {
        if(screen == SonicareScreenScanning) {
            furi_timer_stop(app->timer);
            sonicare_poller_stop(app->poller);
        }
        view_dispatcher_switch_to_view(app->view_dispatcher, SonicareViewSubmenu);
        return true;
    }
    if(event->key == InputKeyOk && screen == SonicareScreenConfirm) {
        sonicare_start_scan(app, SonicareOpReset);
        return true;
    }
    return false;
}

static void sonicare_submenu_cb(void* context, uint32_t index) {
    SonicareApp* app = context;
    switch(index) {
    case SonicareMenuRead:
        sonicare_start_scan(app, SonicareOpRead);
        break;
    case SonicareMenuReset:
        app->op = SonicareOpReset;
        with_view_model(
            app->work,
            SonicareWorkModel * m,
            {
                m->screen = SonicareScreenConfirm;
                m->op = SonicareOpReset;
            },
            true);
        view_dispatcher_switch_to_view(app->view_dispatcher, SonicareViewWork);
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
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->poller = sonicare_poller_alloc();
    app->timer = furi_timer_alloc(sonicare_timer_cb, FuriTimerTypePeriodic, app);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, sonicare_custom_event_cb);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, sonicare_nav_cb);

    app->submenu = submenu_alloc();
    submenu_add_item(
        app->submenu, "Read brush head", SonicareMenuRead, sonicare_submenu_cb, app);
    submenu_add_item(app->submenu, "Reset to new", SonicareMenuReset, sonicare_submenu_cb, app);
    submenu_add_item(app->submenu, "About", SonicareMenuAbout, sonicare_submenu_cb, app);
    view_dispatcher_add_view(
        app->view_dispatcher, SonicareViewSubmenu, submenu_get_view(app->submenu));

    app->work = view_alloc();
    view_allocate_model(app->work, ViewModelTypeLocking, sizeof(SonicareWorkModel));
    view_set_context(app->work, app);
    view_set_draw_callback(app->work, sonicare_work_draw);
    view_set_input_callback(app->work, sonicare_work_input);
    view_dispatcher_add_view(app->view_dispatcher, SonicareViewWork, app->work);

    view_dispatcher_switch_to_view(app->view_dispatcher, SonicareViewSubmenu);
    view_dispatcher_run(app->view_dispatcher);

    // cleanup
    furi_timer_stop(app->timer);
    sonicare_poller_stop(app->poller);
    view_dispatcher_remove_view(app->view_dispatcher, SonicareViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, SonicareViewWork);
    submenu_free(app->submenu);
    view_free(app->work);
    view_dispatcher_free(app->view_dispatcher);
    furi_timer_free(app->timer);
    sonicare_poller_free(app->poller);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
