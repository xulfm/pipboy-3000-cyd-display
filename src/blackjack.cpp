// ============================================================
// Vault 89 PDI-Boy -- Blackjack
// 1P (local vs. dealer) + Mesh (host-authoritative, up to 6
// seats). 1P uses s_bj_local; mesh uses s_bj_wire which also
// doubles as the GameState.state_buf payload for players.
// ============================================================

#include <Arduino.h>
#include <lvgl.h>
#include <string.h>
#include "pipboy_data.h"
#include "sound_fx.h"
#include "game_proto.h"
#include "net_state.h"

extern void apps_open_hub(void);
extern void apps_swap_sub_scr(lv_obj_t* new_scr);
extern "C" { lv_obj_t* screen_blackjack_menu_create(void); }

// ── Card helpers ──────────────────────────────────────────────
static const char RANK_CHR[13] = {'A','2','3','4','5','6','7','8','9','T','J','Q','K'};
static const char* SUIT_CHR[4] = {"S","H","D","C"};

static uint8_t card_value(uint8_t idx) {
    uint8_t r = idx % 13;
    if (r == 0) return 11;
    if (r >= 9) return 10;
    return r + 1;
}

static uint8_t hand_total(const uint8_t* hand, uint8_t n, bool* soft_out) {
    uint16_t tot = 0;
    uint8_t  aces = 0;
    for (uint8_t i = 0; i < n; i++) {
        tot += card_value(hand[i]);
        if (hand[i] % 13 == 0) aces++;
    }
    while (tot > 21 && aces > 0) { tot -= 10; aces--; }
    if (soft_out) *soft_out = (aces > 0 && tot <= 21);
    return (uint8_t)tot;
}

static void fmt_card(char* buf, size_t n, uint8_t idx) {
    snprintf(buf, n, "%c%s", RANK_CHR[idx % 13], SUIT_CHR[idx / 13]);
}

static void fmt_hand(char* out, size_t n, const uint8_t* hand, uint8_t count,
                     int8_t hide_from) {
    out[0] = '\0';
    size_t used = 0;
    for (uint8_t i = 0; i < count; i++) {
        char c[4];
        if (hide_from >= 0 && i >= (uint8_t)hide_from) strncpy(c, "??", sizeof(c));
        else                                           fmt_card(c, sizeof(c), hand[i]);
        int w = snprintf(out + used, n - used, "%s%s", i ? " " : "", c);
        if (w <= 0 || (size_t)w >= n - used) break;
        used += w;
    }
}

// Shared phase encoding (fits in GameState.phase byte).
enum BjPhase : uint8_t {
    BJ_IDLE         = 0,
    BJ_PLAYER_TURN  = 1,
    BJ_DEALER_TURN  = 2,
    BJ_ROUND_END    = 3,
};

// ── Single-player local state ────────────────────────────────
struct BjLocal {
    uint8_t  deck[52];
    uint8_t  deck_pos;
    uint8_t  dealer[10];
    uint8_t  dealer_n;
    uint8_t  player[10];
    uint8_t  player_n;
    BjPhase  phase;
    int8_t   result;
    uint16_t wins, losses, pushes;
};
static BjLocal s_bj_local = {};

// ── Mesh wire state ──────────────────────────────────────────
// Hosts keep s_bj_wire as authoritative and broadcast a copy as
// GameState.state_buf. Players just memcpy incoming state into
// s_bj_wire and render from it.
#define BJ_SEAT_BUST   0x01
#define BJ_SEAT_STAND  0x02
#define BJ_SEAT_BJ     0x04

struct __attribute__((packed)) BjWire {
    uint8_t round_no;
    uint8_t num_seats;
    uint8_t active_seat;      // 0..n-1, 0xFE=dealer, 0xFF=no turn
    uint8_t phase;
    uint8_t dealer_n;
    uint8_t dealer[10];
    uint8_t seat_n[GAME_MAX_PLAYERS];
    uint8_t seats[GAME_MAX_PLAYERS][10];
    uint8_t seat_flags[GAME_MAX_PLAYERS];
    int8_t  seat_result[GAME_MAX_PLAYERS];   // -2 pending, -1 lose, 0 push, +1 win, +2 blackjack
    uint16_t seat_chips[GAME_MAX_PLAYERS];   // chip balance
    uint16_t seat_bet[GAME_MAX_PLAYERS];     // current bet
};
static_assert(sizeof(BjWire) <= GAME_STATE_MAX, "BjWire too big");

#define BJ_DEALER_SEAT   0xFE
#define BJ_START_CHIPS   100
#define BJ_DEFAULT_BET   10
#define BJ_MIN_BET       5
#define BJ_BET_STEP      5

static BjWire s_bj_wire     = {};
static bool   s_bj_wire_valid = false;

// Host-only: shared shuffled deck used across rounds.
static uint8_t s_host_deck[52];
static uint8_t s_host_deck_pos = 0;
static unsigned long s_host_round_end_ms = 0;

// Host: re-broadcast STATE a few times after game start so late
// START recipients (via retransmit) can catch up on the initial deal.
static uint8_t       s_host_state_retx_left = 0;
static unsigned long s_host_state_retx_ms   = 0;
#define STATE_INITIAL_RETX_COUNT    6
#define STATE_INITIAL_RETX_MS     500

// Host: periodic STATE re-broadcast so missed frames don't freeze
// the game. Single ESP-NOW broadcasts are easily lost on channel hops.
static unsigned long s_host_last_broadcast_ms = 0;
#define STATE_REBROADCAST_MS  2000

// Host: auto-deal delay after ROUND_END (bet phase).
#define AUTO_DEAL_DELAY_MS    8000

// Action IDs over the wire (kept small so they fit in action_id).
enum BjAction : uint8_t {
    BJ_ACT_HIT      = 1,
    BJ_ACT_STAND    = 2,
    BJ_ACT_BET_UP   = 3,
    BJ_ACT_BET_DOWN = 4,
};

// ── Shared UI ────────────────────────────────────────────────
enum BjScreenMode : uint8_t { BJ_MODE_NONE = 0, BJ_MODE_1P, BJ_MODE_MESH };
static BjScreenMode s_mode = BJ_MODE_NONE;

// --- Card visual constants ---
#define CARD_W       22
#define CARD_H       28
#define CARD_OVERLAP 16
#define MAX_HAND     10

// Table colors
#define CLR_FELT   lv_color_hex(0x0a3d1a)   // dark green felt
#define CLR_CREAM  lv_color_hex(0xF0EBD8)   // card face
#define CLR_CARD_BACK lv_color_hex(0x1a4d2e) // face-down card

// --- UI widgets ---
static lv_obj_t* s_bj_scr        = NULL;
static lv_obj_t* s_table          = NULL;
static lv_obj_t* s_lbl_dlr_tag    = NULL;   // "DEALER" label
static lv_obj_t* s_lbl_dlr_total  = NULL;   // dealer total
static lv_obj_t* s_lbl_you_tag    = NULL;   // "YOU" label
static lv_obj_t* s_lbl_you_total  = NULL;   // player total
static lv_obj_t* s_lbl_others     = NULL;
static lv_obj_t* s_lbl_status     = NULL;
static lv_obj_t* s_lbl_score      = NULL;
static lv_obj_t* s_btn_hit        = NULL;
static lv_obj_t* s_btn_stand      = NULL;
static lv_obj_t* s_btn_deal       = NULL;
static lv_obj_t* s_btn_bet_up     = NULL;
static lv_obj_t* s_btn_bet_down   = NULL;

// Pre-allocated card objects (dealer + player)
static lv_obj_t* s_dc[MAX_HAND] = {};   // dealer cards
static lv_obj_t* s_pc[MAX_HAND] = {};   // player cards
static lv_obj_t* s_dc_lbl[MAX_HAND] = {};
static lv_obj_t* s_pc_lbl[MAX_HAND] = {};

// 1P betting
static uint16_t s_1p_chips = 100;
static uint16_t s_1p_bet   = 10;
static bool     s_1p_settled = true;   // false while round result unsettled

// --- Helpers ---
static void set_dis(lv_obj_t* o, bool dis) {
    if (!o) return;
    if (dis) lv_obj_add_state(o, LV_STATE_DISABLED);
    else     lv_obj_clear_state(o, LV_STATE_DISABLED);
}

// Create a card visual inside the table
static lv_obj_t* mk_card(lv_obj_t* parent, lv_obj_t** lbl_out) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_set_size(c, CARD_W, CARD_H);
    lv_obj_set_style_bg_color(c, CLR_CREAM, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x888888), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 3, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* l = lv_label_create(c);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);

    if (lbl_out) *lbl_out = l;
    return c;
}

// Update and show a card at position (x, y) inside the table
static void card_show(lv_obj_t* c, lv_obj_t* lbl, uint8_t idx,
                      bool face_down, int x, int y) {
    lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(c, x, y);
    if (face_down) {
        lv_label_set_text(lbl, "??");
        lv_obj_set_style_bg_color(c, CLR_CARD_BACK, 0);
        lv_obj_set_style_border_color(c, CLR_GOLD, 0);
        lv_obj_set_style_text_color(lbl, CLR_GOLD, 0);
    } else {
        char buf[4]; fmt_card(buf, sizeof(buf), idx);
        lv_label_set_text(lbl, buf);
        uint8_t suit = idx / 13;
        bool red = (suit == 1 || suit == 2);  // hearts, diamonds
        lv_obj_set_style_text_color(lbl, red ? lv_color_hex(0xCC0000)
                                              : lv_color_hex(0x111111), 0);
        lv_obj_set_style_bg_color(c, CLR_CREAM, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(0x888888), 0);
    }
}

static void card_hide(lv_obj_t* c) {
    if (c) lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
}

// Show a row of cards. Returns the x after the last card.
static int show_hand(lv_obj_t* cards[], lv_obj_t* lbls[],
                     const uint8_t* hand, uint8_t n,
                     int x0, int y, int8_t hide_from) {
    for (int i = 0; i < MAX_HAND; i++) {
        if (i < n) {
            bool fd = (hide_from >= 0 && i >= hide_from);
            card_show(cards[i], lbls[i], hand[i], fd, x0 + i * CARD_OVERLAP, y);
        } else {
            card_hide(cards[i]);
        }
    }
    return (n > 0) ? x0 + (n - 1) * CARD_OVERLAP + CARD_W + 6 : x0;
}

// ── Render ───────────────────────────────────────────────────
static void render_1p(void) {
    if (!s_bj_scr) return;
    char buf[64];
    bool soft;

    // Settle bet immediately when round ends
    if (s_bj_local.phase == BJ_ROUND_END && !s_1p_settled) {
        s_1p_settled = true;
        if      (s_bj_local.result ==  2) s_1p_chips += s_1p_bet * 3 / 2;
        else if (s_bj_local.result ==  1) s_1p_chips += s_1p_bet;
        else if (s_bj_local.result == -1) {
            s_1p_chips = (s_1p_chips >= s_1p_bet) ? s_1p_chips - s_1p_bet : 0;
        }
        if (s_1p_chips < 5) { s_1p_chips = 100; s_1p_bet = 10; } // rebuy
        if (s_1p_bet > s_1p_chips) s_1p_bet = (s_1p_chips / 5) * 5;
    }

    // Dealer cards
    bool hide_hole = (s_bj_local.phase == BJ_PLAYER_TURN);
    show_hand(s_dc, s_dc_lbl, s_bj_local.dealer, s_bj_local.dealer_n,
              50, 4, hide_hole ? 1 : -1);

    // Dealer total
    if (hide_hole && s_bj_local.dealer_n > 0) {
        lv_label_set_text(s_lbl_dlr_total, "??");
    } else {
        uint8_t dt = hand_total(s_bj_local.dealer, s_bj_local.dealer_n, &soft);
        snprintf(buf, sizeof(buf), "%u", (unsigned)dt);
        lv_label_set_text(s_lbl_dlr_total, s_bj_local.dealer_n ? buf : "");
    }

    // Player cards
    show_hand(s_pc, s_pc_lbl, s_bj_local.player, s_bj_local.player_n,
              50, 38, -1);

    // Player total
    uint8_t pt = hand_total(s_bj_local.player, s_bj_local.player_n, &soft);
    if (s_bj_local.player_n > 0) {
        snprintf(buf, sizeof(buf), "%u%s", (unsigned)pt, soft ? "*" : "");
        lv_label_set_text(s_lbl_you_total, buf);
    } else {
        lv_label_set_text(s_lbl_you_total, "");
    }

    // Status
    const char* status = "";
    lv_color_t  color  = CLR_GOLD;
    switch (s_bj_local.phase) {
    case BJ_IDLE:         status = "TAP DEAL TO START"; color = CLR_GOLD;  break;
    case BJ_PLAYER_TURN:  status = "HIT OR STAND";       color = CLR_GREEN; break;
    case BJ_DEALER_TURN:  status = "DEALER DRAWING...";  color = CLR_GOLD;  break;
    case BJ_ROUND_END:
        if      (s_bj_local.result ==  2) { status = "BLACKJACK!";  color = CLR_GREEN; }
        else if (s_bj_local.result ==  1) { status = "YOU WIN!";    color = CLR_GREEN; }
        else if (s_bj_local.result ==  0) { status = "PUSH";        color = CLR_GOLD;  }
        else                              { status = "DEALER WINS"; color = CLR_GOLDD; }
        break;
    }
    lv_label_set_text(s_lbl_status, status);
    lv_obj_set_style_text_color(s_lbl_status, color, 0);

    // Score + bet
    snprintf(buf, sizeof(buf), "W:%u L:%u P:%u  $%u BET:%u",
             s_bj_local.wins, s_bj_local.losses, s_bj_local.pushes,
             s_1p_chips, s_1p_bet);
    lv_label_set_text(s_lbl_score, buf);

    bool player_turn = (s_bj_local.phase == BJ_PLAYER_TURN);
    bool can_deal    = (s_bj_local.phase == BJ_IDLE || s_bj_local.phase == BJ_ROUND_END);
    bool can_bet     = can_deal;
    set_dis(s_btn_hit,      !player_turn);
    set_dis(s_btn_stand,    !player_turn);
    set_dis(s_btn_deal,     !can_deal);
    set_dis(s_btn_bet_up,   !can_bet);
    set_dis(s_btn_bet_down, !can_bet);
}

static const char* seat_short_name(uint8_t seat) {
    static char buf[GAME_MAX_PLAYERS][24];
    if (seat >= GAME_MAX_PLAYERS) return "?";
    char names[GAME_MAX_PLAYERS][GAME_NAME_LEN];
    game_seats(NULL, names);
    if (names[seat][0]) {
        snprintf(buf[seat], sizeof(buf[0]), "%s", names[seat]);
    } else {
        snprintf(buf[seat], sizeof(buf[0]), "SEAT %u", seat);
    }
    return buf[seat];
}

static void render_mesh(void) {
    if (!s_bj_scr || !s_bj_wire_valid) return;
    char buf[96];
    bool soft;
    uint8_t me = game_my_seat();
    const BjWire& w = s_bj_wire;

    // Dealer cards
    bool hide_hole = (w.phase == BJ_PLAYER_TURN);
    show_hand(s_dc, s_dc_lbl, w.dealer, w.dealer_n,
              50, 4, hide_hole ? 1 : -1);

    if (hide_hole && w.dealer_n > 0) {
        lv_label_set_text(s_lbl_dlr_total, "??");
    } else {
        uint8_t dt = hand_total(w.dealer, w.dealer_n, &soft);
        snprintf(buf, sizeof(buf), "%u", (unsigned)dt);
        lv_label_set_text(s_lbl_dlr_total, w.dealer_n ? buf : "");
    }

    // My hand cards
    if (me < GAME_MAX_PLAYERS && w.seat_n[me] > 0) {
        show_hand(s_pc, s_pc_lbl, w.seats[me], w.seat_n[me],
                  50, 38, -1);
        uint8_t pt = hand_total(w.seats[me], w.seat_n[me], &soft);
        const char* tag = (w.seat_flags[me] & BJ_SEAT_BUST)  ? " X"
                        : (w.seat_flags[me] & BJ_SEAT_STAND) ? " ="
                        : (w.seat_flags[me] & BJ_SEAT_BJ)    ? " BJ"
                        : (soft ? "*" : "");
        snprintf(buf, sizeof(buf), "%u%s", (unsigned)pt, tag);
        lv_label_set_text(s_lbl_you_total, buf);
    } else {
        for (int i = 0; i < MAX_HAND; i++) card_hide(s_pc[i]);
        lv_label_set_text(s_lbl_you_total, "--");
    }

    // Other seats — show name, hand total, and chips
    char row[128] = "";
    size_t used = 0;
    char names[GAME_MAX_PLAYERS][GAME_NAME_LEN];
    game_seats(NULL, names);
    for (uint8_t i = 0; i < w.num_seats; i++) {
        if (i == me) continue;
        const char* nm = names[i][0] ? names[i] : "?";
        uint8_t t = hand_total(w.seats[i], w.seat_n[i], &soft);
        const char* mark = (i == w.active_seat)              ? "*"
                         : (w.seat_flags[i] & BJ_SEAT_BUST)  ? "X"
                         : (w.seat_flags[i] & BJ_SEAT_STAND) ? "="
                         : " ";
        int n = snprintf(row + used, sizeof(row) - used, "%s%s:%u%s$%u",
                         used ? "  " : "", nm, (unsigned)t, mark,
                         (unsigned)w.seat_chips[i]);
        if (n <= 0 || (size_t)n >= sizeof(row) - used) break;
        used += n;
    }
    lv_label_set_text(s_lbl_others, row);

    // Status
    const char* status = "";
    lv_color_t  color  = CLR_GOLD;
    if (w.phase == BJ_PLAYER_TURN) {
        if (w.active_seat == me) { status = ">> YOUR TURN <<"; color = CLR_GREEN; }
        else {
            snprintf(buf, sizeof(buf), "%s's TURN",
                     names[w.active_seat][0] ? names[w.active_seat] : "PLAYER");
            lv_label_set_text(s_lbl_status, buf);
            lv_obj_set_style_text_color(s_lbl_status, CLR_GOLD, 0);
            goto buttons;
        }
    } else if (w.phase == BJ_DEALER_TURN) { status = "DEALER DRAWING..."; color = CLR_GOLD; }
    else if (w.phase == BJ_ROUND_END) {
        int8_t r = (me < GAME_MAX_PLAYERS) ? w.seat_result[me] : -2;
        // Show result briefly, then switch to bet prompt with countdown
        unsigned long since_end = millis() - s_host_round_end_ms;
        if (since_end < 3000) {
            // First 3s: show round result
            if      (r ==  2) { status = "BLACKJACK!";   color = CLR_GREEN; }
            else if (r ==  1) { status = "YOU WIN!";     color = CLR_GREEN; }
            else if (r ==  0) { status = "PUSH";         color = CLR_GOLD;  }
            else if (r == -1) { status = "YOU LOSE";     color = CLR_GOLDD; }
            else              { status = "ROUND OVER";   color = CLR_GOLD;  }
        } else {
            // After 3s: show bet prompt with countdown
            unsigned long deal_in = 0;
            if (AUTO_DEAL_DELAY_MS > since_end)
                deal_in = (AUTO_DEAL_DELAY_MS - since_end) / 1000;
            snprintf(buf, sizeof(buf), "PLACE BETS  (%lus)", deal_in);
            status = buf;
            color = CLR_GOLD;
        }
    }
    lv_label_set_text(s_lbl_status, status);
    lv_obj_set_style_text_color(s_lbl_status, color, 0);

buttons:
    // Score line — show round, my chips, and my bet
    if (me < GAME_MAX_PLAYERS) {
        snprintf(buf, sizeof(buf), "RD:%u  $%u  BET:%u",
                 w.round_no, (unsigned)w.seat_chips[me],
                 (unsigned)w.seat_bet[me]);
    } else {
        snprintf(buf, sizeof(buf), "ROUND %u", w.round_no);
    }
    lv_label_set_text(s_lbl_score, buf);

    bool my_turn = (w.phase == BJ_PLAYER_TURN) && (w.active_seat == me);
    bool between = (w.phase == BJ_ROUND_END);
    set_dis(s_btn_hit,      !my_turn);
    set_dis(s_btn_stand,    !my_turn);
    set_dis(s_btn_bet_up,   !between);
    set_dis(s_btn_bet_down, !between);
}

static void bj_render(void) {
    if (s_mode == BJ_MODE_1P)   render_1p();
    else if (s_mode == BJ_MODE_MESH) render_mesh();
}

// ── 1P logic ─────────────────────────────────────────────────
static void bj1_shuffle(void) {
    for (int i = 0; i < 52; i++) s_bj_local.deck[i] = i;
    for (int i = 51; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        uint8_t t = s_bj_local.deck[i]; s_bj_local.deck[i] = s_bj_local.deck[j]; s_bj_local.deck[j] = t;
    }
    s_bj_local.deck_pos = 0;
}
static uint8_t bj1_draw(void) {
    if (s_bj_local.deck_pos >= 52) bj1_shuffle();
    return s_bj_local.deck[s_bj_local.deck_pos++];
}
static void bj1_deal(void) {
    s_1p_settled = false;   // mark for settlement when round resolves
    s_bj_local.dealer_n = 0;
    s_bj_local.player_n = 0;
    s_bj_local.result = 127;
    s_bj_local.phase = BJ_PLAYER_TURN;
    s_bj_local.player[s_bj_local.player_n++] = bj1_draw();
    s_bj_local.dealer[s_bj_local.dealer_n++] = bj1_draw();
    s_bj_local.player[s_bj_local.player_n++] = bj1_draw();
    s_bj_local.dealer[s_bj_local.dealer_n++] = bj1_draw();
    bool soft;
    uint8_t p = hand_total(s_bj_local.player, s_bj_local.player_n, &soft);
    uint8_t d = hand_total(s_bj_local.dealer, s_bj_local.dealer_n, &soft);
    if (p == 21 || d == 21) {
        s_bj_local.phase = BJ_ROUND_END;
        if (p == 21 && d == 21)       { s_bj_local.result =  0; s_bj_local.pushes++; }
        else if (p == 21)             { s_bj_local.result =  2; s_bj_local.wins++;   }
        else                          { s_bj_local.result = -1; s_bj_local.losses++; }
    }
}
static void bj1_dealer_play(void) {
    bool soft;
    for (;;) {
        uint8_t t = hand_total(s_bj_local.dealer, s_bj_local.dealer_n, &soft);
        if (t < 17 || (t == 17 && soft)) {
            if (s_bj_local.dealer_n >= 10) break;
            s_bj_local.dealer[s_bj_local.dealer_n++] = bj1_draw();
        } else break;
    }
}
static void bj1_resolve(void) {
    bool soft;
    uint8_t p = hand_total(s_bj_local.player, s_bj_local.player_n, &soft);
    uint8_t d = hand_total(s_bj_local.dealer, s_bj_local.dealer_n, &soft);
    if (p > 21)       { s_bj_local.result = -1; s_bj_local.losses++; }
    else if (d > 21)  { s_bj_local.result =  1; s_bj_local.wins++;   }
    else if (p > d)   { s_bj_local.result =  1; s_bj_local.wins++;   }
    else if (p < d)   { s_bj_local.result = -1; s_bj_local.losses++; }
    else              { s_bj_local.result =  0; s_bj_local.pushes++; }
    s_bj_local.phase = BJ_ROUND_END;
}

// ── Mesh host logic ──────────────────────────────────────────
static void bjh_shuffle(void) {
    for (int i = 0; i < 52; i++) s_host_deck[i] = i;
    for (int i = 51; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        uint8_t t = s_host_deck[i]; s_host_deck[i] = s_host_deck[j]; s_host_deck[j] = t;
    }
    s_host_deck_pos = 0;
}
static uint8_t bjh_draw(void) {
    if (s_host_deck_pos >= 52) bjh_shuffle();
    return s_host_deck[s_host_deck_pos++];
}

// Flagged when host mutates s_bj_wire from a non-main core (RX task
// running bj_on_action). bj_host_tick() consumes on the LVGL thread.
static volatile bool s_host_render_pending = false;

static void bjh_broadcast(void) {
    game_host_broadcast_state(s_bj_wire.active_seat, s_bj_wire.phase,
                              (const uint8_t*)&s_bj_wire, sizeof(s_bj_wire));
    s_bj_wire_valid = true;
    s_host_last_broadcast_ms = millis();
    s_host_render_pending = true;
}

static void bjh_advance_active(void) {
    // Move to next seat that isn't already done; if none, dealer plays.
    uint8_t n = s_bj_wire.num_seats;
    for (uint8_t step = 1; step <= n; step++) {
        uint8_t cand = (s_bj_wire.active_seat + step) % n;
        uint8_t f = s_bj_wire.seat_flags[cand];
        if (!(f & (BJ_SEAT_BUST | BJ_SEAT_STAND | BJ_SEAT_BJ))) {
            s_bj_wire.active_seat = cand;
            s_bj_wire.phase = BJ_PLAYER_TURN;
            return;
        }
    }
    // All seats settled — dealer turn.
    s_bj_wire.phase       = BJ_DEALER_TURN;
    s_bj_wire.active_seat = BJ_DEALER_SEAT;
}

static void bjh_dealer_play(void) {
    bool soft;
    for (;;) {
        uint8_t t = hand_total(s_bj_wire.dealer, s_bj_wire.dealer_n, &soft);
        if (t < 17 || (t == 17 && soft)) {
            if (s_bj_wire.dealer_n >= 10) break;
            s_bj_wire.dealer[s_bj_wire.dealer_n++] = bjh_draw();
        } else break;
    }
}

static void bjh_resolve_round(void) {
    bool soft;
    uint8_t d = hand_total(s_bj_wire.dealer, s_bj_wire.dealer_n, &soft);
    bool dealer_bust = (d > 21);
    for (uint8_t i = 0; i < s_bj_wire.num_seats; i++) {
        if (s_bj_wire.seat_flags[i] & BJ_SEAT_BJ) { s_bj_wire.seat_result[i] = 2; continue; }
        if (s_bj_wire.seat_flags[i] & BJ_SEAT_BUST) { s_bj_wire.seat_result[i] = -1; continue; }
        uint8_t p = hand_total(s_bj_wire.seats[i], s_bj_wire.seat_n[i], &soft);
        if (dealer_bust || p > d) s_bj_wire.seat_result[i] =  1;
        else if (p < d)           s_bj_wire.seat_result[i] = -1;
        else                      s_bj_wire.seat_result[i] =  0;
    }
    // Settle bets
    for (uint8_t i = 0; i < s_bj_wire.num_seats; i++) {
        uint16_t bet = s_bj_wire.seat_bet[i];
        int8_t r = s_bj_wire.seat_result[i];
        if      (r ==  2) s_bj_wire.seat_chips[i] += bet * 3 / 2;  // BJ pays 3:2
        else if (r ==  1) s_bj_wire.seat_chips[i] += bet;           // win 1:1
        else if (r == -1) {
            s_bj_wire.seat_chips[i] = (s_bj_wire.seat_chips[i] >= bet)
                                    ? s_bj_wire.seat_chips[i] - bet : 0;
        }
        // push (r == 0): no change
        // Rebuy if broke
        if (s_bj_wire.seat_chips[i] < BJ_MIN_BET) {
            s_bj_wire.seat_chips[i] = BJ_START_CHIPS;
            s_bj_wire.seat_bet[i]   = BJ_DEFAULT_BET;
        }
        // Cap bet to available chips
        if (s_bj_wire.seat_bet[i] > s_bj_wire.seat_chips[i]) {
            s_bj_wire.seat_bet[i] = (s_bj_wire.seat_chips[i] / BJ_BET_STEP) * BJ_BET_STEP;
            if (s_bj_wire.seat_bet[i] < BJ_MIN_BET) s_bj_wire.seat_bet[i] = BJ_MIN_BET;
        }
    }
    s_bj_wire.phase       = BJ_ROUND_END;
    s_bj_wire.active_seat = 0xFF;
    s_host_round_end_ms   = millis();
}

static void bjh_deal_round(void) {
    // Fresh round: clear hands + flags, deal 2 cards each.
    if (s_host_deck_pos > 32) bjh_shuffle();   // reshuffle before running low
    s_bj_wire.round_no++;
    s_bj_wire.dealer_n = 0;
    for (uint8_t i = 0; i < GAME_MAX_PLAYERS; i++) {
        s_bj_wire.seat_n[i]      = 0;
        s_bj_wire.seat_flags[i]  = 0;
        s_bj_wire.seat_result[i] = -2;
    }
    // Player first card
    for (uint8_t i = 0; i < s_bj_wire.num_seats; i++) {
        s_bj_wire.seats[i][s_bj_wire.seat_n[i]++] = bjh_draw();
    }
    s_bj_wire.dealer[s_bj_wire.dealer_n++] = bjh_draw();
    for (uint8_t i = 0; i < s_bj_wire.num_seats; i++) {
        s_bj_wire.seats[i][s_bj_wire.seat_n[i]++] = bjh_draw();
    }
    s_bj_wire.dealer[s_bj_wire.dealer_n++] = bjh_draw();

    // Natural-blackjack check per seat.
    bool soft;
    for (uint8_t i = 0; i < s_bj_wire.num_seats; i++) {
        if (hand_total(s_bj_wire.seats[i], s_bj_wire.seat_n[i], &soft) == 21)
            s_bj_wire.seat_flags[i] |= BJ_SEAT_BJ;
    }
    uint8_t d = hand_total(s_bj_wire.dealer, s_bj_wire.dealer_n, &soft);
    bool dealer_bj = (d == 21);

    // Determine starting state.
    if (dealer_bj) {
        bjh_resolve_round();
    } else {
        s_bj_wire.phase = BJ_PLAYER_TURN;
        s_bj_wire.active_seat = 0;
        // Skip past any seat that already has a natural BJ.
        if (s_bj_wire.seat_flags[0] & BJ_SEAT_BJ) bjh_advance_active();
        if (s_bj_wire.phase == BJ_DEALER_TURN) {
            bjh_dealer_play();
            bjh_resolve_round();
        }
    }
}

// ── UI callbacks ─────────────────────────────────────────────
static void bj_hit_cb(lv_event_t* e) {
    (void)e;
    if (s_mode == BJ_MODE_1P) {
        if (s_bj_local.phase != BJ_PLAYER_TURN) return;
        if (s_bj_local.player_n >= 10) return;
        s_bj_local.player[s_bj_local.player_n++] = bj1_draw();
        sfx_tone(880, 40);
        bool soft;
        if (hand_total(s_bj_local.player, s_bj_local.player_n, &soft) > 21) bj1_resolve();
        bj_render();
    } else if (s_mode == BJ_MODE_MESH) {
        sfx_tone(880, 40);
        if (game_host_started()) {
            // Host: process own action directly (game_send_action is player-only)
            uint8_t me = game_my_seat();
            if (me >= GAME_MAX_PLAYERS) return;
            if (s_bj_wire.phase != BJ_PLAYER_TURN || s_bj_wire.active_seat != me) return;
            if (s_bj_wire.seat_n[me] >= 10) return;
            s_bj_wire.seats[me][s_bj_wire.seat_n[me]++] = bjh_draw();
            bool soft;
            uint8_t t = hand_total(s_bj_wire.seats[me], s_bj_wire.seat_n[me], &soft);
            if (t > 21) { s_bj_wire.seat_flags[me] |= BJ_SEAT_BUST; bjh_advance_active(); }
            else if (t == 21) { s_bj_wire.seat_flags[me] |= BJ_SEAT_STAND; bjh_advance_active(); }
            if (s_bj_wire.phase == BJ_DEALER_TURN) {
                bjh_dealer_play();
                bjh_resolve_round();
            }
            bjh_broadcast();
        } else {
            game_send_action(BJ_ACT_HIT, NULL, 0);
        }
    }
}

static void bj_stand_cb(lv_event_t* e) {
    (void)e;
    if (s_mode == BJ_MODE_1P) {
        if (s_bj_local.phase != BJ_PLAYER_TURN) return;
        s_bj_local.phase = BJ_DEALER_TURN;
        bj_render();
        lv_timer_handler();
        delay(250);
        bj1_dealer_play();
        bj1_resolve();
        bj_render();
    } else if (s_mode == BJ_MODE_MESH) {
        sfx_tone(440, 40);
        if (game_host_started()) {
            // Host: process own action directly
            uint8_t me = game_my_seat();
            if (me >= GAME_MAX_PLAYERS) return;
            if (s_bj_wire.phase != BJ_PLAYER_TURN || s_bj_wire.active_seat != me) return;
            s_bj_wire.seat_flags[me] |= BJ_SEAT_STAND;
            bjh_advance_active();
            if (s_bj_wire.phase == BJ_DEALER_TURN) {
                bjh_dealer_play();
                bjh_resolve_round();
            }
            bjh_broadcast();
        } else {
            game_send_action(BJ_ACT_STAND, NULL, 0);
        }
    }
}

static void bj_deal_cb(lv_event_t* e) {
    (void)e;
    if (s_mode == BJ_MODE_1P) {
        if (s_bj_local.phase != BJ_IDLE && s_bj_local.phase != BJ_ROUND_END) return;
        bj1_deal();
        sfx_tone(660, 40);
        bj_render();
    } else if (s_mode == BJ_MODE_MESH && game_host_started()) {
        if (s_bj_wire.phase != BJ_ROUND_END) return;
        bjh_deal_round();
        bjh_broadcast();
    }
}

static void bj_bet_up_cb(lv_event_t* e) {
    (void)e;
    if (s_mode == BJ_MODE_1P) {
        if (s_1p_bet + 5 <= s_1p_chips) s_1p_bet += 5;
        sfx_tone(500, 30);
        bj_render();
    } else if (s_mode == BJ_MODE_MESH) {
        if (s_bj_wire.phase != BJ_ROUND_END) return;
        sfx_tone(500, 30);
        if (game_host_started()) {
            uint8_t me = game_my_seat();
            if (me >= GAME_MAX_PLAYERS) return;
            if (s_bj_wire.seat_bet[me] + BJ_BET_STEP <= s_bj_wire.seat_chips[me]) {
                s_bj_wire.seat_bet[me] += BJ_BET_STEP;
                bjh_broadcast();
            }
        } else {
            game_send_action(BJ_ACT_BET_UP, NULL, 0);
        }
    }
}
static void bj_bet_down_cb(lv_event_t* e) {
    (void)e;
    if (s_mode == BJ_MODE_1P) {
        if (s_1p_bet > 5) s_1p_bet -= 5;
        sfx_tone(400, 30);
        bj_render();
    } else if (s_mode == BJ_MODE_MESH) {
        if (s_bj_wire.phase != BJ_ROUND_END) return;
        sfx_tone(400, 30);
        if (game_host_started()) {
            uint8_t me = game_my_seat();
            if (me >= GAME_MAX_PLAYERS) return;
            if (s_bj_wire.seat_bet[me] > BJ_MIN_BET) {
                s_bj_wire.seat_bet[me] -= BJ_BET_STEP;
                bjh_broadcast();
            }
        } else {
            game_send_action(BJ_ACT_BET_DOWN, NULL, 0);
        }
    }
}

static void bj_back_cb(lv_event_t* e) {
    (void)e;
    if (s_mode == BJ_MODE_MESH) {
        if (game_host_started()) {
            // Find the richest player for the summary
            uint8_t best = 0;
            for (uint8_t i = 1; i < s_bj_wire.num_seats; i++) {
                if (s_bj_wire.seat_chips[i] > s_bj_wire.seat_chips[best]) best = i;
            }
            char names[GAME_MAX_PLAYERS][GAME_NAME_LEN];
            game_seats(NULL, names);
            char summary[64];
            snprintf(summary, sizeof(summary), "%s WINS! $%u",
                     names[best][0] ? names[best] : "?",
                     s_bj_wire.seat_chips[best]);
            game_host_end(&best, 1, summary);
        } else {
            game_player_leave();
        }
    }
    s_mode = BJ_MODE_NONE;
    apps_open_hub();
}

// ── Screen builder ───────────────────────────────────────────
// Upgraded button with more visual weight
static lv_obj_t* mk_bjbtn(lv_obj_t* par, const char* txt, int x, int y, int w, int h,
                          lv_event_cb_t cb, lv_color_t border_clr) {
    lv_obj_t* b = lv_obj_create(par);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x0a2a10), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, border_clr, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    // Pressed visual
    lv_obj_set_style_bg_color(b, lv_color_hex(0x004420),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    // Disabled visual
    lv_obj_set_style_bg_opa(b, LV_OPA_40,
                            LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_border_opa(b, LV_OPA_40,
                                LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(l, border_clr, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
    return b;
}

static lv_obj_t* screen_blackjack_game_create(BjScreenMode mode) {
    s_mode = mode;

    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, CLR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── Header ──
    lv_obj_t* hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, 320, 20);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, CLR_HEADER, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(hdr, CLR_GOLD, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* ht = lv_label_create(hdr);
    lv_label_set_text(ht, "BLACKJACK");
    lv_obj_set_style_text_font(ht, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(ht, CLR_GOLD, 0);
    lv_obj_align(ht, LV_ALIGN_LEFT_MID, 6, 0);

    s_lbl_score = lv_label_create(hdr);
    lv_obj_set_style_text_font(s_lbl_score, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_lbl_score, CLR_GOLDD, 0);
    lv_obj_align(s_lbl_score, LV_ALIGN_RIGHT_MID, -6, 0);

    // ── Table felt ──
    s_table = lv_obj_create(scr);
    lv_obj_set_size(s_table, 316, 100);
    lv_obj_set_pos(s_table, 2, 22);
    lv_obj_set_style_bg_color(s_table, CLR_FELT, 0);
    lv_obj_set_style_bg_opa(s_table, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_table, CLR_GOLDD, 0);
    lv_obj_set_style_border_width(s_table, 2, 0);
    lv_obj_set_style_radius(s_table, 8, 0);
    lv_obj_set_style_pad_all(s_table, 0, 0);
    lv_obj_clear_flag(s_table, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Dealer label + total
    s_lbl_dlr_tag = lv_label_create(s_table);
    lv_label_set_text(s_lbl_dlr_tag, "DEALER");
    lv_obj_set_style_text_font(s_lbl_dlr_tag, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_lbl_dlr_tag, CLR_GOLDD, 0);
    lv_obj_set_pos(s_lbl_dlr_tag, 4, 8);

    s_lbl_dlr_total = lv_label_create(s_table);
    lv_obj_set_style_text_font(s_lbl_dlr_total, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_lbl_dlr_total, CLR_GREEN, 0);
    lv_obj_set_pos(s_lbl_dlr_total, 270, 14);

    // Horizontal divider
    lv_obj_t* div = lv_obj_create(s_table);
    lv_obj_set_size(div, 296, 1);
    lv_obj_set_pos(div, 8, 34);
    lv_obj_set_style_bg_color(div, CLR_GOLDD, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_60, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_set_style_pad_all(div, 0, 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Player label + total
    s_lbl_you_tag = lv_label_create(s_table);
    lv_label_set_text(s_lbl_you_tag, "YOU");
    lv_obj_set_style_text_font(s_lbl_you_tag, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_lbl_you_tag, CLR_GREEN, 0);
    lv_obj_set_pos(s_lbl_you_tag, 4, 42);

    s_lbl_you_total = lv_label_create(s_table);
    lv_obj_set_style_text_font(s_lbl_you_total, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_lbl_you_total, CLR_GREEN, 0);
    lv_obj_set_pos(s_lbl_you_total, 270, 48);

    // Other players (mesh only)
    s_lbl_others = lv_label_create(s_table);
    lv_obj_set_style_text_font(s_lbl_others, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_lbl_others, CLR_GOLDD, 0);
    lv_obj_set_pos(s_lbl_others, 4, 78);
    if (mode == BJ_MODE_1P) lv_obj_add_flag(s_lbl_others, LV_OBJ_FLAG_HIDDEN);

    // Pre-create card objects inside the table
    for (int i = 0; i < MAX_HAND; i++) {
        s_dc[i] = mk_card(s_table, &s_dc_lbl[i]);
        s_pc[i] = mk_card(s_table, &s_pc_lbl[i]);
    }

    // ── Status line ──
    s_lbl_status = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_lbl_status, CLR_GOLD, 0);
    lv_obj_set_pos(s_lbl_status, 8, 126);

    // ── Buttons ──
    // Row 1: HIT / STAND / BET+ / BET-
    s_btn_hit    = mk_bjbtn(scr, "HIT",    4, 142, 74, 34, bj_hit_cb, CLR_GREEN);
    s_btn_stand  = mk_bjbtn(scr, "STAND", 82, 142, 74, 34, bj_stand_cb, CLR_GREEN);
    s_btn_bet_up   = mk_bjbtn(scr, "BET+", 160, 142, 74, 34, bj_bet_up_cb, CLR_GOLD);
    s_btn_bet_down = mk_bjbtn(scr, "BET-", 238, 142, 74, 34, bj_bet_down_cb, CLR_GOLD);
    // Row 2: BACK / DEAL (DEAL only in 1P — mesh auto-deals)
    mk_bjbtn(scr, "< BACK", 4, 180, 80, 28, bj_back_cb, CLR_GOLDD);
    if (mode == BJ_MODE_1P) {
        s_btn_deal = mk_bjbtn(scr, "DEAL", 236, 180, 80, 28, bj_deal_cb, CLR_GREEN);
    } else {
        s_btn_deal = NULL;
    }

    // ── Cleanup handler ──
    lv_obj_add_event_cb(scr, [](lv_event_t* e){
        (void)e;
        s_bj_scr = NULL;
        s_table = NULL;
        s_lbl_dlr_tag = s_lbl_dlr_total = NULL;
        s_lbl_you_tag = s_lbl_you_total = NULL;
        s_lbl_others = s_lbl_status = s_lbl_score = NULL;
        s_btn_hit = s_btn_stand = s_btn_deal = NULL;
        s_btn_bet_up = s_btn_bet_down = NULL;
        memset(s_dc, 0, sizeof(s_dc));
        memset(s_pc, 0, sizeof(s_pc));
        memset(s_dc_lbl, 0, sizeof(s_dc_lbl));
        memset(s_pc_lbl, 0, sizeof(s_pc_lbl));
    }, LV_EVENT_DELETE, NULL);

    s_bj_scr = scr;

    if (mode == BJ_MODE_1P) {
        if (s_bj_local.wins == 0 && s_bj_local.losses == 0 && s_bj_local.pushes == 0) {
            bj1_shuffle();
            s_bj_local.phase = BJ_IDLE;
        }
    }
    bj_render();
    return scr;
}

// ── Mesh lobby ───────────────────────────────────────────────
static lv_obj_t*   s_lobby_scr     = NULL;
static lv_obj_t*   s_lobby_status  = NULL;
static lv_obj_t*   s_lobby_seats   = NULL;
static lv_obj_t*   s_btn_start_now = NULL;
static lv_timer_t* s_lobby_timer   = NULL;
static bool        s_lobby_am_host = false;
static bool        s_lobby_closing = false;
static unsigned long s_lobby_created_ms = 0;
#define LOBBY_PLAYER_TIMEOUT_MS 45000

static void lobby_tick(lv_timer_t* t) {
    (void)t;
    if (!s_lobby_status || s_lobby_closing) return;

    // Detect session cancellation / expiry and auto-return.
    bool should_close = false;
    const char* close_msg = NULL;
    if (s_lobby_am_host) {
        // Host: invite expired without reaching quorum → game_host_cancel()
        // already fired, session is gone.
        if (!game_host_is_inviting() && !game_host_started()) {
            should_close = true;
            close_msg = "NO PLAYERS JOINED\nRETURNING...";
        }
    } else {
        // Player: host canceled, or session ended externally.
        if (!game_in_session()) {
            should_close = true;
            close_msg = "HOST CANCELED\nRETURNING...";
        }
        // Safety timeout — if we never receive START in 45s, bail.
        if (!should_close &&
            (millis() - s_lobby_created_ms) > LOBBY_PLAYER_TIMEOUT_MS) {
            should_close = true;
            close_msg = "TIMED OUT\nRETURNING...";
        }
    }
    if (should_close) {
        s_lobby_closing = true;
        // Reset player session state so future invites aren't blocked.
        if (!s_lobby_am_host) game_player_leave();
        lv_label_set_text(s_lobby_status, close_msg);
        if (s_lobby_seats) lv_label_set_text(s_lobby_seats, "");
        if (s_btn_start_now) set_dis(s_btn_start_now, true);
        lv_timer_create([](lv_timer_t* t2){
            lv_timer_del(t2);
            apps_open_hub();
        }, 2000, NULL);
        return;
    }

    if (s_lobby_am_host) {
        uint8_t n = game_seat_count();
        char names[GAME_MAX_PLAYERS][GAME_NAME_LEN];
        game_seats(NULL, names);
        char buf[160] = "";
        size_t used = 0;
        for (uint8_t i = 0; i < n; i++) {
            int w = snprintf(buf + used, sizeof(buf) - used, "  %u. %s%s\n",
                             i + 1, names[i][0] ? names[i] : "?",
                             i == 0 ? " (you)" : "");
            if (w <= 0 || (size_t)w >= sizeof(buf) - used) break;
            used += w;
        }
        lv_label_set_text(s_lobby_seats, buf);
        lv_label_set_text_fmt(s_lobby_status,
                              "WAITING FOR PLAYERS (%u/6)\nTAP START NOW WHEN READY",
                              (unsigned)n);
        set_dis(s_btn_start_now, n < 2);
    } else {
        lv_label_set_text(s_lobby_status,
                          "JOINED -- WAITING FOR HOST\nto START the game...");
    }
}

static void lobby_close(void) {
    if (s_lobby_timer) { lv_timer_del(s_lobby_timer); s_lobby_timer = NULL; }
    s_lobby_scr     = NULL;
    s_lobby_status  = NULL;
    s_lobby_seats   = NULL;
    s_btn_start_now = NULL;
}

static void lobby_back_cb(lv_event_t* e) {
    (void)e;
    Serial.println("[bj] lobby_back tapped");
    if (game_host_is_inviting()) game_host_cancel();
    else                         game_player_leave();
    apps_open_hub();
}

static void lobby_start_cb(lv_event_t* e) {
    (void)e;
    Serial.println("[bj] lobby_start tapped");
    if (!s_btn_start_now) return;
    if (lv_obj_has_state(s_btn_start_now, LV_STATE_DISABLED)) {
        Serial.println("[bj] ...but START NOW is disabled (need 2+ players)");
        return;
    }
    game_host_start();
}

static lv_obj_t* screen_blackjack_lobby_create(bool am_host) {
    s_lobby_am_host = am_host;
    s_lobby_closing = false;
    s_lobby_created_ms = millis();

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
    lv_label_set_text(ht, am_host ? "** BLACKJACK -- HOST LOBBY **"
                                   : "** BLACKJACK -- JOINED **");
    lv_obj_set_style_text_font(ht, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(ht, CLR_GREEN, 0);
    lv_obj_align(ht, LV_ALIGN_CENTER, 0, 0);

    s_lobby_status = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lobby_status, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_lobby_status, CLR_GOLD, 0);
    lv_obj_set_style_text_align(s_lobby_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lobby_status, am_host ? "WAITING FOR PLAYERS"
                                                : "WAITING FOR HOST");
    lv_obj_align(s_lobby_status, LV_ALIGN_TOP_MID, 0, 30);

    s_lobby_seats = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lobby_seats, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_lobby_seats, CLR_GREEN, 0);
    lv_label_set_text(s_lobby_seats, "");
    lv_obj_set_pos(s_lobby_seats, 40, 80);

    if (am_host) {
        s_btn_start_now = mk_bjbtn(scr, "START NOW",  60, 160, 200, 36, lobby_start_cb, CLR_GREEN);
        set_dis(s_btn_start_now, true);
    }
    mk_bjbtn(scr, am_host ? "CANCEL" : "< BACK", 112, 205, 96, 30, lobby_back_cb, CLR_GOLDD);

    if (s_lobby_timer) { lv_timer_del(s_lobby_timer); s_lobby_timer = NULL; }
    s_lobby_timer = lv_timer_create(lobby_tick, 500, NULL);

    lv_obj_add_event_cb(scr, [](lv_event_t* e){
        (void)e;
        lobby_close();
    }, LV_EVENT_DELETE, NULL);

    s_lobby_scr = scr;
    lobby_tick(NULL);
    return scr;
}

// ── Menu (1P vs MESH) ────────────────────────────────────────
static void menu_1p_cb(lv_event_t* e) {
    (void)e;
    apps_swap_sub_scr(screen_blackjack_game_create(BJ_MODE_1P));
}
static void menu_mesh_cb(lv_event_t* e) {
    (void)e;
    Serial.println("[bj] menu_mesh_cb: enter");
    game_host_invite(GAME_BLACKJACK, 2, 6);
    Serial.println("[bj] menu_mesh_cb: invite sent");
    sfx_tone(600, 60);
    lv_obj_t* lobby = screen_blackjack_lobby_create(true);
    Serial.println("[bj] menu_mesh_cb: lobby created");
    apps_swap_sub_scr(lobby);
    Serial.println("[bj] menu_mesh_cb: done");
}
static void menu_back_cb(lv_event_t* e) {
    (void)e;
    apps_open_hub();
}

extern "C" lv_obj_t* screen_blackjack_menu_create(void) {
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
    lv_label_set_text(ht, "** BLACKJACK **");
    lv_obj_set_style_text_font(ht, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(ht, CLR_GREEN, 0);
    lv_obj_align(ht, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* sub = lv_label_create(scr);
    lv_label_set_text(sub, "SELECT MODE");
    lv_obj_set_style_text_font(sub, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(sub, CLR_GOLD, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 40);

    mk_bjbtn(scr, "1-PLAYER",     60, 90,  200, 44, menu_1p_cb, CLR_GREEN);
    mk_bjbtn(scr, "MESH (HOST)",  60, 142, 200, 44, menu_mesh_cb, CLR_GREEN);
    mk_bjbtn(scr, "< BACK",       10, 204,  96, 28, menu_back_cb, CLR_GOLDD);
    return scr;
}

// ── GameModule callbacks ─────────────────────────────────────
static void bj_on_state(const GameState* s, bool is_host) {
    (void)is_host;
    if (!s || s->state_len != sizeof(BjWire)) return;
    uint8_t old_phase = s_bj_wire.phase;
    memcpy(&s_bj_wire, s->state_buf, sizeof(BjWire));
    s_bj_wire_valid = true;
    // Track when ROUND_END began so the player can show the bet countdown.
    if (s_bj_wire.phase == BJ_ROUND_END && old_phase != BJ_ROUND_END) {
        s_host_round_end_ms = millis();
    }
    bj_render();
}

static void bj_on_action(const GameAction* a, const uint8_t* from_mac) {
    if (!a || !game_host_started()) return;
    // Find which seat sent this.
    uint8_t macs[GAME_MAX_PLAYERS][6];
    game_seats(macs, NULL);
    uint8_t seat = 0xFF;
    for (uint8_t i = 0; i < game_seat_count(); i++) {
        if (memcmp(macs[i], from_mac, 6) == 0) { seat = i; break; }
    }
    if (seat == 0xFF || seat >= s_bj_wire.num_seats) return;

    // Bet actions — any seat can adjust during ROUND_END
    if (a->action_id == BJ_ACT_BET_UP) {
        if (s_bj_wire.phase != BJ_ROUND_END) return;
        if (s_bj_wire.seat_bet[seat] + BJ_BET_STEP <= s_bj_wire.seat_chips[seat]) {
            s_bj_wire.seat_bet[seat] += BJ_BET_STEP;
        }
        bjh_broadcast();
        return;
    }
    if (a->action_id == BJ_ACT_BET_DOWN) {
        if (s_bj_wire.phase != BJ_ROUND_END) return;
        if (s_bj_wire.seat_bet[seat] > BJ_MIN_BET) {
            s_bj_wire.seat_bet[seat] -= BJ_BET_STEP;
        }
        bjh_broadcast();
        return;
    }

    // Game actions — only the active seat during PLAYER_TURN
    if (s_bj_wire.phase != BJ_PLAYER_TURN) return;
    if (seat != s_bj_wire.active_seat) return;

    if (a->action_id == BJ_ACT_HIT) {
        if (s_bj_wire.seat_n[seat] >= 10) return;
        s_bj_wire.seats[seat][s_bj_wire.seat_n[seat]++] = bjh_draw();
        bool soft;
        uint8_t t = hand_total(s_bj_wire.seats[seat], s_bj_wire.seat_n[seat], &soft);
        if (t > 21) { s_bj_wire.seat_flags[seat] |= BJ_SEAT_BUST; bjh_advance_active(); }
        else if (t == 21) { s_bj_wire.seat_flags[seat] |= BJ_SEAT_STAND; bjh_advance_active(); }
    } else if (a->action_id == BJ_ACT_STAND) {
        s_bj_wire.seat_flags[seat] |= BJ_SEAT_STAND;
        bjh_advance_active();
    }
    if (s_bj_wire.phase == BJ_DEALER_TURN) {
        bjh_dealer_play();
        bjh_resolve_round();
    }
    bjh_broadcast();
}

static void bj_host_tick(void) {
    unsigned long now = millis();

    // Consume any pending host-side render flagged from the RX task.
    if (s_host_render_pending) {
        s_host_render_pending = false;
        bj_render();
    }

    // Re-broadcast STATE after game start so players who received
    // START from a retransmit (they were on a different hop channel
    // when the initial STATE was sent) catch up on the deal.
    if (s_host_state_retx_left > 0) {
        if ((now - s_host_state_retx_ms) >= STATE_INITIAL_RETX_MS) {
            s_host_state_retx_ms = now;
            s_host_state_retx_left--;
            bjh_broadcast();
        }
    }

    // Periodic STATE re-broadcast during active gameplay. A single
    // ESP-NOW broadcast is easily missed on channel hops — without
    // this, a missed STATE freezes the game (both sides think it's
    // the other player's turn).
    if (s_bj_wire_valid && s_host_state_retx_left == 0 &&
        (now - s_host_last_broadcast_ms) >= STATE_REBROADCAST_MS) {
        bjh_broadcast();
    }

    // Auto-deal: after ROUND_END, wait for the bet phase timer then
    // start the next round automatically (no NEXT button needed).
    if (s_bj_wire.phase == BJ_ROUND_END && s_host_round_end_ms > 0 &&
        (now - s_host_round_end_ms) >= AUTO_DEAL_DELAY_MS) {
        Serial.println("[bj] auto-deal (bet timer expired)");
        bjh_deal_round();
        bjh_broadcast();
    }

    // Keep host render fresh during bet countdown so the timer updates.
    if (s_bj_wire.phase == BJ_ROUND_END) bj_render();
}

static void bj_on_start(uint8_t seat_count) {
    // Both host and player arrive here. Host: deal round 1 and
    // broadcast. Player: just open the game screen; first STATE
    // will populate s_bj_wire shortly.
    memset(&s_bj_wire, 0, sizeof(s_bj_wire));
    s_bj_wire.num_seats = seat_count;
    s_bj_wire.phase     = BJ_PLAYER_TURN;
    s_bj_wire.active_seat = 0xFF;
    s_bj_wire_valid = false;
    // Everyone starts with 100 chips, 10 bet
    for (uint8_t i = 0; i < seat_count && i < GAME_MAX_PLAYERS; i++) {
        s_bj_wire.seat_chips[i] = BJ_START_CHIPS;
        s_bj_wire.seat_bet[i]   = BJ_DEFAULT_BET;
    }

    apps_swap_sub_scr(screen_blackjack_game_create(BJ_MODE_MESH));

    if (game_host_started()) {
        bjh_shuffle();
        bjh_deal_round();
        bjh_broadcast();
        // Kick off STATE retransmissions — the player may receive
        // START from a retransmit and still be waiting for the
        // first STATE, which was sent once and easily missed.
        s_host_state_retx_left = STATE_INITIAL_RETX_COUNT;
        s_host_state_retx_ms   = millis();
    }
}

static void bj_build_ui(void) {
    // Invitee just tapped ACCEPT -- land them in the joined lobby.
    apps_swap_sub_scr(screen_blackjack_lobby_create(false));
}

static const GameModule s_bj_module = {
    GAME_BLACKJACK,
    "BLACKJACK",
    bj_on_state,
    bj_on_action,
    bj_host_tick,
    bj_build_ui,
    bj_on_start,
};

void blackjack_init(void) {
    game_register(&s_bj_module);
}
