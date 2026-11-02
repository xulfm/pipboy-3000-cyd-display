#include <Arduino.h>
#include <SPI.h>
#include "board_config.h"
#if defined(BOARD_FREENOVE35)
#include "freenove35_display.h"
#else
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#endif
#include <lvgl.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <Preferences.h>
#include "pipboy_data.h"
#include "net_state.h"
#include "sound_fx.h"
#include "clock_sync.h"
#include "brightness.h"
#include "game_proto.h"

// ── WiFi credential persistence (NVS) ──
static Preferences prefs;

void wifi_creds_save(const char* ssid, const char* pass) {
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
    Serial.printf("[nvs] Saved WiFi creds for '%s'\n", ssid);
}

bool wifi_creds_load(char* ssid, size_t ssid_len, char* pass, size_t pass_len) {
    prefs.begin("wifi", true);
    String s = prefs.getString("ssid", "");
    String p = prefs.getString("pass", "");
    prefs.end();
    if (s.length() == 0) return false;
    strncpy(ssid, s.c_str(), ssid_len - 1);
    ssid[ssid_len - 1] = '\0';
    strncpy(pass, p.c_str(), pass_len - 1);
    pass[pass_len - 1] = '\0';
    Serial.printf("[nvs] Loaded WiFi creds for '%s'\n", ssid);
    return true;
}

void wifi_creds_clear(void) {
    prefs.begin("wifi", false);
    prefs.clear();
    prefs.end();
    Serial.println("[nvs] WiFi creds cleared");
}

// ── Gateway election ──────────────────────────────────────────
// In a small-fleet deployment (several PDI-Boys on desks in the same
// office) only ONE of them needs to hold a WiFi uplink. That device
// fetches NTP + weather from the internet and fans the results out
// over ESP-NOW via MSG_TIME / MSG_WEATHER; all the others skip WiFi
// entirely and just consume the mesh broadcasts.
//
// Election state machine (per boot):
//   NO_CREDS   — no saved WiFi; mesh-only forever unless user picks one
//   ELECTING   — 6 s listening window, collecting MSG_GATEWAY_ADV frames
//   DEFERRED   — heard a peer claiming GW_FLAG_WIFI_CONN, so we skip
//                WiFi.begin() and wait for its mesh data. If it goes
//                silent for GW_DEFER_TIMEOUT_MS we promote ourselves.
//   ACTIVE     — we own the uplink and have called WiFi.begin()
//
// Simultaneous boots are the one race we don't solve here — if two
// fresh devices come up inside the same 6 s window, both become ACTIVE
// (neither has WIFI_CONN set yet so there's nothing to defer to). The
// real-world case (existing gateway + new peer) works cleanly: the
// new peer hears GW_ADV with WIFI_CONN set within the first 10 s.
enum GatewayState : uint8_t {
    GW_STATE_NO_CREDS = 0,
    GW_STATE_ELECTING,
    GW_STATE_DEFERRED,
    GW_STATE_ACTIVE,
};

static GatewayState  s_gw_state           = GW_STATE_NO_CREDS;
static unsigned long s_gw_state_enter_ms  = 0;
static char          s_gw_ssid[33]        = {};
static char          s_gw_pass[65]        = {};

#define GW_ELECTION_MS        6000UL
#define GW_DEFER_TIMEOUT_MS   60000UL

// Return true if there's a peer within the last `within_ms` that has
// the GW_FLAG_WIFI_CONN bit set (i.e. claims an active uplink).
static bool gw_find_active_peer(unsigned long within_ms, int* out_idx) {
    unsigned long now = millis();
    for (int i = 0; i < g_peer_count; i++) {
        if (!(g_peers[i].gw_flags & GW_FLAG_WIFI_CONN)) continue;
        if (g_peers[i].gw_last_adv_ms == 0) continue;
        if ((now - g_peers[i].gw_last_adv_ms) > within_ms) continue;
        if (out_idx) *out_idx = i;
        return true;
    }
    return false;
}

static void gateway_election_init(void) {
    if (wifi_creds_load(s_gw_ssid, sizeof(s_gw_ssid),
                        s_gw_pass, sizeof(s_gw_pass))) {
        s_gw_state = GW_STATE_ELECTING;
        s_gw_state_enter_ms = millis();
        Serial.printf("[gw] Creds loaded for '%s' -> ELECTING (%lums)\n",
                      s_gw_ssid, (unsigned long)GW_ELECTION_MS);
    } else {
        s_gw_state = GW_STATE_NO_CREDS;
        Serial.println("[gw] No saved creds -> mesh-only");
    }
}

static void gateway_election_poll(void) {
    if (s_gw_state == GW_STATE_NO_CREDS) return;
    unsigned long now = millis();

    if (s_gw_state == GW_STATE_ELECTING) {
        if ((now - s_gw_state_enter_ms) < GW_ELECTION_MS) return;
        int peer_idx = -1;
        if (gw_find_active_peer(GW_ELECTION_MS + 4000, &peer_idx)) {
            const EspNowPeer& p = g_peers[peer_idx];
            Serial.printf("[gw] ELECTING done -> DEFERRED to peer '%s' "
                          "rssi=%d flags=0x%02X\n",
                          p.name[0] ? p.name : "(peer)",
                          (int)p.gw_wifi_rssi, p.gw_flags);
            s_gw_state = GW_STATE_DEFERRED;
            s_gw_state_enter_ms = now;
        } else {
            Serial.println("[gw] ELECTING done -> ACTIVE (no peer gateway)");
            s_gw_state = GW_STATE_ACTIVE;
            s_gw_state_enter_ms = now;
            espnow_announce_wifi_channel(s_gw_ssid);
            WiFi.begin(s_gw_ssid, s_gw_pass);
        }
        return;
    }

    if (s_gw_state == GW_STATE_DEFERRED) {
        // Watchdog: if no peer gateway heard for the timeout, promote.
        if (!gw_find_active_peer(GW_DEFER_TIMEOUT_MS, NULL)) {
            Serial.println("[gw] DEFERRED peer lost -> promoting to ACTIVE");
            s_gw_state = GW_STATE_ACTIVE;
            s_gw_state_enter_ms = now;
            espnow_announce_wifi_channel(s_gw_ssid);
            WiFi.begin(s_gw_ssid, s_gw_pass);
        }
        return;
    }
    // GW_STATE_ACTIVE — wifi_poll handles reconnect on drop
}

lv_obj_t* screen_stat_create(void);
void      screen_stat_load(int idx);
lv_obj_t* screen_inv_create(void);
lv_obj_t* screen_data_create(void);
lv_obj_t* screen_map_create(void);
lv_obj_t* screen_radio_create(void);
lv_obj_t* screen_login_create(void);
lv_obj_t* screen_wifi_create(void);
lv_obj_t* screen_clock_create(void);
lv_obj_t* screen_weather_create(void);
lv_obj_t* screen_mainmenu_create(void);
lv_obj_t* screen_bluetooth_create(void);
lv_obj_t* screen_settings_create(void);
lv_obj_t* screen_apps_create(void);
lv_obj_t* screen_pomodoro_create(void);
lv_obj_t* screen_stopwatch_create(void);
lv_obj_t* screen_ping_create(void);
lv_obj_t* screen_dnd_create(void);
lv_obj_t* screen_presence_create(void);
lv_obj_t* screen_pager_create(void);
lv_obj_t* screen_dice_create(void);
lv_obj_t* screen_holotape_create(void);
lv_obj_t* screen_radstats_create(void);
lv_obj_t* screen_soundboard_create(void);
lv_obj_t* screen_snake_create(void);
lv_obj_t* screen_quests_create(void);
extern "C" lv_obj_t* screen_blackjack_menu_create(void);
extern void blackjack_init(void);

// Weather subsystem (src/weather.cpp)
extern void weather_init(void);

// Apps backend (src/apps_state.cpp)
#include "apps.h"

// Settings navigation — called from STAT screen gear menu
void nav_to_wifi(void);
void nav_to_login(void);
void nav_to_main_menu(void);
void clock_mode_enter(void);
void clock_mode_exit(void);
void clock_to_weather(void);
void weather_to_clock(void);
void weather_mode_exit(void);

// Called by sub-screens (bluetooth, settings) to go back to wherever
// they were launched from (main menu OR tabs).
void nav_back_from_sub(void);

// Main menu button targets
void menu_nav_login(void);
void menu_nav_wifi(void);
void menu_nav_bluetooth(void);
void menu_nav_clock(void);
void menu_nav_settings(void);
void menu_nav_apps(void);
// Apps nav (called from screen_apps.cpp tile handlers)
void apps_open_hub(void);
void apps_nav_back(void);
void apps_swap_sub_scr(lv_obj_t* new_scr);
void apps_open_pomodoro(void);
void apps_open_stopwatch(void);
void apps_open_ping(void);
void apps_open_dnd(void);
void apps_open_soundboard(void);
void apps_open_holotape(void);
void apps_open_dice(void);
void apps_open_snake(void);
void apps_open_radstats(void);
void apps_open_presence(void);
void apps_open_pager(void);
void apps_open_quests(void);
void apps_open_blackjack(void);
void apps_open_calibrate(void);

int g_dweller_idx = 0;

// ── Battery monitoring ──
// BATT_ADC_PIN comes from board_config.h (100K/100K divider on both boards)
#define BATT_SAMPLES   8          // average N readings
static lv_obj_t* batt_pct_lbl  = NULL;
static lv_obj_t* batt_body     = NULL;
static lv_obj_t* batt_fill     = NULL;
static lv_obj_t* batt_tip      = NULL;
static lv_timer_t* batt_timer  = NULL;
static bool batt_detected      = false;

// Exposed so other screens (e.g. the clock) can render their own battery UI
int read_batt_pct(void) {
    // Use analogReadMilliVolts() — it applies ESP32 eFuse Vref calibration
    // and corrects the ~0.2V nonlinearity that plain analogRead() has at the
    // top of the range. Without this, a fully charged pack reads ~3.98V.
    long mv_sum = 0;
    for (int i = 0; i < BATT_SAMPLES; i++) mv_sum += analogReadMilliVolts(BATT_ADC_PIN);
    float pin_mv = (float)mv_sum / BATT_SAMPLES;
    // CYD has 100K/100K divider → actual battery voltage = pin * 2
    float voltage = (pin_mv * 2.0f) / 1000.0f;
    // Detect if battery is actually connected (>2.5V means something is there)
    if (voltage < 2.5f) return -1;
    // LiPo curve: 4.15V=100% (typical charger termination), 3.30V=0% (safe cutoff)
    // This avoids the "stuck at 82%" issue where the charger never reaches 4.2V.
    const float V_FULL  = 4.15f;
    const float V_EMPTY = 3.30f;
    int pct = (int)((voltage - V_EMPTY) / (V_FULL - V_EMPTY) * 100.0f + 0.5f);
    if (pct > 100) pct = 100;
    if (pct < 0)   pct = 0;
    return pct;
}

static void batt_timer_cb(lv_timer_t* t) {
    int pct = read_batt_pct();
    if (pct < 0) {
        // No battery detected
        if (batt_detected) {
            batt_detected = false;
            if (batt_pct_lbl) lv_obj_add_flag(batt_pct_lbl, LV_OBJ_FLAG_HIDDEN);
            if (batt_body)    lv_obj_add_flag(batt_body, LV_OBJ_FLAG_HIDDEN);
            if (batt_fill)    lv_obj_add_flag(batt_fill, LV_OBJ_FLAG_HIDDEN);
            if (batt_tip)     lv_obj_add_flag(batt_tip, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    batt_detected = true;
    if (batt_pct_lbl) {
        lv_obj_clear_flag(batt_pct_lbl, LV_OBJ_FLAG_HIDDEN);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(batt_pct_lbl, buf);
    }
    if (batt_body) lv_obj_clear_flag(batt_body, LV_OBJ_FLAG_HIDDEN);
    if (batt_tip)  lv_obj_clear_flag(batt_tip, LV_OBJ_FLAG_HIDDEN);
    if (batt_fill) {
        lv_obj_clear_flag(batt_fill, LV_OBJ_FLAG_HIDDEN);
        // Fill bar: max 14px wide based on percentage
        int fw = (14 * pct) / 100;
        if (fw < 1 && pct > 0) fw = 1;
        lv_obj_set_width(batt_fill, fw);
        // Color: green >40%, yellow 15-40%, red <15%
        lv_color_t c = pct > 40 ? CLR_GREEN :
                       pct > 15 ? lv_color_hex(0xCCCC00) :
                                  lv_color_hex(0xCC0000);
        lv_obj_set_style_bg_color(batt_fill, c, 0);
    }
}


#if !defined(BOARD_FREENOVE35)
SPIClass touchSPI(VSPI);
#define TOUCH_CS   33
#define TOUCH_IRQ  36
#endif
// LCD_BL_PIN comes from board_config.h
#define SLEEP_BTN  0            // BOOT button (GPIO 0) on both boards

// ── Backlight PWM + auto-dim ──
#define BL_PWM_CHANNEL  0
#define BL_PWM_FREQ     5000
#define BL_PWM_BITS     8           // 0-255
#define BL_BRIGHT       255         // 100% when active
#define BL_DIM          64          // ~25% when idle
#define BL_DIM_MS       30000       // dim after 30s no touch
static uint8_t         bl_current  = BL_BRIGHT;
static unsigned long   last_touch_ms = 0;
static bool            bl_dimmed   = false;

static void set_backlight(uint8_t level) {
    // Kept for explicit overrides (e.g. sleep). Normal day/night
    // operation goes through brightness_apply() which caps PWM
    // based on the schedule.
    if (level == bl_current) return;
    bl_current = level;
    ledcWrite(BL_PWM_CHANNEL, level);
}

#if !defined(BOARD_FREENOVE35)
TFT_eSPI            tft;
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
#endif

static const uint16_t SCREEN_W  = 320;
static const uint16_t SCREEN_H  = 240;
static const uint16_t TAB_H     = 18;
static const uint16_t BUF_LINES = 10;

// Touch calibration — loaded from NVS at boot, defaults to safe range
static int TOUCH_MIN_X = 200;
static int TOUCH_MAX_X = 3800;
static int TOUCH_MIN_Y = 200;
static int TOUCH_MAX_Y = 3800;

// Raw touch values for calibration screen
static int g_touch_raw_x = 0;
static int g_touch_raw_y = 0;

static int       g_active_tab = -1;
static lv_obj_t* g_screens[5] = {};
static lv_obj_t* g_tab_btns[5]   = {};
static lv_obj_t* g_tab_labels[5] = {};

static const char* TAB_NAMES[] = {"STAT","INV","DATA","MAP","RADIO"};

// Touch state — read once per loop, shared by tab handler and LVGL
static bool          g_touch_pressed = false;
static int           g_touch_x = 0;
static int           g_touch_y = 0;
static bool          g_tabs_built = false;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[SCREEN_W * BUF_LINES];
// buf2 removed to save 6.4KB RAM for BLE stack

// Set during a screenshot: lv_flush_cb also streams each flushed strip
// over serial so the host can reassemble a full-screen PNG. No extra
// framebuffer needed — we piggyback on LVGL's normal partial flushes.
volatile bool g_screenshot_capturing = false;
static void screenshot_dump_region(const lv_area_t* area, lv_color_t* color_p);

void lv_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
#if defined(BOARD_FREENOVE35)
    (void)w; (void)h;
    fn35_flush(area, color_p);   // scales the 320x240 UI up to the 480x320 panel
#else
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t*)color_p, w * h, true);
    tft.endWrite();
#endif
    if (g_screenshot_capturing) screenshot_dump_region(area, color_p);
    lv_disp_flush_ready(drv);
}

#if defined(BOARD_FREENOVE35) && FN35_TOUCH_DEBUG
// On-screen touch diagnostics: a status line at the bottom and a red dot
// that follows your finger. Turn off with -DFN35_TOUCH_DEBUG=0.
static lv_obj_t* s_tdbg_lbl = NULL;
static lv_obj_t* s_tdbg_dot = NULL;
static void tdbg_timer_cb(lv_timer_t* t) {
    (void)t;
    char line[96];
    fn35_touch_status(line, sizeof(line));
    lv_label_set_text(s_tdbg_lbl, line);
    if (g_touch_pressed) {
        lv_obj_set_pos(s_tdbg_dot, g_touch_x - 3, g_touch_y - 3);
        lv_obj_clear_flag(s_tdbg_dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_tdbg_dot, LV_OBJ_FLAG_HIDDEN);
    }
}
static void tdbg_create(void) {
    s_tdbg_lbl = lv_label_create(lv_layer_sys());   // sys layer: the app never clears it
    lv_obj_set_width(s_tdbg_lbl, SCREEN_W);
    lv_label_set_long_mode(s_tdbg_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_tdbg_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_tdbg_lbl, lv_color_hex(0xFFD000), 0);
    lv_obj_set_style_bg_color(s_tdbg_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_tdbg_lbl, LV_OPA_COVER, 0);
    lv_obj_align(s_tdbg_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_label_set_text(s_tdbg_lbl, "TOUCH: starting");

    s_tdbg_dot = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(s_tdbg_dot, 7, 7);
    lv_obj_set_style_radius(s_tdbg_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_tdbg_dot, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_bg_opa(s_tdbg_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_tdbg_dot, 0, 0);
    lv_obj_clear_flag(s_tdbg_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_tdbg_dot, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(tdbg_timer_cb, 100, NULL);
}
#endif

void lv_touch_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
    if (g_touch_pressed) {
        data->point.x = g_touch_x;
        data->point.y = g_touch_y;
        data->state   = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

void update_tab_bar(int active) {
    for (int i = 0; i < 5; i++) {
        if (!g_tab_btns[i]) continue;
        lv_obj_set_style_bg_color(g_tab_btns[i],
            i == active ? CLR_SEL : CLR_HEADER, LV_PART_MAIN);
        lv_obj_set_style_text_color(g_tab_labels[i],
            i == active ? CLR_GREEN : CLR_DIM, LV_PART_MAIN);
    }
}

void switch_tab(int idx);

static void tab_click_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    switch_tab(idx);
}

void build_tab_bar(void) {
    lv_obj_t* bar = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bar, SCREEN_W, TAB_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, CLR_HEADER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, CLR_GREEN, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    int tw = SCREEN_W / 5;
    for (int i = 0; i < 5; i++) {
        lv_obj_t* btn = lv_obj_create(bar);
        lv_obj_set_size(btn, tw, TAB_H - 1);
        lv_obj_set_pos(btn, i * tw, 0);
        lv_obj_set_style_bg_color(btn, i == 0 ? CLR_SEL : CLR_HEADER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_RIGHT, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, CLR_DARK, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, i < 4 ? 1 : 0, LV_PART_MAIN);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, tab_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        g_tab_btns[i] = btn;

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, TAB_NAMES[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, i == 0 ? CLR_GREEN : CLR_DIM, LV_PART_MAIN);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        g_tab_labels[i] = lbl;
    }

    // ── Battery indicator (bottom-right, on layer_top so always visible) ──
    lv_obj_t* layer = lv_layer_top();

    // Battery body outline: 18x10 rect
    batt_body = lv_obj_create(layer);
    lv_obj_set_size(batt_body, 18, 10);
    lv_obj_set_pos(batt_body, 270, 228);
    lv_obj_set_style_bg_opa(batt_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(batt_body, CLR_DIM, 0);
    lv_obj_set_style_border_width(batt_body, 1, 0);
    lv_obj_set_style_radius(batt_body, 1, 0);
    lv_obj_set_style_pad_all(batt_body, 0, 0);
    lv_obj_clear_flag(batt_body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Battery tip (positive terminal nub)
    batt_tip = lv_obj_create(layer);
    lv_obj_set_size(batt_tip, 3, 5);
    lv_obj_set_pos(batt_tip, 288, 231);
    lv_obj_set_style_bg_color(batt_tip, CLR_DIM, 0);
    lv_obj_set_style_bg_opa(batt_tip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(batt_tip, 0, 0);
    lv_obj_set_style_radius(batt_tip, 0, 0);
    lv_obj_clear_flag(batt_tip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Battery fill bar (inside body)
    batt_fill = lv_obj_create(batt_body);
    lv_obj_set_size(batt_fill, 14, 6);
    lv_obj_set_pos(batt_fill, 1, 1);
    lv_obj_set_style_bg_color(batt_fill, CLR_GREEN, 0);
    lv_obj_set_style_bg_opa(batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(batt_fill, 0, 0);
    lv_obj_set_style_radius(batt_fill, 0, 0);
    lv_obj_clear_flag(batt_fill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Percentage label
    batt_pct_lbl = lv_label_create(layer);
    lv_label_set_text(batt_pct_lbl, "");
    lv_obj_set_style_text_font(batt_pct_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(batt_pct_lbl, CLR_DIM, 0);
    lv_obj_set_pos(batt_pct_lbl, 293, 230);

    // Hide until first reading confirms a battery
    lv_obj_add_flag(batt_body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(batt_tip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(batt_fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(batt_pct_lbl, LV_OBJ_FLAG_HIDDEN);

    // Start battery polling timer (every 5 seconds)
    batt_timer = lv_timer_create(batt_timer_cb, 5000, NULL);
    // Do one immediate read
    batt_timer_cb(NULL);
}

static lv_obj_t* create_tab_screen(int idx) {
    switch (idx) {
        case 0: return screen_stat_create();
        case 1: return screen_inv_create();
        case 2: return screen_data_create();
        case 3: return screen_map_create();
        case 4: return screen_radio_create();
    }
    return NULL;
}

void switch_tab(int idx) {
    if (idx == g_active_tab && g_screens[idx] != NULL) return;

    // Free the OUTGOING screen before building the new one. These screens
    // are object-heavy and this board has only ~90 KB free heap with no
    // PSRAM — holding two at once (build-new-then-free-old) peaks over the
    // limit and reboots the device on heavier transitions (e.g. STAT->INV).
    // LVGL can't delete the active screen, so bridge through a cheap blank
    // screen first (same pattern as teardown_tabs).
    if (g_active_tab >= 0 && g_screens[g_active_tab] != NULL) {
        lv_obj_t* blank = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(blank, CLR_BG, 0);
        lv_obj_set_style_bg_opa(blank, LV_OPA_COVER, 0);
        lv_scr_load(blank);                       // active screen is now cheap
        lv_obj_del(g_screens[g_active_tab]);      // free the heavy outgoing one
        g_screens[g_active_tab] = NULL;

        lv_obj_t* new_scr = create_tab_screen(idx);
        if (!new_scr) { update_tab_bar(g_active_tab); return; }
        g_screens[idx] = new_scr;
        g_active_tab   = idx;
        lv_scr_load(new_scr);
        lv_obj_del(blank);
    } else {
        lv_obj_t* new_scr = create_tab_screen(idx);
        if (!new_scr) return;
        g_screens[idx] = new_scr;
        g_active_tab   = idx;
        lv_scr_load(new_scr);
    }
    update_tab_bar(idx);
}

static lv_obj_t* g_wifi_scr  = NULL;
static lv_obj_t* g_login_scr = NULL;
static lv_obj_t* g_menu_scr  = NULL;
static lv_obj_t* g_sub_scr   = NULL;   // current generic sub-screen (BT/Settings)

// Where a sub-screen should return to when the user hits "BACK".
// Set by whoever launches the sub-screen.
enum SubReturn { RET_MAIN_MENU, RET_TABS };
static SubReturn g_sub_return = RET_MAIN_MENU;

// Helper: load a new screen, delete the old one via a blank bridge
static void swap_screen(lv_obj_t*& old_ref, lv_obj_t* new_scr) {
    lv_obj_t* blank = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(blank, CLR_BG, 0);
    lv_obj_set_style_bg_opa(blank, LV_OPA_COVER, 0);
    lv_scr_load(blank);
    if (old_ref) {
        lv_obj_del(old_ref);
        old_ref = NULL;
    }
    lv_scr_load(new_scr);
    lv_obj_del_async(blank);
}

void on_wifi_done(void) {
    Serial.printf("[main] on_wifi_done (connected=%d, return=%s)\n",
                  g_net.wifi_connected,
                  g_sub_return == RET_TABS ? "tabs" : "menu");
    if (g_sub_return == RET_TABS) {
        // Came from STAT gear menu — rebuild tabs
        lv_obj_t* old = g_wifi_scr;
        g_wifi_scr = NULL;
        build_tab_bar();
        g_tabs_built = true;
        switch_tab(0);
        if (old) lv_obj_del_async(old);
    } else {
        // Default: return to main menu
        g_menu_scr = screen_mainmenu_create();
        swap_screen(g_wifi_scr, g_menu_scr);
    }
    Serial.printf("[main] heap=%u\n", ESP.getFreeHeap());
}

void on_login_complete(int idx) {
    g_dweller_idx = idx;
    g_logged_in   = true;   // now allowed to broadcast ESP-NOW heartbeats
    ws_send_login(idx);
    build_tab_bar();
    g_tabs_built = true;
    switch_tab(0);
    // Delete login screen after switching away (free memory + stop its timers)
    if (g_login_scr) {
        lv_obj_del_async(g_login_scr);
        g_login_scr = NULL;
    }
}

// Public: tear down tab bar if it's up. Called by game_proto when a
// player accepts an invite while the tab view is active — the tab bar
// (on lv_layer_top) would otherwise cover the game screen's header.
void nav_teardown_tabs_if_needed(void) {
    if (!g_tabs_built) return;
    Serial.println("[nav] teardown_tabs (game invite accepted from tab view)");
    // Delete the current tab screen
    if (g_active_tab >= 0 && g_screens[g_active_tab]) {
        lv_obj_del(g_screens[g_active_tab]);
        g_screens[g_active_tab] = NULL;
    }
    g_active_tab = -1;
    g_tabs_built = false;
    // Remove tab bar + battery indicator from layer_top
    if (batt_timer) { lv_timer_del(batt_timer); batt_timer = NULL; }
    lv_obj_clean(lv_layer_top());
    batt_pct_lbl = batt_body = batt_fill = batt_tip = NULL;
}

// ── Settings navigation ──
static void teardown_tabs(void) {
    // Load a blank bridge screen FIRST so LVGL stops using the active screen
    lv_obj_t* blank = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(blank, CLR_BG, 0);
    lv_obj_set_style_bg_opa(blank, LV_OPA_COVER, 0);
    lv_scr_load(blank);

    // Now safe to delete the old tab screen
    if (g_active_tab >= 0 && g_screens[g_active_tab]) {
        lv_obj_del(g_screens[g_active_tab]);
        g_screens[g_active_tab] = NULL;
    }
    g_active_tab = -1;
    g_tabs_built = false;

    // Remove tab bar + battery indicator from layer_top
    if (batt_timer) { lv_timer_del(batt_timer); batt_timer = NULL; }
    lv_obj_clean(lv_layer_top());
    batt_pct_lbl = batt_body = batt_fill = batt_tip = NULL;

    // blank is now the active screen — caller loads new screen then deletes blank
    // Store it so caller can delete
    g_login_scr = blank;  // reuse pointer temporarily
}

void nav_to_wifi(void) {
    Serial.println("[nav] -> WiFi picker (from tabs)");
    g_sub_return = RET_TABS;
    wifi_creds_clear();
    WiFi.disconnect(true);
    g_net.wifi_connected = false;

    teardown_tabs();
    lv_obj_t* blank = g_login_scr;
    g_login_scr = NULL;

    g_wifi_scr = screen_wifi_create();
    lv_scr_load(g_wifi_scr);
    lv_obj_del_async(blank);
}

void nav_to_login(void) {
    Serial.println("[nav] -> Login (switch user)");

    teardown_tabs();
    lv_obj_t* blank = g_login_scr;
    g_login_scr = NULL;

    g_login_scr = screen_login_create();
    lv_scr_load(g_login_scr);
    lv_obj_del_async(blank);
}

// ── Clock mode (full-screen) ──
static lv_obj_t* g_clock_scr   = NULL;
static lv_obj_t* g_weather_scr = NULL;

void clock_mode_enter(void) {
    Serial.println("[nav] -> Clock mode (from tabs)");
    g_sub_return = RET_TABS;
    teardown_tabs();
    lv_obj_t* blank = g_login_scr;
    g_login_scr = NULL;

    g_clock_scr = screen_clock_create();
    lv_scr_load(g_clock_scr);
    lv_obj_del_async(blank);
}

void clock_mode_exit(void) {
    Serial.println("[nav] <- Clock mode");
    lv_obj_t* old = g_clock_scr;
    g_clock_scr = NULL;

    if (g_sub_return == RET_TABS) {
        // Rebuild tab UI and go back to STAT tab
        build_tab_bar();
        g_tabs_built = true;
        switch_tab(0);
        if (old) lv_obj_del_async(old);
    } else {
        // Return to main menu
        g_menu_scr = screen_mainmenu_create();
        lv_scr_load(g_menu_scr);
        if (old) lv_obj_del_async(old);
    }
}

// Clock -> Weather: swap in the full-page weather screen. Same return
// behavior (clock_to_weather/weather_to_clock is a sub-modal inside
// "clock mode" — g_sub_return is preserved so the eventual exit goes
// back to the right place).
void clock_to_weather(void) {
    Serial.println("[nav] Clock -> Weather");
    if (g_weather_scr) return;   // already there
    lv_obj_t* old = g_clock_scr;
    g_clock_scr = NULL;
    g_weather_scr = screen_weather_create();
    lv_scr_load(g_weather_scr);
    if (old) lv_obj_del_async(old);
}

// Weather -> Clock: come back to the flip-clock view.
void weather_to_clock(void) {
    Serial.println("[nav] Weather -> Clock");
    if (g_clock_scr) return;
    lv_obj_t* old = g_weather_scr;
    g_weather_scr = NULL;
    g_clock_scr = screen_clock_create();
    lv_scr_load(g_clock_scr);
    if (old) lv_obj_del_async(old);
}

// Exit clock mode while on the weather screen (TAP TO EXIT label there).
void weather_mode_exit(void) {
    Serial.println("[nav] <- Weather mode");
    lv_obj_t* old = g_weather_scr;
    g_weather_scr = NULL;

    if (g_sub_return == RET_TABS) {
        build_tab_bar();
        g_tabs_built = true;
        switch_tab(0);
        if (old) lv_obj_del_async(old);
    } else {
        g_menu_scr = screen_mainmenu_create();
        lv_scr_load(g_menu_scr);
        if (old) lv_obj_del_async(old);
    }
}

// ── Main menu ↔ sub-screen navigation ──
//
// From main menu, each button creates a sub-screen and deletes the menu.
// From tabs (STAT gear), the same sub-screens can be opened, but they
// return to tabs instead of the menu. g_sub_return tracks which.

void nav_to_main_menu(void) {
    Serial.println("[nav] -> Main menu (from tabs)");
    teardown_tabs();
    lv_obj_t* blank = g_login_scr;
    g_login_scr = NULL;
    g_menu_scr = screen_mainmenu_create();
    lv_scr_load(g_menu_scr);
    lv_obj_del_async(blank);
}

void menu_nav_login(void) {
    Serial.println("[nav] Menu -> Login");
    g_login_scr = screen_login_create();
    swap_screen(g_menu_scr, g_login_scr);
}

void menu_nav_wifi(void) {
    Serial.println("[nav] Menu -> WiFi picker");
    g_sub_return = RET_MAIN_MENU;
    wifi_creds_clear();
    WiFi.disconnect(true);
    g_net.wifi_connected = false;
    g_wifi_scr = screen_wifi_create();
    swap_screen(g_menu_scr, g_wifi_scr);
}

void menu_nav_bluetooth(void) {
    Serial.println("[nav] Menu -> Bluetooth");
    g_sub_return = RET_MAIN_MENU;
    g_sub_scr = screen_bluetooth_create();
    swap_screen(g_menu_scr, g_sub_scr);
}

void menu_nav_clock(void) {
    Serial.println("[nav] Menu -> Clock");
    g_sub_return = RET_MAIN_MENU;
    g_clock_scr = screen_clock_create();
    swap_screen(g_menu_scr, g_clock_scr);
}

void menu_nav_settings(void) {
    Serial.println("[nav] Menu -> Settings");
    g_sub_return = RET_MAIN_MENU;
    g_sub_scr = screen_settings_create();
    swap_screen(g_menu_scr, g_sub_scr);
}

// ── Apps hub + individual app launchers ─────────────────────────────────────
// g_sub_scr holds whichever app screen is currently active (including the
// hub itself). Opening another app tears down the current screen and
// swaps in the new one. apps_nav_back() returns to the main menu.

void menu_nav_apps(void) {
    Serial.println("[nav] Menu -> Apps");
    g_sub_return = RET_MAIN_MENU;
    g_sub_scr = screen_apps_create();
    swap_screen(g_menu_scr, g_sub_scr);
}

void apps_nav_back(void) {
    Serial.println("[nav] Apps -> Main menu");
    lv_obj_t* old = g_sub_scr;
    g_sub_scr = NULL;
    g_menu_scr = screen_mainmenu_create();
    lv_scr_load(g_menu_scr);
    if (old) lv_obj_del_async(old);
}

void apps_open_hub(void) {
    Serial.println("[nav] Apps -> Hub");
    lv_obj_t* old = g_sub_scr;
    g_sub_scr = screen_apps_create();
    lv_scr_load(g_sub_scr);
    if (old) lv_obj_del_async(old);
}

// Generic helper: teardown current sub-screen, swap in a new one
static void apps_swap_to(lv_obj_t* (*creator)(void), const char* name) {
    Serial.printf("[nav] Apps -> %s\n", name);
    lv_obj_t* old = g_sub_scr;
    g_sub_scr = creator();
    lv_scr_load(g_sub_scr);
    if (old) lv_obj_del_async(old);
}

void apps_open_pomodoro(void)  { apps_swap_to(screen_pomodoro_create,  "Pomodoro");  }
void apps_open_stopwatch(void) { apps_swap_to(screen_stopwatch_create, "Stopwatch"); }
void apps_open_ping(void)      { apps_swap_to(screen_ping_create,      "Ping");      }
void apps_open_dnd(void)       { apps_swap_to(screen_dnd_create,       "DND");       }
void apps_open_soundboard(void){ apps_swap_to(screen_soundboard_create,"Soundboard");}
void apps_open_holotape(void)  { apps_swap_to(screen_holotape_create,  "Holotape");  }
void apps_open_dice(void)      { apps_swap_to(screen_dice_create,      "Dice");      }
void apps_open_snake(void)     { apps_swap_to(screen_snake_create,     "Snake");     }
void apps_open_radstats(void)  { apps_swap_to(screen_radstats_create,  "Radstats");  }
void apps_open_presence(void)  { apps_swap_to(screen_presence_create,  "Presence");  }
void apps_open_pager(void)     { apps_swap_to(screen_pager_create,     "Pager");     }
void apps_open_quests(void)    { apps_swap_to(screen_quests_create,    "Quests");    }
void apps_open_blackjack(void) { apps_swap_to(screen_blackjack_menu_create, "Blackjack"); }

// Called by app modules (e.g. blackjack) that swap between their own
// internal screens without going through apps_swap_to(). Keeps g_sub_scr
// pointing at the currently-live screen so apps_open_hub() won't try to
// delete a dangling pointer on BACK.
void apps_swap_sub_scr(lv_obj_t* new_scr) {
    lv_obj_t* old = g_sub_scr;
    g_sub_scr = new_scr;
    lv_scr_load(new_scr);
    if (old && old != new_scr) lv_obj_del_async(old);
}

// ── Touch calibration ────────────────────────────────────────
void touch_load_cal(void) {
    Preferences prefs;
    if (prefs.begin("touch", true)) {
        TOUCH_MIN_X = prefs.getInt("x_min", 200);
        TOUCH_MAX_X = prefs.getInt("x_max", 3800);
        TOUCH_MIN_Y = prefs.getInt("y_min", 200);
        TOUCH_MAX_Y = prefs.getInt("y_max", 3800);
        prefs.end();
        Serial.printf("[touch] cal loaded: X %d..%d  Y %d..%d\n",
                      TOUCH_MIN_X, TOUCH_MAX_X, TOUCH_MIN_Y, TOUCH_MAX_Y);
    }
}

static void touch_save_cal(void) {
    Preferences prefs;
    if (prefs.begin("touch", false)) {
        prefs.putInt("x_min", TOUCH_MIN_X);
        prefs.putInt("x_max", TOUCH_MAX_X);
        prefs.putInt("y_min", TOUCH_MIN_Y);
        prefs.putInt("y_max", TOUCH_MAX_Y);
        prefs.end();
        Serial.printf("[touch] cal saved: X %d..%d  Y %d..%d\n",
                      TOUCH_MIN_X, TOUCH_MAX_X, TOUCH_MIN_Y, TOUCH_MAX_Y);
    }
}

// Calibration state
static int   s_cal_step = 0;          // 0-3 = corners, 4 = done
static int   s_cal_raw[4][2] = {};    // raw {x,y} per corner
static bool  s_cal_waiting = false;   // waiting for touch release before next
static lv_obj_t* s_cal_cross = NULL;
static lv_obj_t* s_cal_label = NULL;

// Crosshair target positions (screen pixels)
static const int CAL_PTS[4][2] = {
    { 20,  20},   // top-left
    {300,  20},   // top-right
    { 20, 220},   // bottom-left
    {300, 220},   // bottom-right
};
static const char* CAL_NAMES[4] = {
    "TAP TOP-LEFT +",
    "TAP TOP-RIGHT +",
    "TAP BOTTOM-LEFT +",
    "TAP BOTTOM-RIGHT +",
};

static void cal_update(void);

static void cal_timer_cb(lv_timer_t* t) {
    (void)t;
    cal_update();
}

static void cal_update(void) {
    if (s_cal_step >= 4) return;

    // Position crosshair
    if (s_cal_cross) {
        lv_obj_set_pos(s_cal_cross,
                       CAL_PTS[s_cal_step][0] - 6,
                       CAL_PTS[s_cal_step][1] - 4);
    }
    if (s_cal_label) {
        lv_label_set_text(s_cal_label, CAL_NAMES[s_cal_step]);
    }

    // Wait for touch release first, then capture next touch
    if (s_cal_waiting) {
        if (!g_touch_pressed) s_cal_waiting = false;
        return;
    }

    if (g_touch_pressed) {
        s_cal_raw[s_cal_step][0] = g_touch_raw_x;
        s_cal_raw[s_cal_step][1] = g_touch_raw_y;
        Serial.printf("[touch] cal pt %d: raw(%d, %d) target(%d, %d)\n",
                      s_cal_step,
                      g_touch_raw_x, g_touch_raw_y,
                      CAL_PTS[s_cal_step][0], CAL_PTS[s_cal_step][1]);
        sfx_tone(800, 40);
        s_cal_step++;
        s_cal_waiting = true;

        if (s_cal_step >= 4) {
            // Compute calibration from 4 points
            int avg_lx = (s_cal_raw[0][0] + s_cal_raw[2][0]) / 2;  // left x
            int avg_rx = (s_cal_raw[1][0] + s_cal_raw[3][0]) / 2;  // right x
            int avg_ty = (s_cal_raw[0][1] + s_cal_raw[1][1]) / 2;  // top y
            int avg_by = (s_cal_raw[2][1] + s_cal_raw[3][1]) / 2;  // bottom y

            // Extrapolate from inner points to full screen edges
            float sx = (float)(avg_rx - avg_lx) / (300.0f - 20.0f);
            float sy = (float)(avg_by - avg_ty) / (220.0f - 20.0f);

            TOUCH_MIN_X = (int)(avg_lx - 20.0f * sx);
            TOUCH_MAX_X = (int)(avg_rx + 19.0f * sx);
            TOUCH_MIN_Y = (int)(avg_ty - 20.0f * sy);
            TOUCH_MAX_Y = (int)(avg_by + 19.0f * sy);

            touch_save_cal();

            if (s_cal_label) {
                char buf[64];
                snprintf(buf, sizeof(buf), "DONE! X:%d-%d Y:%d-%d",
                         TOUCH_MIN_X, TOUCH_MAX_X, TOUCH_MIN_Y, TOUCH_MAX_Y);
                lv_label_set_text(s_cal_label, buf);
            }
            if (s_cal_cross) lv_obj_add_flag(s_cal_cross, LV_OBJ_FLAG_HIDDEN);

            // Auto-return after 2 seconds
            lv_timer_create([](lv_timer_t* t){
                lv_timer_del(t);
                apps_open_hub();
            }, 2000, NULL);
        }
    }
}

extern "C" lv_obj_t* screen_calibrate_create(void) {
    s_cal_step = 0;
    s_cal_waiting = false;

    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_cal_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_cal_label, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_cal_label, CLR_GREEN, 0);
    lv_obj_align(s_cal_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_cal_label, CAL_NAMES[0]);

    // Crosshair marker
    s_cal_cross = lv_label_create(scr);
    lv_obj_set_style_text_font(s_cal_cross, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_cal_cross, CLR_GOLD, 0);
    lv_label_set_text(s_cal_cross, "+");
    lv_obj_set_pos(s_cal_cross, CAL_PTS[0][0] - 6, CAL_PTS[0][1] - 4);

    // Poll timer
    lv_timer_t* tm = lv_timer_create(cal_timer_cb, 50, NULL);
    lv_obj_set_user_data(scr, tm);
    lv_obj_add_event_cb(scr, [](lv_event_t* e){
        s_cal_cross = NULL;
        s_cal_label = NULL;
        lv_timer_t* t = (lv_timer_t*)lv_obj_get_user_data(lv_event_get_target(e));
        if (t) lv_timer_del(t);
    }, LV_EVENT_DELETE, NULL);

    return scr;
}

void apps_open_calibrate(void) { apps_swap_to(screen_calibrate_create, "Calibrate"); }

// Open Settings/Bluetooth from the STAT gear menu (return to tabs on BACK)
void tabs_open_settings(void) {
    Serial.println("[nav] Tabs -> Settings");
    g_sub_return = RET_TABS;
    teardown_tabs();
    lv_obj_t* blank = g_login_scr;
    g_login_scr = NULL;
    g_sub_scr = screen_settings_create();
    lv_scr_load(g_sub_scr);
    lv_obj_del_async(blank);
}

void tabs_open_bluetooth(void) {
    Serial.println("[nav] Tabs -> Bluetooth");
    g_sub_return = RET_TABS;
    teardown_tabs();
    lv_obj_t* blank = g_login_scr;
    g_login_scr = NULL;
    g_sub_scr = screen_bluetooth_create();
    lv_scr_load(g_sub_scr);
    lv_obj_del_async(blank);
}

// Called by generic sub-screens (BT, Settings) via their BACK button.
void nav_back_from_sub(void) {
    Serial.printf("[nav] <- sub screen (return=%s)\n",
                  g_sub_return == RET_TABS ? "tabs" : "menu");
    lv_obj_t* old = g_sub_scr;
    g_sub_scr = NULL;
    if (g_sub_return == RET_TABS) {
        build_tab_bar();
        g_tabs_built = true;
        switch_tab(0);
        if (old) lv_obj_del_async(old);
    } else {
        g_menu_scr = screen_mainmenu_create();
        lv_scr_load(g_menu_scr);
        if (old) lv_obj_del_async(old);
    }
}

static unsigned long last_tap_ms = 0;

// Read touch ONCE per loop — share result with both tab handler and LVGL
static bool g_was_pressed = false;

void poll_touch(void) {
    bool pressed = false;
#if defined(BOARD_FREENOVE35)
    // Capacitive touch: already in 320x240 UI coordinates, no calibration needed
    int tx = 0, ty = 0;
    if (fn35_touch_read(&tx, &ty)) {
        g_touch_raw_x = tx;
        g_touch_raw_y = ty;
        g_touch_x = tx;
        g_touch_y = ty;
        pressed = true;
    }
#else
    if (touch.tirqTouched() && touch.touched()) {
        TS_Point p = touch.getPoint();
        g_touch_raw_x = p.x;
        g_touch_raw_y = p.y;
        g_touch_x = constrain(map(p.x, TOUCH_MIN_X, TOUCH_MAX_X, 0, SCREEN_W - 1), 0, SCREEN_W - 1);
        g_touch_y = constrain(map(p.y, TOUCH_MIN_Y, TOUCH_MAX_Y, 0, SCREEN_H - 1), 0, SCREEN_H - 1);
        pressed = true;
    }
#endif
    if (pressed) {
        g_touch_pressed = true;
        last_touch_ms = millis();
        if (bl_dimmed) {
            brightness_apply(BL_LEVEL_FULL);
            bl_dimmed = false;
        }
        // Tap click sound — only on initial touch-down
        if (!g_was_pressed) {
            sfx_play(SFX_CLICK);
        }
        g_was_pressed = true;
    } else {
        g_touch_pressed = false;
        g_was_pressed = false;
    }
}

void check_auto_dim(void) {
    // Re-evaluate the night schedule every loop; if we crossed into
    // night the brightness module will have capped PWM already.
    brightness_poll();

    if (bl_dimmed) return;
    if (millis() - last_touch_ms > BL_DIM_MS) {
        brightness_apply(BL_LEVEL_IDLE);
        bl_dimmed = true;
    }
}

// ── Deep sleep (charging mode) ──
static unsigned long sleep_btn_down = 0;
static bool          sleep_btn_held = false;

void enter_deep_sleep(void) {
    Serial.println("[sleep] Entering deep sleep...");

    // Show sleep message
    lv_obj_t* overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* msg = lv_label_create(overlay);
    lv_label_set_text(msg, "SLEEP MODE\nPRESS BOOT TO WAKE");
    lv_obj_set_style_text_font(msg, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(msg, CLR_GREEN, 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);

    // Render the sleep message
    lv_timer_handler();
    delay(1500);

    // Turn off backlight
    set_backlight(0);

    // Disconnect WiFi cleanly
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    // Configure wake on BOOT button (GPIO 0, active LOW)
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);

    // Enter deep sleep — draws ~10μA
    esp_deep_sleep_start();
}

void check_sleep_button(void) {
    if (digitalRead(SLEEP_BTN) == LOW) {
        // Button pressed (active LOW)
        if (!sleep_btn_held) {
            sleep_btn_down = millis();
            sleep_btn_held = true;
        }
        // Require 1.5s hold to avoid accidental sleeps
        if (millis() - sleep_btn_down > 1500) {
            enter_deep_sleep();
        }
    } else {
        sleep_btn_held = false;
    }
}

void check_tab_touch(void) {
    if (!g_tabs_built || !g_touch_pressed) return;
    unsigned long now = millis();
    if (now - last_tap_ms < 350) return;

    if (g_touch_y <= TAB_H + 6) {  // +6px margin for resistive touch
        last_tap_ms = now;
        int tab = constrain(g_touch_x / (SCREEN_W / 5), 0, 4);
        Serial.printf("[tab] -> %s\n", TAB_NAMES[tab]);
        switch_tab(tab);
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("\n  " BRAND_TITLE " -- BOOT");

    // Backlight PWM setup
    ledcSetup(BL_PWM_CHANNEL, BL_PWM_FREQ, BL_PWM_BITS);
    ledcAttachPin(LCD_BL_PIN, BL_PWM_CHANNEL);
    ledcWrite(BL_PWM_CHANNEL, BL_BRIGHT);
    bl_current = BL_BRIGHT;
    last_touch_ms = millis();

    // Sleep button setup
    pinMode(SLEEP_BTN, INPUT_PULLUP);

    // Battery ADC setup
    pinMode(BATT_ADC_PIN, INPUT);
    analogSetAttenuation(ADC_11db);  // 0-3.3V range

    // Sound effects system
    sfx_init();
    clock_init();
    brightness_init();
    weather_init();
    pomo_init();
    presence_init();
    pager_init();
    quests_init();
    user_name_load();   // restore custom display name (if any) from NVS
    anchor_load();      // restore anchor-mode (fixed map position) from NVS
    blackjack_init();
    touch_load_cal();

#if defined(BOARD_FREENOVE35)
    // Panel + touch. Orientation is FN35_ROTATION in platformio.ini.
    fn35_init();
#else
    tft.init();
    tft.fillScreen(TFT_BLACK);
    tft.setRotation(1);  // flipped 180° from original (was 3)
    tft.fillScreen(TFT_BLACK);

    touchSPI.begin(25, 39, 32, TOUCH_CS);
    touch.begin(touchSPI);
    touch.setRotation(1);  // match display rotation
#endif

    lv_init();

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, SCREEN_W * BUF_LINES);
    disp_drv.hor_res  = SCREEN_W;
    disp_drv.ver_res  = SCREEN_H;
    disp_drv.flush_cb = lv_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lv_touch_cb;
    lv_indev_drv_register(&indev_drv);

#if defined(BOARD_FREENOVE35) && FN35_TOUCH_DEBUG
    tdbg_create();
#endif

    net_init_deferred();  // init WS/ESP-NOW subsystems but skip WiFi.begin

    // Always start the WiFi driver in pure STA mode. ESP-NOW requires the
    // WiFi driver to be running even when not connected to any AP — if we
    // skip WiFi.mode() entirely, the first net_poll() crashes inside
    // esp_now_init(). AP_STA mode used to be set here but it causes ESP-NOW
    // broadcast to go out on the AP's phantom channel, so two devices on
    // the same router can't actually see each other. Pure STA fixes that.
    WiFi.mode(WIFI_STA);

    // Kick off the gateway election. This replaces the old unconditional
    // WiFi.begin(): we now listen for ~6s of MSG_GATEWAY_ADV broadcasts
    // first, and only bring up our own uplink if nobody else in the
    // room already has one. See gateway_election_init/poll above for
    // the state machine.
    gateway_election_init();

    // Always boot into the main menu
    g_menu_scr = screen_mainmenu_create();
    lv_scr_load(g_menu_scr);

    Serial.printf("  Boot complete heap=%u\n", ESP.getFreeHeap());
    sfx_play(SFX_BOOT);
}

// ── USB-serial screenshot (for docs) ────────────────────────────────
// The CYD panel can't be read back (readRect returns black) and there's
// no RAM for a full-screen framebuffer. Instead we force LVGL to redraw
// the whole screen and stream each flushed strip over USB as base64
// RGB565 rows tagged with their (x,y). tools/snap.py reassembles a PNG.
// Trigger by sending a line containing "SNAP" on Serial at 115200.
#include <mbedtls/base64.h>

// Emits one "SCRP x y w <base64-row>" line per pixel row of a flushed
// region. Called from lv_flush_cb while g_screenshot_capturing is set.
static void screenshot_dump_region(const lv_area_t* area, lv_color_t* color_p) {
    int x = area->x1;
    int y = area->y1;
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    char b64[920];   // base64(320*2=640 bytes) = 856 chars + nul
    for (int row = 0; row < h; row++) {
        const uint8_t* src = (const uint8_t*)(color_p + (size_t)row * w);
        size_t outLen = 0;
        if (mbedtls_base64_encode((unsigned char*)b64, sizeof(b64), &outLen,
                                  src, (size_t)w * 2) != 0) {
            return;
        }
        b64[outLen] = 0;
        Serial.printf("SCRP %d %d %d ", x, y + row, w);
        Serial.println(b64);
    }
}

static void cmd_screenshot(void) {
    Serial.printf("SCR-BEGIN %d %d %d\n", 320, 240, 16);
    g_screenshot_capturing = true;
    // Redraw everything: the active screen plus the top layer (tab bar,
    // modals, battery). lv_refr_now flushes synchronously, firing
    // lv_flush_cb (and our dump) for every tile before it returns.
    lv_obj_invalidate(lv_scr_act());
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(lv_disp_get_default());
    g_screenshot_capturing = false;
    Serial.println("SCR-END");
}

// Serial-driven navigation (for docs/automation). Maps short commands to
// the same nav functions the touch UI uses, so the screenshot tool can
// drive the device to any screen and snap it without a human tapping.
// Hooks for screens behind internal overlays:
extern void blackjack_open_1p_demo(void);
extern void blackjack_open_host_lobby(void);
extern void screen_map_force_radar(void);
extern void screen_login_demo_name_entry(void);
extern int  g_settings_open_panel;   // 3 == PANEL_ANCHOR

static void nav_serial_command(const char* c) {
    Serial.printf("[nav] serial cmd: %s\n", c);
    // --- apps / sub-screen mode ---
    if      (!strcmp(c, "apps"))        menu_nav_apps();
    else if (!strcmp(c, "hub"))         apps_open_hub();
    else if (!strcmp(c, "blackjackmenu")) apps_swap_sub_scr(screen_blackjack_menu_create());
    else if (!strcmp(c, "blackjack1p")) blackjack_open_1p_demo();
    else if (!strcmp(c, "blackjackhost")) blackjack_open_host_lobby();
    else if (!strcmp(c, "pomodoro"))    apps_open_pomodoro();
    else if (!strcmp(c, "stopwatch"))   apps_open_stopwatch();
    else if (!strcmp(c, "ping"))        apps_open_ping();
    else if (!strcmp(c, "status"))      apps_open_presence();
    else if (!strcmp(c, "soundboard"))  apps_open_soundboard();
    else if (!strcmp(c, "holotape"))    apps_open_holotape();
    else if (!strcmp(c, "dice"))        apps_open_dice();
    else if (!strcmp(c, "snake"))       apps_open_snake();
    else if (!strcmp(c, "radstats"))    apps_open_radstats();
    else if (!strcmp(c, "pager"))       apps_open_pager();
    else if (!strcmp(c, "quests"))      apps_open_quests();
    else if (!strcmp(c, "settings"))    apps_swap_sub_scr(screen_settings_create());
    else if (!strcmp(c, "anchor"))    { g_settings_open_panel = 3; apps_swap_sub_scr(screen_settings_create()); }
    else if (!strcmp(c, "clock"))       apps_swap_sub_scr(screen_clock_create());
    else if (!strcmp(c, "menu"))        apps_nav_back();
    // --- login + tabbed dweller UI ---
    else if (!strcmp(c, "login"))       menu_nav_login();
    else if (!strcmp(c, "nameentry"))   screen_login_demo_name_entry();
    else if (!strcmp(c, "autologin"))   on_login_complete(0);
    else if (!strcmp(c, "stat"))        switch_tab(0);
    else if (!strcmp(c, "inv"))         switch_tab(1);
    else if (!strcmp(c, "data"))        switch_tab(2);
    else if (!strcmp(c, "map"))         switch_tab(3);
    else if (!strcmp(c, "radio"))       switch_tab(4);
    else if (!strcmp(c, "radar"))     { switch_tab(3); screen_map_force_radar(); }
    // --- misc ---
    else if (!strcmp(c, "reset"))       ESP.restart();
    else Serial.printf("[nav] unknown cmd: %s\n", c);
}

static void screenshot_serial_poll(void) {
    static char    line[48];
    static uint8_t n = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            line[n] = 0;
            if (n) {
                if (strstr(line, "SNAP") || strstr(line, "screenshot"))
                    cmd_screenshot();
                else if (strncmp(line, "NAV ", 4) == 0)
                    nav_serial_command(line + 4);
            }
            n = 0;
        } else if (n < sizeof(line) - 1) {
            line[n++] = c;
        } else {
            n = 0;   // overflow — drop the line
        }
    }
}

void loop() {
    screenshot_serial_poll();
    poll_touch();
    check_tab_touch();
    check_auto_dim();
    check_sleep_button();
    gateway_election_poll();
    net_poll();
    clock_poll();
    pomo_tick();
    presence_poll();
    game_poll();
    sfx_tick();
    lv_timer_handler();
    delay(5);
}