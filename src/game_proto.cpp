// ============================================================
// Vault 89 PDI-Boy -- Game Protocol Router
// Handles invites, joins, session tracking, nudges, screen
// flash, and modal UI. Individual games register a GameModule
// and receive on_state / on_action / host_tick callbacks.
// ============================================================

#include <Arduino.h>
#include <lvgl.h>
#include <string.h>
#include <esp_now.h>
#include "game_proto.h"
#include "net_state.h"
#include "net_config.h"
#include "pipboy_data.h"
#include "sound_fx.h"

// Tears down the tab bar (on lv_layer_top) if the player accepts a
// game invite from the tab view. Defined in main.cpp.
extern void nav_teardown_tabs_if_needed(void);

// ── Module registry ───────────────────────────────────────────
#define GAME_MAX_MODULES 8
static const GameModule* s_modules[GAME_MAX_MODULES] = {};
static int s_module_count = 0;

static const GameModule* find_module(uint8_t game_id) {
    for (int i = 0; i < s_module_count; i++) {
        if (s_modules[i] && s_modules[i]->game_id == game_id) return s_modules[i];
    }
    return NULL;
}

void game_register(const GameModule* m) {
    if (!m || s_module_count >= GAME_MAX_MODULES) return;
    s_modules[s_module_count++] = m;
    Serial.printf("[game] Registered %s (id=%u)\n",
                  m->display_name ? m->display_name : "?", m->game_id);
}

// ── Session state ─────────────────────────────────────────────
// Exactly one active session per device at a time. Sessions are
// keyed by the random session_id the host assigned at INVITE.
enum SessionRole : uint8_t { ROLE_NONE = 0, ROLE_HOST, ROLE_PLAYER };
static uint8_t       s_role          = ROLE_NONE;
static uint32_t      s_session_id    = 0;
static uint8_t       s_game_id       = 0;
static uint8_t       s_host_mac[6]   = {};
static uint16_t      s_last_seq_seen = 0;

// Host-only: track players who've JOINed this session so we
// know who to nudge and when to START.
static uint8_t       s_host_seat_count = 0;
static uint8_t       s_host_seats[GAME_MAX_PLAYERS][6] = {};
static char          s_host_names[GAME_MAX_PLAYERS][GAME_NAME_LEN] = {};
static unsigned long s_host_last_action_ms[GAME_MAX_PLAYERS] = {};
static unsigned long s_host_invite_started_ms = 0;
static uint8_t       s_host_min_players = 2;
static uint8_t       s_host_max_players = GAME_MAX_PLAYERS;
static uint16_t      s_host_next_seq    = 1;
static bool          s_host_started     = false;

// Cached invite for periodic re-broadcast during the invite window.
static GameInvite    s_host_invite_cache = {};
static unsigned long s_host_last_invite_tx_ms = 0;
#define INVITE_REBROADCAST_MS 3000   // re-send invite every 3s

// Cached START for periodic re-broadcast after game_host_start().
// Channel hopping means the initial 3x burst may land on the wrong channel;
// re-sending over ~3s catches peers on any dwell interval.
static GameStart     s_host_start_cache = {};
static uint8_t       s_host_start_retx_count = 0;
static unsigned long s_host_start_retx_ms = 0;
#define START_RETX_MAX         6       // up to 6 extra retransmissions
#define START_RETX_INTERVAL_MS 500     // every 500ms after initial burst

// Player side: my seat index once I receive a MSG_GAME_START.
static uint8_t       s_my_seat          = 0xFF;

// Post-session cooldown — prevent phantom invites from stale re-broadcasts
// or random ESP-NOW traffic right after a session ends.
static unsigned long s_session_end_ms    = 0;
#define SESSION_COOLDOWN_MS  3000

// ── Incoming-invite queue ─────────────────────────────────────
// on_recv() runs on the WiFi task (core 0); we stash the bytes
// and let game_poll() on the main core open the modal.
static volatile bool s_pending_invite = false;
static GameInvite    s_pending_invite_data = {};
static uint8_t       s_pending_invite_mac[6] = {};

// START and STATE frames also touch LVGL via the module's
// on_start / on_state callbacks, so they're deferred through
// game_poll() too. Latest STATE wins (we drop stale pending state
// if a newer one arrives before poll consumes it).
static volatile bool s_pending_start = false;
static GameStart     s_pending_start_data = {};
static volatile bool s_pending_state = false;
static GameState     s_pending_state_data = {};
static volatile bool s_pending_nudge = false;
static GameNudge     s_pending_nudge_data = {};
static volatile bool s_pending_end = false;
static GameEnd       s_pending_end_data = {};

// ── Invite modal + screen flash ──────────────────────────────
static lv_obj_t*  s_modal         = NULL;
static lv_obj_t*  s_modal_timer_lbl = NULL;
static lv_timer_t* s_modal_timer  = NULL;
static uint32_t   s_modal_session = 0;
static unsigned long s_modal_opened_ms = 0;
static uint8_t    s_modal_ttl_sec = 30;

static lv_obj_t*   s_flash_overlay   = NULL;
static lv_timer_t* s_flash_timer     = NULL;
static uint8_t     s_flash_remaining = 0;
static bool        s_flash_visible   = false;

// Forward decls
static void modal_close(void);
static void modal_accept_cb(lv_event_t* e);
static void modal_decline_cb(lv_event_t* e);
static void modal_tick_cb(lv_timer_t* t);
static void flash_tick_cb(lv_timer_t* t);
static void flash_start(void);
static void send_join(uint32_t session_id, const uint8_t* host_mac, uint8_t game_id);
static void fill_hdr(GameHdr& h, uint8_t type, uint16_t seq);

// ── Screen flash ──────────────────────────────────────────────
// White full-screen overlay at 40% opacity, toggled 3× at 400ms
// period. Driven entirely by an LVGL timer so it never blocks.
static void flash_start(void) {
    if (s_flash_overlay) {
        lv_obj_del(s_flash_overlay);
        s_flash_overlay = NULL;
    }
    if (s_flash_timer) {
        lv_timer_del(s_flash_timer);
        s_flash_timer = NULL;
    }
    s_flash_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_flash_overlay,
                    lv_disp_get_hor_res(NULL),
                    lv_disp_get_ver_res(NULL));
    lv_obj_set_pos(s_flash_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_flash_overlay, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_flash_overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_flash_overlay, 0, 0);
    lv_obj_clear_flag(s_flash_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_flash_overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
    // Don't absorb touch — user can still tap the modal buttons.
    lv_obj_clear_flag(s_flash_overlay, LV_OBJ_FLAG_CLICKABLE);
    s_flash_visible   = true;
    s_flash_remaining = GAME_FLASH_COUNT * 2;   // on/off pairs
    s_flash_timer = lv_timer_create(flash_tick_cb, GAME_FLASH_PERIOD_MS / 2, NULL);
}

static void flash_tick_cb(lv_timer_t* t) {
    (void)t;
    if (s_flash_remaining == 0) {
        if (s_flash_overlay) { lv_obj_del(s_flash_overlay); s_flash_overlay = NULL; }
        if (s_flash_timer)   { lv_timer_del(s_flash_timer); s_flash_timer = NULL; }
        return;
    }
    s_flash_visible = !s_flash_visible;
    if (s_flash_overlay) {
        lv_obj_set_style_bg_opa(s_flash_overlay,
                                s_flash_visible ? LV_OPA_50 : LV_OPA_TRANSP, 0);
    }
    s_flash_remaining--;
}

// ── Invite modal ──────────────────────────────────────────────
static void modal_open(const GameInvite* inv) {
    modal_close();   // drop any prior

    const GameModule* mod = find_module(inv->hdr.game_id);
    const char* game_name = mod ? mod->display_name : "UNKNOWN GAME";

    s_modal_session  = inv->hdr.session_id;
    s_modal_ttl_sec  = inv->ttl_sec ? inv->ttl_sec : 30;
    s_modal_opened_ms = millis();

    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_modal, 280, 180);
    lv_obj_center(s_modal);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_modal, CLR_GREEN, 0);
    lv_obj_set_style_border_width(s_modal, 2, 0);
    lv_obj_set_style_pad_all(s_modal, 10, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(s_modal);
    lv_label_set_text(title, "INCOMING INVITE");
    lv_obj_set_style_text_font(title, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(title, CLR_GOLD, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    char line[64];
    snprintf(line, sizeof(line), "%s\nhosted by %s",
             game_name, inv->host_name[0] ? inv->host_name : "(peer)");
    lv_obj_t* info = lv_label_create(s_modal);
    lv_label_set_text(info, line);
    lv_obj_set_style_text_font(info, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(info, CLR_GREEN, 0);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 22);

    s_modal_timer_lbl = lv_label_create(s_modal);
    lv_obj_set_style_text_font(s_modal_timer_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_modal_timer_lbl, CLR_GOLDD, 0);
    lv_obj_align(s_modal_timer_lbl, LV_ALIGN_TOP_MID, 0, 68);
    lv_label_set_text_fmt(s_modal_timer_lbl, "%us", s_modal_ttl_sec);

    lv_obj_t* accept = lv_btn_create(s_modal);
    lv_obj_set_size(accept, 120, 48);
    lv_obj_align(accept, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(accept, lv_color_hex(0x003b17), 0);
    lv_obj_set_style_border_color(accept, CLR_GREEN, 0);
    lv_obj_set_style_border_width(accept, 2, 0);
    lv_obj_add_event_cb(accept, modal_accept_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* al = lv_label_create(accept);
    lv_label_set_text(al, "ACCEPT");
    lv_obj_set_style_text_font(al, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(al, CLR_GREEN, 0);
    lv_obj_center(al);

    lv_obj_t* decline = lv_btn_create(s_modal);
    lv_obj_set_size(decline, 120, 48);
    lv_obj_align(decline, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(decline, lv_color_hex(0x2a0000), 0);
    lv_obj_set_style_border_color(decline, CLR_GOLDD, 0);
    lv_obj_set_style_border_width(decline, 2, 0);
    lv_obj_add_event_cb(decline, modal_decline_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* dl = lv_label_create(decline);
    lv_label_set_text(dl, "DECLINE");
    lv_obj_set_style_text_font(dl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(dl, CLR_GOLDD, 0);
    lv_obj_center(dl);

    s_modal_timer = lv_timer_create(modal_tick_cb, 500, NULL);

    // Screen flash + tone — runs in parallel with the modal.
    flash_start();
    sfx_tone(880, 120);
}

static void modal_close(void) {
    if (s_modal_timer) { lv_timer_del(s_modal_timer); s_modal_timer = NULL; }
    if (s_modal)       { lv_obj_del(s_modal); s_modal = NULL; }
    s_modal_timer_lbl = NULL;
    s_modal_session   = 0;
}

static void modal_tick_cb(lv_timer_t* t) {
    (void)t;
    unsigned long elapsed = (millis() - s_modal_opened_ms) / 1000UL;
    if (elapsed >= s_modal_ttl_sec) {
        Serial.println("[game] Invite modal timed out");
        modal_close();
        return;
    }
    if (s_modal_timer_lbl) {
        lv_label_set_text_fmt(s_modal_timer_lbl, "%lus",
                              (unsigned long)(s_modal_ttl_sec - elapsed));
    }
}

static void modal_accept_cb(lv_event_t* e) {
    (void)e;
    uint32_t sid = s_modal_session;
    uint8_t  gid = s_game_id;     // set by caller before opening
    uint8_t  host[6];
    memcpy(host, s_host_mac, 6);
    modal_close();
    // Tear down the tab bar if the player was on the tab view —
    // it lives on lv_layer_top and would cover the game header.
    nav_teardown_tabs_if_needed();
    game_player_accept(sid);
    Serial.printf("[game] Invite accepted session=%08lX game=%u\n",
                  (unsigned long)sid, gid);
    // Open the game screen for this module, if registered.
    const GameModule* mod = find_module(gid);
    if (mod && mod->build_ui) mod->build_ui();
    (void)host;
}

static void modal_decline_cb(lv_event_t* e) {
    (void)e;
    uint32_t sid = s_modal_session;
    modal_close();
    game_player_decline(sid);
    Serial.printf("[game] Invite declined session=%08lX\n", (unsigned long)sid);
}

// ── Wire send helpers ─────────────────────────────────────────
static const uint8_t BCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static void fill_hdr(GameHdr& h, uint8_t type, uint16_t seq) {
    h.type       = type;
    h.game_id    = s_game_id;
    espnow_my_mac(h.host_mac);     // will be overwritten if we're a player
    h.session_id = s_session_id;
    h.seq        = seq;
    h.reserved   = 0;
}

static void send_join(uint32_t session_id, const uint8_t* host_mac, uint8_t game_id) {
    GameJoin j = {};
    j.hdr.type       = MSG_GAME_JOIN;
    j.hdr.game_id    = game_id;
    memcpy(j.hdr.host_mac, host_mac, 6);
    j.hdr.session_id = session_id;
    j.hdr.seq        = 0;
    espnow_my_mac(j.player_mac);
    if (g_logged_in) {
        strncpy(j.player_name, user_display_name(), sizeof(j.player_name) - 1);
    }
    // Broadcast the JOIN 3x with spacing — a single broadcast is easily
    // missed, especially if the host is on a different hop channel at
    // the instant of TX. Same pattern as channel hints.
    for (int attempt = 0; attempt < 3; attempt++) {
        esp_err_t rc = espnow_send_raw(BCAST_MAC, (uint8_t*)&j, sizeof(j));
        Serial.printf("[game] JOIN TX #%d (bcast, host=%02X:%02X:%02X:%02X:%02X:%02X) rc=%d\n",
                      attempt + 1,
                      host_mac[0], host_mac[1], host_mac[2],
                      host_mac[3], host_mac[4], host_mac[5], (int)rc);
        if (attempt < 2) delay(80);
    }
}

// ── Public API ────────────────────────────────────────────────
void game_host_invite(uint8_t game_id, uint8_t min_p, uint8_t max_p) {
    if (min_p < 2) min_p = 2;
    if (max_p > GAME_MAX_PLAYERS) max_p = GAME_MAX_PLAYERS;
    if (min_p > max_p) min_p = max_p;

    s_role       = ROLE_HOST;
    s_game_id    = game_id;
    s_session_id = ((uint32_t)esp_random());
    espnow_my_mac(s_host_mac);
    s_host_min_players = min_p;
    s_host_max_players = max_p;
    s_host_seat_count  = 1;
    memcpy(s_host_seats[0], s_host_mac, 6);
    if (g_logged_in) {
        strncpy(s_host_names[0], user_display_name(), sizeof(s_host_names[0]) - 1);
    }
    s_host_last_action_ms[0] = millis();
    s_host_invite_started_ms = millis();
    s_host_next_seq = 1;
    s_last_seq_seen = 0;

    GameInvite inv = {};
    fill_hdr(inv.hdr, MSG_GAME_INVITE, 0);
    memcpy(inv.hdr.host_mac, s_host_mac, 6);
    if (g_logged_in) {
        strncpy(inv.host_name, user_display_name(), sizeof(inv.host_name) - 1);
    }
    inv.max_players = max_p;
    inv.min_players = min_p;
    inv.ttl_sec     = GAME_INVITE_TTL_MS / 1000;

    // Cache for periodic re-broadcast during the invite window.
    memcpy(&s_host_invite_cache, &inv, sizeof(inv));
    s_host_last_invite_tx_ms = millis();

    esp_err_t rc = espnow_send_raw(BCAST_MAC, (uint8_t*)&inv, sizeof(inv));
    Serial.printf("[game] INVITE TX game=%u session=%08lX min=%u max=%u rc=%d\n",
                  game_id, (unsigned long)s_session_id, min_p, max_p, (int)rc);
}

void game_player_accept(uint32_t session_id) {
    // Modal stored the invite's game_id + host_mac into s_game_id /
    // s_host_mac before opening. Confirm session matches and fire JOIN.
    if (session_id != s_session_id) {
        Serial.println("[game] accept: session mismatch, ignoring");
        return;
    }
    s_role = ROLE_PLAYER;
    s_last_seq_seen = 0;   // fresh session -- don't drop STATE on old seq
    s_my_seat = 0xFF;
    send_join(s_session_id, s_host_mac, s_game_id);
}

void game_player_decline(uint32_t session_id) {
    // Declines are intentionally silent on the wire — host times out.
    if (session_id == s_session_id) {
        s_role       = ROLE_NONE;
        s_session_id = 0;
        s_game_id    = 0;
        s_session_end_ms = millis();
    }
}

void game_send_action(uint8_t action_id, const uint8_t* buf, uint8_t len) {
    if (s_role != ROLE_PLAYER || s_session_id == 0) return;
    if (len > GAME_ACTION_MAX) len = GAME_ACTION_MAX;
    GameAction a = {};
    a.hdr.type       = MSG_GAME_ACTION;
    a.hdr.game_id    = s_game_id;
    memcpy(a.hdr.host_mac, s_host_mac, 6);
    a.hdr.session_id = s_session_id;
    a.hdr.seq        = s_last_seq_seen;
    a.action_id      = action_id;
    a.action_len     = len;
    if (buf && len) memcpy(a.action_buf, buf, len);
    // Broadcast: same reasoning as send_join — avoids needing unicast peer
    // registration. Host filters by ROLE_HOST + session_id; other players
    // ignore ACTION frames (filter in on_rx).
    espnow_send_raw(BCAST_MAC, (uint8_t*)&a, sizeof(a));
}

void game_host_broadcast_state(uint8_t active_seat, uint8_t phase,
                               const uint8_t* state, uint16_t len) {
    if (s_role != ROLE_HOST || s_session_id == 0) return;
    if (len > GAME_STATE_MAX) len = GAME_STATE_MAX;
    GameState st = {};
    fill_hdr(st.hdr, MSG_GAME_STATE, s_host_next_seq++);
    st.active_seat = active_seat;
    st.phase       = phase;
    st.state_len   = len;
    if (state && len) memcpy(st.state_buf, state, len);
    esp_err_t rc = espnow_send_raw(BCAST_MAC, (uint8_t*)&st, sizeof(st));
    Serial.printf("[game] STATE TX seq=%u active=%u phase=%u len=%u rc=%d\n",
                  (unsigned)st.hdr.seq, active_seat, phase, len, (int)rc);
}

void game_host_end(const uint8_t* winner_seats, uint8_t n, const char* summary) {
    if (s_role != ROLE_HOST) return;
    GameEnd e = {};
    fill_hdr(e.hdr, MSG_GAME_END, s_host_next_seq++);
    if (n > GAME_MAX_PLAYERS) n = GAME_MAX_PLAYERS;
    e.winner_count = n;
    for (uint8_t i = 0; i < n; i++) e.winner_seats[i] = winner_seats[i];
    if (summary) { strncpy(e.summary, summary, sizeof(e.summary) - 1); }
    espnow_send_raw(BCAST_MAC, (uint8_t*)&e, sizeof(e));
    // Session is retired — reset role so a fresh invite can succeed.
    s_role          = ROLE_NONE;
    s_session_id    = 0;
    s_game_id       = 0;
    s_host_started  = false;
    s_my_seat       = 0xFF;
    s_session_end_ms = millis();
}

void game_host_start(void) {
    if (s_role != ROLE_HOST || s_host_started) return;
    if (s_host_seat_count < s_host_min_players) return;
    s_host_started = true;
    s_host_invite_started_ms = 0;   // close the invite window

    GameStart st = {};
    fill_hdr(st.hdr, MSG_GAME_START, s_host_next_seq++);
    st.seat_count = s_host_seat_count;
    for (uint8_t i = 0; i < s_host_seat_count; i++) {
        memcpy(st.seats[i], s_host_seats[i], 6);
        strncpy(st.names[i], s_host_names[i], GAME_NAME_LEN - 1);
    }
    // Broadcast START 3x with spacing — easily missed on a single frame,
    // same pattern as JOIN. Do this BEFORE on_start() so the player can
    // catch the START while the host builds its game screen.
    for (int attempt = 0; attempt < 3; attempt++) {
        espnow_send_raw(BCAST_MAC, (uint8_t*)&st, sizeof(st));
        if (attempt < 2) delay(80);
    }
    Serial.printf("[game] START TX (3x) seats=%u\n", s_host_seat_count);

    // Cache for continued retransmission in game_poll() — catches
    // players dwelling on a different hop channel during the burst.
    memcpy(&s_host_start_cache, &st, sizeof(st));
    s_host_start_retx_count = 0;
    s_host_start_retx_ms = millis();

    // We're seat 0 on our own table.
    s_my_seat = 0;
    const GameModule* mod = find_module(s_game_id);
    if (mod && mod->on_start) mod->on_start(s_host_seat_count);
}

void game_host_cancel(void) {
    if (s_role != ROLE_HOST || s_host_started) return;
    GameEnd e = {};
    fill_hdr(e.hdr, MSG_GAME_END, s_host_next_seq++);
    e.winner_count = 0;
    strncpy(e.summary, "INVITE CANCELED", sizeof(e.summary) - 1);
    // Multi-send so the player catches it even on a different hop channel.
    for (int attempt = 0; attempt < 3; attempt++) {
        espnow_send_raw(BCAST_MAC, (uint8_t*)&e, sizeof(e));
        if (attempt < 2) delay(80);
    }
    Serial.println("[game] CANCEL TX (3x END)");
    s_role       = ROLE_NONE;
    s_session_id = 0;
    s_game_id    = 0;
    s_host_seat_count = 0;
    s_host_invite_started_ms = 0;
    s_session_end_ms = millis();
}

uint8_t game_seat_count(void) {
    return s_host_seat_count;
}

void game_seats(uint8_t out_macs[][6], char out_names[][GAME_NAME_LEN]) {
    uint8_t n = s_host_seat_count;
    for (uint8_t i = 0; i < n; i++) {
        if (out_macs)  memcpy(out_macs[i], s_host_seats[i], 6);
        if (out_names) {
            strncpy(out_names[i], s_host_names[i], GAME_NAME_LEN - 1);
            out_names[i][GAME_NAME_LEN - 1] = '\0';
        }
    }
}

uint8_t game_my_seat(void) {
    return s_my_seat;
}

bool game_host_is_inviting(void) {
    return (s_role == ROLE_HOST) && (s_host_invite_started_ms != 0) && !s_host_started;
}

bool game_host_started(void) {
    return (s_role == ROLE_HOST) && s_host_started;
}

bool game_in_session(void) {
    return s_role != ROLE_NONE && s_session_id != 0;
}

void game_player_leave(void) {
    if (s_role != ROLE_PLAYER) return;
    Serial.printf("[game] Player leaving session %08lX\n",
                  (unsigned long)s_session_id);
    s_role            = ROLE_NONE;
    s_session_id      = 0;
    s_game_id         = 0;
    s_my_seat         = 0xFF;
    s_last_seq_seen   = 0;
    s_host_started    = false;
    s_host_seat_count = 0;
    s_session_end_ms  = millis();
}

// ── Receive path ──────────────────────────────────────────────
// on_recv() runs on the WiFi task (core 0). We only do lightweight
// work here — the modal and any game UI mutation happens on the
// main core via game_poll(). INVITE is queued; JOIN/STATE/ACTION
// are handled inline since they don't touch LVGL.
void game_on_rx(const uint8_t* mac, const uint8_t* data, int len) {
    if (len < (int)sizeof(GameHdr)) return;
    const GameHdr* hdr = (const GameHdr*)data;

    switch (hdr->type) {
    case MSG_GAME_INVITE: {
        if (len < (int)sizeof(GameInvite)) return;
        // Drop our own broadcasts.
        uint8_t mine[6]; espnow_my_mac(mine);
        if (memcmp(hdr->host_mac, mine, 6) == 0) return;
        // Already in a session? Drop.
        if (s_role != ROLE_NONE) return;
        // Must match a registered game module (filters random ESP-NOW noise).
        if (!find_module(hdr->game_id)) return;
        // Post-session cooldown — ignore invites shortly after a session ends
        // so stale re-broadcasts from the previous host don't re-trigger.
        if (s_session_end_ms != 0 &&
            (millis() - s_session_end_ms) < SESSION_COOLDOWN_MS) return;
        memcpy(&s_pending_invite_data, data, sizeof(GameInvite));
        memcpy(s_pending_invite_mac, mac, 6);
        s_pending_invite = true;
        break;
    }
    case MSG_GAME_JOIN: {
        if (len < (int)sizeof(GameJoin)) return;
        Serial.printf("[game] JOIN RX role=%d my_sess=%08lX rx_sess=%08lX\n",
                      (int)s_role, (unsigned long)s_session_id,
                      (unsigned long)hdr->session_id);
        if (s_role != ROLE_HOST || hdr->session_id != s_session_id) {
            Serial.println("[game] JOIN dropped (not host / session mismatch)");
            return;
        }
        const GameJoin* j = (const GameJoin*)data;
        // De-dup by MAC.
        for (int i = 0; i < s_host_seat_count; i++) {
            if (memcmp(s_host_seats[i], j->player_mac, 6) == 0) {
                Serial.println("[game] JOIN ignored: duplicate MAC");
                return;
            }
        }
        if (s_host_seat_count >= s_host_max_players) {
            Serial.println("[game] JOIN ignored: table full");
            return;
        }
        int idx = s_host_seat_count++;
        memcpy(s_host_seats[idx], j->player_mac, 6);
        strncpy(s_host_names[idx], j->player_name, GAME_NAME_LEN - 1);
        s_host_names[idx][GAME_NAME_LEN - 1] = '\0';
        s_host_last_action_ms[idx] = millis();
        Serial.printf("[game] JOIN from %s (seat %d / %u)\n",
                      s_host_names[idx], idx, s_host_max_players);
        break;
    }
    case MSG_GAME_START: {
        if (len < (int)sizeof(GameStart)) return;
        if (hdr->session_id != s_session_id || s_role != ROLE_PLAYER) return;
        // Guard against duplicate START re-broadcasts — once we've
        // processed the first START (s_my_seat assigned), drop repeats.
        if (s_my_seat != 0xFF) return;
        if (s_pending_start) return;   // already queued, not yet consumed
        // Stash; game_poll() consumes on main core (LVGL lives there).
        memcpy(&s_pending_start_data, data, sizeof(GameStart));
        s_pending_start = true;
        Serial.println("[game] START RX queued");
        break;
    }
    case MSG_GAME_STATE: {
        if (len < (int)sizeof(GameState)) return;
        if (hdr->session_id != s_session_id) return;
        if (hdr->seq <= s_last_seq_seen && s_last_seq_seen != 0) return;
        s_last_seq_seen = hdr->seq;
        // Stash; game_poll() renders on main core. Latest wins.
        memcpy(&s_pending_state_data, data, sizeof(GameState));
        s_pending_state = true;
        break;
    }
    case MSG_GAME_ACTION: {
        if (len < (int)sizeof(GameAction)) return;
        if (s_role != ROLE_HOST || hdr->session_id != s_session_id) return;
        const GameModule* mod = find_module(hdr->game_id);
        if (mod && mod->on_action) mod->on_action((const GameAction*)data, mac);
        // Refresh the originating seat's last-action time for nudge gating.
        for (int i = 0; i < s_host_seat_count; i++) {
            if (memcmp(s_host_seats[i], mac, 6) == 0) {
                s_host_last_action_ms[i] = millis();
                break;
            }
        }
        break;
    }
    case MSG_GAME_NUDGE: {
        if (len < (int)sizeof(GameNudge)) return;
        if (hdr->session_id != s_session_id) return;
        const GameNudge* n = (const GameNudge*)data;
        uint8_t mine[6]; espnow_my_mac(mine);
        if (memcmp(n->target_mac, mine, 6) != 0) return;
        // Defer to main core — flash_start() and build_ui() create
        // LVGL objects. Running them here (core 0) corrupts LVGL and
        // can freeze the entire UI.
        memcpy(&s_pending_nudge_data, data, sizeof(GameNudge));
        s_pending_nudge = true;
        Serial.println("[game] NUDGE RX queued");
        break;
    }
    case MSG_GAME_END: {
        if (len < (int)sizeof(GameEnd)) return;
        if (hdr->session_id != s_session_id) return;
        // Immediately mark role so core-0 handlers stop processing
        // frames for this session. Full teardown deferred to main core
        // so lobby_tick sees a consistent state.
        s_role = ROLE_NONE;
        memcpy(&s_pending_end_data, data, sizeof(GameEnd));
        s_pending_end = true;
        Serial.printf("[game] END RX session=%08lX (queued)\n",
                      (unsigned long)s_session_id);
        break;
    }
    default:
        break;
    }
}

// ── Main-loop tick ────────────────────────────────────────────
// Consumes the pending-invite flag (opens modal) and runs the
// host-side nudge + auto-start logic.
void game_poll(void) {
    // Promote any pending invite from the RX task to the UI task.
    if (s_pending_invite && s_modal == NULL) {
        s_pending_invite = false;
        // Stash the invite's session_id / game_id / host_mac in the
        // same slots a player session would use, so accept/decline can
        // act without needing a second copy.
        const GameInvite* inv = &s_pending_invite_data;
        s_session_id = inv->hdr.session_id;
        s_game_id    = inv->hdr.game_id;
        memcpy(s_host_mac, inv->hdr.host_mac, 6);
        modal_open(inv);
    }

    // Promote pending START (player side) — sets my_seat, fires module on_start.
    if (s_pending_start) {
        s_pending_start = false;
        const GameStart* st = &s_pending_start_data;
        uint8_t mine[6]; espnow_my_mac(mine);
        s_my_seat = 0xFF;
        // Populate seat roster so game_seats() / game_seat_count() work on
        // the player side too (render_mesh needs names for other seats).
        s_host_seat_count = (st->seat_count <= GAME_MAX_PLAYERS)
                          ? st->seat_count : GAME_MAX_PLAYERS;
        for (uint8_t i = 0; i < s_host_seat_count; i++) {
            memcpy(s_host_seats[i], st->seats[i], 6);
            strncpy(s_host_names[i], st->names[i], GAME_NAME_LEN - 1);
            s_host_names[i][GAME_NAME_LEN - 1] = '\0';
            if (memcmp(st->seats[i], mine, 6) == 0) s_my_seat = i;
        }
        Serial.printf("[game] START consumed seats=%u my_seat=%u\n",
                      s_host_seat_count, s_my_seat);
        const GameModule* mod = find_module(st->hdr.game_id);
        if (mod && mod->on_start) mod->on_start(s_host_seat_count);
    }

    // Promote pending STATE — module renders on main core.
    if (s_pending_state) {
        s_pending_state = false;
        const GameState* gs = &s_pending_state_data;
        const GameModule* mod = find_module(gs->hdr.game_id);
        if (mod && mod->on_state) mod->on_state(gs, s_role == ROLE_HOST);
    }

    // Promote pending NUDGE — flash + SFX + wake must run on main
    // core because they create LVGL objects.
    if (s_pending_nudge) {
        s_pending_nudge = false;
        const GameNudge* n = &s_pending_nudge_data;
        flash_start();
        sfx_tone(660, 100);
        if (n->wake_screen) {
            const GameModule* mod = find_module(n->hdr.game_id);
            if (mod && mod->build_ui) mod->build_ui();
        }
        Serial.println("[game] NUDGE consumed (flash + SFX)");
    }

    // Promote pending END — session teardown on main core so
    // lobby_tick / game_in_session reads see consistent state.
    if (s_pending_end) {
        s_pending_end = false;
        Serial.printf("[game] Session %08lX ended (main core)\n",
                      (unsigned long)s_pending_end_data.hdr.session_id);
        // s_role already set to ROLE_NONE on core 0 (so handlers stop
        // processing stale frames immediately). Finish the cleanup here.
        s_session_id      = 0;
        s_game_id         = 0;
        s_host_started    = false;
        s_my_seat         = 0xFF;
        s_last_seq_seen   = 0;
        s_host_seat_count = 0;
        s_session_end_ms  = millis();
    }

    // Host: auto-start when table is full. Partial tables wait for
    // the user to tap START NOW in the lobby, or let the invite TTL
    // abort the session if we never reach min_players.
    if (s_role == ROLE_HOST && s_host_invite_started_ms != 0 && !s_host_started) {
        unsigned long now = millis();
        bool full   = s_host_seat_count >= s_host_max_players;
        bool quorum = s_host_seat_count >= s_host_min_players;
        bool elapsed = (now - s_host_invite_started_ms) >= GAME_INVITE_TTL_MS;
        if (full) {
            Serial.printf("[game] auto-start (table full %u)\n", s_host_seat_count);
            game_host_start();
        } else if (elapsed && !quorum) {
            Serial.printf("[game] invite expired (seats=%u min=%u) -- canceling\n",
                          s_host_seat_count, s_host_min_players);
            // Broadcast END so any player who already JOINed knows the
            // session is dead (instead of silently resetting and leaving
            // them stuck in the "JOINED" lobby).
            game_host_cancel();
        } else if (!elapsed && (now - s_host_last_invite_tx_ms) >= INVITE_REBROADCAST_MS) {
            // Re-broadcast invite so peers on other hop channels can catch it.
            // Recalculate remaining TTL so players who catch a late re-broadcast
            // see an accurate countdown rather than a stale 30s.
            s_host_last_invite_tx_ms = now;
            unsigned long elapsed_s = (now - s_host_invite_started_ms) / 1000;
            uint8_t remaining = (elapsed_s < 30) ? (uint8_t)(30 - elapsed_s) : 1;
            s_host_invite_cache.ttl_sec = remaining;
            espnow_send_raw(BCAST_MAC, (uint8_t*)&s_host_invite_cache,
                            sizeof(s_host_invite_cache));
            Serial.printf("[game] INVITE re-TX seats=%u/%u ttl=%us\n",
                          s_host_seat_count, s_host_max_players, remaining);
        }
        // elapsed && quorum: sit waiting for user to tap START NOW.
    }

    // Host: call the module's tick so it can drive game-specific
    // timers (nudges, dealer-draw pacing, round-end auto-continue).
    if (s_role == ROLE_HOST && s_host_started) {
        const GameModule* mod = find_module(s_game_id);
        if (mod && mod->host_tick) mod->host_tick();

        // Continue re-broadcasting START for a few seconds after the
        // initial burst. The 3x burst in game_host_start() might land
        // while the player's radio is on a different hop channel; these
        // slower retransmissions span multiple dwell intervals (~4s each).
        if (s_host_start_retx_count < START_RETX_MAX) {
            unsigned long now = millis();
            if ((now - s_host_start_retx_ms) >= START_RETX_INTERVAL_MS) {
                s_host_start_retx_ms = now;
                s_host_start_retx_count++;
                espnow_send_raw(BCAST_MAC, (uint8_t*)&s_host_start_cache,
                                sizeof(s_host_start_cache));
                Serial.printf("[game] START re-TX #%u/%u\n",
                              s_host_start_retx_count, START_RETX_MAX);
            }
        }
    }
}
