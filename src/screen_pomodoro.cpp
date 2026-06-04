// ============================================================
// Vault 89 PDI-Boy -- Pomodoro Timer
// Big countdown + phase indicator. Start / Pause / Reset.
// Backend (apps_state.cpp) keeps the timer running even if
// you navigate away from this screen.
// ============================================================
#include <Arduino.h>
#include <lvgl.h>
#include "pipboy_data.h"
#include "apps.h"

extern void apps_open_hub(void);   // back to apps hub

static lv_obj_t*   s_scr       = NULL;
static lv_obj_t*   s_time_lbl  = NULL;
static lv_obj_t*   s_phase_lbl = NULL;
static lv_obj_t*   s_cycle_lbl = NULL;
static lv_obj_t*   s_start_btn = NULL;
static lv_obj_t*   s_start_lbl = NULL;
static lv_obj_t*   s_bar_fill  = NULL;
static lv_timer_t* s_tmr       = NULL;

static const char* phase_text(PomoPhase p) {
    switch (p) {
        case POMO_IDLE:    return "READY";
        case POMO_WORK:    return ">> FOCUS <<";
        case POMO_BREAK:   return "-- BREAK --";
        case POMO_LONGBRK: return "== LONG BREAK ==";
    }
    return "?";
}

static lv_color_t phase_color(PomoPhase p) {
    switch (p) {
        case POMO_WORK:    return CLR_GREEN;
        case POMO_BREAK:   return CLR_GOLD;
        case POMO_LONGBRK: return CLR_GOLD;
        default:           return CLR_DIM;
    }
}

static void refresh_cb(lv_timer_t* t) {
    (void)t;
    uint32_t rem_ms = pomo_remaining_ms();
    uint32_t rem_s  = rem_ms / 1000;
    if (g_pomo.phase == POMO_IDLE && !g_pomo.running) {
        rem_s = g_pomo.work_len_s;
    }
    if (s_time_lbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%02u:%02u",
                 rem_s / 60, rem_s % 60);
        lv_label_set_text(s_time_lbl, buf);
        lv_obj_set_style_text_color(s_time_lbl, phase_color(g_pomo.phase), 0);
    }
    if (s_phase_lbl) {
        lv_label_set_text(s_phase_lbl, phase_text(g_pomo.phase));
        lv_obj_set_style_text_color(s_phase_lbl, phase_color(g_pomo.phase), 0);
    }
    if (s_cycle_lbl) {
        char cbuf[32];
        snprintf(cbuf, sizeof(cbuf), "CYCLE #%u   W:%um B:%um",
                 g_pomo.cycle_count,
                 g_pomo.work_len_s / 60, g_pomo.break_len_s / 60);
        lv_label_set_text(s_cycle_lbl, cbuf);
    }
    if (s_start_lbl) {
        if (g_pomo.phase == POMO_IDLE)  lv_label_set_text(s_start_lbl, "START");
        else if (g_pomo.running)        lv_label_set_text(s_start_lbl, "PAUSE");
        else                            lv_label_set_text(s_start_lbl, "RESUME");
    }
    if (s_bar_fill) {
        // Visual progress bar 0..100% of current phase
        int w = 0;
        if (g_pomo.phase_len_ms > 0) {
            uint32_t done_ms = g_pomo.phase_len_ms - rem_ms;
            w = (int)((done_ms * 280) / g_pomo.phase_len_ms);
        }
        if (w < 0)   w = 0;
        if (w > 280) w = 280;
        lv_obj_set_width(s_bar_fill, w);
        lv_obj_set_style_bg_color(s_bar_fill, phase_color(g_pomo.phase), 0);
    }
}

static void start_cb(lv_event_t* e) {
    (void)e;
    pomo_toggle_pause();
    refresh_cb(NULL);
}
static void reset_cb(lv_event_t* e) {
    (void)e;
    pomo_reset();
    refresh_cb(NULL);
}
static void back_cb(lv_event_t* e) {
    (void)e;
    apps_open_hub();
}

// Adjust work/break lengths in 5-minute steps
static void work_up_cb(lv_event_t* e) {
    (void)e;
    if (g_pomo.work_len_s < 60 * 60) g_pomo.work_len_s += 5 * 60;
    refresh_cb(NULL);
}
static void work_dn_cb(lv_event_t* e) {
    (void)e;
    if (g_pomo.work_len_s > 5 * 60) g_pomo.work_len_s -= 5 * 60;
    refresh_cb(NULL);
}

static lv_obj_t* mk_btn(lv_obj_t* par, const char* txt, int x, int y, int w, int h,
                        lv_event_cb_t cb) {
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
    return b;
}

lv_obj_t* screen_pomodoro_create(void) {
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, CLR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Header
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
    lv_label_set_text(ht, "** POMODORO TIMER **");
    lv_obj_set_style_text_font(ht, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(ht, CLR_GREEN, 0);
    lv_obj_align(ht, LV_ALIGN_CENTER, 0, 0);

    // Phase label
    s_phase_lbl = lv_label_create(scr);
    lv_label_set_text(s_phase_lbl, "READY");
    lv_obj_set_style_text_font(s_phase_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_phase_lbl, CLR_GOLD, 0);
    lv_obj_align(s_phase_lbl, LV_ALIGN_TOP_MID, 0, 28);

    // Big countdown
    s_time_lbl = lv_label_create(scr);
    lv_label_set_text(s_time_lbl, "25:00");
    lv_obj_set_style_text_font(s_time_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_time_lbl, CLR_GREEN, 0);
    lv_obj_align(s_time_lbl, LV_ALIGN_TOP_MID, 0, 46);

    // Progress bar (border + fill)
    lv_obj_t* bar_bg = lv_obj_create(scr);
    lv_obj_set_size(bar_bg, 280, 10);
    lv_obj_set_pos(bar_bg, 20, 112);
    lv_obj_set_style_bg_opa(bar_bg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(bar_bg, CLR_GREEN, 0);
    lv_obj_set_style_border_width(bar_bg, 1, 0);
    lv_obj_set_style_radius(bar_bg, 1, 0);
    lv_obj_set_style_pad_all(bar_bg, 0, 0);
    lv_obj_clear_flag(bar_bg, LV_OBJ_FLAG_SCROLLABLE);

    s_bar_fill = lv_obj_create(bar_bg);
    lv_obj_set_size(s_bar_fill, 0, 8);
    lv_obj_set_pos(s_bar_fill, 0, 0);
    lv_obj_set_style_bg_color(s_bar_fill, CLR_GREEN, 0);
    lv_obj_set_style_bg_opa(s_bar_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bar_fill, 0, 0);
    lv_obj_set_style_radius(s_bar_fill, 0, 0);
    lv_obj_set_style_pad_all(s_bar_fill, 0, 0);
    lv_obj_clear_flag(s_bar_fill, LV_OBJ_FLAG_SCROLLABLE);

    // Cycle info
    s_cycle_lbl = lv_label_create(scr);
    lv_label_set_text(s_cycle_lbl, "CYCLE #0   W:25m B:5m");
    lv_obj_set_style_text_font(s_cycle_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_cycle_lbl, CLR_DIM, 0);
    lv_obj_align(s_cycle_lbl, LV_ALIGN_TOP_MID, 0, 128);

    // Row: START/PAUSE | RESET | W- | W+
    s_start_btn = mk_btn(scr, "START", 20,  150, 80, 30, start_cb);
    s_start_lbl = lv_obj_get_child(s_start_btn, 0);
    mk_btn(scr, "RESET",  110, 150, 60, 30, reset_cb);
    mk_btn(scr, "W -",    180, 150, 50, 30, work_dn_cb);
    mk_btn(scr, "W +",    240, 150, 50, 30, work_up_cb);

    // Back
    mk_btn(scr, "< BACK", 6, 208, 80, 26, back_cb);

    // Hint
    lv_obj_t* hint = lv_label_create(scr);
    lv_label_set_text(hint, "RUNS IN BACKGROUND");
    lv_obj_set_style_text_font(hint, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(hint, CLR_DARK, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_RIGHT, -6, -6);

    refresh_cb(NULL);
    s_tmr = lv_timer_create(refresh_cb, 250, NULL);

    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        (void)e;
        if (s_tmr) { lv_timer_del(s_tmr); s_tmr = NULL; }
        s_scr = s_time_lbl = s_phase_lbl = s_cycle_lbl = NULL;
        s_start_btn = s_start_lbl = s_bar_fill = NULL;
    }, LV_EVENT_DELETE, NULL);

    s_scr = scr;
    return scr;
}
