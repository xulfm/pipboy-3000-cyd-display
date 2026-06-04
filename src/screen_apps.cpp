// ============================================================
// Vault 89 PDI-Boy -- Apps hub
// Scrollable 2-column grid with full-width labels so the tile
// names are easy to read on the 320x240 CYD display.
// ============================================================
#include <Arduino.h>
#include <lvgl.h>
#include "pipboy_data.h"
#include "apps.h"

extern void apps_open_pomodoro(void);
extern void apps_open_stopwatch(void);
extern void apps_open_ping(void);
extern void apps_open_soundboard(void);
extern void apps_open_holotape(void);
extern void apps_open_dice(void);
extern void apps_open_snake(void);
extern void apps_open_radstats(void);
extern void apps_open_presence(void);   // merged DND + Presence → "STATUS"
extern void apps_open_pager(void);
extern void apps_open_quests(void);
extern void apps_open_blackjack(void);
extern void apps_open_calibrate(void);
extern void apps_nav_back(void);

// ── Tile definitions ──

struct AppTile {
    const char* label;
    void       (*open)(void);
};

static const AppTile s_tiles[] = {
    {"POMODORO",    apps_open_pomodoro},
    {"STOPWATCH",   apps_open_stopwatch},
    {"PING",        apps_open_ping},
    {"STATUS",      apps_open_presence},   // was DND + MESH → now one entry
    {"SOUNDBOARD",  apps_open_soundboard},
    {"HOLOTAPE",    apps_open_holotape},
    {"DICE",        apps_open_dice},
    {"BLACKJACK",   apps_open_blackjack},
    {"SNAKE",       apps_open_snake},
    {"RAD STATS",   apps_open_radstats},
    {"PAGER",       apps_open_pager},
    {"QUESTS",      apps_open_quests},
    {"CALIBRATE",   apps_open_calibrate},
};
static const int NUM_TILES = sizeof(s_tiles) / sizeof(s_tiles[0]);

static lv_obj_t* s_scr = NULL;

static void tile_click_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= NUM_TILES) return;
    if (s_tiles[idx].open) s_tiles[idx].open();
}

static void back_click_cb(lv_event_t* e) {
    (void)e;
    apps_nav_back();
}

lv_obj_t* screen_apps_create(void) {
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, CLR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── Header ──
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
    lv_label_set_text(htitle, "** VAULT-TEC APPLICATIONS **");
    lv_obj_set_style_text_font(htitle, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(htitle, CLR_GREEN, 0);
    lv_obj_align(htitle, LV_ALIGN_CENTER, 0, 0);

    // ── Footer (back button + hint) ──
    const int FOOTER_H = 28;
    lv_obj_t* footer = lv_obj_create(scr);
    lv_obj_set_size(footer, 320, FOOTER_H);
    lv_obj_set_pos(footer, 0, 240 - FOOTER_H);
    lv_obj_set_style_bg_color(footer, CLR_BG, 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(footer, CLR_GREEN, 0);
    lv_obj_set_style_border_width(footer, 1, 0);
    lv_obj_set_style_radius(footer, 0, 0);
    lv_obj_set_style_pad_all(footer, 0, 0);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back = lv_obj_create(footer);
    lv_obj_set_size(back, 80, 22);
    lv_obj_set_pos(back, 4, 2);
    lv_obj_set_style_bg_color(back, CLR_HEADER, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(back, CLR_GREEN, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_radius(back, 2, 0);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, back_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* blbl = lv_label_create(back);
    lv_label_set_text(blbl, "< BACK");
    lv_obj_set_style_text_font(blbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(blbl, CLR_GREEN, 0);
    lv_obj_align(blbl, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* hint = lv_label_create(footer);
    lv_label_set_text(hint, "SELECT APP");
    lv_obj_set_style_text_font(hint, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(hint, CLR_DARK, 0);
    lv_obj_align(hint, LV_ALIGN_RIGHT_MID, -6, 0);

    // ── Scrollable body ──
    const int BODY_Y = 23;
    const int BODY_H = 240 - BODY_Y - FOOTER_H;

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_set_size(body, 320, BODY_H);
    lv_obj_set_pos(body, 0, BODY_Y);
    lv_obj_set_style_bg_color(body, CLR_BG, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_pad_left(body, 0, 0);
    lv_obj_set_style_pad_right(body, 0, 0);
    lv_obj_set_style_pad_top(body, 4, 0);
    lv_obj_set_style_pad_bottom(body, 4, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    // Thin green scrollbar
    lv_obj_set_style_bg_color(body, CLR_GREEN, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(body, LV_OPA_60, LV_PART_SCROLLBAR);

    // ── 2-column tile grid ──
    const int COLS  = 2;
    const int TW    = 148;       // tile width
    const int TH    = 32;        // tile height
    const int GAP_X = 8;
    const int GAP_Y = 6;
    const int X0    = (320 - (COLS * TW + (COLS - 1) * GAP_X)) / 2;

    for (int i = 0; i < NUM_TILES; i++) {
        int col = i % COLS;
        int row = i / COLS;
        int x = X0 + col * (TW + GAP_X);
        int y = row * (TH + GAP_Y);

        lv_obj_t* t = lv_obj_create(body);
        lv_obj_set_size(t, TW, TH);
        lv_obj_set_pos(t, x, y);
        lv_obj_set_style_bg_color(t, CLR_HEADER, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(t, CLR_GREEN, 0);
        lv_obj_set_style_border_width(t, 1, 0);
        lv_obj_set_style_radius(t, 2, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, tile_click_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)i);

        lv_obj_t* lbl = lv_label_create(t);
        lv_label_set_text(lbl, s_tiles[i].label);
        lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(lbl, CLR_GREEN, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

        // Pager unread badge
        if (strcmp(s_tiles[i].label, "PAGER") == 0 && g_pager_unread > 0) {
            lv_obj_t* bdg = lv_label_create(t);
            char bbuf[8];
            snprintf(bbuf, sizeof(bbuf), "(%d)", g_pager_unread);
            lv_label_set_text(bdg, bbuf);
            lv_obj_set_style_text_font(bdg, &lv_font_unscii_8, 0);
            lv_obj_set_style_text_color(bdg, CLR_GOLD, 0);
            lv_obj_align(bdg, LV_ALIGN_RIGHT_MID, -4, 0);
        }
        // Pomodoro live timer
        if (strcmp(s_tiles[i].label, "POMODORO") == 0 &&
            g_pomo.phase != POMO_IDLE) {
            lv_obj_t* live = lv_label_create(t);
            uint32_t rem = pomo_remaining_ms() / 1000;
            char rbuf[16];
            snprintf(rbuf, sizeof(rbuf), "%02u:%02u", rem / 60, rem % 60);
            lv_label_set_text(live, rbuf);
            lv_obj_set_style_text_font(live, &lv_font_unscii_8, 0);
            lv_obj_set_style_text_color(live, CLR_GOLD, 0);
            lv_obj_align(live, LV_ALIGN_RIGHT_MID, -4, 0);
        }
    }

    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        (void)e;
        s_scr = NULL;
    }, LV_EVENT_DELETE, NULL);

    s_scr = scr;
    return scr;
}
