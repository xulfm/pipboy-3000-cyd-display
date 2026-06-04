// ============================================================
// Vault 89 PDI-Boy -- Flip Clock Mode
// Big digit "flip" card display themed Pip-Boy green.
// Flip animation: a solid green card sweeps down over the digit
// pair, label updates at midpoint, then the card sweeps away.
// No numbers drawn on the flip card itself (per user request).
// ============================================================
#include <Arduino.h>
#include <lvgl.h>
#include <time.h>
#include "pipboy_data.h"
#include "clock_sync.h"
#include "net_state.h"
#include "weather.h"

extern void clock_mode_exit(void);  // defined in main.cpp
extern void clock_to_weather(void); // defined in main.cpp
extern int  read_batt_pct(void);    // defined in main.cpp (-1 = no batt)

// Card geometry — two big pairs (HH and MM) side by side
#define CARD_W      112
#define CARD_H      112
#define CARD_Y       50
#define CARD_GAP     12

static lv_obj_t*   s_clock_scr    = NULL;
static lv_obj_t*   s_hh_card      = NULL;
static lv_obj_t*   s_mm_card      = NULL;
static lv_obj_t*   s_hh_lbl       = NULL;
static lv_obj_t*   s_mm_lbl       = NULL;
static lv_obj_t*   s_colon_lbl    = NULL;
static lv_obj_t*   s_hh_flap      = NULL;
static lv_obj_t*   s_mm_flap      = NULL;
static lv_obj_t*   s_date_lbl     = NULL;
static lv_obj_t*   s_ampm_lbl     = NULL;
static lv_obj_t*   s_status_lbl   = NULL;
// Bottom-right corner: battery icon + percent (replaces the old TZ label)
static lv_obj_t*   s_batt_body    = NULL;
static lv_obj_t*   s_batt_fill    = NULL;
static lv_obj_t*   s_batt_tip     = NULL;
static lv_obj_t*   s_batt_lbl     = NULL;
// Tappable weather strip (above the date line)
static lv_obj_t*   s_wx_strip     = NULL;
static lv_obj_t*   s_wx_temp_lbl  = NULL;
static lv_obj_t*   s_wx_cond_lbl  = NULL;
static lv_obj_t*   s_wx_hilo_lbl  = NULL;
static lv_obj_t*   s_wx_sun_lbl   = NULL;
static lv_obj_t*   s_wx_alert_lbl = NULL;
static lv_timer_t* s_tick_timer   = NULL;

static int s_last_hh = -1;
static int s_last_mm = -1;
static bool s_first_draw = true;

static const char* day_names[]   = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
static const char* month_names[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                    "JUL","AUG","SEP","OCT","NOV","DEC"};

// ── Flap animation ──
//
// The "flap" is a full-height card-sized rectangle with its y position and
// height animated so it looks like a solid bar wiping down across the digit.
// Two phases:
//   Phase A (0 → 50%):  flap grows from the top of the card downward.
//                       At 50% complete, label text is swapped.
//   Phase B (50 → 100%): flap shrinks off the bottom.
// The flap itself is plain green, so no numbers are shown mid-flip.

struct FlapAnimCtx {
    lv_obj_t* flap;
    lv_obj_t* label;
    char      new_text[8];
    bool      text_swapped;
};
static FlapAnimCtx s_hh_ctx = {};
static FlapAnimCtx s_mm_ctx = {};

static void flap_exec_cb(void* var, int32_t v) {
    FlapAnimCtx* ctx = (FlapAnimCtx*)var;
    if (!ctx || !ctx->flap) return;
    // v goes 0 → 200 across the whole animation
    // Phase A: 0..100  → flap grows from y=0 to height CARD_H
    // Phase B: 100..200 → flap shrinks to 0 while its y moves down
    int h, y;
    if (v <= 100) {
        h = (CARD_H * v) / 100;
        y = 0;
    } else {
        int vv = v - 100;
        h = CARD_H - (CARD_H * vv) / 100;
        y = (CARD_H * vv) / 100;
    }
    if (h < 1) h = 1;
    lv_obj_set_height(ctx->flap, h);
    lv_obj_set_y(ctx->flap, y);
    // Swap text right at the midpoint while the flap fully covers the digit
    if (!ctx->text_swapped && v >= 95 && ctx->label) {
        lv_label_set_text(ctx->label, ctx->new_text);
        ctx->text_swapped = true;
    }
}

static void flap_ready_cb(lv_anim_t* a) {
    FlapAnimCtx* ctx = (FlapAnimCtx*)a->var;
    if (ctx && ctx->flap) {
        lv_obj_add_flag(ctx->flap, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(ctx->flap, 1);
        lv_obj_set_y(ctx->flap, 0);
    }
}

static void start_flap(FlapAnimCtx* ctx, const char* new_text) {
    if (!ctx || !ctx->flap || !ctx->label) return;
    strncpy(ctx->new_text, new_text, sizeof(ctx->new_text) - 1);
    ctx->new_text[sizeof(ctx->new_text) - 1] = '\0';
    ctx->text_swapped = false;
    lv_obj_clear_flag(ctx->flap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(ctx->flap, 0);
    lv_obj_set_height(ctx->flap, 1);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ctx);
    lv_anim_set_values(&a, 0, 200);
    lv_anim_set_time(&a, 500);
    lv_anim_set_exec_cb(&a, flap_exec_cb);
    lv_anim_set_ready_cb(&a, flap_ready_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
}

static void tick_cb(lv_timer_t* t) {
    (void)t;
    struct tm tm_now;
    bool ok = clock_get(&tm_now);

    if (!ok) {
        if (s_hh_lbl) lv_label_set_text(s_hh_lbl, "--");
        if (s_mm_lbl) lv_label_set_text(s_mm_lbl, "--");
        if (s_ampm_lbl) lv_label_set_text(s_ampm_lbl, "");
        if (s_date_lbl) lv_label_set_text(s_date_lbl, "AWAITING SYNC");
        if (s_status_lbl) {
            if (g_net.wifi_connected) {
                lv_label_set_text(s_status_lbl, "NTP SYNCING...");
            } else if (g_peer_count > 0) {
                // We're offline but have mesh peers — we'll pick up time
                // from a gateway peer's next MSG_TIME broadcast (<=30s).
                lv_label_set_text(s_status_lbl, "WAITING FOR MESH TIME...");
            } else {
                lv_label_set_text(s_status_lbl, "NO WIFI - TAP GEAR TO SET");
            }
        }
        s_last_hh = s_last_mm = -1;
        s_first_draw = true;
        return;
    }

    bool use24 = clock_get_24h();
    int hh;
    const char* ampm = "";
    if (use24) {
        hh = tm_now.tm_hour;
    } else {
        hh = tm_now.tm_hour % 12;
        if (hh == 0) hh = 12;
        ampm = tm_now.tm_hour < 12 ? "AM" : "PM";
    }
    int mm = tm_now.tm_min;

    char hbuf[4], mbuf[4];
    snprintf(hbuf, sizeof(hbuf), "%02d", hh);
    snprintf(mbuf, sizeof(mbuf), "%02d", mm);

    if (s_first_draw) {
        if (s_hh_lbl) lv_label_set_text(s_hh_lbl, hbuf);
        if (s_mm_lbl) lv_label_set_text(s_mm_lbl, mbuf);
        s_first_draw = false;
    } else {
        if (hh != s_last_hh && s_hh_lbl) start_flap(&s_hh_ctx, hbuf);
        if (mm != s_last_mm && s_mm_lbl) start_flap(&s_mm_ctx, mbuf);
    }
    s_last_hh = hh;
    s_last_mm = mm;

    if (s_ampm_lbl) lv_label_set_text(s_ampm_lbl, ampm);

    char dbuf[32];
    snprintf(dbuf, sizeof(dbuf), "%s  %s %02d  %04d",
             day_names[tm_now.tm_wday], month_names[tm_now.tm_mon],
             tm_now.tm_mday, tm_now.tm_year + 1900);
    if (s_date_lbl) lv_label_set_text(s_date_lbl, dbuf);

    if (s_status_lbl) lv_label_set_text(s_status_lbl, TITLE_CHRONO_STATUS);

    // Update battery readout in the bottom-right corner
    int pct = read_batt_pct();
    if (pct < 0) {
        if (s_batt_lbl)  lv_label_set_text(s_batt_lbl, "NO BAT");
        if (s_batt_fill) lv_obj_add_flag(s_batt_fill, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (s_batt_lbl) {
            char bbuf[8];
            snprintf(bbuf, sizeof(bbuf), "%d%%", pct);
            lv_label_set_text(s_batt_lbl, bbuf);
        }
        if (s_batt_fill) {
            lv_obj_clear_flag(s_batt_fill, LV_OBJ_FLAG_HIDDEN);
            // Fill width: inner area of the 22x10 body is 20x8 (1px border).
            int w = (pct * 18) / 100;
            if (w < 1) w = 1;
            lv_obj_set_width(s_batt_fill, w);
            lv_color_t c = CLR_GREEN;
            if (pct < 20)      c = lv_color_hex(0xCC0000);
            else if (pct < 40) c = CLR_GOLD;
            lv_obj_set_style_bg_color(s_batt_fill, c, 0);
            if (s_batt_lbl) lv_obj_set_style_text_color(s_batt_lbl, c, 0);
        }
    }

    // Update weather strip (picks up background fetches)
    if (s_wx_temp_lbl && s_wx_cond_lbl && s_wx_hilo_lbl && s_wx_sun_lbl) {
        if (!g_weather.valid) {
            lv_label_set_text(s_wx_temp_lbl, "--F");
            lv_label_set_text(s_wx_cond_lbl,
                g_weather.fetch_in_progress ? "FETCHING WX..." :
                (g_weather.located ? "NO DATA" : "LOCATING..."));
            lv_label_set_text(s_wx_hilo_lbl, "H --/L --");
            lv_label_set_text(s_wx_sun_lbl, "SR --:-- SS --:--");
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "%dF", g_weather.current_f);
            lv_label_set_text(s_wx_temp_lbl, buf);
            lv_label_set_text(s_wx_cond_lbl,
                weather_code_short(g_weather.current_code));
            snprintf(buf, sizeof(buf), "H %d/L %d",
                     g_weather.daily[0].hi_f, g_weather.daily[0].lo_f);
            lv_label_set_text(s_wx_hilo_lbl, buf);
            const WeatherDay& d = g_weather.daily[0];
            char sr[12], ss[12];
            weather_format_time(sr, sizeof(sr), d.sr_h, d.sr_m);
            weather_format_time(ss, sizeof(ss), d.ss_h, d.ss_m);
            snprintf(buf, sizeof(buf), "SR %s SS %s", sr, ss);
            lv_label_set_text(s_wx_sun_lbl, buf);
        }
    }
    if (s_wx_alert_lbl) {
        if (g_weather.valid && g_weather.has_alert) {
            lv_label_set_text(s_wx_alert_lbl, g_weather.alert);
            lv_obj_clear_flag(s_wx_alert_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_wx_alert_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Tap handler for the lower-left "TAP TO EXIT" label only.
// Stop bubbling so the weather-strip or background won't also fire.
static void exit_click_cb(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    clock_mode_exit();
}

// Tap handler for the weather strip -> open full-page weather screen.
static void wx_click_cb(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    clock_to_weather();
}

// Helper: make a big card with a centered label inside.
// Returns the card; out_label receives the label pointer.
static lv_obj_t* mk_card(lv_obj_t* par, int x, int y, int w, int h,
                         lv_obj_t** out_label) {
    lv_obj_t* card = lv_obj_create(par);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, CLR_HEADER, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, CLR_GREEN, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 4, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);

    // Midline divider (classic flip-clock look)
    lv_obj_t* mid = lv_obj_create(card);
    lv_obj_set_size(mid, w, 2);
    lv_obj_set_pos(mid, 0, h / 2 - 1);
    lv_obj_set_style_bg_color(mid, CLR_BG, 0);
    lv_obj_set_style_bg_opa(mid, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mid, 0, 0);
    lv_obj_set_style_radius(mid, 0, 0);
    lv_obj_clear_flag(mid, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl = lv_label_create(card);
    lv_label_set_text(lbl, "--");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl, CLR_GREEN, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    *out_label = lbl;
    return card;
}

static lv_obj_t* mk_flap(lv_obj_t* card) {
    lv_obj_t* flap = lv_obj_create(card);
    lv_obj_set_size(flap, CARD_W - 4, 1);
    lv_obj_set_pos(flap, 2, 0);
    lv_obj_set_style_bg_color(flap, CLR_GREEN, 0);
    lv_obj_set_style_bg_opa(flap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(flap, 0, 0);
    lv_obj_set_style_radius(flap, 2, 0);
    lv_obj_set_style_pad_all(flap, 0, 0);
    lv_obj_clear_flag(flap, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(flap, LV_OBJ_FLAG_HIDDEN);
    return flap;
}

lv_obj_t* screen_clock_create(void) {
    s_last_hh = s_last_mm = -1;
    s_first_draw = true;

    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, CLR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    // NOTE: deliberately NOT adding a whole-screen click handler. Only the
    // explicit TAP TO EXIT label (and the weather strip, for nav) are
    // clickable so stray taps don't accidentally exit clock mode.

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

    lv_obj_t* btitle = lv_label_create(banner);
    lv_label_set_text(btitle, TITLE_CHRONO);
    lv_obj_set_style_text_font(btitle, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(btitle, CLR_GREEN, 0);
    lv_obj_align(btitle, LV_ALIGN_CENTER, 0, 0);

    // Two big cards
    int total_w = CARD_W * 2 + CARD_GAP;
    int left_x  = (320 - total_w) / 2;
    int right_x = left_x + CARD_W + CARD_GAP;

    s_hh_card = mk_card(scr, left_x,  CARD_Y, CARD_W, CARD_H, &s_hh_lbl);
    s_mm_card = mk_card(scr, right_x, CARD_Y, CARD_W, CARD_H, &s_mm_lbl);

    s_hh_flap = mk_flap(s_hh_card);
    s_mm_flap = mk_flap(s_mm_card);

    s_hh_ctx.flap  = s_hh_flap;
    s_hh_ctx.label = s_hh_lbl;
    s_mm_ctx.flap  = s_mm_flap;
    s_mm_ctx.label = s_mm_lbl;

    // Colon between cards
    s_colon_lbl = lv_label_create(scr);
    lv_label_set_text(s_colon_lbl, ":");
    lv_obj_set_style_text_font(s_colon_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_colon_lbl, CLR_GREEN, 0);
    lv_obj_align(s_colon_lbl, LV_ALIGN_TOP_MID, 0, CARD_Y + CARD_H / 2 - 32);

    // AM/PM top-right of the MM card
    s_ampm_lbl = lv_label_create(scr);
    lv_label_set_text(s_ampm_lbl, "");
    lv_obj_set_style_text_font(s_ampm_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_ampm_lbl, CLR_GOLD, 0);
    lv_obj_set_pos(s_ampm_lbl, right_x + CARD_W + 2, CARD_Y + 2);

    // Weather strip — tappable, sits between the flip cards and the date line.
    // 300 wide x 28 tall. Shows current temp, condition short, hi/lo, sun times.
    s_wx_strip = lv_obj_create(scr);
    lv_obj_set_size(s_wx_strip, 300, 22);
    lv_obj_set_pos(s_wx_strip, 10, CARD_Y + CARD_H + 2);
    // Generous touch ext area so even slightly-off taps register
    lv_obj_set_ext_click_area(s_wx_strip, 4);
    lv_obj_set_style_bg_color(s_wx_strip, CLR_HEADER, 0);
    lv_obj_set_style_bg_opa(s_wx_strip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_wx_strip, CLR_GREEN, 0);
    lv_obj_set_style_border_width(s_wx_strip, 1, 0);
    lv_obj_set_style_radius(s_wx_strip, 3, 0);
    lv_obj_set_style_pad_all(s_wx_strip, 2, 0);
    lv_obj_clear_flag(s_wx_strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wx_strip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wx_strip, wx_click_cb, LV_EVENT_CLICKED, NULL);

    // Line 1 of strip: [ temp ]   [ cond ]   [ hi/lo ]
    s_wx_temp_lbl = lv_label_create(s_wx_strip);
    lv_label_set_text(s_wx_temp_lbl, "--F");
    lv_obj_set_style_text_font(s_wx_temp_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_wx_temp_lbl, CLR_GREEN, 0);
    lv_obj_align(s_wx_temp_lbl, LV_ALIGN_TOP_LEFT, 4, 0);

    s_wx_cond_lbl = lv_label_create(s_wx_strip);
    lv_label_set_text(s_wx_cond_lbl, "...");
    lv_obj_set_style_text_font(s_wx_cond_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_wx_cond_lbl, CLR_GOLD, 0);
    lv_obj_align(s_wx_cond_lbl, LV_ALIGN_TOP_MID, 0, 0);

    s_wx_hilo_lbl = lv_label_create(s_wx_strip);
    lv_label_set_text(s_wx_hilo_lbl, "H --/L --");
    lv_obj_set_style_text_font(s_wx_hilo_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_wx_hilo_lbl, CLR_GREEN, 0);
    lv_obj_align(s_wx_hilo_lbl, LV_ALIGN_TOP_RIGHT, -4, 0);

    // Line 2: sunrise/sunset (or alert overlay when active)
    s_wx_sun_lbl = lv_label_create(s_wx_strip);
    lv_label_set_text(s_wx_sun_lbl, "SR --:-- SS --:--");
    lv_obj_set_style_text_font(s_wx_sun_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_wx_sun_lbl, CLR_DIM, 0);
    lv_obj_align(s_wx_sun_lbl, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Alert overlay (hidden unless an alert is active). Sits on top of the
    // sun line and uses gold to stand out.
    s_wx_alert_lbl = lv_label_create(s_wx_strip);
    lv_label_set_text(s_wx_alert_lbl, "");
    lv_obj_set_style_text_font(s_wx_alert_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_wx_alert_lbl, CLR_GOLD, 0);
    lv_obj_set_style_bg_color(s_wx_alert_lbl, CLR_HEADER, 0);
    lv_obj_set_style_bg_opa(s_wx_alert_lbl, LV_OPA_COVER, 0);
    lv_obj_align(s_wx_alert_lbl, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_wx_alert_lbl, LV_OBJ_FLAG_HIDDEN);

    // Date line
    s_date_lbl = lv_label_create(scr);
    lv_label_set_text(s_date_lbl, "AWAITING SYNC");
    lv_obj_set_style_text_font(s_date_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_date_lbl, CLR_GOLD, 0);
    lv_obj_align(s_date_lbl, LV_ALIGN_BOTTOM_MID, 0, -34);

    // Status line
    s_status_lbl = lv_label_create(scr);
    lv_label_set_text(s_status_lbl, "...");
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_status_lbl, CLR_DIM, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_BOTTOM_MID, 0, -20);

    // Battery corner (replaces the old TZ display): a little 22x10 battery
    // body with a 2x4 tip, filled proportionally, and a percent label to
    // the left of it. Anchored to BOTTOM_RIGHT.
    //   [ 65% ] [===  ]D
    const int BATT_BODY_W = 22;
    const int BATT_BODY_H = 10;
    const int BATT_X      = 320 - 6 - BATT_BODY_W - 2; // body x
    const int BATT_Y      = 240 - 6 - BATT_BODY_H;

    s_batt_body = lv_obj_create(scr);
    lv_obj_set_size(s_batt_body, BATT_BODY_W, BATT_BODY_H);
    lv_obj_set_pos(s_batt_body, BATT_X, BATT_Y);
    lv_obj_set_style_bg_opa(s_batt_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_batt_body, CLR_GREEN, 0);
    lv_obj_set_style_border_width(s_batt_body, 1, 0);
    lv_obj_set_style_radius(s_batt_body, 1, 0);
    lv_obj_set_style_pad_all(s_batt_body, 0, 0);
    lv_obj_clear_flag(s_batt_body, LV_OBJ_FLAG_SCROLLABLE);

    s_batt_tip = lv_obj_create(scr);
    lv_obj_set_size(s_batt_tip, 2, 4);
    lv_obj_set_pos(s_batt_tip, BATT_X + BATT_BODY_W, BATT_Y + 3);
    lv_obj_set_style_bg_color(s_batt_tip, CLR_GREEN, 0);
    lv_obj_set_style_bg_opa(s_batt_tip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_batt_tip, 0, 0);
    lv_obj_set_style_radius(s_batt_tip, 0, 0);
    lv_obj_set_style_pad_all(s_batt_tip, 0, 0);
    lv_obj_clear_flag(s_batt_tip, LV_OBJ_FLAG_SCROLLABLE);

    s_batt_fill = lv_obj_create(s_batt_body);
    lv_obj_set_size(s_batt_fill, 18, 8);
    lv_obj_set_pos(s_batt_fill, 1, 0);
    lv_obj_set_style_bg_color(s_batt_fill, CLR_GREEN, 0);
    lv_obj_set_style_bg_opa(s_batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_batt_fill, 0, 0);
    lv_obj_set_style_radius(s_batt_fill, 0, 0);
    lv_obj_set_style_pad_all(s_batt_fill, 0, 0);
    lv_obj_clear_flag(s_batt_fill, LV_OBJ_FLAG_SCROLLABLE);

    s_batt_lbl = lv_label_create(scr);
    lv_label_set_text(s_batt_lbl, "---");
    lv_obj_set_style_text_font(s_batt_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_batt_lbl, CLR_GREEN, 0);
    // Place to the left of the battery body, vertically aligned
    lv_obj_set_pos(s_batt_lbl, BATT_X - 34, BATT_Y + 1);

    // Exit hint — the ONLY thing on this screen that triggers clock_mode_exit.
    lv_obj_t* hint = lv_label_create(scr);
    lv_label_set_text(hint, "TAP TO EXIT");
    lv_obj_set_style_text_font(hint, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(hint, CLR_DARK, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 6, -6);
    lv_obj_add_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(hint, 14);  // bigger touch target
    lv_obj_add_event_cb(hint, exit_click_cb, LV_EVENT_CLICKED, NULL);

    // Tick immediately then every 1s
    tick_cb(NULL);
    s_tick_timer = lv_timer_create(tick_cb, 1000, NULL);

    // Teardown on delete
    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        (void)e;
        if (s_tick_timer) { lv_timer_del(s_tick_timer); s_tick_timer = NULL; }
        // Cancel any in-flight flap animations so their exec callbacks don't
        // touch freed widgets after delete.
        lv_anim_del(&s_hh_ctx, NULL);
        lv_anim_del(&s_mm_ctx, NULL);
        s_hh_card = s_mm_card = NULL;
        s_hh_lbl = s_mm_lbl = NULL;
        s_hh_flap = s_mm_flap = NULL;
        s_colon_lbl = s_date_lbl = s_ampm_lbl = NULL;
        s_status_lbl = NULL;
        s_batt_body = s_batt_fill = s_batt_tip = s_batt_lbl = NULL;
        s_wx_strip = s_wx_temp_lbl = s_wx_cond_lbl = NULL;
        s_wx_hilo_lbl = s_wx_sun_lbl = s_wx_alert_lbl = NULL;
        s_hh_ctx = {};
        s_mm_ctx = {};
        s_clock_scr = NULL;
    }, LV_EVENT_DELETE, NULL);

    s_clock_scr = scr;
    return scr;
}
