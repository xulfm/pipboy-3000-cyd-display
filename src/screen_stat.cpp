// ============================================================
// Vault 89 PDI-Boy -- STAT Screen
// Left panel: Vault dweller figure, name/title, HP/MORALE, VAULT 89
// Right panel: dweller stats grid, STATUS bars, TRADE PERKS, effects
// ============================================================

#include <Arduino.h>
#include <stdio.h>
#ifndef snprintf
#define snprintf(buf,n,...) sprintf(buf,__VA_ARGS__)
#endif
#include <lvgl.h>
#include "pipboy_data.h"
#include "sound_fx.h"
#include "ble_grill.h"

extern void nav_to_wifi(void);
extern void nav_to_login(void);
extern void nav_to_main_menu(void);
extern void clock_mode_enter(void);
extern void tabs_open_settings(void);
extern void tabs_open_bluetooth(void);

static lv_obj_t* scr_stat = NULL;
static lv_obj_t* settings_menu = NULL;

// ── Grill monitor panel ──
static lv_obj_t*   grill_panel   = NULL;
static lv_obj_t*   grill_status  = NULL;
static lv_obj_t*   grill_dev_list = NULL;
static lv_obj_t*   grill_temp_labels[GRILL_MAX_PROBES] = {};
static lv_obj_t*   grill_probe_labels[GRILL_MAX_PROBES] = {};
static lv_obj_t*   grill_conn_lbl = NULL;
static lv_timer_t* grill_timer   = NULL;
static int         s_last_dev_count = -1;

// ── Speaker test panel — uses sfx module ──
static lv_obj_t*   spk_panel    = NULL;
static lv_obj_t*   spk_freq_lbl = NULL;
static lv_obj_t*   spk_vol_lbl  = NULL;
static lv_obj_t*   spk_status   = NULL;
static uint16_t    spk_freq     = 1000;

static void spk_close_panel(void) {
    sfx_stop();
    if (spk_panel) { lv_obj_del(spk_panel); spk_panel = NULL; }
    spk_freq_lbl = spk_vol_lbl = spk_status = NULL;
}

static void spk_freq_slider_cb(lv_event_t* e) {
    lv_obj_t* slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    spk_freq = 100 + (val * val * 49) / 100;
    if (spk_freq_lbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d Hz", spk_freq);
        lv_label_set_text(spk_freq_lbl, buf);
    }
}

static void spk_vol_slider_cb(lv_event_t* e) {
    lv_obj_t* slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    sfx_set_volume((uint8_t)val);
    if (spk_vol_lbl) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", val);
        lv_label_set_text(spk_vol_lbl, buf);
    }
}

static void spk_tone_cb(lv_event_t* e) {
    (void)e;
    sfx_tone(spk_freq, 500);  // 500ms test tone
    if (spk_status) {
        char buf[24];
        snprintf(buf, sizeof(buf), "TONE %d Hz", spk_freq);
        lv_label_set_text(spk_status, buf);
        lv_obj_set_style_text_color(spk_status, lv_color_hex(0x00ff00), 0);
    }
}

static void spk_close_cb(lv_event_t* e) {
    (void)e;
    spk_close_panel();
}

// Sound effect preset buttons
static void spk_sfx_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    static const SoundFX fxs[] = {
        SFX_MSG_IN, SFX_VAULT_ALERT, SFX_PEER_JOIN,
        SFX_CONNECT, SFX_ERROR, SFX_BOOT,
    };
    static const char* names[] = {
        "MSG IN", "VAULT ALERT", "PEER JOIN",
        "CONNECT", "ERROR", "BOOT",
    };
    if (idx < 6) {
        sfx_play(fxs[idx]);
        if (spk_status) {
            lv_label_set_text(spk_status, names[idx]);
            lv_obj_set_style_text_color(spk_status, lv_color_hex(0x00ff00), 0);
        }
    }
}

static void open_speaker_test(void) {
    if (spk_panel) return;

    // Full overlay on layer_top
    spk_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(spk_panel, 320, 222);
    lv_obj_set_pos(spk_panel, 0, 18);
    lv_obj_set_style_bg_color(spk_panel, CLR_BG, 0);
    lv_obj_set_style_bg_opa(spk_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(spk_panel, 0, 0);
    lv_obj_set_style_radius(spk_panel, 0, 0);
    lv_obj_set_style_pad_all(spk_panel, 10, 0);
    lv_obj_clear_flag(spk_panel, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t* title = lv_label_create(spk_panel);
    lv_label_set_text(title, "SPEAKER TEST");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, CLR_GREEN, 0);
    lv_obj_set_pos(title, 90, 0);

    // ── Frequency slider + PLAY TONE ──
    lv_obj_t* f_lbl = lv_label_create(spk_panel);
    lv_label_set_text(f_lbl, "FREQ");
    lv_obj_set_style_text_font(f_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(f_lbl, CLR_DIM, 0);
    lv_obj_set_pos(f_lbl, 0, 24);

    lv_obj_t* f_slider = lv_slider_create(spk_panel);
    lv_obj_set_size(f_slider, 180, 10);
    lv_obj_set_pos(f_slider, 40, 26);
    lv_slider_set_range(f_slider, 0, 100);
    lv_slider_set_value(f_slider, 30, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(f_slider, CLR_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(f_slider, CLR_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(f_slider, CLR_GREEN, LV_PART_KNOB);
    lv_obj_set_style_pad_all(f_slider, 3, LV_PART_KNOB);
    lv_obj_add_event_cb(f_slider, spk_freq_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    spk_freq_lbl = lv_label_create(spk_panel);
    lv_label_set_text(spk_freq_lbl, "1000 Hz");
    lv_obj_set_style_text_font(spk_freq_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(spk_freq_lbl, CLR_GREEN, 0);
    lv_obj_set_pos(spk_freq_lbl, 228, 24);

    // ── Volume slider ──
    lv_obj_t* v_lbl = lv_label_create(spk_panel);
    lv_label_set_text(v_lbl, "VOL");
    lv_obj_set_style_text_font(v_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(v_lbl, CLR_DIM, 0);
    lv_obj_set_pos(v_lbl, 0, 44);

    lv_obj_t* v_slider = lv_slider_create(spk_panel);
    lv_obj_set_size(v_slider, 180, 10);
    lv_obj_set_pos(v_slider, 40, 46);
    lv_slider_set_range(v_slider, 0, 100);
    lv_slider_set_value(v_slider, sfx_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(v_slider, CLR_DARK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(v_slider, CLR_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(v_slider, CLR_GREEN, LV_PART_KNOB);
    lv_obj_set_style_pad_all(v_slider, 3, LV_PART_KNOB);
    lv_obj_add_event_cb(v_slider, spk_vol_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    spk_vol_lbl = lv_label_create(spk_panel);
    char vbuf[8];
    snprintf(vbuf, sizeof(vbuf), "%d%%", sfx_get_volume());
    lv_label_set_text(spk_vol_lbl, vbuf);
    lv_obj_set_style_text_font(spk_vol_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(spk_vol_lbl, CLR_GREEN, 0);
    lv_obj_set_pos(spk_vol_lbl, 228, 44);

    // PLAY TONE button
    lv_obj_t* tone_btn = lv_obj_create(spk_panel);
    lv_obj_set_size(tone_btn, 90, 22);
    lv_obj_set_pos(tone_btn, 0, 64);
    lv_obj_set_style_bg_color(tone_btn, lv_color_hex(0x004c1a), 0);
    lv_obj_set_style_bg_opa(tone_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tone_btn, CLR_GREEN, 0);
    lv_obj_set_style_border_width(tone_btn, 1, 0);
    lv_obj_set_style_radius(tone_btn, 0, 0);
    lv_obj_set_style_pad_all(tone_btn, 0, 0);
    lv_obj_clear_flag(tone_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tone_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tone_btn, spk_tone_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* tone_lbl = lv_label_create(tone_btn);
    lv_label_set_text(tone_lbl, "PLAY TONE");
    lv_obj_set_style_text_font(tone_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(tone_lbl, CLR_GREEN, 0);
    lv_obj_align(tone_lbl, LV_ALIGN_CENTER, 0, 0);

    // ── Sound effect preset buttons ──
    lv_obj_t* sfx_lbl = lv_label_create(spk_panel);
    lv_label_set_text(sfx_lbl, "SOUND EFFECTS");
    lv_obj_set_style_text_font(sfx_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(sfx_lbl, CLR_DIM, 0);
    lv_obj_set_pos(sfx_lbl, 0, 96);

    static const char* sfx_names[] = {
        "MSG IN", "ALERT", "JOIN",
        "CONNECT", "ERROR", "BOOT",
    };
    for (int i = 0; i < 6; i++) {
        lv_obj_t* btn = lv_obj_create(spk_panel);
        lv_obj_set_size(btn, 90, 22);
        lv_obj_set_pos(btn, (i % 3) * 98, 110 + (i / 3) * 28);
        lv_obj_set_style_bg_color(btn, CLR_DARK, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, CLR_DIM, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, spk_sfx_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t* bl = lv_label_create(btn);
        lv_label_set_text(bl, sfx_names[i]);
        lv_obj_set_style_text_font(bl, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(bl, CLR_GREEN, 0);
        lv_obj_align(bl, LV_ALIGN_CENTER, 0, 0);
    }

    // ── Status label ──
    spk_status = lv_label_create(spk_panel);
    lv_label_set_text(spk_status, "READY");
    lv_obj_set_style_text_font(spk_status, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(spk_status, CLR_DIM, 0);
    lv_obj_set_pos(spk_status, 0, 172);

    // ── Close button ──
    lv_obj_t* close_btn = lv_obj_create(spk_panel);
    lv_obj_set_size(close_btn, 90, 22);
    lv_obj_set_pos(close_btn, 196, 172);
    lv_obj_set_style_bg_color(close_btn, CLR_DARK, 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(close_btn, CLR_DIM, 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_radius(close_btn, 0, 0);
    lv_obj_set_style_pad_all(close_btn, 0, 0);
    lv_obj_clear_flag(close_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(close_btn, spk_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "[ CLOSE ]");
    lv_obj_set_style_text_font(close_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(close_lbl, CLR_GREEN, 0);
    lv_obj_align(close_lbl, LV_ALIGN_CENTER, 0, 0);

    // ── Info ──
    lv_obj_t* hint = lv_label_create(spk_panel);
    lv_label_set_text(hint, "AMP:GPIO4(LOW) DAC:GPIO26");
    lv_obj_set_style_text_font(hint, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(hint, CLR_DIM, 0);
    lv_obj_set_pos(hint, 0, 192);
}

// Updateable label pointers
static lv_obj_t* lbl_name    = NULL;
static lv_obj_t* lbl_title   = NULL;
static lv_obj_t* bar_hp      = NULL;
static lv_obj_t* bar_morale  = NULL;
static lv_obj_t* bar_rad     = NULL;
static lv_obj_t* bar_hunger  = NULL;
static lv_obj_t* lbl_sv[7]   = {};
static lv_obj_t* lbl_p1      = NULL;
static lv_obj_t* lbl_p2      = NULL;
static lv_obj_t* lbl_p3      = NULL;

// ── Helpers ──
static lv_obj_t* make_label(lv_obj_t* parent, const char* text,
                              const lv_font_t* font, lv_color_t color,
                              int x, int y, int w = 0) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lbl, 0, 0);
    lv_obj_set_style_pad_all(lbl, 0, 0);
    if (w > 0) {
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(lbl, w);
    }
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

static lv_obj_t* make_bar(lv_obj_t* parent,
                            int x, int y, int w, int h,
                            lv_color_t fill_color, int pct) {
    lv_obj_t* outer = lv_obj_create(parent);
    lv_obj_set_size(outer, w, h);
    lv_obj_set_pos(outer, x, y);
    lv_obj_set_style_bg_color(outer, lv_color_hex(0x00a83e), 0);
    lv_obj_set_style_bg_opa(outer, 20, 0);
    lv_obj_set_style_border_color(outer, CLR_DIM, 0);
    lv_obj_set_style_border_width(outer, 1, 0);
    lv_obj_set_style_radius(outer, 0, 0);
    lv_obj_set_style_pad_all(outer, 0, 0);
    lv_obj_clear_flag(outer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* fill = lv_obj_create(outer);
    int fill_w = (w - 2) * pct / 100;
    if (fill_w < 1) fill_w = 1;
    lv_obj_set_size(fill, fill_w, h - 2);
    lv_obj_set_pos(fill, 0, 0);
    lv_obj_set_style_bg_color(fill, fill_color, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_set_style_radius(fill, 0, 0);
    lv_obj_set_style_pad_all(fill, 0, 0);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    return fill;
}

static lv_obj_t* make_sec_label(lv_obj_t* parent, const char* text, int x, int y, int w) {
    lv_obj_t* lbl = make_label(parent, text, &lv_font_unscii_8, CLR_DIM, x, y, w);
    lv_obj_t* div = lv_obj_create(parent);
    lv_obj_set_size(div, w, 1);
    lv_obj_set_pos(div, x, y + 10);
    lv_obj_set_style_bg_color(div, CLR_DARK, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);
    return lbl;
}

static lv_obj_t* make_stat_cell(lv_obj_t* parent,
                                  const char* stat_name, uint8_t val,
                                  int x, int y, int w, int h) {
    lv_obj_t* cell = lv_obj_create(parent);
    lv_obj_set_size(cell, w, h);
    lv_obj_set_pos(cell, x, y);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x00a83e), 0);
    lv_obj_set_style_bg_opa(cell, 13, 0);
    lv_obj_set_style_border_color(cell, CLR_DARK, 0);
    lv_obj_set_style_border_width(cell, 1, 0);
    lv_obj_set_style_radius(cell, 0, 0);
    lv_obj_set_style_pad_all(cell, 2, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* name_lbl = lv_label_create(cell);
    lv_label_set_text(name_lbl, stat_name);
    lv_obj_set_style_text_font(name_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(name_lbl, CLR_DIM, 0);
    lv_obj_set_style_pad_all(name_lbl, 0, 0);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t* val_lbl = lv_label_create(cell);
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", val);
    lv_label_set_text(val_lbl, buf);
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_all(val_lbl, 0, 0);

    if (val >= 10)
        lv_obj_set_style_text_color(val_lbl, CLR_GOLD, 0);
    else if (val >= 8)
        lv_obj_set_style_text_color(val_lbl, CLR_HI, 0);
    else
        lv_obj_set_style_text_color(val_lbl, CLR_GREEN, 0);

    lv_obj_align(val_lbl, LV_ALIGN_BOTTOM_MID, 0, 0);
    return val_lbl;
}

static void make_etag(lv_obj_t* parent, const char* text,
                       bool gold, int* x_cursor, int y) {
    lv_obj_t* tag = lv_obj_create(parent);
    lv_obj_set_style_bg_color(tag, gold ? CLR_GOLD : CLR_DIM, 0);
    lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tag, 0, 0);
    lv_obj_set_style_radius(tag, 0, 0);
    lv_obj_set_style_pad_hor(tag, 3, 0);
    lv_obj_set_style_pad_ver(tag, 1, 0);
    lv_obj_clear_flag(tag, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(tag);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(lbl, gold ? lv_color_hex(0x1a0f00) : CLR_BG, 0);
    lv_obj_set_style_pad_all(lbl, 0, 0);
    lv_obj_set_pos(lbl, 0, 0);

    int tw = strlen(text) * 8 + 6;
    lv_obj_set_size(tag, tw, 11);
    lv_obj_set_pos(tag, *x_cursor, y);
    *x_cursor += tw + 3;
}

// ── Draw Vault Dweller figure (Pip-Boy style line art) ──
static void draw_vault_dweller(lv_obj_t* parent, int ox, int oy, bool female) {
    lv_color_t c = CLR_GREEN;

    // Head — filled circle
    lv_obj_t* head = lv_obj_create(parent);
    lv_obj_set_size(head, 14, 14);
    lv_obj_set_pos(head, ox + 13, oy);
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(head, c, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    // Neck
    static lv_point_t neck_pts[] = {{20, 14}, {20, 18}};
    lv_obj_t* neck = lv_line_create(parent);
    lv_line_set_points(neck, neck_pts, 2);
    lv_obj_set_style_line_color(neck, c, 0);
    lv_obj_set_style_line_width(neck, 2, 0);
    lv_obj_set_pos(neck, ox, oy);

    // Body / torso
    if (female) {
        // Fitted top + flared skirt
        static lv_point_t body_pts[] = {
            {16, 18}, {24, 18},  // shoulders
            {24, 28},            // waist right
            {28, 40},            // skirt right
            {12, 40},            // skirt left
            {16, 28},            // waist left
            {16, 18}             // close
        };
        lv_obj_t* body = lv_line_create(parent);
        lv_line_set_points(body, body_pts, 7);
        lv_obj_set_style_line_color(body, c, 0);
        lv_obj_set_style_line_width(body, 2, 0);
        lv_obj_set_pos(body, ox, oy);

        // Belt
        static lv_point_t belt_pts[] = {{15, 28}, {25, 28}};
        lv_obj_t* belt = lv_line_create(parent);
        lv_line_set_points(belt, belt_pts, 2);
        lv_obj_set_style_line_color(belt, CLR_GOLD, 0);
        lv_obj_set_style_line_width(belt, 1, 0);
        lv_obj_set_pos(belt, ox, oy);
    } else {
        // Rectangular torso
        static lv_point_t body_pts[] = {
            {16, 18}, {24, 18},  // shoulders
            {24, 38},            // hip right
            {16, 38},            // hip left
            {16, 18}             // close
        };
        lv_obj_t* body = lv_line_create(parent);
        lv_line_set_points(body, body_pts, 5);
        lv_obj_set_style_line_color(body, c, 0);
        lv_obj_set_style_line_width(body, 2, 0);
        lv_obj_set_pos(body, ox, oy);

        // Belt
        static lv_point_t belt_pts[] = {{15, 30}, {25, 30}};
        lv_obj_t* belt = lv_line_create(parent);
        lv_line_set_points(belt, belt_pts, 2);
        lv_obj_set_style_line_color(belt, CLR_GOLD, 0);
        lv_obj_set_style_line_width(belt, 1, 0);
        lv_obj_set_pos(belt, ox, oy);
    }

    // Left arm — raised, thumbs up!
    static lv_point_t arm_l_pts[] = {{16, 22}, {6, 16}, {4, 8}};
    lv_obj_t* arm_l = lv_line_create(parent);
    lv_line_set_points(arm_l, arm_l_pts, 3);
    lv_obj_set_style_line_color(arm_l, c, 0);
    lv_obj_set_style_line_width(arm_l, 2, 0);
    lv_obj_set_pos(arm_l, ox, oy);

    // Thumb (small line at top of left arm)
    static lv_point_t thumb_pts[] = {{4, 8}, {2, 4}};
    lv_obj_t* thumb = lv_line_create(parent);
    lv_line_set_points(thumb, thumb_pts, 2);
    lv_obj_set_style_line_color(thumb, c, 0);
    lv_obj_set_style_line_width(thumb, 2, 0);
    lv_obj_set_pos(thumb, ox, oy);

    // Fist circle at end of raised arm
    lv_obj_t* fist = lv_obj_create(parent);
    lv_obj_set_size(fist, 6, 6);
    lv_obj_set_pos(fist, ox + 2, oy + 6);
    lv_obj_set_style_radius(fist, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(fist, c, 0);
    lv_obj_set_style_bg_opa(fist, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(fist, 0, 0);
    lv_obj_set_style_pad_all(fist, 0, 0);
    lv_obj_clear_flag(fist, LV_OBJ_FLAG_SCROLLABLE);

    // Right arm — down/relaxed
    static lv_point_t arm_r_pts[] = {{24, 22}, {32, 28}, {30, 36}};
    lv_obj_t* arm_r = lv_line_create(parent);
    lv_line_set_points(arm_r, arm_r_pts, 3);
    lv_obj_set_style_line_color(arm_r, c, 0);
    lv_obj_set_style_line_width(arm_r, 2, 0);
    lv_obj_set_pos(arm_r, ox, oy);

    int leg_top = female ? 40 : 38;

    // Left leg
    lv_point_t* leg_l_pts = (lv_point_t*)lv_mem_alloc(sizeof(lv_point_t) * 2);
    leg_l_pts[0] = {18, (lv_coord_t)leg_top};
    leg_l_pts[1] = {14, 54};
    lv_obj_t* leg_l = lv_line_create(parent);
    lv_line_set_points(leg_l, leg_l_pts, 2);
    lv_obj_set_style_line_color(leg_l, c, 0);
    lv_obj_set_style_line_width(leg_l, 2, 0);
    lv_obj_set_pos(leg_l, ox, oy);

    // Right leg
    lv_point_t* leg_r_pts = (lv_point_t*)lv_mem_alloc(sizeof(lv_point_t) * 2);
    leg_r_pts[0] = {22, (lv_coord_t)leg_top};
    leg_r_pts[1] = {26, 54};
    lv_obj_t* leg_r = lv_line_create(parent);
    lv_line_set_points(leg_r, leg_r_pts, 2);
    lv_obj_set_style_line_color(leg_r, c, 0);
    lv_obj_set_style_line_width(leg_r, 2, 0);
    lv_obj_set_pos(leg_r, ox, oy);

    // Boots (small rectangles at feet)
    lv_obj_t* boot_l = lv_obj_create(parent);
    lv_obj_set_size(boot_l, 6, 3);
    lv_obj_set_pos(boot_l, ox + 11, oy + 54);
    lv_obj_set_style_bg_color(boot_l, c, 0);
    lv_obj_set_style_bg_opa(boot_l, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(boot_l, 0, 0);
    lv_obj_set_style_radius(boot_l, 0, 0);
    lv_obj_set_style_pad_all(boot_l, 0, 0);
    lv_obj_clear_flag(boot_l, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* boot_r = lv_obj_create(parent);
    lv_obj_set_size(boot_r, 6, 3);
    lv_obj_set_pos(boot_r, ox + 24, oy + 54);
    lv_obj_set_style_bg_color(boot_r, c, 0);
    lv_obj_set_style_bg_opa(boot_r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(boot_r, 0, 0);
    lv_obj_set_style_radius(boot_r, 0, 0);
    lv_obj_set_style_pad_all(boot_r, 0, 0);
    lv_obj_clear_flag(boot_r, LV_OBJ_FLAG_SCROLLABLE);
}

// ── Grill monitor panel ──

static void grill_close_panel(void) {
    if (grill_timer) { lv_timer_del(grill_timer); grill_timer = NULL; }
    grill_ble_stop();
    s_last_dev_count = -1;
    if (grill_panel) { lv_obj_del(grill_panel); grill_panel = NULL; }
    grill_status = NULL;
    grill_dev_list = NULL;
    grill_conn_lbl = NULL;
    for (int i = 0; i < GRILL_MAX_PROBES; i++) {
        grill_temp_labels[i] = NULL;
        grill_probe_labels[i] = NULL;
    }
}

static void grill_close_cb(lv_event_t* e) {
    (void)e;
    grill_close_panel();
}

static void grill_scan_cb(lv_event_t* e) {
    (void)e;
    s_last_dev_count = -1;  // force rebuild after scan
    grill_scan();
    if (grill_status) lv_label_set_text(grill_status, "Scanning...");
    sfx_play(SFX_CLICK);
}

static void grill_dev_click_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    grill_connect(idx);
    if (grill_status) {
        char buf[40];
        snprintf(buf, sizeof(buf), "Connecting to %s...", g_grill.devices[idx].name);
        lv_label_set_text(grill_status, buf);
    }
    sfx_play(SFX_CONNECT);
}

static void grill_disconnect_cb(lv_event_t* e) {
    (void)e;
    grill_disconnect();
    if (grill_status) lv_label_set_text(grill_status, "Disconnected");
    sfx_play(SFX_DISCONNECT);
}

// Rebuild scan results list
static void grill_rebuild_devices(void) {
    // Only rebuild if device count changed
    if (g_grill.device_count == s_last_dev_count) return;
    s_last_dev_count = g_grill.device_count;
    if (!grill_dev_list) return;
    lv_obj_clean(grill_dev_list);

    for (int i = 0; i < g_grill.device_count; i++) {
        lv_obj_t* row = lv_obj_create(grill_dev_list);
        lv_obj_set_size(row, 290, 20);
        lv_obj_set_style_bg_color(row, CLR_DARK, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, CLR_DIM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 2, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, grill_dev_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        char buf[48];
        snprintf(buf, sizeof(buf), "%s  %ddBm", g_grill.devices[i].name, g_grill.devices[i].rssi);
        lv_obj_t* l = lv_label_create(row);
        lv_label_set_text(l, buf);
        lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(l, CLR_GREEN, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    }

    if (g_grill.device_count == 0 && !g_grill.scanning) {
        lv_obj_t* l = lv_label_create(grill_dev_list);
        lv_label_set_text(l, "No devices found.\nMake sure grill BLE is on.\nTap SCAN to retry.");
        lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(l, CLR_DIM, 0);
        lv_obj_set_pos(l, 10, 10);
    }
}

// Timer callback — update grill panel
static void grill_timer_cb(lv_timer_t* t) {
    grill_poll();

    if (g_grill.connected) {
        // Show connected view — hide device list, show probes
        if (grill_dev_list) lv_obj_add_flag(grill_dev_list, LV_OBJ_FLAG_HIDDEN);

        if (grill_conn_lbl) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Connected: %s", g_grill.conn_name);
            lv_label_set_text(grill_conn_lbl, buf);
            lv_obj_clear_flag(grill_conn_lbl, LV_OBJ_FLAG_HIDDEN);
        }

        // Update temperature displays
        for (int i = 0; i < GRILL_MAX_PROBES; i++) {
            if (grill_probe_labels[i]) {
                lv_obj_clear_flag(grill_probe_labels[i], LV_OBJ_FLAG_HIDDEN);
            }
            if (grill_temp_labels[i]) {
                lv_obj_clear_flag(grill_temp_labels[i], LV_OBJ_FLAG_HIDDEN);
                if (g_grill.probes[i].connected && g_grill.probes[i].temp_f > 0) {
                    char buf[16];
                    int whole = g_grill.probes[i].temp_f / 10;
                    int frac  = abs(g_grill.probes[i].temp_f % 10);
                    snprintf(buf, sizeof(buf), "%d.%d F", whole, frac);
                    lv_label_set_text(grill_temp_labels[i], buf);
                    // Color by temperature
                    lv_color_t clr = CLR_GREEN;
                    if (whole >= 275) clr = lv_color_hex(0xCC0000);       // red — hot!
                    else if (whole >= 200) clr = lv_color_hex(0xCC6600);  // orange
                    else if (whole >= 150) clr = CLR_GOLD;                // gold
                    lv_obj_set_style_text_color(grill_temp_labels[i], clr, 0);
                } else {
                    lv_label_set_text(grill_temp_labels[i], "---");
                    lv_obj_set_style_text_color(grill_temp_labels[i], CLR_DIM, 0);
                }
            }
        }

        if (grill_status) {
            char buf[32];
            if (g_grill.last_update_ms > 0) {
                uint32_t ago = (millis() - g_grill.last_update_ms) / 1000;
                snprintf(buf, sizeof(buf), "Updated %ds ago", ago);
            } else {
                snprintf(buf, sizeof(buf), "Waiting for data...");
            }
            lv_label_set_text(grill_status, buf);
        }
    } else {
        // Show scan view
        if (grill_dev_list) lv_obj_clear_flag(grill_dev_list, LV_OBJ_FLAG_HIDDEN);
        if (grill_conn_lbl) lv_obj_add_flag(grill_conn_lbl, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < GRILL_MAX_PROBES; i++) {
            if (grill_probe_labels[i]) lv_obj_add_flag(grill_probe_labels[i], LV_OBJ_FLAG_HIDDEN);
            if (grill_temp_labels[i]) lv_obj_add_flag(grill_temp_labels[i], LV_OBJ_FLAG_HIDDEN);
        }

        if (grill_is_connecting()) {
            if (grill_status) lv_label_set_text(grill_status, "Connecting...");
        } else if (g_grill.scanning) {
            if (grill_status) lv_label_set_text(grill_status, "Scanning...");
        } else {
            grill_rebuild_devices();
            if (grill_status && g_grill.device_count > 0)
                lv_label_set_text(grill_status, "Tap device to connect");
        }
    }
}

static void open_grill_monitor(void) {
    if (grill_panel) return;

    grill_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(grill_panel, 320, 222);
    lv_obj_set_pos(grill_panel, 0, 18);
    lv_obj_set_style_bg_color(grill_panel, CLR_BG, 0);
    lv_obj_set_style_bg_opa(grill_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(grill_panel, 0, 0);
    lv_obj_set_style_pad_all(grill_panel, 4, 0);
    lv_obj_clear_flag(grill_panel, LV_OBJ_FLAG_SCROLLABLE);

    // Title bar
    lv_obj_t* title = lv_label_create(grill_panel);
    lv_label_set_text(title, PROBE_MONITOR_TITLE);
    lv_obj_set_style_text_font(title, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(title, CLR_GREEN, 0);
    lv_obj_set_pos(title, 4, 2);

    // Status label
    grill_status = lv_label_create(grill_panel);
    lv_label_set_text(grill_status, "Starting BLE...");
    lv_obj_set_style_text_font(grill_status, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(grill_status, CLR_DIM, 0);
    lv_obj_set_pos(grill_status, 4, 14);
    lv_label_set_long_mode(grill_status, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(grill_status, 240);

    // SCAN button
    lv_obj_t* scan_btn = lv_btn_create(grill_panel);
    lv_obj_set_size(scan_btn, 52, 20);
    lv_obj_set_pos(scan_btn, 200, 0);
    lv_obj_set_style_bg_color(scan_btn, CLR_SEL, 0);
    lv_obj_set_style_border_color(scan_btn, CLR_GREEN, 0);
    lv_obj_set_style_border_width(scan_btn, 1, 0);
    lv_obj_set_style_radius(scan_btn, 2, 0);
    lv_obj_set_style_pad_all(scan_btn, 0, 0);
    lv_obj_add_event_cb(scan_btn, grill_scan_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* scan_l = lv_label_create(scan_btn);
    lv_label_set_text(scan_l, "SCAN");
    lv_obj_set_style_text_font(scan_l, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(scan_l, CLR_GREEN, 0);
    lv_obj_align(scan_l, LV_ALIGN_CENTER, 0, 0);

    // DISCONNECT button
    lv_obj_t* dc_btn = lv_btn_create(grill_panel);
    lv_obj_set_size(dc_btn, 32, 20);
    lv_obj_set_pos(dc_btn, 256, 0);
    lv_obj_set_style_bg_color(dc_btn, CLR_SEL, 0);
    lv_obj_set_style_border_color(dc_btn, lv_color_hex(0xCC6600), 0);
    lv_obj_set_style_border_width(dc_btn, 1, 0);
    lv_obj_set_style_radius(dc_btn, 2, 0);
    lv_obj_set_style_pad_all(dc_btn, 0, 0);
    lv_obj_add_event_cb(dc_btn, grill_disconnect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* dc_l = lv_label_create(dc_btn);
    lv_label_set_text(dc_l, "DC");
    lv_obj_set_style_text_font(dc_l, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(dc_l, lv_color_hex(0xCC6600), 0);
    lv_obj_align(dc_l, LV_ALIGN_CENTER, 0, 0);

    // CLOSE button
    lv_obj_t* close_btn = lv_btn_create(grill_panel);
    lv_obj_set_size(close_btn, 24, 20);
    lv_obj_set_pos(close_btn, 290, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0xCC0000), 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_set_style_radius(close_btn, 2, 0);
    lv_obj_set_style_pad_all(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, grill_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* cl_l = lv_label_create(close_btn);
    lv_label_set_text(cl_l, "X");
    lv_obj_set_style_text_font(cl_l, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(cl_l, lv_color_white(), 0);
    lv_obj_align(cl_l, LV_ALIGN_CENTER, 0, 0);

    // Divider
    lv_obj_t* div = lv_obj_create(grill_panel);
    lv_obj_set_size(div, 308, 1);
    lv_obj_set_pos(div, 0, 26);
    lv_obj_set_style_bg_color(div, CLR_DARK, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);

    // Device list (scan results)
    grill_dev_list = lv_obj_create(grill_panel);
    lv_obj_set_size(grill_dev_list, 306, 120);
    lv_obj_set_pos(grill_dev_list, 2, 30);
    lv_obj_set_style_bg_opa(grill_dev_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grill_dev_list, 0, 0);
    lv_obj_set_style_pad_all(grill_dev_list, 2, 0);
    lv_obj_set_style_pad_row(grill_dev_list, 2, 0);
    lv_obj_set_flex_flow(grill_dev_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(grill_dev_list, LV_SCROLLBAR_MODE_AUTO);

    // Connected device label (hidden until connected)
    grill_conn_lbl = lv_label_create(grill_panel);
    lv_label_set_text(grill_conn_lbl, "");
    lv_obj_set_style_text_font(grill_conn_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(grill_conn_lbl, CLR_GREEN, 0);
    lv_obj_set_pos(grill_conn_lbl, 4, 32);
    lv_obj_add_flag(grill_conn_lbl, LV_OBJ_FLAG_HIDDEN);

    // Probe temperature displays (2x2 grid, large text)
    const char* probe_names[] = {"PROBE 1", "PROBE 2", "PROBE 3", "PROBE 4"};
    int px[] = {10, 160, 10, 160};
    int py[] = {50, 50, 120, 120};

    for (int i = 0; i < GRILL_MAX_PROBES; i++) {
        grill_probe_labels[i] = lv_label_create(grill_panel);
        lv_label_set_text(grill_probe_labels[i], probe_names[i]);
        lv_obj_set_style_text_font(grill_probe_labels[i], &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(grill_probe_labels[i], CLR_DIM, 0);
        lv_obj_set_pos(grill_probe_labels[i], px[i], py[i]);
        lv_obj_add_flag(grill_probe_labels[i], LV_OBJ_FLAG_HIDDEN);

        grill_temp_labels[i] = lv_label_create(grill_panel);
        lv_label_set_text(grill_temp_labels[i], "---");
        lv_obj_set_style_text_font(grill_temp_labels[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(grill_temp_labels[i], CLR_GREEN, 0);
        lv_obj_set_pos(grill_temp_labels[i], px[i], py[i] + 12);
        lv_obj_add_flag(grill_temp_labels[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Footer hint
    lv_obj_t* hint = lv_label_create(grill_panel);
    lv_label_set_text(hint, "ThermoPro BLE | Check serial for debug");
    lv_obj_set_style_text_font(hint, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(hint, CLR_DIM, 0);
    lv_obj_set_pos(hint, 4, 198);

    // Start BLE and scan
    s_last_dev_count = -1;  // force rebuild on open
    grill_ble_start();

    // Update timer — poll BLE + refresh UI every 500ms
    grill_timer = lv_timer_create(grill_timer_cb, 500, NULL);

    sfx_play(SFX_BOOT);
}

// ── Settings menu callbacks ──
static void close_settings_menu(void) {
    if (settings_menu) {
        lv_obj_del(settings_menu);
        settings_menu = NULL;
    }
}

static void settings_wifi_cb(lv_event_t* e) {
    (void)e;
    close_settings_menu();
    // Defer navigation so event callback finishes first
    lv_timer_create([](lv_timer_t* t) { lv_timer_del(t); nav_to_wifi(); }, 50, NULL);
}

static void settings_user_cb(lv_event_t* e) {
    (void)e;
    close_settings_menu();
    lv_timer_create([](lv_timer_t* t) { lv_timer_del(t); nav_to_login(); }, 50, NULL);
}

static void settings_cancel_cb(lv_event_t* e) {
    (void)e;
    close_settings_menu();
}

static void settings_grill_cb(lv_event_t* e) {
    (void)e;
    close_settings_menu();
    open_grill_monitor();
}

static void settings_clock_cb(lv_event_t* e) {
    (void)e;
    close_settings_menu();
    lv_timer_create([](lv_timer_t* t) { lv_timer_del(t); clock_mode_enter(); }, 50, NULL);
}

static void settings_settings_cb(lv_event_t* e) {
    (void)e;
    close_settings_menu();
    lv_timer_create([](lv_timer_t* t) { lv_timer_del(t); tabs_open_settings(); }, 50, NULL);
}

static void settings_mainmenu_cb(lv_event_t* e) {
    (void)e;
    close_settings_menu();
    lv_timer_create([](lv_timer_t* t) { lv_timer_del(t); nav_to_main_menu(); }, 50, NULL);
}

static void gear_click_cb(lv_event_t* e) {
    (void)e;
    if (grill_panel) { grill_close_panel(); return; }
    if (spk_panel) { spk_close_panel(); return; }
    if (settings_menu) {
        close_settings_menu();
        return;
    }

    // Create popup menu on layer_top so it's above everything
    settings_menu = lv_obj_create(lv_layer_top());
    lv_obj_set_size(settings_menu, 140, 162);
    lv_obj_set_pos(settings_menu, 4, 70);
    lv_obj_set_style_bg_color(settings_menu, lv_color_hex(0x002a0e), 0);
    lv_obj_set_style_bg_opa(settings_menu, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(settings_menu, CLR_GREEN, 0);
    lv_obj_set_style_border_width(settings_menu, 1, 0);
    lv_obj_set_style_radius(settings_menu, 0, 0);
    lv_obj_set_style_pad_all(settings_menu, 4, 0);
    lv_obj_clear_flag(settings_menu, LV_OBJ_FLAG_SCROLLABLE);

    auto mk_menu_btn = [](lv_obj_t* par, const char* txt, int y,
                          lv_event_cb_t cb) {
        lv_obj_t* btn = lv_obj_create(par);
        lv_obj_set_size(btn, 128, 18);
        lv_obj_set_pos(btn, 2, y);
        lv_obj_set_style_bg_color(btn, CLR_DARK, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, CLR_DIM, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, txt);
        lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(lbl, CLR_GREEN, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    };

    mk_menu_btn(settings_menu, PROBE_MONITOR_TITLE,  0,   settings_grill_cb);
    mk_menu_btn(settings_menu, "CLOCK MODE",     22,  settings_clock_cb);
    mk_menu_btn(settings_menu, "SETTINGS",       44,  settings_settings_cb);
    mk_menu_btn(settings_menu, "CHANGE WIFI",    66,  settings_wifi_cb);
    mk_menu_btn(settings_menu, "SWITCH USER",    88,  settings_user_cb);
    mk_menu_btn(settings_menu, "MAIN MENU",     110,  settings_mainmenu_cb);
    mk_menu_btn(settings_menu, "CANCEL",        132,  settings_cancel_cb);
}

// ── Delete callback ──
static void stat_screen_delete_cb(lv_event_t* e) {
    grill_close_panel();
    spk_close_panel();
    if (settings_menu) {
        lv_obj_del(settings_menu);
        settings_menu = NULL;
    }
    scr_stat = NULL;
}

// ── Build screen ──
lv_obj_t* screen_stat_create(void) {
    const Dweller& d = team[g_dweller_idx];

    // Reset stale pointers from previous instance
    settings_menu = NULL;
    spk_panel = NULL;
    spk_freq_lbl = NULL;
    grill_panel = NULL;
    grill_timer = NULL;

    scr_stat = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_stat, CLR_BG, 0);
    lv_obj_set_style_bg_opa(scr_stat, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr_stat, 0, 0);
    lv_obj_set_style_border_width(scr_stat, 0, 0);
    lv_obj_clear_flag(scr_stat, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_stat, stat_screen_delete_cb, LV_EVENT_DELETE, NULL);

    // ═══ LEFT PANEL (90px) ═══
    lv_obj_t* left = lv_obj_create(scr_stat);
    lv_obj_set_size(left, 90, 222);
    lv_obj_set_pos(left, 0, 18);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(left, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_color(left, CLR_DARK, 0);
    lv_obj_set_style_border_width(left, 1, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_set_style_radius(left, 0, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    // Vault Dweller figure (Pip-Boy style, ~40x57px)
    draw_vault_dweller(left, 24, 2, d.female);

    // Char name (centered, clip)
    lbl_name = make_label(left, d.name, &lv_font_unscii_8, CLR_GREEN, 1, 62, 88);
    lv_obj_set_style_text_align(lbl_name, LV_TEXT_ALIGN_CENTER, 0);

    // Title (scrolling if too wide)
    lbl_title = lv_label_create(left);
    lv_label_set_text(lbl_title, d.title);
    lv_obj_set_style_text_font(lbl_title, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(lbl_title, CLR_DIM, 0);
    lv_obj_set_style_bg_opa(lbl_title, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lbl_title, 0, 0);
    lv_obj_set_style_pad_all(lbl_title, 0, 0);
    lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(lbl_title, 86);
    lv_obj_set_pos(lbl_title, 2, 73);

    // HP bar
    make_label(left, "HP", &lv_font_unscii_8, CLR_DIM, 5, 86, 16);
    char hp_txt[12];
    snprintf(hp_txt, sizeof(hp_txt), "%d/100", d.hp);
    make_label(left, hp_txt, &lv_font_unscii_8, CLR_DIM, 24, 86, 60);
    bar_hp = make_bar(left, 5, 96, 80, 5, CLR_GREEN, d.hp);

    // MORALE bar
    make_label(left, "MRL", &lv_font_unscii_8, CLR_DIM, 5, 104, 24);
    char mor_txt[12];
    snprintf(mor_txt, sizeof(mor_txt), "%d/100", d.morale);
    make_label(left, mor_txt, &lv_font_unscii_8, CLR_DIM, 32, 104, 52);
    bar_morale = make_bar(left, 5, 114, 80, 5, CLR_DIM, d.morale);

    // VAULT 89 sign box
    lv_obj_t* vsign = lv_obj_create(left);
    lv_obj_set_size(vsign, 80, 38);
    lv_obj_set_pos(vsign, 5, 124);
    lv_obj_set_style_bg_color(vsign, lv_color_hex(0x003410), 0);
    lv_obj_set_style_bg_opa(vsign, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(vsign, CLR_DIM, 0);
    lv_obj_set_style_border_width(vsign, 1, 0);
    lv_obj_set_style_radius(vsign, 0, 0);
    lv_obj_set_style_pad_all(vsign, 2, 0);
    lv_obj_clear_flag(vsign, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* vs_top = make_label(vsign, "VAULT", &lv_font_unscii_8, CLR_DIM, 0, 1, 76);
    lv_obj_set_style_text_align(vs_top, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* vs_num = make_label(vsign, VAULT_NUM, &lv_font_montserrat_20, CLR_GREEN, 0, 10, 76);
    lv_obj_set_style_text_align(vs_num, LV_TEXT_ALIGN_CENTER, 0);

    // EST row
    make_label(left, "EST.", &lv_font_unscii_8, CLR_DIM, 5, 166, 30);
    make_label(left, "2287", &lv_font_unscii_8, CLR_GREEN, 48, 166, 36);

    // ═══ RIGHT PANEL (229px) ═══
    lv_obj_t* right = lv_obj_create(scr_stat);
    lv_obj_set_size(right, 229, 222);
    lv_obj_set_pos(right, 91, 18);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 5, 0);
    lv_obj_set_style_radius(right, 0, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    // Section: dweller aptitude scores
    make_sec_label(right, "DWELLER STATS", 0, 0, 219);

    const char* stat_names[] = {"SAF","PRC","EFF","CRF","INN","ADP","LDR"};
    uint8_t stat_vals[] = {d.saf, d.prc, d.eff, d.crf, d.inn, d.adp, d.ldr};
    int cw = 52, ch = 30, gap = 2;
    for (int i = 0; i < 7; i++) {
        int col = i % 4;
        int row = i / 4;
        int cx  = col * (cw + gap);
        int cy  = 14 + row * (ch + gap);
        lbl_sv[i] = make_stat_cell(right, stat_names[i], stat_vals[i], cx, cy, cw, ch);
    }

    // Section: STATUS
    make_sec_label(right, "STATUS", 0, 80, 219);

    make_label(right, "RAD", &lv_font_unscii_8, CLR_DIM, 0, 93, 22);
    bar_rad = make_bar(right, 26, 93, 193, 5, CLR_RAD, d.rad);

    make_label(right, "HNG", &lv_font_unscii_8, CLR_DIM, 0, 103, 22);
    bar_hunger = make_bar(right, 26, 103, 193, 5, CLR_DIM, d.hunger);

    // Section: TRADE PERKS
    make_sec_label(right, "TRADE PERKS", 0, 115, 219);

    auto make_perk = [&](const char* icon, const char* name, const char* rank, int y) {
        lv_obj_t* ico = lv_obj_create(right);
        lv_obj_set_size(ico, 10, 10);
        lv_obj_set_pos(ico, 0, y);
        lv_obj_set_style_bg_opa(ico, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(ico, CLR_DARK, 0);
        lv_obj_set_style_border_width(ico, 1, 0);
        lv_obj_set_style_radius(ico, 0, 0);
        lv_obj_set_style_pad_all(ico, 0, 0);
        lv_obj_clear_flag(ico, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* ico_lbl = lv_label_create(ico);
        lv_label_set_text(ico_lbl, icon);
        lv_obj_set_style_text_font(ico_lbl, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(ico_lbl, CLR_GREEN, 0);
        lv_obj_set_style_pad_all(ico_lbl, 0, 0);
        lv_obj_align(ico_lbl, LV_ALIGN_CENTER, 0, 0);

        lv_obj_t* nl = make_label(right, name, &lv_font_unscii_8, CLR_GREEN, 14, y, 175);
        make_label(right, rank, &lv_font_unscii_8, CLR_DIM, 194, y, 25);
        return nl;
    };

    lbl_p1 = make_perk("Z", d.p1, "III", 129);
    lbl_p2 = make_perk("#", d.p2, "II",  141);
    lbl_p3 = make_perk("^", d.p3, "I",   153);

    // Effect tags — anchored to bottom of right panel
    int tag_x = 0;
    make_etag(right, "WELL RESTED", true,  &tag_x, 194);
    make_etag(right, "WELL FED",    false, &tag_x, 194);
    tag_x = 0;
    make_etag(right, VAULT_ID,      false, &tag_x, 207);
    make_etag(right, "DWELLER",     false, &tag_x, 207);

    // ── Settings gear button (bottom-left) ──
    lv_obj_t* gear_btn = lv_obj_create(scr_stat);
    lv_obj_set_size(gear_btn, 36, 24);
    lv_obj_set_pos(gear_btn, 0, 216);
    lv_obj_set_style_bg_color(gear_btn, CLR_DARK, 0);
    lv_obj_set_style_bg_opa(gear_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(gear_btn, CLR_DIM, 0);
    lv_obj_set_style_border_width(gear_btn, 1, 0);
    lv_obj_set_style_radius(gear_btn, 0, 0);
    lv_obj_set_style_pad_all(gear_btn, 0, 0);
    lv_obj_clear_flag(gear_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(gear_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(gear_btn, gear_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* gear_lbl = lv_label_create(gear_btn);
    lv_label_set_text(gear_lbl, "*");
    lv_obj_set_style_text_font(gear_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(gear_lbl, CLR_DIM, 0);
    lv_obj_align(gear_lbl, LV_ALIGN_CENTER, 0, 0);

    return scr_stat;
}

void screen_stat_load(int dweller_idx) {
    g_dweller_idx = dweller_idx;
    if (!scr_stat) return;

    const Dweller& d = team[dweller_idx];

    lv_label_set_text(lbl_name,  d.name);
    lv_label_set_text(lbl_title, d.title);

    uint8_t vals[] = {d.saf, d.prc, d.eff, d.crf, d.inn, d.adp, d.ldr};
    for (int i = 0; i < 7; i++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", vals[i]);
        lv_label_set_text(lbl_sv[i], buf);
        if (vals[i] >= 10)
            lv_obj_set_style_text_color(lbl_sv[i], CLR_GOLD, 0);
        else if (vals[i] >= 8)
            lv_obj_set_style_text_color(lbl_sv[i], CLR_HI, 0);
        else
            lv_obj_set_style_text_color(lbl_sv[i], CLR_GREEN, 0);
    }

    lv_label_set_text(lbl_p1, d.p1);
    lv_label_set_text(lbl_p2, d.p2);
    lv_label_set_text(lbl_p3, d.p3);
}

lv_obj_t* screen_stat_get(void) { return scr_stat; }
