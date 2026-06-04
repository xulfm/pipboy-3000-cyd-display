// ============================================================
// Vault 89 PDI-Boy -- Grab-bag of smaller app screens
//   - DND toggle
//   - Presence selector (ACTIVE / AWAY / BUSY / DND)
//   - Pager inbox
//   - DnD dice roller
//   - Holotape messenger (random Vault-Tec quotes)
//   - Radiation stats (uptime / heap / rssi as S.P.E.C.I.A.L.)
//   - Fallout soundboard
//   - Shared quest board
// Kept in one file so the project isn't drowning in tiny
// screen_*.cpp files. Each screen is a standalone create()
// function called from main.cpp's app nav wrappers.
// ============================================================
#include <Arduino.h>
#include <lvgl.h>
#include <WiFi.h>
#include <string.h>
#include "pipboy_data.h"
#include "apps.h"
#include "sound_fx.h"
#include "net_state.h"

extern void apps_open_hub(void);
extern int  read_batt_pct(void);

// ── Shared helpers ──────────────────────────────────────────────────────────

static lv_obj_t* mk_screen_with_header(const char* title) {
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, CLR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, 320, 22);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, CLR_HEADER, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(hdr, CLR_GREEN, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* ht = lv_label_create(hdr);
    lv_label_set_text(ht, title);
    lv_obj_set_style_text_font(ht, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(ht, CLR_GREEN, 0);
    lv_obj_align(ht, LV_ALIGN_CENTER, 0, 0);
    return scr;
}

static void back_to_hub_cb(lv_event_t* e) {
    (void)e;
    apps_open_hub();
}

static lv_obj_t* mk_btn(lv_obj_t* par, const char* txt, int x, int y, int w, int h,
                        lv_event_cb_t cb, void* ud = NULL) {
    lv_obj_t* b = lv_obj_create(par);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, CLR_HEADER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, CLR_GREEN, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 2, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(l, CLR_GREEN, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
    return b;
}

static lv_obj_t* mk_back_btn(lv_obj_t* scr) {
    return mk_btn(scr, "< BACK", 6, 208, 80, 26, back_to_hub_cb);
}

// ── DND + Presence selector (shared screen) ─────────────────────────────────
// Both the DND and MESH tiles land here — DND just starts with the toggle
// highlighted.

static lv_obj_t* s_pres_status = NULL;

static void pres_set_cb(lv_event_t* e) {
    int m = (int)(intptr_t)lv_event_get_user_data(e);
    presence_set((PresenceMode)m);
    if (s_pres_status) {
        char buf[32];
        snprintf(buf, sizeof(buf), "STATUS: %s",
                 presence_label(g_presence.mode));
        lv_label_set_text(s_pres_status, buf);
        lv_color_t c = CLR_GREEN;
        if (g_presence.mode == PRESENCE_DND)    c = lv_color_hex(0xCC0000);
        else if (g_presence.mode == PRESENCE_BUSY) c = CLR_GOLD;
        else if (g_presence.mode == PRESENCE_AWAY) c = CLR_DIM;
        lv_obj_set_style_text_color(s_pres_status, c, 0);
    }
    sfx_play(SFX_CLICK);
}

lv_obj_t* screen_presence_create(void) {
    lv_obj_t* scr = mk_screen_with_header("** PRESENCE / DND **");

    lv_obj_t* info = lv_label_create(scr);
    lv_label_set_text(info,
        "Broadcasts your state to\n"
        "other " PEER_PLURAL ". DND + BUSY\n"
        "also suppress pager beeps.");
    lv_obj_set_style_text_font(info, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(info, CLR_DIM, 0);
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 28);

    s_pres_status = lv_label_create(scr);
    char buf[32];
    snprintf(buf, sizeof(buf), "STATUS: %s", presence_label(g_presence.mode));
    lv_label_set_text(s_pres_status, buf);
    lv_obj_set_style_text_font(s_pres_status, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_pres_status, CLR_GREEN, 0);
    lv_obj_align(s_pres_status, LV_ALIGN_TOP_MID, 0, 72);

    // Selector buttons
    mk_btn(scr, "ACTIVE",  14, 100, 72, 30, pres_set_cb, (void*)(intptr_t)PRESENCE_ACTIVE);
    mk_btn(scr, "AWAY",    96, 100, 72, 30, pres_set_cb, (void*)(intptr_t)PRESENCE_AWAY);
    mk_btn(scr, "BUSY",   178, 100, 60, 30, pres_set_cb, (void*)(intptr_t)PRESENCE_BUSY);
    mk_btn(scr, "[ DND ]",248, 100, 66, 30, pres_set_cb, (void*)(intptr_t)PRESENCE_DND);

    mk_back_btn(scr);

    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        (void)e;
        s_pres_status = NULL;
    }, LV_EVENT_DELETE, NULL);

    return scr;
}

// The DND tile opens the presence screen but quickly toggles DND if already
// selected. Simpler: reuse the presence screen.
lv_obj_t* screen_dnd_create(void) {
    // Auto-flip DND when entering this screen for the first time each tap
    if (g_presence.mode == PRESENCE_DND) presence_set(PRESENCE_ACTIVE);
    else                                 presence_set(PRESENCE_DND);
    return screen_presence_create();
}

// ── Pager inbox ─────────────────────────────────────────────────────────────

lv_obj_t* screen_pager_create(void) {
    lv_obj_t* scr = mk_screen_with_header("** INCOMING PAGER **");

    if (g_pager_count == 0) {
        lv_obj_t* empty = lv_label_create(scr);
        lv_label_set_text(empty, "NO PAGES\n\n(server or mesh\n pages appear here)");
        lv_obj_set_style_text_font(empty, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(empty, CLR_DIM, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, -10);
    } else {
        // List newest first (up to 5 visible)
        for (int i = 0; i < g_pager_count && i < 5; i++) {
            lv_obj_t* row = lv_obj_create(scr);
            lv_obj_set_size(row, 300, 28);
            lv_obj_set_pos(row, 10, 30 + i * 32);
            lv_obj_set_style_bg_color(row, CLR_HEADER, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(row,
                g_pager[i].seen ? CLR_DARK : CLR_GREEN, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_radius(row, 2, 0);
            lv_obj_set_style_pad_all(row, 2, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t* from = lv_label_create(row);
            char fbuf[32];
            snprintf(fbuf, sizeof(fbuf), "%s:", g_pager[i].from);
            lv_label_set_text(from, fbuf);
            lv_obj_set_style_text_font(from, &lv_font_unscii_8, 0);
            lv_obj_set_style_text_color(from, CLR_GOLD, 0);
            lv_obj_align(from, LV_ALIGN_TOP_LEFT, 0, 0);

            lv_obj_t* txt = lv_label_create(row);
            lv_label_set_text(txt, g_pager[i].text);
            lv_obj_set_style_text_font(txt, &lv_font_unscii_8, 0);
            lv_obj_set_style_text_color(txt, CLR_GREEN, 0);
            lv_obj_align(txt, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        }
    }

    pager_mark_all_read();
    mk_back_btn(scr);
    return scr;
}

// ── Dice roller ─────────────────────────────────────────────────────────────
//
// Classic DnD die set: d4, d6, d8, d10, d12, d20, d100
// Tap a die to roll 1x, plus a "ROLL AGAIN" button.

static const int s_die_sides[] = {4, 6, 8, 10, 12, 20, 100};
static const int s_die_count   = sizeof(s_die_sides) / sizeof(s_die_sides[0]);
static int s_last_die    = 20;
static int s_last_result = 0;
static lv_obj_t* s_die_result_lbl = NULL;
static lv_obj_t* s_die_info_lbl   = NULL;

static void die_roll_and_show(int sides) {
    s_last_die    = sides;
    s_last_result = (int)(esp_random() % sides) + 1;
    if (s_die_result_lbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", s_last_result);
        lv_label_set_text(s_die_result_lbl, buf);
    }
    if (s_die_info_lbl) {
        char buf[24];
        snprintf(buf, sizeof(buf), "1d%d", sides);
        lv_label_set_text(s_die_info_lbl, buf);
    }
    sfx_tone(1200, 40);
}

static void die_click_cb(lv_event_t* e) {
    int sides = (int)(intptr_t)lv_event_get_user_data(e);
    die_roll_and_show(sides);
}
static void die_reroll_cb(lv_event_t* e) {
    (void)e;
    die_roll_and_show(s_last_die);
}

lv_obj_t* screen_dice_create(void) {
    lv_obj_t* scr = mk_screen_with_header("** DICE ROLLER **");

    // Big result display
    s_die_result_lbl = lv_label_create(scr);
    lv_label_set_text(s_die_result_lbl, "--");
    lv_obj_set_style_text_font(s_die_result_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_die_result_lbl, CLR_GREEN, 0);
    lv_obj_align(s_die_result_lbl, LV_ALIGN_TOP_MID, 0, 26);

    s_die_info_lbl = lv_label_create(scr);
    lv_label_set_text(s_die_info_lbl, "TAP A DIE");
    lv_obj_set_style_text_font(s_die_info_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_die_info_lbl, CLR_GOLD, 0);
    lv_obj_align(s_die_info_lbl, LV_ALIGN_TOP_MID, 0, 90);

    // Die selector row
    const int DB_W = 38;
    const int DB_H = 26;
    const int DB_GAP = 4;
    int total = s_die_count * DB_W + (s_die_count - 1) * DB_GAP;
    int x0 = (320 - total) / 2;
    for (int i = 0; i < s_die_count; i++) {
        char label[8];
        snprintf(label, sizeof(label), "d%d", s_die_sides[i]);
        mk_btn(scr, label, x0 + i * (DB_W + DB_GAP), 110, DB_W, DB_H,
               die_click_cb, (void*)(intptr_t)s_die_sides[i]);
    }

    mk_btn(scr, "ROLL AGAIN", 110, 148, 100, 28, die_reroll_cb);

    mk_back_btn(scr);
    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        (void)e;
        s_die_result_lbl = s_die_info_lbl = NULL;
    }, LV_EVENT_DELETE, NULL);
    return scr;
}

// ── Holotape Messenger (random Vault-Tec quotes) ────────────────────────────

static const char* HOLOTAPE_QUOTES[] = {
    "WAR. WAR NEVER CHANGES.",
    "PREPARE FOR THE FUTURE.",
    "VAULT-TEC APPROVED MORALE.",
    "CITIZENS: REMAIN CALM.",
    "THE OVERSEER IS WATCHING.",
    "HYDRATE OR DIE-DRATE.",
    "RADROACH SIGHTED. REPORT IT.",
    "STAY ON THE DESIGNATED PATH.",
    "HAVE A SUPER-DUPER DAY!",
    "DINNER IS MEAT FLAVOR.",
    "NUKA-COLA: 100% PLEASE!",
    "REMEMBER: YOU'RE S.P.E.C.I.A.L!",
    "CONGRATULATIONS ON YOUR\nFIRST 200 YEARS UNDERGROUND.",
    "MENTATS. REAL MENTATS. NOW.",
    "WORK KEEPS YOU FROM\nNOTICING THINGS.",
    "TODAY'S FORECAST:\nSLIGHTLY RADIOACTIVE.",
    "THE GROUND IS LAVA.\nAND ALSO LITERALLY LAVA.",
    "PLEASE DO NOT FEED\nTHE DEATHCLAWS.",
    "PIP-BOY LOVES YOU.",
    ORG_NAME ": BUILDING A\nBRIGHTER FUTURE. UNDERGROUND.",
};
static const int HOLOTAPE_COUNT =
    sizeof(HOLOTAPE_QUOTES) / sizeof(HOLOTAPE_QUOTES[0]);

static lv_obj_t* s_holo_text = NULL;

static void holo_next_cb(lv_event_t* e) {
    (void)e;
    if (!s_holo_text) return;
    const char* q = HOLOTAPE_QUOTES[esp_random() % HOLOTAPE_COUNT];
    lv_label_set_text(s_holo_text, q);
    sfx_tone(800, 30);
}

lv_obj_t* screen_holotape_create(void) {
    lv_obj_t* scr = mk_screen_with_header("** HOLOTAPE MESSENGER **");

    s_holo_text = lv_label_create(scr);
    lv_label_set_text(s_holo_text, HOLOTAPE_QUOTES[0]);
    lv_obj_set_style_text_font(s_holo_text, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_holo_text, CLR_GREEN, 0);
    lv_obj_set_width(s_holo_text, 300);
    lv_label_set_long_mode(s_holo_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_holo_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_holo_text, LV_ALIGN_CENTER, 0, -10);

    // Play an initial random one
    lv_label_set_text(s_holo_text, HOLOTAPE_QUOTES[esp_random() % HOLOTAPE_COUNT]);

    mk_btn(scr, "NEXT TAPE", 108, 170, 104, 30, holo_next_cb);
    mk_back_btn(scr);

    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        (void)e;
        s_holo_text = NULL;
    }, LV_EVENT_DELETE, NULL);
    return scr;
}

// ── Radiation Stats (uptime / heap / rssi as S.P.E.C.I.A.L.) ────────────────

static lv_obj_t*   s_rs_lines[7] = { NULL };
static lv_timer_t* s_rs_tmr      = NULL;

static int clamp10(int v) { if (v < 1) return 1; if (v > 10) return 10; return v; }

static void rs_refresh_cb(lv_timer_t* t) {
    (void)t;
    uint32_t uptime_s = millis() / 1000;
    uint32_t heap     = ESP.getFreeHeap();
    uint32_t min_heap = ESP.getMinFreeHeap();
    int rssi          = g_net.wifi_connected ? WiFi.RSSI() : -100;
    int batt          = read_batt_pct();
    if (batt < 0) batt = 50;

    // Map metrics into S.P.E.C.I.A.L. stats (1..10). Pure flavor.
    int S = clamp10((int)(uptime_s / 3600) + 3);              // hours up
    int P = clamp10(100 - abs(rssi + 100) / 5);               // RSSI
    int E = clamp10(batt / 10);                               // battery
    int C = clamp10(g_peer_count * 2 + 2);                    // peers
    int I = clamp10((int)(heap / 10000));                     // free heap
    int A = clamp10((int)(min_heap / 8000));                  // min heap
    int L = clamp10((int)(esp_random() % 10) + 1);            // pure luck

    const char* names[7] = {
        "S-STRENGTH", "P-PERCEPTION", "E-ENDURANCE",
        "C-CHARISMA", "I-INTELLIGENCE", "A-AGILITY", "L-LUCK"
    };
    int vals[7] = { S, P, E, C, I, A, L };

    for (int i = 0; i < 7; i++) {
        if (!s_rs_lines[i]) continue;
        char bar[12];
        int v = vals[i];
        for (int b = 0; b < 10; b++) bar[b] = (b < v) ? '#' : '.';
        bar[10] = '\0';
        char line[48];
        snprintf(line, sizeof(line), "%-16s %s %2d", names[i], bar, v);
        lv_label_set_text(s_rs_lines[i], line);
    }
}

lv_obj_t* screen_radstats_create(void) {
    lv_obj_t* scr = mk_screen_with_header("** RADIATION / S.P.E.C.I.A.L. **");

    for (int i = 0; i < 7; i++) {
        s_rs_lines[i] = lv_label_create(scr);
        lv_label_set_text(s_rs_lines[i], "...");
        lv_obj_set_style_text_font(s_rs_lines[i], &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(s_rs_lines[i], CLR_GREEN, 0);
        lv_obj_set_pos(s_rs_lines[i], 14, 30 + i * 14);
    }

    // Extra diag line
    lv_obj_t* diag = lv_label_create(scr);
    char db[48];
    snprintf(db, sizeof(db), "CHIP: ESP32   HEAP: %u", ESP.getFreeHeap());
    lv_label_set_text(diag, db);
    lv_obj_set_style_text_font(diag, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(diag, CLR_DIM, 0);
    lv_obj_align(diag, LV_ALIGN_BOTTOM_MID, 0, -34);

    rs_refresh_cb(NULL);
    s_rs_tmr = lv_timer_create(rs_refresh_cb, 1000, NULL);

    mk_back_btn(scr);

    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        (void)e;
        if (s_rs_tmr) { lv_timer_del(s_rs_tmr); s_rs_tmr = NULL; }
        for (int i = 0; i < 7; i++) s_rs_lines[i] = NULL;
    }, LV_EVENT_DELETE, NULL);
    return scr;
}

// ── Fallout soundboard ──────────────────────────────────────────────────────

struct SoundEntry {
    const char* label;
    // Either use an SFX preset (fx != -1) or a custom tone sequence
    int fx;
    uint16_t f1, d1, f2, d2, f3, d3;
};

static const SoundEntry SOUNDS[] = {
    {"BOOT",     SFX_BOOT,        0,0,0,0,0,0},
    {"MSGIN",    SFX_MSG_IN,      0,0,0,0,0,0},
    {"ALERT",    SFX_VAULT_ALERT, 0,0,0,0,0,0},
    {"JOIN",     SFX_PEER_JOIN,   0,0,0,0,0,0},
    {"LEAVE",    SFX_PEER_LEAVE,  0,0,0,0,0,0},
    {"CONN",     SFX_CONNECT,     0,0,0,0,0,0},
    {"DISC",     SFX_DISCONNECT,  0,0,0,0,0,0},
    {"ERR",      SFX_ERROR,       0,0,0,0,0,0},
    // Custom tones
    {"RADMET",  -1,  900,40, 1100,40, 1300,40},
    {"PIPKEY",  -1, 1400,30,    0,30, 1400,30},
    {"VAULTD",  -1,  600,80,  800,80, 1000,80},
    {"BRHMN",   -1,  300,90,    0,60,  300,90},
};
static const int SOUND_COUNT = sizeof(SOUNDS) / sizeof(SOUNDS[0]);

static void sound_click_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= SOUND_COUNT) return;
    const SoundEntry& s = SOUNDS[idx];
    if (s.fx >= 0) {
        sfx_play((SoundFX)s.fx);
    } else {
        sfx_tone(s.f1, s.d1);
        delay(s.d1 + 5);
        if (s.f2 > 0 || s.d2 > 0) { sfx_tone(s.f2, s.d2); delay(s.d2 + 5); }
        if (s.f3 > 0 || s.d3 > 0) { sfx_tone(s.f3, s.d3); delay(s.d3 + 5); }
    }
}

lv_obj_t* screen_soundboard_create(void) {
    lv_obj_t* scr = mk_screen_with_header("** SOUNDBOARD **");

    const int COLS = 4;
    const int ROWS = 3;
    const int BW = 68;
    const int BH = 34;
    const int GX = 6;
    const int GY = 6;
    const int TW = COLS * BW + (COLS - 1) * GX;
    const int X0 = (320 - TW) / 2;
    const int Y0 = 30;
    for (int i = 0; i < SOUND_COUNT && i < COLS * ROWS; i++) {
        int c = i % COLS;
        int r = i / COLS;
        mk_btn(scr, SOUNDS[i].label,
               X0 + c * (BW + GX), Y0 + r * (BH + GY), BW, BH,
               sound_click_cb, (void*)(intptr_t)i);
    }

    mk_back_btn(scr);
    return scr;
}

// ── Shared Quest Board ──────────────────────────────────────────────────────
//
// Tap an existing quest to toggle DONE. Tap ADD to insert a canned
// placeholder quest (keyboard input is heavyweight for now).

static lv_obj_t*  s_quest_rows[QUEST_MAX] = { NULL };
static lv_obj_t*  s_quest_lbls[QUEST_MAX] = { NULL };

static const char* PLACEHOLDER_QUESTS[] = {
    "CLEAN COFFEE POT",
    "STAND UP AT 2PM",
    "GRAB LUNCH 12:15",
    "REVIEW PR BACKLOG",
    "WATER THE PLANT",
    "SYNC WITH TEAM",
    "SUBMIT TIMESHEET",
    "TAKE A WALK",
};

static void rebuild_quest_rows(lv_obj_t* scr);

static void quest_row_cb(lv_event_t* e) {
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    quests_toggle(slot);
    // Update label in place
    if (s_quest_lbls[slot]) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s %s",
                 g_quests[slot].done ? "[X]" : "[ ]",
                 g_quests[slot].text);
        lv_label_set_text(s_quest_lbls[slot], buf);
        lv_obj_set_style_text_color(s_quest_lbls[slot],
            g_quests[slot].done ? CLR_DIM : CLR_GREEN, 0);
    }
    sfx_play(SFX_CLICK);
}

static void quest_add_cb(lv_event_t* e) {
    lv_obj_t* scr = lv_event_get_current_target(e);
    if (g_quest_count >= QUEST_MAX) return;
    const char* q = PLACEHOLDER_QUESTS[esp_random() %
        (sizeof(PLACEHOLDER_QUESTS) / sizeof(PLACEHOLDER_QUESTS[0]))];
    quests_add_local(q);
    rebuild_quest_rows(scr);
    sfx_tone(900, 40);
}

static void quest_clear_cb(lv_event_t* e) {
    lv_obj_t* scr = lv_event_get_current_target(e);
    // Remove all done quests (keep pending ones)
    for (int i = g_quest_count - 1; i >= 0; i--) {
        if (g_quests[i].done) quests_remove(i);
    }
    rebuild_quest_rows(scr);
    sfx_tone(600, 60);
}

static void rebuild_quest_rows(lv_obj_t* scr) {
    // Delete any existing row widgets and rebuild from state
    for (int i = 0; i < QUEST_MAX; i++) {
        if (s_quest_rows[i]) {
            lv_obj_del(s_quest_rows[i]);
            s_quest_rows[i] = NULL;
            s_quest_lbls[i] = NULL;
        }
    }
    for (int i = 0; i < g_quest_count && i < 6; i++) {
        lv_obj_t* row = lv_obj_create(scr);
        lv_obj_set_size(row, 304, 22);
        lv_obj_set_pos(row, 8, 28 + i * 25);
        lv_obj_set_style_bg_color(row, CLR_HEADER, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, CLR_GREEN, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 2, 0);
        lv_obj_set_style_pad_all(row, 2, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, quest_row_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)i);
        lv_obj_t* lbl = lv_label_create(row);
        char buf[64];
        snprintf(buf, sizeof(buf), "%s %s",
                 g_quests[i].done ? "[X]" : "[ ]", g_quests[i].text);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(lbl,
            g_quests[i].done ? CLR_DIM : CLR_GREEN, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 2, 0);
        s_quest_rows[i] = row;
        s_quest_lbls[i] = lbl;
    }
}

lv_obj_t* screen_quests_create(void) {
    lv_obj_t* scr = mk_screen_with_header("** SHARED QUEST BOARD **");

    for (int i = 0; i < QUEST_MAX; i++) {
        s_quest_rows[i] = NULL;
        s_quest_lbls[i] = NULL;
    }
    rebuild_quest_rows(scr);

    // Add / Clear done buttons row
    mk_btn(scr, "+ ADD",   94, 186, 72, 26, quest_add_cb);
    mk_btn(scr, "- CLEAR",174, 186, 80, 26, quest_clear_cb);

    mk_back_btn(scr);
    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        (void)e;
        for (int i = 0; i < QUEST_MAX; i++) {
            s_quest_rows[i] = NULL;
            s_quest_lbls[i] = NULL;
        }
    }, LV_EVENT_DELETE, NULL);
    return scr;
}

// ── Ping monitor (ICMP-like: just resolves + HEAD request to gateway) ───────
//
// Full ICMP ping requires raw sockets which are awkward on Arduino core. As
// a desk-friendly approximation we measure TCP-connect time to 1.1.1.1:80
// and draw a small sparkline of the last 60 samples.

#include <WiFiClient.h>
static lv_obj_t*   s_ping_value   = NULL;
static lv_obj_t*   s_ping_spark   = NULL;
static lv_timer_t* s_ping_tmr     = NULL;
static int         s_ping_hist[60] = {0};
static int         s_ping_head    = 0;
static int         s_ping_count   = 0;
static uint32_t    s_ping_last_ms = 0;

// Mesh ping result — written by the callback registered with
// espnow_set_mesh_ping_cb(), read by ping_tick() on the next cycle.
static volatile int  s_mesh_ping_result  = -999;  // -999 = no result pending
static volatile bool s_mesh_ping_waiting = false;

static void on_mesh_ping_result(int ms) {
    s_mesh_ping_result = ms;
    s_mesh_ping_waiting = false;
}

static int measure_ping_ms(void) {
    // Direct TCP ping if we have WiFi
    if (g_net.wifi_connected) {
        WiFiClient c;
        c.setTimeout(2);
        uint32_t t0 = millis();
        bool ok = c.connect("1.1.1.1", 80);
        uint32_t dt = millis() - t0;
        if (ok) c.stop();
        return ok ? (int)dt : -1;
    }

    // Mesh relay: check if a previous request came back
    if (s_mesh_ping_result != -999) {
        int ms = s_mesh_ping_result;
        s_mesh_ping_result = -999;
        return ms;
    }

    // Fire a new request if we're not already waiting
    if (!s_mesh_ping_waiting) {
        s_mesh_ping_waiting = true;
        espnow_request_ping();
    }
    return -2;  // "waiting for mesh reply" (distinct from -1 = timeout)
}

static void ping_spark_event_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target(e);
    lv_draw_ctx_t* draw_ctx = lv_event_get_draw_ctx(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int W = a.x2 - a.x1;
    int H = a.y2 - a.y1;

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = CLR_GREEN;
    dsc.width = 1;

    int n = s_ping_count;
    if (n < 2) return;
    // Find max for scaling
    int maxv = 1;
    for (int i = 0; i < n; i++) {
        int idx = (s_ping_head + 60 - n + i) % 60;
        if (s_ping_hist[idx] > maxv) maxv = s_ping_hist[idx];
    }
    lv_point_t p1, p2;
    int prev_y = -1, prev_x = 0;
    for (int i = 0; i < n; i++) {
        int idx = (s_ping_head + 60 - n + i) % 60;
        int v = s_ping_hist[idx];
        if (v < 0) v = 0;
        int y = a.y2 - 2 - (v * (H - 4)) / maxv;
        int x = a.x1 + (i * W) / (n > 1 ? (n - 1) : 1);
        if (prev_y >= 0) {
            p1.x = prev_x; p1.y = prev_y;
            p2.x = x;      p2.y = y;
            lv_draw_line(draw_ctx, &dsc, &p1, &p2);
        }
        prev_x = x;
        prev_y = y;
    }
}

static void ping_tick(lv_timer_t* t) {
    (void)t;
    uint32_t now = millis();
    if (now - s_ping_last_ms < 1000) return;
    s_ping_last_ms = now;
    int ms = measure_ping_ms();
    // -2 means "waiting for mesh reply" — don't record a sample yet,
    // just show the waiting label.
    if (ms != -2) {
        s_ping_hist[s_ping_head] = ms < 0 ? 0 : ms;
        s_ping_head = (s_ping_head + 1) % 60;
        if (s_ping_count < 60) s_ping_count++;
    }
    if (s_ping_value) {
        char buf[24];
        if (ms == -2)     snprintf(buf, sizeof(buf), "MESH...");
        else if (ms < 0)  snprintf(buf, sizeof(buf), "--- ms");
        else              snprintf(buf, sizeof(buf), "%d ms", ms);
        lv_label_set_text(s_ping_value, buf);
        lv_color_t c = CLR_GREEN;
        if (ms < 0) c = lv_color_hex(0xCC0000);
        else if (ms == -2) c = CLR_GOLD;
        else if (ms > 150) c = CLR_GOLD;
        lv_obj_set_style_text_color(s_ping_value, c, 0);
    }
    if (s_ping_spark) lv_obj_invalidate(s_ping_spark);
}

lv_obj_t* screen_ping_create(void) {
    lv_obj_t* scr = mk_screen_with_header("** PING MONITOR **");

    s_ping_count = 0;
    s_ping_head  = 0;
    s_mesh_ping_result  = -999;
    s_mesh_ping_waiting = false;

    // Register mesh ping callback so we can receive relay results
    espnow_set_mesh_ping_cb(on_mesh_ping_result);

    lv_obj_t* info = lv_label_create(scr);
    const char* mode = g_net.wifi_connected
        ? "TCP 1.1.1.1:80 (WIFI)"
        : "TCP 1.1.1.1:80 (MESH)";
    lv_label_set_text(info, mode);
    lv_obj_set_style_text_font(info, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(info, CLR_DIM, 0);
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 26);

    s_ping_value = lv_label_create(scr);
    lv_label_set_text(s_ping_value, "--- ms");
    lv_obj_set_style_text_font(s_ping_value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_ping_value, CLR_GREEN, 0);
    lv_obj_align(s_ping_value, LV_ALIGN_TOP_MID, 0, 44);

    // Sparkline area
    s_ping_spark = lv_obj_create(scr);
    lv_obj_set_size(s_ping_spark, 296, 48);
    lv_obj_set_pos(s_ping_spark, 12, 112);
    lv_obj_set_style_bg_opa(s_ping_spark, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_ping_spark, CLR_GREEN, 0);
    lv_obj_set_style_border_width(s_ping_spark, 1, 0);
    lv_obj_set_style_radius(s_ping_spark, 2, 0);
    lv_obj_set_style_pad_all(s_ping_spark, 0, 0);
    lv_obj_clear_flag(s_ping_spark, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_ping_spark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ping_spark, ping_spark_event_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_ping_tmr = lv_timer_create(ping_tick, 200, NULL);

    mk_back_btn(scr);

    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        (void)e;
        if (s_ping_tmr) { lv_timer_del(s_ping_tmr); s_ping_tmr = NULL; }
        s_ping_value = s_ping_spark = NULL;
        espnow_set_mesh_ping_cb(NULL);
    }, LV_EVENT_DELETE, NULL);
    return scr;
}
