#include <Arduino.h>
#include <lvgl.h>
#include "pipboy_data.h"
#include "net_state.h"

extern void on_login_complete(int idx);

static int login_sel = -1;
static lv_obj_t* login_list = NULL;
static lv_obj_t* footer_msg = NULL;
static lv_obj_t* wifi_lbl   = NULL;
static lv_timer_t* wifi_timer = NULL;
static lv_obj_t* tag_labels[TEAM_COUNT] = {};   // AVAIL/IN USE tags per row
static lv_obj_t* row_objs[TEAM_COUNT]   = {};   // row objects for styling
static bool      taken[TEAM_COUNT]      = {};   // whether each dweller is in use
static uint32_t  login_open_ms = 0;

// ── Name-entry overlay (custom display name) ──
static lv_obj_t* s_name_overlay = NULL;
static lv_obj_t* s_name_ta      = NULL;
static void finish_login(void);   // fwd: teardown + on_login_complete

// Check if a dweller name is currently in use on the network
static bool is_dweller_taken(int idx) {
    const char* name = team[idx].name;
    for (int i = 0; i < g_peer_count; i++) {
        if (g_peers[i].online && strcmp(g_peers[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

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

static void login_row_click_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    // Block selection if this dweller is already logged in elsewhere
    if (taken[idx]) {
        if (footer_msg) {
            char buf[40];
            snprintf(buf, sizeof(buf), "%s is IN USE", team[idx].name);
            lv_label_set_text(footer_msg, buf);
        }
        return;
    }

    login_sel = idx;

    uint32_t cnt = lv_obj_get_child_cnt(login_list);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t* row = lv_obj_get_child(login_list, i);
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

    if (footer_msg) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s selected", team[idx].name);
        lv_label_set_text(footer_msg, buf);
    }
}

// Refresh the AVAIL/IN USE tags and auto-select first available dweller
static void refresh_availability(bool allow_auto_select = true) {
    for (int i = 0; i < TEAM_COUNT; i++) {
        bool was_taken = taken[i];
        taken[i] = is_dweller_taken(i);
        if (tag_labels[i]) {
            if (taken[i]) {
                lv_label_set_text(tag_labels[i], "IN USE");
                lv_obj_set_style_text_color(tag_labels[i], lv_color_hex(0xCC6600), 0);
            } else if (was_taken) {
                lv_label_set_text(tag_labels[i], "AVAIL");
                lv_obj_set_style_text_color(tag_labels[i], CLR_DIM, 0);
            }
        }
    }

    // Auto-select first available if nothing selected
    if (allow_auto_select && login_sel < 0) {
        for (int i = 0; i < TEAM_COUNT; i++) {
            if (!taken[i]) {
                // Simulate a click on the first available row
                if (row_objs[i]) {
                    login_sel = i;
                    for (int j = 0; j < TEAM_COUNT; j++) {
                        if (!row_objs[j]) continue;
                        if (j == i) {
                            lv_obj_set_style_bg_color(row_objs[j], CLR_SEL, 0);
                            lv_obj_set_style_bg_opa(row_objs[j], LV_OPA_COVER, 0);
                            lv_obj_set_style_border_side(row_objs[j], LV_BORDER_SIDE_LEFT, 0);
                            lv_obj_set_style_border_color(row_objs[j], CLR_GREEN, 0);
                            lv_obj_set_style_border_width(row_objs[j], 2, 0);
                        }
                    }
                    if (footer_msg) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%s selected", team[i].name);
                        lv_label_set_text(footer_msg, buf);
                    }
                }
                break;
            }
        }
    }
}

// Teardown the login screen's timers/pointers, then hand off to the
// app. Called after the name-entry step is confirmed/skipped.
static void finish_login(void) {
    Serial.printf("[login] finish_login idx=%d name='%s' heap=%u\n",
                  login_sel, user_display_name(),
                  (unsigned)ESP.getFreeHeap());
    if (wifi_timer) { lv_timer_del(wifi_timer); wifi_timer = NULL; }
    // Free the full-screen keyboard overlay NOW — before on_login_complete
    // builds the (memory-heavy) tab bar + STAT screen. Holding the overlay
    // while the new screen allocates pushed the ~90KB heap over the edge and
    // tripped an LVGL OOM assert (whose default handler spins → "freeze").
    if (s_name_overlay) { lv_obj_del(s_name_overlay); s_name_overlay = NULL; }
    s_name_ta  = NULL;
    wifi_lbl   = NULL;
    footer_msg = NULL;
    login_list = NULL;
    for (int i = 0; i < TEAM_COUNT; i++) { tag_labels[i] = NULL; row_objs[i] = NULL; }
    Serial.printf("[login] overlay freed, heap=%u\n", (unsigned)ESP.getFreeHeap());
    on_login_complete(login_sel);
}

// Deferred entry point so we never delete the login screen (and the
// button matrix we're being called from) while still inside its event
// callback — that's a use-after-free in LVGL. lv_async_call runs this
// on the next lv_timer_handler tick, safely outside the event context.
static void finish_login_async(void* p) {
    (void)p;
    finish_login();
}

// Persist whatever is in the name textarea (trimmed). If it's empty or
// unchanged from the archetype name, clear the override so the archetype
// name is used. Then complete login.
static void name_confirm(void) {
    char buf[CUSTOM_NAME_LEN] = {0};
    if (s_name_ta) {
        const char* t = lv_textarea_get_text(s_name_ta);
        if (t) { strncpy(buf, t, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0'; }
    }
    // Trim trailing spaces.
    for (int i = (int)strlen(buf) - 1; i >= 0 && buf[i] == ' '; i--) buf[i] = '\0';
    if (buf[0] == '\0' ||
        (login_sel >= 0 && strcmp(buf, team[login_sel].name) == 0)) {
        user_name_save("");        // use archetype name
    } else {
        user_name_save(buf);       // custom override
    }
    // Defer the screen teardown — see finish_login_async().
    lv_async_call(finish_login_async, NULL);
}

// Compact on-screen keyboard (LV_USE_KEYBOARD is off, so we use a
// button matrix like the radio chat does).
static const char* name_kb_map[] = {
    "1","2","3","4","5","6","7","8","9","0","\n",
    "Q","W","E","R","T","Y","U","I","O","P","\n",
    "A","S","D","F","G","H","J","K","L","\n",
    "Z","X","C","V","B","N","M","\n",
    "DEL","SPACE","OK",""
};

static void name_kb_cb(lv_event_t* e) {
    lv_obj_t* btnm = lv_event_get_target(e);
    uint32_t id = lv_btnmatrix_get_selected_btn(btnm);
    const char* txt = lv_btnmatrix_get_btn_text(btnm, id);
    if (!txt) return;
    Serial.printf("[login] kb key='%s'\n", txt);
    if      (strcmp(txt, "DEL")   == 0) lv_textarea_del_char(s_name_ta);
    else if (strcmp(txt, "SPACE") == 0) lv_textarea_add_char(s_name_ta, ' ');
    else if (strcmp(txt, "OK")    == 0) name_confirm();
    else if (txt[0])                    lv_textarea_add_char(s_name_ta, txt[0]);
}

// Open the name-entry overlay, prefilled with the selected archetype's
// name. The user can keep it or type their own.
static void open_name_entry(void) {
    Serial.println("[login] open_name_entry");
    s_name_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_name_overlay, 320, 240);
    lv_obj_set_pos(s_name_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_name_overlay, CLR_BG, 0);
    lv_obj_set_style_bg_opa(s_name_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_name_overlay, 0, 0);
    lv_obj_set_style_radius(s_name_overlay, 0, 0);
    lv_obj_set_style_border_width(s_name_overlay, 0, 0);
    lv_obj_clear_flag(s_name_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* prompt = lv_label_create(s_name_overlay);
    lv_label_set_text(prompt, "ENTER YOUR NAME  (or keep default)");
    lv_obj_set_style_text_font(prompt, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(prompt, CLR_GOLD, 0);
    lv_obj_set_pos(prompt, 8, 6);

    s_name_ta = lv_textarea_create(s_name_overlay);
    lv_textarea_set_one_line(s_name_ta, true);
    lv_textarea_set_max_length(s_name_ta, CUSTOM_NAME_LEN - 1);
    lv_obj_set_size(s_name_ta, 304, 28);
    lv_obj_set_pos(s_name_ta, 8, 20);
    lv_obj_set_style_text_font(s_name_ta, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_name_ta, CLR_GREEN, 0);
    lv_obj_set_style_bg_color(s_name_ta, CLR_SEL, 0);
    lv_obj_set_style_bg_opa(s_name_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_name_ta, CLR_GREEN, 0);
    lv_obj_set_style_border_width(s_name_ta, 1, 0);
    lv_obj_set_style_radius(s_name_ta, 2, 0);
    lv_obj_set_style_pad_all(s_name_ta, 4, 0);
    if (login_sel >= 0) lv_textarea_set_text(s_name_ta, team[login_sel].name);

    // Button-matrix keyboard — mirrors the working radio-chat setup
    // (styles applied to LV_PART_ITEMS, pad_row/pad_column for gaps).
    lv_obj_t* kb = lv_btnmatrix_create(s_name_overlay);
    lv_obj_set_size(kb, 320, 182);
    lv_obj_set_pos(kb, 0, 54);
    lv_btnmatrix_set_map(kb, name_kb_map);
    lv_obj_set_style_bg_color(kb, CLR_BG, 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 2, 0);
    lv_obj_set_style_pad_row(kb, 3, 0);
    lv_obj_set_style_pad_column(kb, 3, 0);
    lv_obj_set_style_bg_color(kb, CLR_HEADER, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &lv_font_unscii_8, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, CLR_GREEN, LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb, CLR_DARK, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 2, LV_PART_ITEMS);
    lv_obj_add_event_cb(kb, name_kb_cb, LV_EVENT_VALUE_CHANGED, NULL);
    Serial.println("[login] open_name_entry done");
}

static void enter_vault_cb(lv_event_t* e) {
    (void)e;
    if (login_sel < 0) return;
    // Final check: someone else may have grabbed this name between selection and confirmation
    if (taken[login_sel]) {
        if (footer_msg) {
            char buf[40];
            snprintf(buf, sizeof(buf), "%s is IN USE", team[login_sel].name);
            lv_label_set_text(footer_msg, buf);
        }
        login_sel = -1;
        return;
    }
    // Pause the availability timer while the name overlay is up.
    if (wifi_timer) { lv_timer_del(wifi_timer); wifi_timer = NULL; }
    open_name_entry();
}

lv_obj_t* screen_login_create(void) {
    login_sel = -1;
    for (int i = 0; i < TEAM_COUNT; i++) { tag_labels[i] = NULL; row_objs[i] = NULL; taken[i] = false; }
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, CLR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Header bar (18px)
    lv_obj_t* hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, 320, 18);
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
    lv_label_set_text(htitle, TITLE_LOGIN);
    lv_obj_set_style_text_font(htitle, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(htitle, CLR_GREEN, 0);
    lv_obj_align(htitle, LV_ALIGN_CENTER, 0, 0);

    // Subtitle
    mk_label(scr, "SELECT YOUR DWELLER TO CONTINUE", &lv_font_unscii_8, CLR_DIM, 36, 22, 260);

    // Scrollable list
    login_list = lv_obj_create(scr);
    lv_obj_set_size(login_list, 300, 170);
    lv_obj_set_pos(login_list, 10, 34);
    lv_obj_set_style_bg_opa(login_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(login_list, 0, 0);
    lv_obj_set_style_radius(login_list, 0, 0);
    lv_obj_set_style_pad_all(login_list, 0, 0);
    lv_obj_set_style_pad_row(login_list, 0, 0);
    lv_obj_set_flex_flow(login_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(login_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(login_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(login_list, CLR_DARK, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(login_list, LV_OPA_COVER, LV_PART_SCROLLBAR);

    for (int i = 0; i < TEAM_COUNT; i++) {
        const Dweller& d = team[i];

        lv_obj_t* row = lv_obj_create(login_list);
        lv_obj_set_size(row, 298, 18);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x005c22), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_left(row, 4, 0);
        lv_obj_set_style_pad_right(row, 4, 0);
        lv_obj_set_style_pad_top(row, 2, 0);
        lv_obj_set_style_pad_bottom(row, 2, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, login_row_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        row_objs[i] = row;

        // Initials circle
        lv_obj_t* circ = lv_obj_create(row);
        lv_obj_set_size(circ, 14, 14);
        lv_obj_set_pos(circ, 0, 0);
        lv_obj_set_style_bg_color(circ, CLR_DARK, 0);
        lv_obj_set_style_bg_opa(circ, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(circ, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(circ, CLR_DIM, 0);
        lv_obj_set_style_border_width(circ, 1, 0);
        lv_obj_set_style_pad_all(circ, 0, 0);
        lv_obj_clear_flag(circ, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* init_lbl = lv_label_create(circ);
        lv_label_set_text(init_lbl, d.init);
        lv_obj_set_style_text_font(init_lbl, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(init_lbl, CLR_GREEN, 0);
        lv_obj_align(init_lbl, LV_ALIGN_CENTER, 0, 0);

        // Name
        mk_label(row, d.name, &lv_font_unscii_8, CLR_GREEN, 20, 2, 90);

        // Title
        mk_label(row, d.title, &lv_font_unscii_8, CLR_DIM, 115, 2, 110);

        // AVAIL tag
        lv_obj_t* tag = lv_obj_create(row);
        lv_obj_set_size(tag, 36, 10);
        lv_obj_set_pos(tag, 248, 2);
        lv_obj_set_style_bg_color(tag, CLR_DARK, 0);
        lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tag, 0, 0);
        lv_obj_set_style_radius(tag, 0, 0);
        lv_obj_set_style_pad_all(tag, 0, 0);
        lv_obj_clear_flag(tag, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* tag_lbl = lv_label_create(tag);
        lv_label_set_text(tag_lbl, "AVAIL");
        lv_obj_set_style_text_font(tag_lbl, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(tag_lbl, CLR_DIM, 0);
        lv_obj_align(tag_lbl, LV_ALIGN_CENTER, 0, 0);
        tag_labels[i] = tag_lbl;
    }

    // Initial refresh: tag any already-known peers but don't auto-select yet —
    // we give heartbeats a few seconds to populate before picking a dweller.
    login_open_ms = millis();
    refresh_availability(false);

    // Footer
    lv_obj_t* foot = lv_obj_create(scr);
    lv_obj_set_size(foot, 320, 30);
    lv_obj_set_pos(foot, 0, 210);
    lv_obj_set_style_bg_opa(foot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(foot, CLR_DARK, 0);
    lv_obj_set_style_border_width(foot, 1, 0);
    lv_obj_set_style_radius(foot, 0, 0);
    lv_obj_set_style_pad_all(foot, 0, 0);
    lv_obj_clear_flag(foot, LV_OBJ_FLAG_SCROLLABLE);

    footer_msg = mk_label(foot, "Select a dweller", &lv_font_unscii_8, CLR_DIM, 10, 2, 160);
    wifi_lbl = mk_label(foot, "LINK: --", &lv_font_unscii_8, CLR_DIM, 10, 14, 170);

    // Link status + availability refresh timer
    wifi_timer = lv_timer_create([](lv_timer_t* t) {
        if (wifi_lbl) {
            char buf[40];
            if (g_net.wifi_connected) {
                snprintf(buf, sizeof(buf), "LINK: WIFI  (ch %u)", g_net.wifi_channel);
                lv_obj_set_style_text_color(wifi_lbl, CLR_GREEN, 0);
            } else if (g_net.link_mode == LINK_MESH) {
                snprintf(buf, sizeof(buf), "LINK: MESH  (%d peer%s)",
                         g_peer_count, g_peer_count == 1 ? "" : "s");
                lv_obj_set_style_text_color(wifi_lbl, lv_color_hex(0xCCCC00), 0);
            } else {
                snprintf(buf, sizeof(buf), "LINK: OFFLINE - SCANNING...");
                lv_obj_set_style_text_color(wifi_lbl, CLR_DIM, 0);
            }
            lv_label_set_text(wifi_lbl, buf);
        }
        // Only start auto-selecting after ~4s to let heartbeats populate peer list
        bool allow_auto = (millis() - login_open_ms) >= 4000;
        refresh_availability(allow_auto);
    }, 1500, NULL);

    // Enter Vault button
    lv_obj_t* btn = lv_obj_create(foot);
    lv_obj_set_size(btn, 120, 18);
    lv_obj_set_pos(btn, 190, 6);
    lv_obj_set_style_bg_color(btn, CLR_DARK, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, CLR_DIM, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, enter_vault_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "[ ENTER VAULT ]");
    lv_obj_set_style_text_font(btn_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(btn_lbl, CLR_GREEN, 0);
    lv_obj_align(btn_lbl, LV_ALIGN_CENTER, 0, 0);

    return scr;
}
