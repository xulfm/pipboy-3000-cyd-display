// ============================================================
// Vault 89 PDI-Boy -- Full-page Weather Forecast
// Launched from the clock screen by tapping the weather strip.
// Tap anywhere on this screen (except the lower-left "TAP TO
// EXIT" label) to return to the clock. The exit label performs
// a full exit from clock mode back to where we came from.
// ============================================================
#include <Arduino.h>
#include <lvgl.h>
#include "pipboy_data.h"
#include "weather.h"

extern void weather_to_clock(void);   // defined in main.cpp
extern void weather_mode_exit(void);  // defined in main.cpp

static lv_obj_t*   s_scr         = NULL;
static lv_obj_t*   s_title_lbl   = NULL;
static lv_obj_t*   s_city_lbl    = NULL;
static lv_obj_t*   s_cur_temp    = NULL;
static lv_obj_t*   s_cur_cond    = NULL;
static lv_obj_t*   s_sun_lbl     = NULL;
static lv_obj_t*   s_alert_lbl   = NULL;
static lv_obj_t*   s_day_box[WEATHER_DAILY_SLOTS] = { NULL };
static lv_obj_t*   s_day_name[WEATHER_DAILY_SLOTS] = { NULL };
static lv_obj_t*   s_day_hi[WEATHER_DAILY_SLOTS]   = { NULL };
static lv_obj_t*   s_day_lo[WEATHER_DAILY_SLOTS]   = { NULL };
static lv_obj_t*   s_day_code[WEATHER_DAILY_SLOTS] = { NULL };
static lv_timer_t* s_refresh_tmr = NULL;

// ── Tap handlers ──

// Click on the lower-left exit label: bubble-block so the background tap
// handler doesn't also fire, then actually exit clock mode.
static void exit_click_cb(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    weather_mode_exit();
}

// Click anywhere else on the screen returns to the clock.
static void back_click_cb(lv_event_t* e) {
    (void)e;
    weather_to_clock();
}

// ── Refresh ──

static const char* day_label_for(int slot) {
    if (slot == 0) return "TODAY";
    if (slot == 1) return "TOMRW";
    return "DAY+2";
}

static void refresh_cb(lv_timer_t* t) {
    (void)t;

    if (s_city_lbl) {
        if (g_weather.located && g_weather.city[0]) {
            lv_label_set_text(s_city_lbl, g_weather.city);
        } else {
            lv_label_set_text(s_city_lbl, "LOCATING...");
        }
    }

    if (!g_weather.valid) {
        if (s_cur_temp) lv_label_set_text(s_cur_temp, "--F");
        if (s_cur_cond) lv_label_set_text(s_cur_cond,
            g_weather.fetch_in_progress ? "FETCHING..." : "NO DATA");
        if (s_sun_lbl)  lv_label_set_text(s_sun_lbl, "SR --:--  SS --:--");
        if (s_alert_lbl) {
            lv_label_set_text(s_alert_lbl, "");
            lv_obj_add_flag(s_alert_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        for (int i = 0; i < WEATHER_DAILY_SLOTS; i++) {
            if (s_day_hi[i])   lv_label_set_text(s_day_hi[i], "--");
            if (s_day_lo[i])   lv_label_set_text(s_day_lo[i], "--");
            if (s_day_code[i]) lv_label_set_text(s_day_code[i], "--");
        }
        return;
    }

    // Current
    if (s_cur_temp) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%dF", g_weather.current_f);
        lv_label_set_text(s_cur_temp, buf);
    }
    if (s_cur_cond) {
        lv_label_set_text(s_cur_cond, weather_code_long(g_weather.current_code));
    }
    if (s_sun_lbl) {
        const WeatherDay& d = g_weather.daily[0];
        char buf[48];
        char sr[12], ss[12];
        weather_format_time(sr, sizeof(sr), d.sr_h, d.sr_m);
        weather_format_time(ss, sizeof(ss), d.ss_h, d.ss_m);
        snprintf(buf, sizeof(buf), "SR %s  SS %s", sr, ss);
        lv_label_set_text(s_sun_lbl, buf);
    }
    if (s_alert_lbl) {
        if (g_weather.has_alert) {
            lv_label_set_text(s_alert_lbl, g_weather.alert);
            lv_obj_clear_flag(s_alert_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(s_alert_lbl, "");
            lv_obj_add_flag(s_alert_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Daily cards
    for (int i = 0; i < WEATHER_DAILY_SLOTS; i++) {
        const WeatherDay& d = g_weather.daily[i];
        if (s_day_hi[i]) {
            char buf[8];
            snprintf(buf, sizeof(buf), "HI %d", d.hi_f);
            lv_label_set_text(s_day_hi[i], buf);
        }
        if (s_day_lo[i]) {
            char buf[8];
            snprintf(buf, sizeof(buf), "LO %d", d.lo_f);
            lv_label_set_text(s_day_lo[i], buf);
        }
        if (s_day_code[i]) {
            lv_label_set_text(s_day_code[i], weather_code_short(d.code));
        }
    }
}

// ── Screen construction ──

lv_obj_t* screen_weather_create(void) {
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, CLR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, back_click_cb, LV_EVENT_CLICKED, NULL);

    // Top banner
    lv_obj_t* banner = lv_obj_create(scr);
    lv_obj_set_size(banner, 320, 22);
    lv_obj_set_pos(banner, 0, 0);
    lv_obj_set_style_bg_color(banner, CLR_HEADER, 0);
    lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(banner, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(banner, CLR_GREEN, 0);
    lv_obj_set_style_border_width(banner, 1, 0);
    lv_obj_set_style_radius(banner, 0, 0);
    lv_obj_set_style_pad_all(banner, 0, 0);
    lv_obj_clear_flag(banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(banner, LV_OBJ_FLAG_CLICKABLE);

    s_title_lbl = lv_label_create(banner);
    lv_label_set_text(s_title_lbl, "** WEATHER STATION 89 **");
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_title_lbl, CLR_GREEN, 0);
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, 0);

    // City / location line
    s_city_lbl = lv_label_create(scr);
    lv_label_set_text(s_city_lbl, "LOCATING...");
    lv_obj_set_style_text_font(s_city_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_city_lbl, CLR_GOLD, 0);
    lv_obj_align(s_city_lbl, LV_ALIGN_TOP_MID, 0, 26);

    // Current conditions panel (big temperature on left, condition on right)
    lv_obj_t* cur = lv_obj_create(scr);
    lv_obj_set_size(cur, 300, 72);
    lv_obj_set_pos(cur, 10, 42);
    lv_obj_set_style_bg_color(cur, CLR_HEADER, 0);
    lv_obj_set_style_bg_opa(cur, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(cur, CLR_GREEN, 0);
    lv_obj_set_style_border_width(cur, 2, 0);
    lv_obj_set_style_radius(cur, 4, 0);
    lv_obj_set_style_pad_all(cur, 4, 0);
    lv_obj_clear_flag(cur, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(cur, LV_OBJ_FLAG_CLICKABLE);

    s_cur_temp = lv_label_create(cur);
    lv_label_set_text(s_cur_temp, "--F");
    lv_obj_set_style_text_font(s_cur_temp, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_cur_temp, CLR_GREEN, 0);
    lv_obj_align(s_cur_temp, LV_ALIGN_LEFT_MID, 8, -4);

    s_cur_cond = lv_label_create(cur);
    lv_label_set_text(s_cur_cond, "...");
    lv_obj_set_style_text_font(s_cur_cond, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_cur_cond, CLR_GOLD, 0);
    lv_obj_align(s_cur_cond, LV_ALIGN_RIGHT_MID, -8, -10);

    s_sun_lbl = lv_label_create(cur);
    lv_label_set_text(s_sun_lbl, "SR --:--  SS --:--");
    lv_obj_set_style_text_font(s_sun_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_sun_lbl, CLR_DIM, 0);
    lv_obj_align(s_sun_lbl, LV_ALIGN_RIGHT_MID, -8, 12);

    // Alert banner (between current and daily)
    s_alert_lbl = lv_label_create(scr);
    lv_label_set_text(s_alert_lbl, "");
    lv_obj_set_style_text_font(s_alert_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_alert_lbl, CLR_GOLD, 0);
    lv_obj_align(s_alert_lbl, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_add_flag(s_alert_lbl, LV_OBJ_FLAG_HIDDEN);

    // 3-day forecast cards
    const int CARD_W = 96;
    const int CARD_H = 74;
    const int CARD_Y = 138;
    const int GAP    = 6;
    const int TOTAL  = CARD_W * 3 + GAP * 2;
    const int X0     = (320 - TOTAL) / 2;

    for (int i = 0; i < WEATHER_DAILY_SLOTS; i++) {
        int x = X0 + i * (CARD_W + GAP);
        lv_obj_t* card = lv_obj_create(scr);
        lv_obj_set_size(card, CARD_W, CARD_H);
        lv_obj_set_pos(card, x, CARD_Y);
        lv_obj_set_style_bg_color(card, CLR_HEADER, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, CLR_GREEN, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_radius(card, 3, 0);
        lv_obj_set_style_pad_all(card, 2, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
        s_day_box[i] = card;

        lv_obj_t* name = lv_label_create(card);
        lv_label_set_text(name, day_label_for(i));
        lv_obj_set_style_text_font(name, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(name, CLR_GOLD, 0);
        lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 0);
        s_day_name[i] = name;

        lv_obj_t* code = lv_label_create(card);
        lv_label_set_text(code, "--");
        lv_obj_set_style_text_font(code, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(code, CLR_GREEN, 0);
        lv_obj_align(code, LV_ALIGN_TOP_MID, 0, 14);
        s_day_code[i] = code;

        lv_obj_t* hi = lv_label_create(card);
        lv_label_set_text(hi, "HI --");
        lv_obj_set_style_text_font(hi, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(hi, CLR_GREEN, 0);
        lv_obj_align(hi, LV_ALIGN_BOTTOM_MID, 0, -12);
        s_day_hi[i] = hi;

        lv_obj_t* lo = lv_label_create(card);
        lv_label_set_text(lo, "LO --");
        lv_obj_set_style_text_font(lo, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(lo, CLR_DIM, 0);
        lv_obj_align(lo, LV_ALIGN_BOTTOM_MID, 0, -2);
        s_day_lo[i] = lo;
    }

    // Lower-left EXIT label (the ONLY way to leave clock mode from here)
    lv_obj_t* hint = lv_label_create(scr);
    lv_label_set_text(hint, "TAP TO EXIT");
    lv_obj_set_style_text_font(hint, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(hint, CLR_DARK, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 6, -6);
    lv_obj_add_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    // Bigger touch area so it's easy to hit without missing
    lv_obj_set_ext_click_area(hint, 14);
    lv_obj_add_event_cb(hint, exit_click_cb, LV_EVENT_CLICKED, NULL);

    // Bottom-right hint: tap to go back to clock
    lv_obj_t* back_hint = lv_label_create(scr);
    lv_label_set_text(back_hint, "TAP TO RETURN");
    lv_obj_set_style_text_font(back_hint, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(back_hint, CLR_DARK, 0);
    lv_obj_align(back_hint, LV_ALIGN_BOTTOM_RIGHT, -6, -6);

    // Initial paint + 1s refresh (picks up background fetches)
    refresh_cb(NULL);
    s_refresh_tmr = lv_timer_create(refresh_cb, 1000, NULL);

    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        (void)e;
        if (s_refresh_tmr) { lv_timer_del(s_refresh_tmr); s_refresh_tmr = NULL; }
        s_title_lbl = s_city_lbl = s_cur_temp = s_cur_cond = NULL;
        s_sun_lbl = s_alert_lbl = NULL;
        for (int i = 0; i < WEATHER_DAILY_SLOTS; i++) {
            s_day_box[i] = s_day_name[i] = NULL;
            s_day_hi[i] = s_day_lo[i] = s_day_code[i] = NULL;
        }
        s_scr = NULL;
    }, LV_EVENT_DELETE, NULL);

    s_scr = scr;
    return scr;
}
