// ============================================================
// Vault 89 PDI-Boy -- Stopwatch + Lap Timer
// Local (non-persistent) stopwatch. Keeps a ring of the last
// 6 laps. Millisecond resolution.
// ============================================================
#include <Arduino.h>
#include <lvgl.h>
#include "pipboy_data.h"

extern void apps_open_hub(void);

#define SW_MAX_LAPS 6

static lv_obj_t*   s_scr        = NULL;
static lv_obj_t*   s_time_lbl   = NULL;
static lv_obj_t*   s_start_lbl  = NULL;
static lv_obj_t*   s_lap_lbls[SW_MAX_LAPS] = { NULL };
static lv_timer_t* s_tmr        = NULL;

static bool     s_running  = false;
static uint32_t s_start_ms = 0;
static uint32_t s_accum_ms = 0;          // accumulated from prior runs (pause)
static uint32_t s_last_lap_ms = 0;       // absolute ms at the last lap snapshot
static uint32_t s_laps[SW_MAX_LAPS] = {0};
static int      s_lap_count = 0;

static uint32_t elapsed_ms(void) {
    if (s_running) return s_accum_ms + (millis() - s_start_ms);
    return s_accum_ms;
}

static void fmt_time(uint32_t ms, char* out, size_t cap) {
    uint32_t m = ms / 60000;
    uint32_t s = (ms / 1000) % 60;
    uint32_t cs = (ms / 10) % 100;
    snprintf(out, cap, "%02u:%02u.%02u", m, s, cs);
}

static void refresh_cb(lv_timer_t* t) {
    (void)t;
    if (s_time_lbl) {
        char buf[16];
        fmt_time(elapsed_ms(), buf, sizeof(buf));
        lv_label_set_text(s_time_lbl, buf);
    }
    if (s_start_lbl) {
        lv_label_set_text(s_start_lbl, s_running ? "STOP" : "START");
    }
}

static void start_cb(lv_event_t* e) {
    (void)e;
    if (s_running) {
        s_accum_ms += millis() - s_start_ms;
        s_running = false;
    } else {
        s_start_ms = millis();
        s_running = true;
    }
    refresh_cb(NULL);
}

static void lap_cb(lv_event_t* e) {
    (void)e;
    if (!s_running && s_accum_ms == 0) return;
    uint32_t now_total = elapsed_ms();
    uint32_t diff = now_total - s_last_lap_ms;
    s_last_lap_ms = now_total;
    // Shift old laps down, newest at index 0
    int keep = s_lap_count;
    if (keep >= SW_MAX_LAPS) keep = SW_MAX_LAPS - 1;
    for (int i = keep; i > 0; i--) s_laps[i] = s_laps[i - 1];
    s_laps[0] = diff;
    if (s_lap_count < SW_MAX_LAPS) s_lap_count++;
    // Update lap labels
    for (int i = 0; i < SW_MAX_LAPS; i++) {
        if (!s_lap_lbls[i]) continue;
        if (i < s_lap_count) {
            char buf[24];
            char tb[16];
            fmt_time(s_laps[i], tb, sizeof(tb));
            snprintf(buf, sizeof(buf), "L%d  %s", s_lap_count - i, tb);
            lv_label_set_text(s_lap_lbls[i], buf);
        } else {
            lv_label_set_text(s_lap_lbls[i], "");
        }
    }
}

static void reset_cb(lv_event_t* e) {
    (void)e;
    s_running     = false;
    s_accum_ms    = 0;
    s_last_lap_ms = 0;
    s_lap_count   = 0;
    for (int i = 0; i < SW_MAX_LAPS; i++) {
        s_laps[i] = 0;
        if (s_lap_lbls[i]) lv_label_set_text(s_lap_lbls[i], "");
    }
    refresh_cb(NULL);
}

static void back_cb(lv_event_t* e) {
    (void)e;
    apps_open_hub();
}

static lv_obj_t* mk_btn(lv_obj_t* par, const char* txt, int x, int y, int w, int h,
                        lv_event_cb_t cb, lv_obj_t** out_lbl = NULL) {
    lv_obj_t* b = lv_obj_create(par);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, CLR_HEADER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, CLR_GREEN, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 2, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(l, CLR_GREEN, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
    if (out_lbl) *out_lbl = l;
    return b;
}

lv_obj_t* screen_stopwatch_create(void) {
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, CLR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, 320, 22);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, CLR_HEADER, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(hdr, CLR_GREEN, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* ht = lv_label_create(hdr);
    lv_label_set_text(ht, "** CHRONO / LAP TIMER **");
    lv_obj_set_style_text_font(ht, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(ht, CLR_GREEN, 0);
    lv_obj_align(ht, LV_ALIGN_CENTER, 0, 0);

    // Big time
    s_time_lbl = lv_label_create(scr);
    lv_label_set_text(s_time_lbl, "00:00.00");
    lv_obj_set_style_text_font(s_time_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_time_lbl, CLR_GREEN, 0);
    lv_obj_align(s_time_lbl, LV_ALIGN_TOP_MID, 0, 28);

    // Lap list (right half)
    for (int i = 0; i < SW_MAX_LAPS; i++) {
        s_lap_lbls[i] = lv_label_create(scr);
        lv_label_set_text(s_lap_lbls[i], "");
        lv_obj_set_style_text_font(s_lap_lbls[i], &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(s_lap_lbls[i],
                                    i == 0 ? CLR_GOLD : CLR_DIM, 0);
        lv_obj_set_pos(s_lap_lbls[i], 180, 90 + i * 12);
    }

    // Buttons
    mk_btn(scr, "START",  14, 170, 80, 30, start_cb, &s_start_lbl);
    mk_btn(scr, "LAP",   104, 170, 60, 30, lap_cb);
    mk_btn(scr, "RESET", 174, 170, 60, 30, reset_cb);
    mk_btn(scr, "< BACK",  6, 208, 80, 26, back_cb);

    refresh_cb(NULL);
    s_tmr = lv_timer_create(refresh_cb, 50, NULL);

    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        (void)e;
        if (s_tmr) { lv_timer_del(s_tmr); s_tmr = NULL; }
        s_scr = s_time_lbl = s_start_lbl = NULL;
        for (int i = 0; i < SW_MAX_LAPS; i++) s_lap_lbls[i] = NULL;
    }, LV_EVENT_DELETE, NULL);

    s_scr = scr;
    return scr;
}
