// ============================================================
// Vault 89 PDI-Boy -- Snake Mini-Game
// 20x15 grid of 12x12 cells. Touch the arrow buttons to turn.
// Eat the gold pellet to grow. Hit yourself or a wall = game over.
// ============================================================
#include <Arduino.h>
#include <lvgl.h>
#include <string.h>
#include "pipboy_data.h"
#include "sound_fx.h"

extern void apps_open_hub(void);

#define GRID_W 20
#define GRID_H 15
#define CELL   12
#define BOARD_X 40
#define BOARD_Y 28   // below header

#define DIR_UP    0
#define DIR_RIGHT 1
#define DIR_DOWN  2
#define DIR_LEFT  3

struct Pt { int8_t x, y; };

#define SNAKE_MAX 128

static lv_obj_t*   s_scr       = NULL;
static lv_obj_t*   s_board     = NULL;   // draws the game
static lv_obj_t*   s_score_lbl = NULL;
static lv_obj_t*   s_state_lbl = NULL;
static lv_timer_t* s_tmr       = NULL;

static Pt    s_snake[SNAKE_MAX];
static int   s_len      = 0;
static int   s_dir      = DIR_RIGHT;
static int   s_next_dir = DIR_RIGHT;
static Pt    s_food     = {0, 0};
static int   s_score    = 0;
static bool  s_alive    = false;
static uint32_t s_last_step_ms = 0;
static uint32_t s_step_ms      = 180;

static void place_food(void) {
    for (int tries = 0; tries < 200; tries++) {
        int x = esp_random() % GRID_W;
        int y = esp_random() % GRID_H;
        bool hit = false;
        for (int i = 0; i < s_len; i++) {
            if (s_snake[i].x == x && s_snake[i].y == y) { hit = true; break; }
        }
        if (!hit) { s_food = {(int8_t)x, (int8_t)y}; return; }
    }
}

static void reset_game(void) {
    s_len = 4;
    for (int i = 0; i < s_len; i++) s_snake[i] = {(int8_t)(8 - i), (int8_t)7};
    s_dir = DIR_RIGHT;
    s_next_dir = DIR_RIGHT;
    s_score = 0;
    s_alive = true;
    s_step_ms = 180;
    s_last_step_ms = millis();
    place_food();
}

static void step_game(void) {
    if (!s_alive) return;
    // Commit the queued turn (but not 180°)
    if ((s_next_dir == DIR_UP    && s_dir != DIR_DOWN)  ||
        (s_next_dir == DIR_DOWN  && s_dir != DIR_UP)    ||
        (s_next_dir == DIR_LEFT  && s_dir != DIR_RIGHT) ||
        (s_next_dir == DIR_RIGHT && s_dir != DIR_LEFT)) {
        s_dir = s_next_dir;
    }
    Pt head = s_snake[0];
    switch (s_dir) {
        case DIR_UP:    head.y--; break;
        case DIR_DOWN:  head.y++; break;
        case DIR_LEFT:  head.x--; break;
        case DIR_RIGHT: head.x++; break;
    }
    if (head.x < 0 || head.x >= GRID_W || head.y < 0 || head.y >= GRID_H) {
        s_alive = false;
        sfx_play(SFX_ERROR);
        return;
    }
    for (int i = 0; i < s_len; i++) {
        if (s_snake[i].x == head.x && s_snake[i].y == head.y) {
            s_alive = false;
            sfx_play(SFX_ERROR);
            return;
        }
    }
    // Shift body
    bool ate = (head.x == s_food.x && head.y == s_food.y);
    if (ate) {
        if (s_len < SNAKE_MAX) s_len++;
        s_score++;
        sfx_tone(1400, 60);
        if (s_step_ms > 70) s_step_ms -= 4;
        place_food();
    }
    for (int i = s_len - 1; i > 0; i--) s_snake[i] = s_snake[i - 1];
    s_snake[0] = head;
}

static void draw_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target(e);
    lv_draw_ctx_t* draw_ctx = lv_event_get_draw_ctx(e);
    lv_area_t base;
    lv_obj_get_coords(obj, &base);

    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_color = CLR_GREEN;
    rect.bg_opa   = LV_OPA_COVER;
    rect.border_width = 0;
    rect.radius = 1;

    // Snake body
    for (int i = 0; i < s_len; i++) {
        lv_area_t a;
        a.x1 = base.x1 + s_snake[i].x * (CELL - 2);
        a.y1 = base.y1 + s_snake[i].y * (CELL - 2);
        a.x2 = a.x1 + (CELL - 3);
        a.y2 = a.y1 + (CELL - 3);
        rect.bg_color = (i == 0) ? CLR_GREEN : lv_color_hex(0x008800);
        lv_draw_rect(draw_ctx, &rect, &a);
    }
    // Food
    rect.bg_color = CLR_GOLD;
    lv_area_t fa;
    fa.x1 = base.x1 + s_food.x * (CELL - 2);
    fa.y1 = base.y1 + s_food.y * (CELL - 2);
    fa.x2 = fa.x1 + (CELL - 3);
    fa.y2 = fa.y1 + (CELL - 3);
    lv_draw_rect(draw_ctx, &rect, &fa);
}

static void refresh_cb(lv_timer_t* t) {
    (void)t;
    if (s_alive) {
        uint32_t now = millis();
        if (now - s_last_step_ms >= s_step_ms) {
            s_last_step_ms = now;
            step_game();
            if (s_board) lv_obj_invalidate(s_board);
        }
    }
    if (s_score_lbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "SCORE: %d", s_score);
        lv_label_set_text(s_score_lbl, buf);
    }
    if (s_state_lbl) {
        lv_label_set_text(s_state_lbl, s_alive ? "" : "GAME OVER - TAP RESTART");
    }
}

static void dir_cb(lv_event_t* e) {
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    s_next_dir = d;
}

static void restart_cb(lv_event_t* e) {
    (void)e;
    reset_game();
    if (s_board) lv_obj_invalidate(s_board);
}
static void back_cb(lv_event_t* e) {
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

lv_obj_t* screen_snake_create(void) {
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
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* ht = lv_label_create(hdr);
    lv_label_set_text(ht, "** SNAKE **");
    lv_obj_set_style_text_font(ht, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(ht, CLR_GREEN, 0);
    lv_obj_align(ht, LV_ALIGN_CENTER, 0, 0);

    // Board: 20x15 cells of 10 px each = 200x150
    s_board = lv_obj_create(scr);
    lv_obj_set_size(s_board, GRID_W * (CELL - 2) + 2, GRID_H * (CELL - 2) + 2);
    lv_obj_set_pos(s_board, 6, 28);
    lv_obj_set_style_bg_color(s_board, CLR_HEADER, 0);
    lv_obj_set_style_bg_opa(s_board, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_board, CLR_GREEN, 0);
    lv_obj_set_style_border_width(s_board, 1, 0);
    lv_obj_set_style_radius(s_board, 2, 0);
    lv_obj_set_style_pad_all(s_board, 0, 0);
    lv_obj_clear_flag(s_board, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_board, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_board, draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    // Score + state labels (right side)
    s_score_lbl = lv_label_create(scr);
    lv_label_set_text(s_score_lbl, "SCORE: 0");
    lv_obj_set_style_text_font(s_score_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_score_lbl, CLR_GOLD, 0);
    lv_obj_set_pos(s_score_lbl, 220, 30);

    s_state_lbl = lv_label_create(scr);
    lv_label_set_text(s_state_lbl, "");
    lv_obj_set_style_text_font(s_state_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(s_state_lbl, CLR_GOLD, 0);
    lv_obj_set_pos(s_state_lbl, 220, 46);
    lv_label_set_long_mode(s_state_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_state_lbl, 96);

    // D-pad buttons on the right
    mk_btn(scr, "^",  244, 62, 30, 24, dir_cb, (void*)(intptr_t)DIR_UP);
    mk_btn(scr, "<",  220, 90, 30, 24, dir_cb, (void*)(intptr_t)DIR_LEFT);
    mk_btn(scr, ">",  268, 90, 30, 24, dir_cb, (void*)(intptr_t)DIR_RIGHT);
    mk_btn(scr, "v",  244,118, 30, 24, dir_cb, (void*)(intptr_t)DIR_DOWN);

    mk_btn(scr, "RESTART", 220, 150, 90, 24, restart_cb);
    mk_btn(scr, "< BACK",    6, 208,  80, 26, back_cb);

    reset_game();
    s_tmr = lv_timer_create(refresh_cb, 30, NULL);

    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        (void)e;
        if (s_tmr) { lv_timer_del(s_tmr); s_tmr = NULL; }
        s_scr = s_board = s_score_lbl = s_state_lbl = NULL;
    }, LV_EVENT_DELETE, NULL);

    s_scr = scr;
    return scr;
}
