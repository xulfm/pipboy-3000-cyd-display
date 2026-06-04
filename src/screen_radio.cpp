// ============================================================
// Vault 89 PDI-Boy -- RADIO Screen
// Left panel: Live ESP-NOW peers with signal bars
// Right panel: Chat (group + DM) with keyboard
// ============================================================

#include <Arduino.h>
#include <lvgl.h>
#include "pipboy_data.h"
#include "net_state.h"
#include "sound_fx.h"

#define CLR_RED lv_color_hex(0xCC0000)

static lv_obj_t* scr_radio = NULL;

// State
static int radio_sel = -1;       // selected peer index (-1 = none)
static bool chat_dm_mode = false; // false = GROUP, true = DM to selected peer
static int  last_chat_count = 0;  // track new messages

// UI pointers
static lv_obj_t* peer_list      = NULL;
static lv_obj_t* peer_cnt_lbl   = NULL;
static lv_obj_t* chat_list      = NULL;   // scrollable chat message area
static lv_obj_t* chat_input     = NULL;   // textarea for typing
static lv_obj_t* mode_btn_lbl   = NULL;   // GROUP/DM label on mode button
static lv_obj_t* dm_target_lbl  = NULL;   // shows DM target name
static lv_obj_t* kb_overlay     = NULL;   // full-screen keyboard overlay
static lv_obj_t* kb_textarea    = NULL;   // textarea in keyboard overlay
static lv_obj_t* kb_btnm        = NULL;   // button matrix keyboard
static lv_obj_t* wifi_status_lbl = NULL;
static lv_obj_t* ws_status_lbl   = NULL;
static lv_timer_t* radio_refresh_timer = NULL;
static lv_timer_t* wave_anim_timer     = NULL;
static lv_obj_t*   wave_line           = NULL;
static lv_point_t  wave_pts[17];
static float       wave_phase          = 0.0f;

// Keyboard state
static bool kb_shift = false;
static int  kb_page  = 0;  // 0=alpha, 1=num

// ── Keyboard layout (reused from WiFi screen pattern) ──
static const char* kb_alpha_lower[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    " ", "a", "s", "d", "f", "g", "h", "j", "k", "l", " ", "\n",
    "^", "z", "x", "c", "v", "b", "n", "m", "<", "\n",
    "#", "_", "                ", ".", ">", ""
};
static const char* kb_alpha_upper[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    " ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
    "^", "Z", "X", "C", "V", "B", "N", "M", "<", "\n",
    "#", "_", "                ", ".", ">", ""
};
static const char* kb_num[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "!", "@", "#", "$", "%", "&", "*", "(", ")", "-", "\n",
    "?", "/", ":", ";", "'", "\"", "+", "=", "<", "\n",
    "a", "_", "                ", ".", ">", ""
};

// ── Helpers ──
static lv_obj_t* mk_label(lv_obj_t* par, const char* txt,
                           const lv_font_t* f, lv_color_t c,
                           int x, int y, int w = 0) {
    lv_obj_t* l = lv_label_create(par);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(l, 0, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    if (w > 0) {
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(l, w);
    }
    lv_obj_set_pos(l, x, y);
    return l;
}

static void make_sig_bars(lv_obj_t* parent, int sig_level, int x, int y) {
    for (int i = 0; i < 4; i++) {
        int h = 3 + i * 2;
        lv_obj_t* bar = lv_obj_create(parent);
        lv_obj_set_size(bar, 3, h);
        lv_obj_set_pos(bar, x + i * 4, y + (10 - h));
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(bar, i < sig_level ? CLR_GREEN : CLR_DARK, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    }
}

// ── Chat display ──
static void rebuild_chat(void) {
    if (!chat_list) return;
    lv_obj_clean(chat_list);

    for (int i = 0; i < g_chat_count; i++) {
        int idx = (g_chat_head + i) % CHAT_MAX;
        const ChatMsg& m = g_chat[idx];

        lv_obj_t* row = lv_obj_create(chat_list);
        lv_obj_set_size(row, 174, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 1, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Format: "[DM] Name: text" or "Name: text"
        char buf[96];
        if (m.is_dm) {
            snprintf(buf, sizeof(buf), "[DM] %s: %s", m.from, m.text);
        } else {
            snprintf(buf, sizeof(buf), "%s: %s", m.from, m.text);
        }

        lv_obj_t* l = lv_label_create(row);
        lv_label_set_text(l, buf);
        lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(l, m.is_dm ? lv_color_hex(0xCC6600) : CLR_GREEN, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, 170);
    }

    // Scroll to bottom
    lv_obj_scroll_to_y(chat_list, LV_COORD_MAX, LV_ANIM_OFF);
    last_chat_count = g_chat_count;
}

// ── Send message ──
static void send_message(const char* text) {
    if (!text || text[0] == '\0') return;

    if (chat_dm_mode && radio_sel >= 0 && radio_sel < g_peer_count) {
        espnow_send_dm(radio_sel, text);
        sfx_play(SFX_CLICK);
    } else {
        espnow_send_chat(text);
        sfx_play(SFX_MSG_IN);
    }
    rebuild_chat();
}

// ── Keyboard callbacks ──
static void kb_key_cb(lv_event_t* e) {
    uint32_t id = lv_btnmatrix_get_selected_btn(kb_btnm);
    const char* txt = lv_btnmatrix_get_btn_text(kb_btnm, id);
    if (!txt) return;

    if (strcmp(txt, "^") == 0) {
        kb_shift = !kb_shift;
        lv_btnmatrix_set_map(kb_btnm, kb_shift ? kb_alpha_upper : kb_alpha_lower);
    } else if (strcmp(txt, "#") == 0) {
        kb_page = 1;
        lv_btnmatrix_set_map(kb_btnm, kb_num);
    } else if (strcmp(txt, "a") == 0 && kb_page == 1) {
        kb_page = 0;
        lv_btnmatrix_set_map(kb_btnm, kb_shift ? kb_alpha_upper : kb_alpha_lower);
    } else if (strcmp(txt, "<") == 0) {
        lv_textarea_del_char(kb_textarea);
    } else if (strcmp(txt, "_") == 0) {
        lv_textarea_add_char(kb_textarea, ' ');
    } else if (strcmp(txt, ">") == 0) {
        // Send and close
        const char* msg = lv_textarea_get_text(kb_textarea);
        send_message(msg);
        lv_textarea_set_text(kb_textarea, "");
        lv_obj_add_flag(kb_overlay, LV_OBJ_FLAG_HIDDEN);
    } else if (strlen(txt) == 1) {
        lv_textarea_add_char(kb_textarea, txt[0]);
        if (kb_shift && kb_page == 0) {
            kb_shift = false;
            lv_btnmatrix_set_map(kb_btnm, kb_alpha_lower);
        }
    }
}

static void kb_close_cb(lv_event_t* e) {
    (void)e;
    lv_obj_add_flag(kb_overlay, LV_OBJ_FLAG_HIDDEN);
}

// Open keyboard overlay
static void open_keyboard(lv_event_t* e) {
    (void)e;
    if (!kb_overlay) return;
    lv_textarea_set_text(kb_textarea, "");
    kb_shift = false;
    kb_page = 0;
    lv_btnmatrix_set_map(kb_btnm, kb_alpha_lower);
    lv_obj_clear_flag(kb_overlay, LV_OBJ_FLAG_HIDDEN);
}

// ── Mode toggle (GROUP / DM) ──
static void mode_toggle_cb(lv_event_t* e) {
    (void)e;
    chat_dm_mode = !chat_dm_mode;
    if (chat_dm_mode) {
        lv_label_set_text(mode_btn_lbl, "DM");
        if (radio_sel >= 0 && radio_sel < g_peer_count) {
            char buf[24];
            snprintf(buf, sizeof(buf), "> %s", g_peers[radio_sel].name);
            lv_label_set_text(dm_target_lbl, buf);
        } else {
            lv_label_set_text(dm_target_lbl, "Select peer");
        }
    } else {
        lv_label_set_text(mode_btn_lbl, "ALL");
        lv_label_set_text(dm_target_lbl, "Group chat");
    }
}

// ── Peer click ──
static void peer_click_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    radio_sel = idx;

    uint32_t cnt = lv_obj_get_child_cnt(peer_list);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t* row = lv_obj_get_child(peer_list, i);
        if ((int)i == idx) {
            lv_obj_set_style_bg_color(row, CLR_SEL, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_color(row, CLR_GREEN, 0);
            lv_obj_set_style_border_width(row, 2, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(row, 0, 0);
        }
    }

    // Update DM target label if in DM mode
    if (chat_dm_mode && dm_target_lbl && idx >= 0 && idx < g_peer_count) {
        char buf[24];
        snprintf(buf, sizeof(buf), "> %s", g_peers[idx].name);
        lv_label_set_text(dm_target_lbl, buf);
    }
}

// ── Rebuild peer list ──
static void rebuild_peer_list(void) {
    if (!peer_list) return;
    lv_obj_clean(peer_list);

    for (int i = 0; i < g_peer_count; i++) {
        const EspNowPeer& p = g_peers[i];

        lv_obj_t* row = lv_obj_create(peer_list);
        lv_obj_set_size(row, 118, 18);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x005c22), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_left(row, 5, 0);
        lv_obj_set_style_pad_right(row, 3, 0);
        lv_obj_set_style_pad_top(row, 2, 0);
        lv_obj_set_style_pad_bottom(row, 2, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, peer_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        // Highlight selected
        if (i == radio_sel) {
            lv_obj_set_style_bg_color(row, CLR_SEL, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_color(row, CLR_GREEN, 0);
            lv_obj_set_style_border_width(row, 2, 0);
        }

        // Online dot
        lv_obj_t* dot = lv_obj_create(row);
        lv_obj_set_size(dot, 5, 5);
        lv_obj_set_pos(dot, 0, 4);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(dot, p.online ? CLR_GREEN : CLR_DARK, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

        // Pre-login peers broadcast an anonymous discovery beacon with
        // an empty name so the login screen's dweller matcher doesn't
        // mark random dwellers as IN USE. Fall back to a placeholder
        // label so the row isn't rendered blank.
        const char* nm = p.name[0] ? p.name : "(peer)";
        mk_label(row, nm, &lv_font_unscii_8,
                 p.online ? CLR_GREEN : CLR_DIM, 9, 2, 68);

        make_sig_bars(row, p.sig_level, 82, 2);
    }

    if (peer_cnt_lbl) {
        int online = 0;
        for (int i = 0; i < g_peer_count; i++) if (g_peers[i].online) online++;
        char buf[12];
        snprintf(buf, sizeof(buf), "%d online", online);
        lv_label_set_text(peer_cnt_lbl, buf);
    }

    if (g_peer_count == 0) {
        mk_label(peer_list, "SCANNING...", &lv_font_unscii_8, CLR_DIM, 10, 30, 90);
    }
}

// ── Refresh timer ──
static void refresh_timer_cb(lv_timer_t* t) {
    rebuild_peer_list();

    // Auto-refresh chat if new messages arrived
    if (g_chat_count != last_chat_count) {
        rebuild_chat();
    }

    if (wifi_status_lbl) {
        if (g_net.wifi_connected) {
            char buf[16];
            snprintf(buf, sizeof(buf), "CH %d", g_net.wifi_channel);
            lv_label_set_text(wifi_status_lbl, buf);
        } else {
            lv_label_set_text(wifi_status_lbl, "NO WIFI");
        }
    }
    if (ws_status_lbl) {
        lv_label_set_text(ws_status_lbl, g_net.ws_connected ? "WS OK" : "WS --");
    }
}

static void wave_anim_cb(lv_timer_t* t) {
    if (!wave_line) return;
    wave_phase += 0.3f;
    for (int i = 0; i < 17; i++) {
        wave_pts[i].y = 9 + (int)(6.0f * sinf(i * 0.8f + wave_phase));
    }
    lv_line_set_points(wave_line, wave_pts, 17);
}

static void radio_screen_delete_cb(lv_event_t* e) {
    if (radio_refresh_timer) { lv_timer_del(radio_refresh_timer); radio_refresh_timer = NULL; }
    if (wave_anim_timer) { lv_timer_del(wave_anim_timer); wave_anim_timer = NULL; }
    wave_line = NULL;
    peer_list = NULL;
    peer_cnt_lbl = NULL;
    chat_list = NULL;
    chat_input = NULL;
    mode_btn_lbl = NULL;
    dm_target_lbl = NULL;
    kb_overlay = NULL;
    kb_textarea = NULL;
    kb_btnm = NULL;
    wifi_status_lbl = ws_status_lbl = NULL;
    scr_radio = NULL;
}

// ── Build screen ──
lv_obj_t* screen_radio_create(void) {
    if (radio_refresh_timer) { lv_timer_del(radio_refresh_timer); radio_refresh_timer = NULL; }
    if (wave_anim_timer) { lv_timer_del(wave_anim_timer); wave_anim_timer = NULL; }
    wave_line = NULL;

    scr_radio = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_radio, CLR_BG, 0);
    lv_obj_set_style_bg_opa(scr_radio, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr_radio, 0, 0);
    lv_obj_set_style_border_width(scr_radio, 0, 0);
    lv_obj_clear_flag(scr_radio, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_radio, radio_screen_delete_cb, LV_EVENT_DELETE, NULL);

    // ═══ LEFT PANEL (120px) ═══
    lv_obj_t* left = lv_obj_create(scr_radio);
    lv_obj_set_size(left, 120, 222);
    lv_obj_set_pos(left, 0, 18);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(left, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_color(left, CLR_DARK, 0);
    lv_obj_set_style_border_width(left, 1, 0);
    lv_obj_set_style_radius(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    // Header
    mk_label(left, "PEERS", &lv_font_unscii_8, CLR_DIM, 5, 3, 42);
    peer_cnt_lbl = mk_label(left, "0 online", &lv_font_unscii_8, CLR_DIM, 50, 3, 68);
    lv_obj_set_style_text_align(peer_cnt_lbl, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t* hdiv = lv_obj_create(left);
    lv_obj_set_size(hdiv, 120, 1);
    lv_obj_set_pos(hdiv, 0, 12);
    lv_obj_set_style_bg_color(hdiv, CLR_DARK, 0);
    lv_obj_set_style_bg_opa(hdiv, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdiv, 0, 0);
    lv_obj_clear_flag(hdiv, LV_OBJ_FLAG_SCROLLABLE);

    // Peer list
    peer_list = lv_obj_create(left);
    lv_obj_set_size(peer_list, 120, 174);
    lv_obj_set_pos(peer_list, 0, 13);
    lv_obj_set_style_bg_opa(peer_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(peer_list, 0, 0);
    lv_obj_set_style_radius(peer_list, 0, 0);
    lv_obj_set_style_pad_all(peer_list, 0, 0);
    lv_obj_set_style_pad_row(peer_list, 0, 0);
    lv_obj_set_flex_flow(peer_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(peer_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(peer_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(peer_list, CLR_DARK, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(peer_list, LV_OPA_COVER, LV_PART_SCROLLBAR);

    rebuild_peer_list();

    // Footer
    lv_obj_t* foot = lv_obj_create(left);
    lv_obj_set_size(foot, 120, 34);
    lv_obj_set_pos(foot, 0, 188);
    lv_obj_set_style_bg_opa(foot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(foot, CLR_DARK, 0);
    lv_obj_set_style_border_width(foot, 1, 0);
    lv_obj_set_style_radius(foot, 0, 0);
    lv_obj_set_style_pad_all(foot, 0, 0);
    lv_obj_clear_flag(foot, LV_OBJ_FLAG_SCROLLABLE);

    mk_label(foot, "WIFI", &lv_font_unscii_8, CLR_DIM, 5, 3, 30);
    wifi_status_lbl = mk_label(foot, "---", &lv_font_unscii_8, CLR_GREEN, 40, 3, 76);
    mk_label(foot, "PROTO", &lv_font_unscii_8, CLR_DIM, 5, 14, 30);
    ws_status_lbl = mk_label(foot, "ESP-NOW", &lv_font_unscii_8, CLR_GREEN, 40, 14, 76);

    // ═══ RIGHT PANEL (200px) — CHAT ═══
    lv_obj_t* right = lv_obj_create(scr_radio);
    lv_obj_set_size(right, 200, 222);
    lv_obj_set_pos(right, 120, 18);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 3, 0);
    lv_obj_set_style_radius(right, 0, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    // ── Station header (compact) ──
    lv_obj_t* stn = lv_obj_create(right);
    lv_obj_set_size(stn, 192, 28);
    lv_obj_set_pos(stn, 0, 0);
    lv_obj_set_style_bg_color(stn, lv_color_hex(0x003410), 0);
    lv_obj_set_style_bg_opa(stn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(stn, CLR_DARK, 0);
    lv_obj_set_style_border_width(stn, 1, 0);
    lv_obj_set_style_radius(stn, 0, 0);
    lv_obj_set_style_pad_all(stn, 2, 0);
    lv_obj_clear_flag(stn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* freq = lv_label_create(stn);
    lv_label_set_text(freq, "89.0 FM");
    lv_obj_set_style_text_font(freq, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(freq, CLR_GREEN, 0);
    lv_obj_set_pos(freq, 2, 2);

    // Wave in station header
    lv_obj_t* wave_cont = lv_obj_create(stn);
    lv_obj_set_size(wave_cont, 100, 18);
    lv_obj_set_pos(wave_cont, 58, 2);
    lv_obj_set_style_bg_opa(wave_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wave_cont, 0, 0);
    lv_obj_set_style_pad_all(wave_cont, 0, 0);
    lv_obj_clear_flag(wave_cont, LV_OBJ_FLAG_SCROLLABLE);

    wave_phase = 0.0f;
    for (int i = 0; i < 17; i++) {
        wave_pts[i].x = i * 6;
        wave_pts[i].y = 9 + (int)(6.0f * sinf(i * 0.8f));
    }
    wave_line = lv_line_create(wave_cont);
    lv_line_set_points(wave_line, wave_pts, 17);
    lv_obj_set_style_line_color(wave_line, CLR_GREEN, 0);
    lv_obj_set_style_line_width(wave_line, 1, 0);

    lv_obj_t* stn_name = lv_label_create(stn);
    lv_label_set_text(stn_name, "RT89");
    lv_obj_set_style_text_font(stn_name, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(stn_name, CLR_DIM, 0);
    lv_obj_set_pos(stn_name, 162, 2);

    // ── Chat mode bar ──
    lv_obj_t* mode_bar = lv_obj_create(right);
    lv_obj_set_size(mode_bar, 192, 16);
    lv_obj_set_pos(mode_bar, 0, 30);
    lv_obj_set_style_bg_opa(mode_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mode_bar, 0, 0);
    lv_obj_set_style_pad_all(mode_bar, 0, 0);
    lv_obj_clear_flag(mode_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Mode toggle button
    lv_obj_t* mode_btn = lv_btn_create(mode_bar);
    lv_obj_set_size(mode_btn, 36, 14);
    lv_obj_set_pos(mode_btn, 0, 0);
    lv_obj_set_style_bg_color(mode_btn, CLR_SEL, 0);
    lv_obj_set_style_bg_opa(mode_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(mode_btn, CLR_GREEN, 0);
    lv_obj_set_style_border_width(mode_btn, 1, 0);
    lv_obj_set_style_radius(mode_btn, 2, 0);
    lv_obj_set_style_pad_all(mode_btn, 0, 0);
    lv_obj_add_event_cb(mode_btn, mode_toggle_cb, LV_EVENT_CLICKED, NULL);

    mode_btn_lbl = lv_label_create(mode_btn);
    lv_label_set_text(mode_btn_lbl, "ALL");
    lv_obj_set_style_text_font(mode_btn_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(mode_btn_lbl, CLR_GREEN, 0);
    lv_obj_align(mode_btn_lbl, LV_ALIGN_CENTER, 0, 0);

    dm_target_lbl = mk_label(mode_bar, "Group chat", &lv_font_unscii_8, CLR_DIM, 40, 2, 100);

    // SYNC button
    lv_obj_t* sync_btn = lv_btn_create(mode_bar);
    lv_obj_set_size(sync_btn, 40, 14);
    lv_obj_set_pos(sync_btn, 150, 0);
    lv_obj_set_style_bg_color(sync_btn, CLR_SEL, 0);
    lv_obj_set_style_bg_opa(sync_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(sync_btn, CLR_DIM, 0);
    lv_obj_set_style_border_width(sync_btn, 1, 0);
    lv_obj_set_style_radius(sync_btn, 2, 0);
    lv_obj_set_style_pad_all(sync_btn, 0, 0);
    lv_obj_add_event_cb(sync_btn, [](lv_event_t* e) {
        espnow_request_sync();
        sfx_play(SFX_CONNECT);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* sync_lbl = lv_label_create(sync_btn);
    lv_label_set_text(sync_lbl, "SYNC");
    lv_obj_set_style_text_font(sync_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(sync_lbl, CLR_DIM, 0);
    lv_obj_align(sync_lbl, LV_ALIGN_CENTER, 0, 0);

    // ── Chat messages area ──
    chat_list = lv_obj_create(right);
    lv_obj_set_size(chat_list, 192, 140);
    lv_obj_set_pos(chat_list, 0, 48);
    lv_obj_set_style_bg_color(chat_list, lv_color_hex(0x001a08), 0);
    lv_obj_set_style_bg_opa(chat_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(chat_list, CLR_DARK, 0);
    lv_obj_set_style_border_width(chat_list, 1, 0);
    lv_obj_set_style_radius(chat_list, 0, 0);
    lv_obj_set_style_pad_all(chat_list, 3, 0);
    lv_obj_set_style_pad_row(chat_list, 2, 0);
    lv_obj_set_flex_flow(chat_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(chat_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(chat_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(chat_list, CLR_DARK, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(chat_list, LV_OPA_COVER, LV_PART_SCROLLBAR);

    rebuild_chat();

    // ── Chat input row ──
    lv_obj_t* input_row = lv_obj_create(right);
    lv_obj_set_size(input_row, 192, 28);
    lv_obj_set_pos(input_row, 0, 192);
    lv_obj_set_style_bg_opa(input_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(input_row, 0, 0);
    lv_obj_set_style_pad_all(input_row, 0, 0);
    lv_obj_clear_flag(input_row, LV_OBJ_FLAG_SCROLLABLE);

    // Type button (opens keyboard)
    lv_obj_t* type_btn = lv_btn_create(input_row);
    lv_obj_set_size(type_btn, 192, 24);
    lv_obj_set_pos(type_btn, 0, 2);
    lv_obj_set_style_bg_color(type_btn, CLR_SEL, 0);
    lv_obj_set_style_bg_opa(type_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(type_btn, CLR_GREEN, 0);
    lv_obj_set_style_border_width(type_btn, 1, 0);
    lv_obj_set_style_radius(type_btn, 2, 0);
    lv_obj_add_event_cb(type_btn, open_keyboard, LV_EVENT_CLICKED, NULL);

    lv_obj_t* type_lbl = lv_label_create(type_btn);
    lv_label_set_text(type_lbl, "TAP TO TYPE...");
    lv_obj_set_style_text_font(type_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(type_lbl, CLR_DIM, 0);
    lv_obj_align(type_lbl, LV_ALIGN_CENTER, 0, 0);

    // ═══ KEYBOARD OVERLAY (full-screen, hidden) ═══
    kb_overlay = lv_obj_create(scr_radio);
    lv_obj_set_size(kb_overlay, 320, 222);
    lv_obj_set_pos(kb_overlay, 0, 18);
    lv_obj_set_style_bg_color(kb_overlay, CLR_BG, 0);
    lv_obj_set_style_bg_opa(kb_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kb_overlay, 0, 0);
    lv_obj_set_style_pad_all(kb_overlay, 4, 0);
    lv_obj_clear_flag(kb_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(kb_overlay, LV_OBJ_FLAG_HIDDEN);

    // Header row in overlay
    mk_label(kb_overlay, "SEND MESSAGE", &lv_font_unscii_8, CLR_GREEN, 4, 2, 140);

    // Close button
    lv_obj_t* kb_close = lv_btn_create(kb_overlay);
    lv_obj_set_size(kb_close, 22, 16);
    lv_obj_set_pos(kb_close, 286, 0);
    lv_obj_set_style_bg_color(kb_close, CLR_RED, 0);
    lv_obj_set_style_bg_opa(kb_close, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kb_close, 0, 0);
    lv_obj_set_style_radius(kb_close, 2, 0);
    lv_obj_set_style_pad_all(kb_close, 0, 0);
    lv_obj_add_event_cb(kb_close, kb_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* kb_x = lv_label_create(kb_close);
    lv_label_set_text(kb_x, "X");
    lv_obj_set_style_text_font(kb_x, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(kb_x, lv_color_white(), 0);
    lv_obj_align(kb_x, LV_ALIGN_CENTER, 0, 0);

    // Mode indicator in overlay
    lv_obj_t* kb_mode_lbl = mk_label(kb_overlay, chat_dm_mode ? "DM" : "GROUP",
                                      &lv_font_unscii_8, CLR_DIM, 200, 2, 80);
    (void)kb_mode_lbl;

    // Textarea
    kb_textarea = lv_textarea_create(kb_overlay);
    lv_obj_set_size(kb_textarea, 306, 60);
    lv_obj_set_pos(kb_textarea, 2, 20);
    lv_textarea_set_max_length(kb_textarea, 31);  // match payload size
    lv_textarea_set_placeholder_text(kb_textarea, "Type message...");
    lv_obj_set_style_bg_color(kb_textarea, lv_color_hex(0x001a08), 0);
    lv_obj_set_style_bg_opa(kb_textarea, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(kb_textarea, CLR_GREEN, 0);
    lv_obj_set_style_border_width(kb_textarea, 1, 0);
    lv_obj_set_style_text_font(kb_textarea, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(kb_textarea, CLR_GREEN, 0);
    lv_obj_set_style_radius(kb_textarea, 2, 0);
    lv_obj_set_style_pad_all(kb_textarea, 4, 0);
    // Cursor style
    lv_obj_set_style_border_color(kb_textarea, CLR_GREEN, LV_PART_CURSOR);
    lv_obj_set_style_border_width(kb_textarea, 1, LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(kb_textarea, LV_OPA_TRANSP, LV_PART_CURSOR);

    // Button matrix keyboard
    kb_btnm = lv_btnmatrix_create(kb_overlay);
    lv_obj_set_size(kb_btnm, 312, 130);
    lv_obj_set_pos(kb_btnm, 0, 84);
    lv_btnmatrix_set_map(kb_btnm, kb_alpha_lower);
    lv_obj_set_style_bg_color(kb_btnm, lv_color_hex(0x0a3a12), 0);
    lv_obj_set_style_bg_opa(kb_btnm, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kb_btnm, 0, 0);
    lv_obj_set_style_pad_all(kb_btnm, 2, 0);
    lv_obj_set_style_pad_row(kb_btnm, 2, 0);
    lv_obj_set_style_pad_column(kb_btnm, 2, 0);
    lv_obj_set_style_radius(kb_btnm, 0, 0);
    // Button styles
    lv_obj_set_style_bg_color(kb_btnm, lv_color_hex(0x0a200a), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb_btnm, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb_btnm, &lv_font_unscii_8, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb_btnm, CLR_GREEN, LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb_btnm, lv_color_hex(0x1a4a1a), LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb_btnm, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb_btnm, 2, LV_PART_ITEMS);
    lv_obj_add_event_cb(kb_btnm, kb_key_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Timers
    radio_refresh_timer = lv_timer_create(refresh_timer_cb, 2000, NULL);
    wave_anim_timer = lv_timer_create(wave_anim_cb, 50, NULL);

    return scr_radio;
}
