// ============================================================
// Vault 89 PDI-Boy -- Main Menu
// First-boot landing page with LOGIN / WIFI / BLUETOOTH /
// CLOCK / SETTINGS buttons.
// ============================================================
#include <Arduino.h>
#include <lvgl.h>
#include "pipboy_data.h"
#include "net_state.h"

// Nav targets implemented in main.cpp — each one tears down the main
// menu screen and loads the corresponding sub-screen.
extern void menu_nav_login(void);
extern void menu_nav_wifi(void);
extern void menu_nav_bluetooth(void);
extern void menu_nav_clock(void);
extern void menu_nav_settings(void);
extern void menu_nav_apps(void);

static lv_obj_t* s_wifi_status = NULL;
static lv_timer_t* s_status_timer = NULL;

static void btn_click_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    switch (idx) {
        case 0: menu_nav_login();     break;
        case 1: menu_nav_wifi();      break;
        case 2: menu_nav_bluetooth(); break;
        case 3: menu_nav_clock();     break;
        case 4: menu_nav_settings();  break;
        case 5: menu_nav_apps();      break;
    }
}

static lv_obj_t* mk_menu_btn(lv_obj_t* par, const char* label, int x, int y,
                             int w, int h, int idx) {
    lv_obj_t* btn = lv_obj_create(par);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, CLR_HEADER, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, CLR_GREEN, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_radius(btn, 2, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, btn_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)idx);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(lbl, CLR_GREEN, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    return btn;
}

lv_obj_t* screen_mainmenu_create(void) {
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, CLR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Header bar
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
    lv_label_set_text(htitle, TITLE_MAINMENU);
    lv_obj_set_style_text_font(htitle, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(htitle, CLR_GREEN, 0);
    lv_obj_align(htitle, LV_ALIGN_CENTER, 0, 0);

    // Subtitle
    lv_obj_t* sub = lv_label_create(scr);
    lv_label_set_text(sub, "-- MAIN MENU --");
    lv_obj_set_style_text_font(sub, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(sub, CLR_DIM, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 28);

    // Five stacked buttons in a 2-column grid (fits 320x240 nicely)
    // Row 1: LOGIN | WIFI
    // Row 2: BLUETOOTH | CLOCK
    // Row 3: SETTINGS (full width)
    const int BTN_W = 140;
    const int BTN_H = 40;
    const int GAP_X = 14;
    const int GAP_Y = 12;
    const int TOP_Y = 46;
    const int LEFT_X  = (320 - (BTN_W * 2 + GAP_X)) / 2;
    const int RIGHT_X = LEFT_X + BTN_W + GAP_X;

    mk_menu_btn(scr, "[ LOGIN ]",     LEFT_X,  TOP_Y + 0 * (BTN_H + GAP_Y), BTN_W, BTN_H, 0);
    mk_menu_btn(scr, "[ WIFI ]",      RIGHT_X, TOP_Y + 0 * (BTN_H + GAP_Y), BTN_W, BTN_H, 1);
    mk_menu_btn(scr, "[ BLUETOOTH ]", LEFT_X,  TOP_Y + 1 * (BTN_H + GAP_Y), BTN_W, BTN_H, 2);
    mk_menu_btn(scr, "[ CLOCK ]",     RIGHT_X, TOP_Y + 1 * (BTN_H + GAP_Y), BTN_W, BTN_H, 3);
    mk_menu_btn(scr, "[ APPS ]",      LEFT_X,  TOP_Y + 2 * (BTN_H + GAP_Y), BTN_W, BTN_H, 5);
    mk_menu_btn(scr, "[ SETTINGS ]",  RIGHT_X, TOP_Y + 2 * (BTN_H + GAP_Y), BTN_W, BTN_H, 4);

    // Link status at bottom — shows WIFI / MESH / OFFLINE
    s_wifi_status = lv_label_create(scr);
    lv_label_set_text(s_wifi_status, "LINK: --");
    lv_obj_set_style_text_font(s_wifi_status, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_wifi_status, CLR_DIM, 0);
    lv_obj_align(s_wifi_status, LV_ALIGN_BOTTOM_LEFT, 6, -6);

    lv_obj_t* hint = lv_label_create(scr);
    lv_label_set_text(hint, "SELECT A MODULE");
    lv_obj_set_style_text_font(hint, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(hint, CLR_DARK, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_RIGHT, -6, -6);

    // Link-mode refresh timer — shows WIFI (direct), MESH (ESP-NOW
    // relay through another online device), or OFFLINE.
    s_status_timer = lv_timer_create([](lv_timer_t* t) {
        if (!s_wifi_status) return;
        char buf[32];
        lv_color_t col;
        if (g_net.wifi_connected) {
            snprintf(buf, sizeof(buf), "LINK: WIFI");
            col = CLR_GREEN;
        } else if (g_net.link_mode == LINK_MESH) {
            snprintf(buf, sizeof(buf), "LINK: MESH (%d)", g_peer_count);
            col = lv_color_hex(0xCCCC00);
        } else {
            snprintf(buf, sizeof(buf), "LINK: OFFLINE");
            col = CLR_DIM;
        }
        lv_label_set_text(s_wifi_status, buf);
        lv_obj_set_style_text_color(s_wifi_status, col, 0);
    }, 1500, NULL);

    // Teardown on delete
    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        (void)e;
        if (s_status_timer) { lv_timer_del(s_status_timer); s_status_timer = NULL; }
        s_wifi_status = NULL;
    }, LV_EVENT_DELETE, NULL);

    return scr;
}
