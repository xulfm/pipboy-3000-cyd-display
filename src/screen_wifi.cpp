// ============================================================
// Vault 89 PDI-Boy -- WiFi Scan / Picker Screen
// Scan for networks, select, connect, then proceed to login.
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>
#include "pipboy_data.h"
#include "net_state.h"

extern void on_wifi_done(void);  // called on connect OR skip
extern void wifi_creds_save(const char* ssid, const char* pass);

// Local gold accent for the mesh peers section. Defined here instead of
// pulling another constant into pipboy_data.h — the WiFi picker is the
// only place that distinguishes mesh rows from AP rows by color.
#define CLR_MESH_GOLD  lv_color_hex(0xE0B040)
#define CLR_MESH_SEP   lv_color_hex(0x5a4a00)

// ── state ──────────────────────────────────────────────────
static lv_obj_t* scr_wifi      = NULL;
static lv_obj_t* net_list       = NULL;
static lv_obj_t* status_lbl     = NULL;
static lv_obj_t* connect_btn    = NULL;
static lv_obj_t* scan_btn       = NULL;
static lv_obj_t* pw_cont        = NULL;   // password row container
static lv_obj_t* pw_ta          = NULL;   // password text area
static lv_obj_t* pw_kb          = NULL;   // on-screen keyboard
static lv_obj_t* footer         = NULL;   // footer container
static lv_timer_t* scan_timer   = NULL;
static lv_timer_t* conn_timer   = NULL;
static lv_timer_t* delay_timer  = NULL;
static lv_timer_t* mesh_timer   = NULL;   // refreshes mesh peers section

static int      wifi_sel       = -1;
static bool     scanning       = false;
static bool     connecting     = false;
static bool     scan_done      = false;
static uint32_t conn_start_ms  = 0;
static bool     s_leaving      = false;

// Mesh peers section — rebuilt when the count changes so a newly
// discovered PDI-Boy pops into view without the user tapping rescan.
static int      s_last_mesh_shown = -1;  // cached peer_online count
static int      s_wifi_row_offset = 0;   // first net_list child idx that is a WiFi row
static lv_obj_t* skip_btn_lbl   = NULL;  // footer [SKIP]/[USE MESH] label ref

// Store scan results (LVGL list rebuilt from these)
#define MAX_SCAN 20
struct ScanEntry {
    char    ssid[33];
    int8_t  rssi;
    bool    encrypted;
};
static ScanEntry scan_results[MAX_SCAN];
static int       scan_count = 0;

// ── helpers ────────────────────────────────────────────────
static const char* sig_icon(int8_t rssi) {
    if (rssi >= -50) return "||||";
    if (rssi >= -60) return "|||.";
    if (rssi >= -70) return "||..";
    if (rssi >= -80) return "|...";
    return "....";
}

static lv_obj_t* mk_lbl(lv_obj_t* par, const char* txt,
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

// ── populate list from scan_results[] ──────────────────────
static void populate_list(void);
static void show_password_row(bool show);
static void cleanup_and_leave(void);

// Count online ESP-NOW peers we've heard on the current channel. The
// WiFi picker uses this to decide whether to show the mesh section and
// whether the footer should read [ SKIP ] or [ USE MESH ].
static int count_online_peers(void) {
    int n = 0;
    for (int i = 0; i < g_peer_count; i++) if (g_peers[i].online) n++;
    return n;
}

static void update_skip_button_label(void) {
    if (!skip_btn_lbl) return;
    if (count_online_peers() > 0) {
        lv_label_set_text(skip_btn_lbl, "[ MESH ]");
    } else {
        lv_label_set_text(skip_btn_lbl, "[ SKIP ]");
    }
}

// Tapping a peer row means "don't bother with WiFi — the mesh will feed
// this device time, weather, and chat." We just proceed the same way the
// SKIP button does; ESP-NOW is already running in the background so no
// handshake is required.
static void mesh_row_click_cb(lv_event_t* e) {
    (void)e;
    Serial.println("[wifi-ui] Mesh peer row tapped -> using mesh");
    if (status_lbl) lv_label_set_text(status_lbl, "USING MESH...");
    cleanup_and_leave();
}

static void row_click_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    wifi_sel = idx;

    // Only restyle WiFi rows — mesh section rows live at child indices
    // 0..s_wifi_row_offset-1 and must keep their gold accent.
    for (int i = 0; i < scan_count; i++) {
        lv_obj_t* row = lv_obj_get_child(net_list, s_wifi_row_offset + i);
        if (!row) continue;
        if (i == idx) {
            lv_obj_set_style_bg_color(row, CLR_SEL, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_color(row, CLR_GREEN, 0);
            lv_obj_set_style_border_width(row, 2, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(0x005c22), 0);
            lv_obj_set_style_border_width(row, 1, 0);
        }
    }

    // Show password row if encrypted
    if (idx >= 0 && idx < scan_count) {
        show_password_row(scan_results[idx].encrypted);
        if (status_lbl) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%s (%ddBm)",
                     scan_results[idx].ssid, scan_results[idx].rssi);
            lv_label_set_text(status_lbl, buf);
        }
    }
}

static void populate_list(void) {
    if (!net_list) return;
    lv_obj_clean(net_list);
    wifi_sel = -1;
    s_wifi_row_offset = 0;

    // ── Mesh peer section ─────────────────────────────────
    // Any device broadcasting ESP-NOW on our channel appears here. The
    // gold accent distinguishes mesh rows from WiFi AP rows, and a tap
    // proceeds directly to login (mesh already provides time/weather/
    // chat so no AP selection is needed).
    int mesh_n = count_online_peers();
    s_last_mesh_shown = mesh_n;

    if (mesh_n > 0) {
        lv_obj_t* mh = lv_obj_create(net_list);
        lv_obj_set_size(mh, 296, 14);
        lv_obj_set_style_bg_opa(mh, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(mh, 0, 0);
        lv_obj_set_style_radius(mh, 0, 0);
        lv_obj_set_style_pad_all(mh, 0, 0);
        lv_obj_clear_flag(mh, LV_OBJ_FLAG_SCROLLABLE);
        mk_lbl(mh, "-- " MESH_BRAND " (TAP TO USE) --",
               &lv_font_unscii_8, CLR_MESH_GOLD, 4, 3, 290);
        s_wifi_row_offset++;

        for (int i = 0; i < g_peer_count; i++) {
            if (!g_peers[i].online) continue;
            lv_obj_t* row = lv_obj_create(net_list);
            lv_obj_set_size(row, 296, 18);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_color(row, CLR_MESH_SEP, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_radius(row, 0, 0);
            lv_obj_set_style_pad_left(row, 4, 0);
            lv_obj_set_style_pad_right(row, 4, 0);
            lv_obj_set_style_pad_top(row, 2, 0);
            lv_obj_set_style_pad_bottom(row, 2, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, mesh_row_click_cb, LV_EVENT_CLICKED, NULL);

            mk_lbl(row, sig_icon(g_peers[i].rssi),
                   &lv_font_unscii_8, CLR_MESH_GOLD, 0, 2, 36);
            // Mesh marker — uppercase 'M' in gold, mirrors the '*' used
            // to indicate encryption on WiFi rows.
            mk_lbl(row, "M", &lv_font_unscii_8, CLR_MESH_GOLD, 38, 2, 10);
            const char* nm = g_peers[i].name[0] ? g_peers[i].name : "(peer)";
            mk_lbl(row, nm, &lv_font_unscii_8, CLR_MESH_GOLD, 50, 2, 196);
            char dbm[10];
            snprintf(dbm, sizeof(dbm), "%ddB", g_peers[i].rssi);
            mk_lbl(row, dbm, &lv_font_unscii_8, CLR_DIM, 248, 2, 48);

            s_wifi_row_offset++;
        }

        // WiFi section divider — only show it if we have (or are about
        // to have) a scan result list to label.
        lv_obj_t* wh = lv_obj_create(net_list);
        lv_obj_set_size(wh, 296, 14);
        lv_obj_set_style_bg_opa(wh, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(wh, 0, 0);
        lv_obj_set_style_radius(wh, 0, 0);
        lv_obj_set_style_pad_all(wh, 0, 0);
        lv_obj_clear_flag(wh, LV_OBJ_FLAG_SCROLLABLE);
        mk_lbl(wh, "-- WIFI NETWORKS --",
               &lv_font_unscii_8, CLR_DIM, 4, 3, 290);
        s_wifi_row_offset++;
    }

    // ── WiFi AP section ───────────────────────────────────
    if (scan_count == 0 && scan_done) {
        mk_lbl(net_list, "NO NETWORKS FOUND", &lv_font_unscii_8, CLR_DIM, 10, 4);
        return;
    }

    for (int i = 0; i < scan_count; i++) {
        ScanEntry& s = scan_results[i];

        lv_obj_t* row = lv_obj_create(net_list);
        lv_obj_set_size(row, 296, 18);
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
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)i);

        // Signal bars
        mk_lbl(row, sig_icon(s.rssi), &lv_font_unscii_8, CLR_DIM, 0, 2, 36);

        // Lock icon for encrypted
        if (s.encrypted) {
            mk_lbl(row, "*", &lv_font_unscii_8, CLR_GOLD, 38, 2, 10);
        }

        // SSID
        int ssid_x = s.encrypted ? 50 : 42;
        mk_lbl(row, s.ssid, &lv_font_unscii_8, CLR_GREEN, ssid_x, 2, 200);

        // dBm
        char dbm[10];
        snprintf(dbm, sizeof(dbm), "%ddB", s.rssi);
        mk_lbl(row, dbm, &lv_font_unscii_8, CLR_DIM, 248, 2, 48);
    }
}

// ── password row (hidden by default) ───────────────────────
static void show_password_row(bool show) {
    if (!pw_cont) return;
    if (show) {
        lv_obj_clear_flag(pw_cont, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(pw_cont, LV_OBJ_FLAG_HIDDEN);
        if (pw_ta) lv_textarea_set_text(pw_ta, "");
    }
}

static void show_keyboard(bool show);

// ── Custom btnmatrix keyboard ──────────────────────────────
// 4 rows: lowercase alpha, numbers, symbols, and control keys
static bool kb_shifted = false;
static uint8_t kb_page = 0;  // 0=alpha, 1=num/sym

static const char* kb_alpha_lower[] = {
    "q","w","e","r","t","y","u","i","o","p","\n",
    "a","s","d","f","g","h","j","k","l","\n",
    "^","z","x","c","v","b","n","m","<","\n",
    "#","_"," ",".",">",""
};
static const char* kb_alpha_upper[] = {
    "Q","W","E","R","T","Y","U","I","O","P","\n",
    "A","S","D","F","G","H","J","K","L","\n",
    "^","Z","X","C","V","B","N","M","<","\n",
    "#","_"," ",".",">",""
};
static const char* kb_numsym[] = {
    "1","2","3","4","5","6","7","8","9","0","\n",
    "!","@","#","$","%","&","*","(",")","\n",
    "-","+","=","/","\\",":",";","\"","<","\n",
    "a","_"," ",".",">",""
};

static void rebuild_kb_map(void) {
    if (!pw_kb) return;
    if (kb_page == 1) {
        lv_btnmatrix_set_map(pw_kb, kb_numsym);
    } else if (kb_shifted) {
        lv_btnmatrix_set_map(pw_kb, kb_alpha_upper);
    } else {
        lv_btnmatrix_set_map(pw_kb, kb_alpha_lower);
    }
}

static void kb_btn_cb(lv_event_t* e) {
    uint16_t id = lv_btnmatrix_get_selected_btn(pw_kb);
    const char* txt = lv_btnmatrix_get_btn_text(pw_kb, id);
    if (!txt || !pw_ta) return;

    if (strcmp(txt, "<") == 0) {
        // Backspace
        lv_textarea_del_char(pw_ta);
    } else if (strcmp(txt, "^") == 0) {
        // Shift toggle
        kb_shifted = !kb_shifted;
        rebuild_kb_map();
    } else if (strcmp(txt, "#") == 0) {
        // Switch to num/sym page
        kb_page = 1;
        rebuild_kb_map();
    } else if (strcmp(txt, "a") == 0 && kb_page == 1) {
        // Switch back to alpha page
        kb_page = 0;
        rebuild_kb_map();
    } else if (strcmp(txt, ">") == 0) {
        // Done — hide keyboard
        show_keyboard(false);
    } else if (strcmp(txt, "_") == 0) {
        // Space
        lv_textarea_add_char(pw_ta, ' ');
    } else {
        // Regular character
        lv_textarea_add_text(pw_ta, txt);
        if (kb_shifted) {
            kb_shifted = false;
            rebuild_kb_map();
        }
    }
}

static void show_keyboard(bool show) {
    if (!pw_kb) return;
    if (show) {
        // Shrink list, move password up, hide footer, show keyboard
        if (net_list) lv_obj_set_height(net_list, 60);
        if (pw_cont) lv_obj_set_pos(pw_cont, 10, 98);
        if (footer) lv_obj_add_flag(footer, LV_OBJ_FLAG_HIDDEN);
        kb_shifted = false;
        kb_page = 0;
        rebuild_kb_map();
        lv_obj_clear_flag(pw_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(pw_kb);
    } else {
        // Restore layout
        lv_obj_add_flag(pw_kb, LV_OBJ_FLAG_HIDDEN);
        if (net_list) lv_obj_set_height(net_list, 144);
        if (pw_cont) lv_obj_set_pos(pw_cont, 10, 180);
        if (footer) lv_obj_clear_flag(footer, LV_OBJ_FLAG_HIDDEN);
    }
}

static void pw_ta_focus_cb(lv_event_t* e) {
    show_keyboard(true);
}

// ── forward declarations ───────────────────────────────────
static void start_scan(void);
static void cleanup_and_leave(void);

static void scan_check_cb(lv_timer_t* t) {
    if (s_leaving) { lv_timer_del(t); scan_timer = NULL; return; }
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;

    // Scan finished
    scanning = false;
    scan_done = true;

    scan_count = 0;
    if (n > 0) {
        int total = (n > MAX_SCAN) ? MAX_SCAN : n;
        for (int i = 0; i < total; i++) {
            strncpy(scan_results[i].ssid, WiFi.SSID(i).c_str(), 32);
            scan_results[i].ssid[32] = '\0';
            scan_results[i].rssi = WiFi.RSSI(i);
            scan_results[i].encrypted =
                (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
            scan_count++;
        }
    }
    WiFi.scanDelete();

    if (scan_timer) { lv_timer_del(scan_timer); scan_timer = NULL; }

    populate_list();

    if (status_lbl) {
        char buf[32];
        snprintf(buf, sizeof(buf), "FOUND %d NETWORKS", scan_count);
        lv_label_set_text(status_lbl, buf);
    }

    // Re-enable buttons
    if (scan_btn) lv_obj_clear_state(scan_btn, LV_STATE_DISABLED);
    if (connect_btn) lv_obj_clear_state(connect_btn, LV_STATE_DISABLED);

    Serial.printf("[wifi-ui] Scan done, %d networks\n", scan_count);
}

static void start_scan(void) {
    if (scanning) return;
    scanning = true;
    scan_done = false;
    wifi_sel = -1;

    if (status_lbl)
        lv_label_set_text(status_lbl, "SCANNING...");

    // Disable buttons during scan
    if (scan_btn) lv_obj_add_state(scan_btn, LV_STATE_DISABLED);
    if (connect_btn) lv_obj_add_state(connect_btn, LV_STATE_DISABLED);

    WiFi.scanNetworks(true);  // async scan

    // Poll for results every 500ms
    if (scan_timer) { lv_timer_del(scan_timer); scan_timer = NULL; }
    scan_timer = lv_timer_create(scan_check_cb, 500, NULL);
}

// ── connect ────────────────────────────────────────────────
static void conn_check_cb(lv_timer_t* t) {
    if (s_leaving) { lv_timer_del(t); conn_timer = NULL; return; }
    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
        connecting = false;
        if (conn_timer) { lv_timer_del(conn_timer); conn_timer = NULL; }

        g_net.wifi_connected = true;
        g_net.wifi_channel = WiFi.channel();
        g_net.wifi_rssi = WiFi.RSSI();

        Serial.printf("[wifi-ui] Connected! IP=%s ch=%d\n",
                      WiFi.localIP().toString().c_str(),
                      g_net.wifi_channel);

        // Save creds to NVS for auto-reconnect on next boot
        wifi_creds_save(WiFi.SSID().c_str(), WiFi.psk().c_str());

        if (status_lbl) {
            char buf[48];
            snprintf(buf, sizeof(buf), "CONNECTED: %s",
                     WiFi.localIP().toString().c_str());
            lv_label_set_text(status_lbl, buf);
        }

        // Brief delay so user sees the connected message, then proceed
        lv_timer_create([](lv_timer_t* t2) {
            lv_timer_del(t2);
            cleanup_and_leave();
        }, 800, NULL);

        return;
    }

    // Check timeout (~15s)
    if (millis() - conn_start_ms > 15000) {
        connecting = false;
        if (conn_timer) { lv_timer_del(conn_timer); conn_timer = NULL; }
        if (status_lbl)
            lv_label_set_text(status_lbl, "CONNECTION FAILED");
        if (scan_btn) lv_obj_clear_state(scan_btn, LV_STATE_DISABLED);
        if (connect_btn) lv_obj_clear_state(connect_btn, LV_STATE_DISABLED);
        Serial.println("[wifi-ui] Connect failed / timeout");
    }
}

static void connect_click_cb(lv_event_t* e) {
    (void)e;
    if (wifi_sel < 0 || wifi_sel >= scan_count) return;
    if (connecting) return;

    const char* ssid = scan_results[wifi_sel].ssid;
    const char* pass = "";
    if (scan_results[wifi_sel].encrypted && pw_ta) {
        pass = lv_textarea_get_text(pw_ta);
    }

    Serial.printf("[wifi-ui] Connecting to '%s'...\n", ssid);
    if (status_lbl) {
        char buf[48];
        snprintf(buf, sizeof(buf), "CONNECTING: %s", ssid);
        lv_label_set_text(status_lbl, buf);
    }

    // Hide keyboard and restore layout
    show_keyboard(false);

    // STA only — AP_STA creates a phantom softap that steals the channel
    // from ESP-NOW when the STA side isn't connected yet.
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    connecting = true;
    conn_start_ms = millis();

    // Disable buttons
    if (scan_btn) lv_obj_add_state(scan_btn, LV_STATE_DISABLED);
    if (connect_btn) lv_obj_add_state(connect_btn, LV_STATE_DISABLED);

    // Reset conn_start for timeout tracking
    // (static in conn_check_cb — reset by setting to 0 isn't possible from here,
    //  so we just restart the timer which effectively resets)
    if (conn_timer) { lv_timer_del(conn_timer); conn_timer = NULL; }
    conn_timer = lv_timer_create(conn_check_cb, 500, NULL);
}

static void scan_click_cb(lv_event_t* e) {
    (void)e;
    show_password_row(false);
    start_scan();
}

// Repopulate the mesh section when the set of online peers changes.
// Throttled to ~2s and skipped entirely while the user is mid-scan,
// mid-connect, or has an AP selected so we don't stomp their selection.
static void mesh_refresh_cb(lv_timer_t* t) {
    if (s_leaving) { lv_timer_del(t); mesh_timer = NULL; return; }
    if (!net_list) return;
    if (scanning || connecting) return;
    if (wifi_sel >= 0) {
        // User has an AP selected — still relabel the footer button so
        // they can see that mesh peers are present, but don't rebuild.
        update_skip_button_label();
        return;
    }
    int n = count_online_peers();
    if (n == s_last_mesh_shown) return;
    Serial.printf("[wifi-ui] Mesh peers %d -> %d, refresh\n",
                  s_last_mesh_shown, n);
    populate_list();
    update_skip_button_label();
    if (status_lbl) {
        if (n > 0) {
            char buf[40];
            snprintf(buf, sizeof(buf), "MESH: %d PEER%s DETECTED",
                     n, n == 1 ? "" : "S");
            lv_label_set_text(status_lbl, buf);
        }
    }
}

// ── skip button — go to login without WiFi ─────────────────
static void cleanup_and_leave(void) {
    if (s_leaving) return;  // prevent double-call
    s_leaving = true;
    Serial.println("[wifi-ui] cleanup_and_leave");
    if (delay_timer) { lv_timer_del(delay_timer); delay_timer = NULL; }
    if (scan_timer) { lv_timer_del(scan_timer); scan_timer = NULL; }
    if (conn_timer) { lv_timer_del(conn_timer); conn_timer = NULL; }
    if (mesh_timer) { lv_timer_del(mesh_timer); mesh_timer = NULL; }
    scanning = false;
    connecting = false;
    // Cancel any in-progress scan
    WiFi.scanDelete();
    net_list = NULL;
    status_lbl = NULL;
    connect_btn = NULL;
    scan_btn = NULL;
    pw_cont = NULL;
    pw_ta = NULL;
    pw_kb = NULL;
    footer = NULL;
    scr_wifi = NULL;
    skip_btn_lbl = NULL;
    s_last_mesh_shown = -1;
    Serial.println("[wifi-ui] calling on_wifi_done");
    on_wifi_done();
    Serial.println("[wifi-ui] on_wifi_done returned");
}

static void skip_click_cb(lv_event_t* e) {
    (void)e;
    Serial.println("[wifi-ui] Skipped, proceeding without WiFi");
    cleanup_and_leave();
}

// ── build screen ───────────────────────────────────────────
lv_obj_t* screen_wifi_create(void) {
    s_leaving = false;
    scr_wifi = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_wifi, CLR_BG, 0);
    lv_obj_set_style_bg_opa(scr_wifi, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr_wifi, 0, 0);
    lv_obj_set_style_border_width(scr_wifi, 0, 0);
    lv_obj_clear_flag(scr_wifi, LV_OBJ_FLAG_SCROLLABLE);

    // ── Header bar (18px) ──
    lv_obj_t* hdr = lv_obj_create(scr_wifi);
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
    lv_label_set_text(htitle, TITLE_WIFI);
    lv_obj_set_style_text_font(htitle, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(htitle, CLR_GREEN, 0);
    lv_obj_align(htitle, LV_ALIGN_CENTER, 0, 0);

    // ── Subtitle ──
    mk_lbl(scr_wifi, "SCANNING WASTELAND FREQUENCIES",
            &lv_font_unscii_8, CLR_DIM, 28, 22, 280);

    // ── Network list (scrollable) ──
    net_list = lv_obj_create(scr_wifi);
    lv_obj_set_size(net_list, 300, 144);
    lv_obj_set_pos(net_list, 10, 34);
    lv_obj_set_style_bg_opa(net_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(net_list, 0, 0);
    lv_obj_set_style_radius(net_list, 0, 0);
    lv_obj_set_style_pad_all(net_list, 0, 0);
    lv_obj_set_style_pad_row(net_list, 0, 0);
    lv_obj_set_flex_flow(net_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(net_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(net_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(net_list, CLR_DARK, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(net_list, LV_OPA_COVER, LV_PART_SCROLLBAR);

    // ── Password row (hidden until encrypted network selected) ──
    pw_cont = lv_obj_create(scr_wifi);
    lv_obj_set_size(pw_cont, 300, 22);
    lv_obj_set_pos(pw_cont, 10, 180);
    lv_obj_set_style_bg_opa(pw_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pw_cont, 0, 0);
    lv_obj_set_style_radius(pw_cont, 0, 0);
    lv_obj_set_style_pad_all(pw_cont, 0, 0);
    lv_obj_clear_flag(pw_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pw_cont, LV_OBJ_FLAG_HIDDEN);

    mk_lbl(pw_cont, "PASS:", &lv_font_unscii_8, CLR_DIM, 0, 4, 40);

    pw_ta = lv_textarea_create(pw_cont);
    lv_obj_set_size(pw_ta, 248, 20);
    lv_obj_set_pos(pw_ta, 44, 0);
    lv_textarea_set_one_line(pw_ta, true);
    lv_textarea_set_password_mode(pw_ta, true);
    lv_textarea_set_placeholder_text(pw_ta, "PASSWORD");
    lv_textarea_set_max_length(pw_ta, 63);
    lv_obj_set_style_text_font(pw_ta, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(pw_ta, CLR_GREEN, 0);
    lv_obj_set_style_bg_color(pw_ta, CLR_HEADER, 0);
    lv_obj_set_style_bg_opa(pw_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(pw_ta, CLR_DARK, 0);
    lv_obj_set_style_border_width(pw_ta, 1, 0);
    lv_obj_set_style_radius(pw_ta, 0, 0);
    lv_obj_set_style_pad_left(pw_ta, 4, 0);
    lv_obj_set_style_pad_right(pw_ta, 4, 0);
    lv_obj_set_style_pad_top(pw_ta, 3, 0);
    lv_obj_set_style_pad_bottom(pw_ta, 3, 0);
    // Cursor style
    lv_obj_set_style_bg_color(pw_ta, CLR_GREEN, LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(pw_ta, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_add_event_cb(pw_ta, pw_ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(pw_ta, pw_ta_focus_cb, LV_EVENT_CLICKED, NULL);

    // ── On-screen keyboard (btnmatrix, hidden until pw_ta tapped) ──
    pw_kb = lv_btnmatrix_create(scr_wifi);
    lv_obj_set_size(pw_kb, 320, 120);
    lv_obj_set_pos(pw_kb, 0, 120);
    lv_btnmatrix_set_map(pw_kb, kb_alpha_lower);
    lv_obj_set_style_bg_color(pw_kb, CLR_BG, 0);
    lv_obj_set_style_bg_opa(pw_kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pw_kb, 0, 0);
    lv_obj_set_style_pad_all(pw_kb, 2, 0);
    lv_obj_set_style_pad_gap(pw_kb, 2, 0);
    lv_obj_set_style_radius(pw_kb, 0, 0);
    // Button styles
    lv_obj_set_style_bg_color(pw_kb, lv_color_hex(0x0a3a12), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(pw_kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_font(pw_kb, &lv_font_unscii_8, LV_PART_ITEMS);
    lv_obj_set_style_text_color(pw_kb, CLR_GREEN, LV_PART_ITEMS);
    lv_obj_set_style_border_color(pw_kb, CLR_DARK, LV_PART_ITEMS);
    lv_obj_set_style_border_width(pw_kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(pw_kb, 2, LV_PART_ITEMS);
    lv_obj_add_flag(pw_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(pw_kb, kb_btn_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // ── Footer ──
    footer = lv_obj_create(scr_wifi);
    lv_obj_t* foot = footer;
    lv_obj_set_size(foot, 320, 36);
    lv_obj_set_pos(foot, 0, 204);
    lv_obj_set_style_bg_opa(foot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(foot, CLR_DARK, 0);
    lv_obj_set_style_border_width(foot, 1, 0);
    lv_obj_set_style_radius(foot, 0, 0);
    lv_obj_set_style_pad_all(foot, 0, 0);
    lv_obj_clear_flag(foot, LV_OBJ_FLAG_SCROLLABLE);

    status_lbl = mk_lbl(foot, "TAP SCAN TO BEGIN",
                         &lv_font_unscii_8, CLR_DIM, 10, 2, 300);

    // ── [ SCAN ] button ──
    scan_btn = lv_obj_create(foot);
    lv_obj_set_size(scan_btn, 70, 18);
    lv_obj_set_pos(scan_btn, 6, 14);
    lv_obj_set_style_bg_color(scan_btn, CLR_DARK, 0);
    lv_obj_set_style_bg_opa(scan_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(scan_btn, CLR_DIM, 0);
    lv_obj_set_style_border_width(scan_btn, 1, 0);
    lv_obj_set_style_radius(scan_btn, 0, 0);
    lv_obj_set_style_pad_all(scan_btn, 0, 0);
    lv_obj_clear_flag(scan_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scan_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scan_btn, scan_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* scan_lbl = lv_label_create(scan_btn);
    lv_label_set_text(scan_lbl, "[ SCAN ]");
    lv_obj_set_style_text_font(scan_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(scan_lbl, CLR_GREEN, 0);
    lv_obj_align(scan_lbl, LV_ALIGN_CENTER, 0, 0);

    // ── [ CONNECT ] button ──
    connect_btn = lv_obj_create(foot);
    lv_obj_set_size(connect_btn, 100, 18);
    lv_obj_set_pos(connect_btn, 82, 14);
    lv_obj_set_style_bg_color(connect_btn, CLR_DARK, 0);
    lv_obj_set_style_bg_opa(connect_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(connect_btn, CLR_DIM, 0);
    lv_obj_set_style_border_width(connect_btn, 1, 0);
    lv_obj_set_style_radius(connect_btn, 0, 0);
    lv_obj_set_style_pad_all(connect_btn, 0, 0);
    lv_obj_clear_flag(connect_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(connect_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(connect_btn, connect_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* conn_lbl = lv_label_create(connect_btn);
    lv_label_set_text(conn_lbl, "[ CONNECT ]");
    lv_obj_set_style_text_font(conn_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(conn_lbl, CLR_GREEN, 0);
    lv_obj_align(conn_lbl, LV_ALIGN_CENTER, 0, 0);

    // ── [ SKIP ] button — proceed without WiFi ──
    lv_obj_t* skip_btn = lv_obj_create(foot);
    lv_obj_set_size(skip_btn, 70, 18);
    lv_obj_set_pos(skip_btn, 244, 14);
    lv_obj_set_style_bg_color(skip_btn, CLR_DARK, 0);
    lv_obj_set_style_bg_opa(skip_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(skip_btn, CLR_DIM, 0);
    lv_obj_set_style_border_width(skip_btn, 1, 0);
    lv_obj_set_style_radius(skip_btn, 0, 0);
    lv_obj_set_style_pad_all(skip_btn, 0, 0);
    lv_obj_clear_flag(skip_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(skip_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(skip_btn, skip_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* skip_lbl = lv_label_create(skip_btn);
    // Initial label depends on whether we've already heard any peers
    // (nav back to WiFi picker can happen after ESP-NOW has discovered
    // peers, in which case we start out as "[ MESH ]").
    lv_label_set_text(skip_lbl,
                      count_online_peers() > 0 ? "[ MESH ]" : "[ SKIP ]");
    lv_obj_set_style_text_font(skip_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(skip_lbl, CLR_GREEN, 0);
    lv_obj_align(skip_lbl, LV_ALIGN_CENTER, 0, 0);
    skip_btn_lbl = skip_lbl;  // stashed so the mesh timer can relabel it

    // Set WiFi mode for scanning — STA only (see main.cpp / net_wifi.cpp)
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, true);  // disconnect + erase saved creds, stay in STA

    // Delay scan slightly so WiFi radio settles after mode change
    delay_timer = lv_timer_create([](lv_timer_t* t) {
        delay_timer = NULL;
        lv_timer_del(t);
        if (!s_leaving) start_scan();
    }, 500, NULL);

    // Seed the mesh section with whatever peers we already know about
    // (e.g. navigated here from tabs after ESP-NOW already discovered a
    // gateway), then poll every 2 s for changes.
    s_last_mesh_shown = -1;
    populate_list();
    mesh_timer = lv_timer_create(mesh_refresh_cb, 2000, NULL);

    return scr_wifi;
}
