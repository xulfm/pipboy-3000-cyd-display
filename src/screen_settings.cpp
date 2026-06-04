// ============================================================
// Vault 89 PDI-Boy -- Settings Screen
// Sound (volume + beep tone), Clock (TZ, 12/24, manual set),
// and Reboot.
// ============================================================
#include <Arduino.h>
#include <lvgl.h>
#include "pipboy_data.h"
#include "sound_fx.h"
#include "clock_sync.h"
#include "net_state.h"
#include "weather.h"
#include "landmarks.h"

extern void nav_back_from_sub(void);  // in main.cpp

// ── State ──
enum SettingsPanel { PANEL_MAIN, PANEL_SOUND, PANEL_CLOCK, PANEL_ANCHOR };
static SettingsPanel s_panel = PANEL_MAIN;
static lv_obj_t*     s_content = NULL;   // swappable body container

// Anchor panel labels
static lv_obj_t*     s_anchor_status_lbl = NULL;
static lv_obj_t*     s_anchor_loc_lbl    = NULL;

// Manual-clock edit buffer
static int s_edit_year  = 2026;
static int s_edit_month = 1;
static int s_edit_day   = 1;
static int s_edit_hour  = 12;
static int s_edit_min   = 0;

// Refresh Now state — label flashes "REFRESHING..." briefly after tap so
// the user gets immediate feedback while the background fetch runs. Also
// used by the periodic status-line updater to show "LAST SYNC: Nm ago".
static lv_obj_t*     s_refresh_lbl         = NULL;
static lv_obj_t*     s_sync_status_lbl     = NULL;
static lv_timer_t*   s_sync_status_timer   = NULL;
static unsigned long s_refresh_flash_until = 0;

static void rebuild_panel(void);

// ── Helpers ──
static lv_obj_t* mk_button(lv_obj_t* par, const char* txt, int x, int y,
                            int w, int h, lv_color_t c,
                            lv_event_cb_t cb, void* ud) {
    lv_obj_t* b = lv_obj_create(par);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, CLR_HEADER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, c, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 2, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
    return b;
}

static lv_obj_t* mk_text(lv_obj_t* par, const char* txt, int x, int y, lv_color_t c) {
    lv_obj_t* l = lv_label_create(par);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

// ── Main panel (hub) ──
static void enter_sound_cb(lv_event_t* e)   { (void)e; s_panel = PANEL_SOUND; rebuild_panel(); }
static void enter_clock_cb(lv_event_t* e)   { (void)e;
    // Seed edit buffer from current time
    struct tm t;
    if (clock_get(&t)) {
        s_edit_year = t.tm_year + 1900;
        s_edit_month = t.tm_mon + 1;
        s_edit_day = t.tm_mday;
        s_edit_hour = t.tm_hour;
        s_edit_min = t.tm_min;
    }
    s_panel = PANEL_CLOCK; rebuild_panel();
}
static void back_to_main_cb(lv_event_t* e)  { (void)e; s_panel = PANEL_MAIN; rebuild_panel(); }
static void exit_settings_cb(lv_event_t* e) { (void)e; nav_back_from_sub(); }
static void enter_anchor_cb(lv_event_t* e)  { (void)e; s_panel = PANEL_ANCHOR; rebuild_panel(); }

static void reboot_cb(lv_event_t* e) {
    (void)e;
    Serial.println("[settings] reboot requested");
    delay(200);
    ESP.restart();
}

// Format a millis-age as "Nm ago" / "Ns ago" / "JUST NOW" / "NEVER".
// Used on the Refresh Now button's status line so the user can tell
// at a glance whether the mesh data is fresh.
static void fmt_age(char* dst, size_t dst_len, uint32_t age_ms) {
    if (age_ms == UINT32_MAX) {
        snprintf(dst, dst_len, "NEVER");
        return;
    }
    uint32_t sec = age_ms / 1000;
    if (sec < 5)          snprintf(dst, dst_len, "JUST NOW");
    else if (sec < 60)    snprintf(dst, dst_len, "%us AGO", (unsigned)sec);
    else if (sec < 3600)  snprintf(dst, dst_len, "%um AGO", (unsigned)(sec / 60));
    else                  snprintf(dst, dst_len, "%uh AGO", (unsigned)(sec / 3600));
}

// Rewrite the "WX: ...  CLK: ..." footer line under the Refresh Now
// button. Called periodically by s_sync_status_timer as long as the
// main panel is mounted.
static void update_sync_status_line(void) {
    if (!s_sync_status_lbl) return;
    char wx_age[16] = "NEVER";
    char ck_age[16] = "NEVER";
    fmt_age(wx_age, sizeof(wx_age), weather_last_update_age_ms());
    uint32_t ck_ms = clock_last_sync_ms();
    if (ck_ms == 0) {
        snprintf(ck_age, sizeof(ck_age), "NEVER");
    } else {
        unsigned long now = millis();
        fmt_age(ck_age, sizeof(ck_age),
                (uint32_t)((now < ck_ms) ? 0 : (now - ck_ms)));
    }
    char line[64];
    snprintf(line, sizeof(line), "WX %s  CLK %s", wx_age, ck_age);
    lv_label_set_text(s_sync_status_lbl, line);

    // Also clear the "REFRESHING..." flash once a new fetch has landed
    // or the grace timeout expired.
    if (s_refresh_lbl && s_refresh_flash_until != 0) {
        unsigned long now = millis();
        if (now > s_refresh_flash_until) {
            lv_label_set_text(s_refresh_lbl, "[ REFRESH NOW ]");
            s_refresh_flash_until = 0;
        }
    }
}

static void sync_status_timer_cb(lv_timer_t* t) {
    (void)t;
    update_sync_status_line();
}

static void refresh_now_cb(lv_event_t* e) {
    (void)e;
    Serial.println("[settings] Refresh Now tapped");
    net_force_refresh();
    if (s_refresh_lbl) {
        lv_label_set_text(s_refresh_lbl, "[ REFRESHING... ]");
        // Auto-revert after 6s so the user isn't left staring at a
        // half-active label if no fetch happened (e.g. mesh-only with
        // no active gateway in range).
        s_refresh_flash_until = millis() + 6000;
    }
    update_sync_status_line();
}

static void build_main_panel(lv_obj_t* par) {
    mk_text(par, "-- SETTINGS --", 8, 4, CLR_DIM);
    mk_button(par, "[ SOUND ]",       10,  22, 300, 24, CLR_GREEN, enter_sound_cb, NULL);
    mk_button(par, "[ CLOCK ]",       10,  48, 300, 24, CLR_GREEN, enter_clock_cb, NULL);
    mk_button(par, "[ ANCHOR / LOCATION ]", 10, 74, 300, 24, CLR_GREEN, enter_anchor_cb, NULL);

    // Refresh Now button + live status line underneath. The label
    // pointer is stashed so refresh_now_cb() can flash "REFRESHING..."
    // and the periodic timer can revert it.
    lv_obj_t* rbtn = mk_button(par, "[ REFRESH NOW ]", 10, 100, 300, 24,
                               CLR_GREEN, refresh_now_cb, NULL);
    // mk_button returns the button; its first (and only) child is the label.
    s_refresh_lbl = lv_obj_get_child(rbtn, 0);
    s_sync_status_lbl = mk_text(par, "WX ...  CLK ...", 12, 128, CLR_DIM);

    mk_button(par, "[ REBOOT ]",  10, 146, 300, 24, CLR_GOLD, reboot_cb,       NULL);
    mk_button(par, "[ BACK ]",    10, 174, 300, 24, CLR_GOLD, exit_settings_cb, NULL);

    // Kick off the periodic status-line refresh. 1s cadence is fine —
    // it's a cheap label update and stops when the panel is rebuilt.
    if (!s_sync_status_timer) {
        s_sync_status_timer = lv_timer_create(sync_status_timer_cb, 1000, NULL);
    }
    update_sync_status_line();
}

// ── Sound panel ──
static lv_obj_t* s_vol_lbl   = NULL;
static lv_obj_t* s_tone_lbl  = NULL;

static void update_sound_labels(void) {
    if (s_vol_lbl) {
        char b[24];
        snprintf(b, sizeof(b), "VOLUME: %3u", sfx_get_volume());
        lv_label_set_text(s_vol_lbl, b);
    }
    if (s_tone_lbl) {
        char b[24];
        snprintf(b, sizeof(b), "TONE: %s", sfx_click_tone_name(sfx_get_click_tone()));
        lv_label_set_text(s_tone_lbl, b);
    }
}

static void vol_down_cb(lv_event_t* e) {
    (void)e;
    int v = (int)sfx_get_volume() - 10;
    if (v < 0) v = 0;
    sfx_set_volume((uint8_t)v);
    update_sound_labels();
}
static void vol_up_cb(lv_event_t* e) {
    (void)e;
    int v = (int)sfx_get_volume() + 10;
    if (v > 100) v = 100;
    sfx_set_volume((uint8_t)v);
    update_sound_labels();
    sfx_play_click_preview();
}
static void tone_cycle_cb(lv_event_t* e) {
    (void)e;
    uint8_t t = sfx_get_click_tone();
    t = (t + 1) % CLICK_TONE_COUNT;
    sfx_set_click_tone(t);
    update_sound_labels();
    sfx_play_click_preview();
}
static void test_sound_cb(lv_event_t* e) {
    (void)e;
    sfx_play_click_preview();
}

static void build_sound_panel(lv_obj_t* par) {
    mk_text(par, "-- SOUND --", 8, 4, CLR_DIM);

    s_vol_lbl = mk_text(par, "VOLUME: ---", 10, 22, CLR_GREEN);
    mk_button(par, "[ - ]", 160,  18, 60, 22, CLR_GREEN, vol_down_cb, NULL);
    mk_button(par, "[ + ]", 230,  18, 60, 22, CLR_GREEN, vol_up_cb,   NULL);

    s_tone_lbl = mk_text(par, "TONE: ----", 10, 52, CLR_GREEN);
    mk_button(par, "[ CYCLE ]", 160, 48, 130, 22, CLR_GREEN, tone_cycle_cb, NULL);

    mk_button(par, "[ TEST SOUND ]",  10,  80, 300, 22, CLR_GOLD,  test_sound_cb,   NULL);
    mk_button(par, "[ BACK ]",        10, 108, 300, 22, CLR_GOLD,  back_to_main_cb, NULL);

    update_sound_labels();
}

// ── Clock panel ──
static lv_obj_t* s_tz_lbl    = NULL;
static lv_obj_t* s_fmt_lbl   = NULL;
static lv_obj_t* s_edit_lbl  = NULL;

static void update_clock_labels(void) {
    if (s_tz_lbl) {
        char b[24];
        snprintf(b, sizeof(b), "TZ: UTC%+d", clock_get_tz());
        lv_label_set_text(s_tz_lbl, b);
    }
    if (s_fmt_lbl) {
        lv_label_set_text(s_fmt_lbl, clock_get_24h() ? "FORMAT: 24H" : "FORMAT: 12H");
    }
    if (s_edit_lbl) {
        char b[40];
        snprintf(b, sizeof(b), "SET: %04d-%02d-%02d  %02d:%02d",
                 s_edit_year, s_edit_month, s_edit_day, s_edit_hour, s_edit_min);
        lv_label_set_text(s_edit_lbl, b);
    }
}

static void tz_down_cb(lv_event_t* e) { (void)e; clock_set_tz(clock_get_tz() - 1); update_clock_labels(); }
static void tz_up_cb(lv_event_t* e)   { (void)e; clock_set_tz(clock_get_tz() + 1); update_clock_labels(); }

static void fmt_toggle_cb(lv_event_t* e) { (void)e; clock_set_24h(!clock_get_24h()); update_clock_labels(); }

// Days in month, with leap-year handling
static int days_in_month(int y, int m) {
    static const uint8_t dim[13] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (m < 1 || m > 12) return 31;
    if (m == 2) {
        bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        return leap ? 29 : 28;
    }
    return dim[m];
}

static void clamp_day(void) {
    int max = days_in_month(s_edit_year, s_edit_month);
    if (s_edit_day > max) s_edit_day = max;
    if (s_edit_day < 1)   s_edit_day = 1;
}

static void h_down_cb(lv_event_t* e)  { (void)e; s_edit_hour = (s_edit_hour + 23) % 24; update_clock_labels(); }
static void h_up_cb(lv_event_t* e)    { (void)e; s_edit_hour = (s_edit_hour + 1) % 24;  update_clock_labels(); }
static void m_down_cb(lv_event_t* e)  { (void)e; s_edit_min = (s_edit_min + 59) % 60;   update_clock_labels(); }
static void m_up_cb(lv_event_t* e)    { (void)e; s_edit_min = (s_edit_min + 1) % 60;    update_clock_labels(); }

static void y_down_cb(lv_event_t* e)  { (void)e; if (s_edit_year > 2024) s_edit_year--; clamp_day(); update_clock_labels(); }
static void y_up_cb(lv_event_t* e)    { (void)e; if (s_edit_year < 2099) s_edit_year++; clamp_day(); update_clock_labels(); }
static void mo_down_cb(lv_event_t* e) { (void)e; s_edit_month = (s_edit_month == 1)  ? 12 : s_edit_month - 1; clamp_day(); update_clock_labels(); }
static void mo_up_cb(lv_event_t* e)   { (void)e; s_edit_month = (s_edit_month == 12) ?  1 : s_edit_month + 1; clamp_day(); update_clock_labels(); }
static void d_down_cb(lv_event_t* e)  { (void)e;
    int max = days_in_month(s_edit_year, s_edit_month);
    s_edit_day = (s_edit_day <= 1) ? max : s_edit_day - 1;
    update_clock_labels();
}
static void d_up_cb(lv_event_t* e)    { (void)e;
    int max = days_in_month(s_edit_year, s_edit_month);
    s_edit_day = (s_edit_day >= max) ? 1 : s_edit_day + 1;
    update_clock_labels();
}

static void today_cb(lv_event_t* e) {
    (void)e;
    // Snap date/time edit buffer to current clock
    struct tm t;
    if (clock_get(&t)) {
        s_edit_year  = t.tm_year + 1900;
        s_edit_month = t.tm_mon + 1;
        s_edit_day   = t.tm_mday;
        s_edit_hour  = t.tm_hour;
        s_edit_min   = t.tm_min;
    }
    update_clock_labels();
}

static void apply_set_cb(lv_event_t* e) {
    (void)e;
    clock_set_manual(s_edit_year, s_edit_month, s_edit_day,
                     s_edit_hour, s_edit_min, 0);
    if (s_edit_lbl) {
        lv_label_set_text(s_edit_lbl, "TIME APPLIED");
    }
}

static void build_clock_panel(lv_obj_t* par) {
    mk_text(par, "-- CLOCK --", 8, 4, CLR_DIM);

    // TZ row
    s_tz_lbl  = mk_text(par, "TZ: --", 10, 22, CLR_GREEN);
    mk_button(par, "-", 150, 18, 36, 20, CLR_GREEN, tz_down_cb, NULL);
    mk_button(par, "+", 190, 18, 36, 20, CLR_GREEN, tz_up_cb,   NULL);

    // Format row
    s_fmt_lbl = mk_text(par, "FMT: --", 10, 44, CLR_GREEN);
    mk_button(par, "TOGGLE", 150, 40, 76, 20, CLR_GREEN, fmt_toggle_cb, NULL);

    // SET label
    s_edit_lbl = mk_text(par, "SET: --", 10, 66, CLR_GOLD);

    // Date picker row: Y- Y+  M- M+  D- D+
    const int BW = 30;
    mk_button(par, "Y-",  10, 82, BW, 20, CLR_GREEN, y_down_cb,  NULL);
    mk_button(par, "Y+",  44, 82, BW, 20, CLR_GREEN, y_up_cb,    NULL);
    mk_button(par, "Mo-", 82, 82, 36, 20, CLR_GREEN, mo_down_cb, NULL);
    mk_button(par, "Mo+", 122, 82, 36, 20, CLR_GREEN, mo_up_cb,   NULL);
    mk_button(par, "D-",  162, 82, BW, 20, CLR_GREEN, d_down_cb, NULL);
    mk_button(par, "D+",  196, 82, BW, 20, CLR_GREEN, d_up_cb,   NULL);
    mk_button(par, "NOW", 234, 82, 76, 20, CLR_GOLD,  today_cb,  NULL);

    // Time row: H- H+ M- M+
    mk_button(par, "H-",  10,  106, BW, 20, CLR_GREEN, h_down_cb, NULL);
    mk_button(par, "H+",  44,  106, BW, 20, CLR_GREEN, h_up_cb,   NULL);
    mk_button(par, "Mi-", 82,  106, 36, 20, CLR_GREEN, m_down_cb, NULL);
    mk_button(par, "Mi+", 122, 106, 36, 20, CLR_GREEN, m_up_cb,   NULL);
    mk_button(par, "[ APPLY ]", 162, 106, 148, 20, CLR_GOLD, apply_set_cb, NULL);

    mk_button(par, "[ BACK ]", 10, 132, 300, 22, CLR_GOLD, back_to_main_cb, NULL);

    update_clock_labels();
}

// ── Panel switching ──
// ── Anchor / Location panel ──
static void update_anchor_labels(void) {
    if (s_anchor_status_lbl) {
        char b[40];
        snprintf(b, sizeof(b), "STATUS: %s", g_is_anchor ? "ANCHOR (ON)" : "OFF (roaming)");
        lv_label_set_text(s_anchor_status_lbl, b);
        lv_obj_set_style_text_color(s_anchor_status_lbl,
                                    g_is_anchor ? CLR_GREEN : CLR_DIM, 0);
    }
    if (s_anchor_loc_lbl) {
        char b[40];
        const char* nm = (g_anchor_landmark >= 0 && g_anchor_landmark < LANDMARK_COUNT)
                         ? LANDMARKS[g_anchor_landmark].name : "?";
        snprintf(b, sizeof(b), "LOCATION: %s", nm);
        lv_label_set_text(s_anchor_loc_lbl, b);
    }
}

static void anchor_toggle_cb(lv_event_t* e) {
    (void)e;
    anchor_set(!g_is_anchor, g_anchor_landmark);
    update_anchor_labels();
}
static void anchor_prev_cb(lv_event_t* e) {
    (void)e;
    int idx = g_anchor_landmark - 1;
    if (idx < 0) idx = LANDMARK_COUNT - 1;
    anchor_set(g_is_anchor, idx);
    update_anchor_labels();
}
static void anchor_next_cb(lv_event_t* e) {
    (void)e;
    int idx = g_anchor_landmark + 1;
    if (idx >= LANDMARK_COUNT) idx = 0;
    anchor_set(g_is_anchor, idx);
    update_anchor_labels();
}

static void build_anchor_panel(lv_obj_t* par) {
    mk_text(par, "-- ANCHOR / LOCATION --", 8, 4, CLR_DIM);
    mk_text(par, "Act as a fixed beacon at a map", 8, 22, CLR_DIM);
    mk_text(par, "landmark so roaming PIP-BOYS can", 8, 34, CLR_DIM);
    mk_text(par, "be positioned by signal strength.", 8, 46, CLR_DIM);

    s_anchor_status_lbl = mk_text(par, "", 12, 66, CLR_DIM);
    mk_button(par, "[ TOGGLE ANCHOR ON / OFF ]", 10, 82, 300, 26,
              CLR_GREEN, anchor_toggle_cb, NULL);

    s_anchor_loc_lbl = mk_text(par, "", 12, 116, CLR_GOLD);
    mk_button(par, "< PREV", 10, 132, 145, 26, CLR_GOLD, anchor_prev_cb, NULL);
    mk_button(par, "NEXT >", 165, 132, 145, 26, CLR_GOLD, anchor_next_cb, NULL);

    mk_button(par, "[ BACK ]", 10, 184, 300, 26, CLR_GOLD, back_to_main_cb, NULL);

    update_anchor_labels();
}

static void rebuild_panel(void) {
    if (!s_content) return;
    // Clear all children of content container
    lv_obj_clean(s_content);
    // These stored-label pointers are now dangling; clear them before rebuild
    s_vol_lbl = s_tone_lbl = NULL;
    s_tz_lbl  = s_fmt_lbl  = s_edit_lbl = NULL;
    s_refresh_lbl     = NULL;
    s_sync_status_lbl = NULL;
    s_anchor_status_lbl = NULL;
    s_anchor_loc_lbl    = NULL;
    // The periodic sync-status timer is a global lv_timer, so only
    // the main panel needs it. Drop it when we switch panels; the
    // main-panel build_* path will re-create it.
    if (s_sync_status_timer) {
        lv_timer_del(s_sync_status_timer);
        s_sync_status_timer = NULL;
    }

    switch (s_panel) {
        case PANEL_MAIN:   build_main_panel(s_content);   break;
        case PANEL_SOUND:  build_sound_panel(s_content);  break;
        case PANEL_CLOCK:  build_clock_panel(s_content);  break;
        case PANEL_ANCHOR: build_anchor_panel(s_content); break;
    }
}

// ── Screen create ──
lv_obj_t* screen_settings_create(void) {
    s_panel = PANEL_MAIN;

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
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* htitle = lv_label_create(hdr);
    lv_label_set_text(htitle, TITLE_SETTINGS);
    lv_obj_set_style_text_font(htitle, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(htitle, CLR_GREEN, 0);
    lv_obj_align(htitle, LV_ALIGN_CENTER, 0, 0);

    // Content container (swapped when panel changes)
    s_content = lv_obj_create(scr);
    lv_obj_set_size(s_content, 320, 218);
    lv_obj_set_pos(s_content, 0, 22);
    lv_obj_set_style_bg_color(s_content, CLR_BG, 0);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_radius(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    build_main_panel(s_content);

    // Teardown on delete
    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        (void)e;
        s_content         = NULL;
        s_vol_lbl         = s_tone_lbl = NULL;
        s_tz_lbl          = s_fmt_lbl  = s_edit_lbl = NULL;
        s_refresh_lbl     = NULL;
        s_sync_status_lbl = NULL;
        s_anchor_status_lbl = NULL;
        s_anchor_loc_lbl    = NULL;
        if (s_sync_status_timer) {
            lv_timer_del(s_sync_status_timer);
            s_sync_status_timer = NULL;
        }
    }, LV_EVENT_DELETE, NULL);

    return scr;
}
