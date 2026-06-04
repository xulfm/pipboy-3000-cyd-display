// ============================================================
// Vault 89 PDI-Boy -- Bluetooth Scanner
// Scans for BLE devices and lets the user tag one as the grill
// probe (saved to NVS so the grill screen can prefer it later).
// ============================================================
#include <Arduino.h>
#include <lvgl.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include "pipboy_data.h"

extern void nav_back_from_sub(void);  // in main.cpp

#define MAX_BT_DEV 12

struct BtEntry {
    char name[24];
    char mac[18];
    int  rssi;
};

static BtEntry s_devs[MAX_BT_DEV];
static int     s_dev_count      = 0;
static bool    s_list_dirty     = true;
static bool    s_scanning       = false;
static int     s_last_rendered  = -1;

static lv_obj_t*   s_list         = NULL;
static lv_obj_t*   s_status_lbl   = NULL;
static lv_obj_t*   s_grill_lbl    = NULL;
static lv_obj_t*   s_scan_btn_lbl = NULL;
static lv_timer_t* s_refresh_timer = NULL;
static NimBLEScan* s_bt_scan = NULL;

// Popup for "Set as grill / Other / Cancel"
static lv_obj_t* s_popup     = NULL;
static int       s_popup_idx = -1;

// ── Scan callback — runs on BLE task ──
class BtScanCb : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* adv) override {
        if (s_dev_count >= MAX_BT_DEV) return;
        std::string mac = adv->getAddress().toString();
        // Dedupe by MAC
        for (int i = 0; i < s_dev_count; i++) {
            if (strcmp(s_devs[i].mac, mac.c_str()) == 0) {
                s_devs[i].rssi = adv->getRSSI();
                return;
            }
        }
        BtEntry& e = s_devs[s_dev_count++];
        strncpy(e.mac, mac.c_str(), sizeof(e.mac) - 1);
        e.mac[sizeof(e.mac) - 1] = '\0';
        std::string nm = adv->haveName() ? adv->getName() : std::string("(unknown)");
        if (nm.empty()) nm = "(unnamed)";
        strncpy(e.name, nm.c_str(), sizeof(e.name) - 1);
        e.name[sizeof(e.name) - 1] = '\0';
        e.rssi = adv->getRSSI();
        s_list_dirty = true;
    }
};
static BtScanCb s_scan_cbs;

static void show_saved_grill(void) {
    Preferences p;
    p.begin("grill", true);
    String mac = p.getString("mac", "");
    String name = p.getString("name", "");
    p.end();
    if (s_grill_lbl) {
        if (mac.length() > 0) {
            char buf[48];
            snprintf(buf, sizeof(buf), PROBE_FEATURE_NAME ": %s", name.length() ? name.c_str() : mac.c_str());
            lv_label_set_text(s_grill_lbl, buf);
            lv_obj_set_style_text_color(s_grill_lbl, CLR_GOLD, 0);
        } else {
            lv_label_set_text(s_grill_lbl, PROBE_FEATURE_NAME ": (none saved)");
            lv_obj_set_style_text_color(s_grill_lbl, CLR_DIM, 0);
        }
    }
}

static void start_scan(void) {
    if (s_scanning) return;
    if (!NimBLEDevice::getInitialized()) {
        NimBLEDevice::init("PipBoy");
        NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    }
    if (!s_bt_scan) s_bt_scan = NimBLEDevice::getScan();
    s_bt_scan->setAdvertisedDeviceCallbacks(&s_scan_cbs, false);
    s_bt_scan->setActiveScan(true);
    s_bt_scan->setInterval(100);
    s_bt_scan->setWindow(99);
    s_dev_count = 0;
    s_list_dirty = true;
    s_scanning = true;
    s_bt_scan->start(6, [](NimBLEScanResults r) {
        (void)r;
        s_scanning = false;
        s_list_dirty = true;
    }, false);
    if (s_status_lbl) lv_label_set_text(s_status_lbl, "SCANNING...");
}

static void stop_scan(void) {
    if (s_bt_scan && s_scanning) {
        s_bt_scan->stop();
    }
    s_scanning = false;
}

static void popup_close(void) {
    if (s_popup) {
        lv_obj_del(s_popup);
        s_popup = NULL;
    }
    s_popup_idx = -1;
}

static void popup_save_grill_cb(lv_event_t* e) {
    (void)e;
    if (s_popup_idx < 0 || s_popup_idx >= s_dev_count) { popup_close(); return; }
    const BtEntry& d = s_devs[s_popup_idx];
    Preferences p;
    p.begin("grill", false);
    p.putString("mac",  d.mac);
    p.putString("name", d.name);
    p.end();
    Serial.printf("[bt] Saved grill: %s / %s\n", d.name, d.mac);
    popup_close();
    show_saved_grill();
    if (s_status_lbl) lv_label_set_text(s_status_lbl, PROBE_FEATURE_NAME " SAVED");
}

static void popup_other_cb(lv_event_t* e) {
    (void)e;
    // "OTHER" — we don't do anything with it yet. Acknowledge and close.
    if (s_status_lbl) lv_label_set_text(s_status_lbl, "TAGGED: OTHER");
    popup_close();
}

static void popup_cancel_cb(lv_event_t* e) {
    (void)e;
    popup_close();
}

static lv_obj_t* mk_popup_btn(lv_obj_t* par, const char* text, int x, int y,
                               int w, int h, lv_color_t c,
                               lv_event_cb_t cb) {
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
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
    return b;
}

static void open_tag_popup(int idx) {
    if (idx < 0 || idx >= s_dev_count) return;
    popup_close();
    s_popup_idx = idx;

    lv_obj_t* pop = lv_obj_create(lv_scr_act());
    lv_obj_set_size(pop, 240, 130);
    lv_obj_set_pos(pop, (320 - 240) / 2, (240 - 130) / 2);
    lv_obj_set_style_bg_color(pop, CLR_BG, 0);
    lv_obj_set_style_bg_opa(pop, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(pop, CLR_GREEN, 0);
    lv_obj_set_style_border_width(pop, 2, 0);
    lv_obj_set_style_radius(pop, 2, 0);
    lv_obj_set_style_pad_all(pop, 4, 0);
    lv_obj_clear_flag(pop, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(pop);
    char tbuf[40];
    snprintf(tbuf, sizeof(tbuf), "TAG: %.20s", s_devs[idx].name);
    lv_label_set_text(title, tbuf);
    lv_obj_set_style_text_font(title, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(title, CLR_GREEN, 0);
    lv_obj_set_pos(title, 4, 2);

    lv_obj_t* mac_lbl = lv_label_create(pop);
    lv_label_set_text(mac_lbl, s_devs[idx].mac);
    lv_obj_set_style_text_font(mac_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(mac_lbl, CLR_DIM, 0);
    lv_obj_set_pos(mac_lbl, 4, 16);

    mk_popup_btn(pop, "[ SET AS " PROBE_FEATURE_NAME " ]", 8,   38, 220, 22, CLR_GREEN, popup_save_grill_cb);
    mk_popup_btn(pop, "[ OTHER ]",        8,   64, 220, 22, CLR_DIM,   popup_other_cb);
    mk_popup_btn(pop, "[ CANCEL ]",       8,   90, 220, 22, CLR_GOLD,  popup_cancel_cb);

    s_popup = pop;
}

static void row_click_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    open_tag_popup(idx);
}

static void rebuild_list(void) {
    if (!s_list) return;
    lv_obj_clean(s_list);
    if (s_dev_count == 0) {
        lv_obj_t* l = lv_label_create(s_list);
        lv_label_set_text(l, s_scanning ? "...scanning..." : "(no devices)");
        lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(l, CLR_DIM, 0);
        lv_obj_set_pos(l, 4, 4);
        return;
    }
    for (int i = 0; i < s_dev_count; i++) {
        lv_obj_t* row = lv_obj_create(s_list);
        lv_obj_set_size(row, 294, 18);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, CLR_DARK, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_left(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        char buf[64];
        snprintf(buf, sizeof(buf), "%-20.20s %4d", s_devs[i].name, s_devs[i].rssi);
        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(lbl, CLR_GREEN, 0);
        lv_obj_set_pos(lbl, 2, 4);
    }
}

static void refresh_tick(lv_timer_t* t) {
    (void)t;
    if (s_list_dirty && s_dev_count != s_last_rendered) {
        rebuild_list();
        s_last_rendered = s_dev_count;
        s_list_dirty = false;
    }
    if (!s_scanning && s_status_lbl) {
        char buf[32];
        snprintf(buf, sizeof(buf), "IDLE - %d FOUND", s_dev_count);
        lv_label_set_text(s_status_lbl, buf);
    } else if (s_scanning && s_status_lbl) {
        char buf[32];
        snprintf(buf, sizeof(buf), "SCANNING... %d", s_dev_count);
        lv_label_set_text(s_status_lbl, buf);
    }
}

static void scan_btn_cb(lv_event_t* e) {
    (void)e;
    if (s_scanning) stop_scan();
    else            start_scan();
}

static void back_btn_cb(lv_event_t* e) {
    (void)e;
    stop_scan();
    nav_back_from_sub();
}

lv_obj_t* screen_bluetooth_create(void) {
    s_dev_count = 0;
    s_last_rendered = -1;
    s_list_dirty = true;
    s_scanning = false;

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
    lv_label_set_text(htitle, "** BLUETOOTH SCAN **");
    lv_obj_set_style_text_font(htitle, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(htitle, CLR_GREEN, 0);
    lv_obj_align(htitle, LV_ALIGN_CENTER, 0, 0);

    // Device list
    s_list = lv_obj_create(scr);
    lv_obj_set_size(s_list, 310, 150);
    lv_obj_set_pos(s_list, 5, 26);
    lv_obj_set_style_bg_color(s_list, CLR_BG, 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_list, CLR_DARK, 0);
    lv_obj_set_style_border_width(s_list, 1, 0);
    lv_obj_set_style_radius(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(s_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_list, CLR_DARK, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, LV_PART_SCROLLBAR);

    // Current grill tag
    s_grill_lbl = lv_label_create(scr);
    lv_label_set_text(s_grill_lbl, PROBE_FEATURE_NAME ": --");
    lv_obj_set_style_text_font(s_grill_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_grill_lbl, CLR_DIM, 0);
    lv_obj_set_pos(s_grill_lbl, 6, 180);

    // Status
    s_status_lbl = lv_label_create(scr);
    lv_label_set_text(s_status_lbl, "IDLE");
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_status_lbl, CLR_DIM, 0);
    lv_obj_set_pos(s_status_lbl, 6, 194);

    // SCAN button
    lv_obj_t* scan_btn = lv_obj_create(scr);
    lv_obj_set_size(scan_btn, 100, 26);
    lv_obj_set_pos(scan_btn, 110, 210);
    lv_obj_set_style_bg_color(scan_btn, CLR_HEADER, 0);
    lv_obj_set_style_bg_opa(scan_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(scan_btn, CLR_GREEN, 0);
    lv_obj_set_style_border_width(scan_btn, 1, 0);
    lv_obj_set_style_radius(scan_btn, 2, 0);
    lv_obj_set_style_pad_all(scan_btn, 0, 0);
    lv_obj_clear_flag(scan_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scan_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scan_btn, scan_btn_cb, LV_EVENT_CLICKED, NULL);
    s_scan_btn_lbl = lv_label_create(scan_btn);
    lv_label_set_text(s_scan_btn_lbl, "[ SCAN ]");
    lv_obj_set_style_text_font(s_scan_btn_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_scan_btn_lbl, CLR_GREEN, 0);
    lv_obj_align(s_scan_btn_lbl, LV_ALIGN_CENTER, 0, 0);

    // BACK button
    lv_obj_t* back_btn = lv_obj_create(scr);
    lv_obj_set_size(back_btn, 90, 26);
    lv_obj_set_pos(back_btn, 220, 210);
    lv_obj_set_style_bg_color(back_btn, CLR_HEADER, 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(back_btn, CLR_GOLD, 0);
    lv_obj_set_style_border_width(back_btn, 1, 0);
    lv_obj_set_style_radius(back_btn, 2, 0);
    lv_obj_set_style_pad_all(back_btn, 0, 0);
    lv_obj_clear_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* bl = lv_label_create(back_btn);
    lv_label_set_text(bl, "[ BACK ]");
    lv_obj_set_style_text_font(bl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(bl, CLR_GOLD, 0);
    lv_obj_align(bl, LV_ALIGN_CENTER, 0, 0);

    show_saved_grill();
    rebuild_list();
    s_refresh_timer = lv_timer_create(refresh_tick, 500, NULL);

    // Auto-start scan on open
    start_scan();

    // Teardown on delete
    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        (void)e;
        stop_scan();
        if (s_refresh_timer) { lv_timer_del(s_refresh_timer); s_refresh_timer = NULL; }
        // Detach callback so it can't fire after delete
        if (s_bt_scan) s_bt_scan->setAdvertisedDeviceCallbacks(NULL, false);
        s_list = NULL;
        s_status_lbl = NULL;
        s_grill_lbl = NULL;
        s_scan_btn_lbl = NULL;
        s_popup = NULL;
        s_popup_idx = -1;
    }, LV_EVENT_DELETE, NULL);

    return scr;
}
