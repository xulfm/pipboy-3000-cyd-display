// ============================================================
// Vault 89 PDI-Boy -- DATA Screen
// Matches pipboy_v3.html #screen-data layout
//
// Layout (320x222 usable):
//   Left panel  : 136px wide
//     - Filter tabs: ALL / ACTIVE / DONE (16px h)
//     - List header (quest count)
//     - Scrollable quest list with pip indicators
//     - XP progress bar
//   Right panel : 184px wide
//     - Quest tag (MAIN/SIDE)
//     - Quest name + location
//     - Objectives list with checkboxes
//     - Reward display
// ============================================================

#include <Arduino.h>
#include <lvgl.h>
#include "pipboy_data.h"

static lv_obj_t* scr_data = NULL;

// State
static int  data_filter = 0;  // 0=ALL, 1=ACTIVE, 2=DONE
static int  data_sel    = -1; // selected quest index in filtered list
static int  filtered_indices[QUEST_COUNT];
static int  filtered_count = 0;

// UI pointers
static lv_obj_t* fil_btns[3]     = {};
static lv_obj_t* fil_lbls[3]     = {};
static lv_obj_t* lbl_quest_count = NULL;
static lv_obj_t* quest_list      = NULL;
static lv_obj_t* xp_lbl          = NULL;
static lv_obj_t* xp_fill         = NULL;

// Detail
static lv_obj_t* dq_tag    = NULL;
static lv_obj_t* dq_name   = NULL;
static lv_obj_t* dq_loc    = NULL;
static lv_obj_t* dq_objs   = NULL;
static lv_obj_t* dq_rwd    = NULL;

static int  detail_quest_idx = -1;

static const char* FIL_NAMES[] = {"ALL", "ACTIVE", "DONE"};

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

// ── Compute XP ──
static int compute_total_xp(void) {
    int total = 0;
    for (int i = 0; i < QUEST_COUNT; i++) {
        total += quests[i].xp_reward;
    }
    return total;
}

static int compute_earned_xp(void) {
    int earned = 0;
    for (int i = 0; i < QUEST_COUNT; i++) {
        if (quests[i].done) earned += quests[i].xp_reward;
        // Partial credit for completed objectives
        if (quests[i].obj1_done && quests[i].obj1) earned += 25;
        if (quests[i].obj2_done && quests[i].obj2) earned += 25;
        if (quests[i].obj3_done && quests[i].obj3) earned += 25;
    }
    return earned;
}

// ── Build filter list ──
static void build_filtered(void) {
    filtered_count = 0;
    for (int i = 0; i < QUEST_COUNT; i++) {
        bool include = false;
        if (data_filter == 0) include = true;
        else if (data_filter == 1) include = !quests[i].done;
        else include = quests[i].done;
        if (include) {
            filtered_indices[filtered_count++] = i;
        }
    }
}

static void show_quest_detail(int quest_idx);
static void populate_quests(void);

// ── Objective click ──
static void obj_click_cb(lv_event_t* e) {
    int obj_idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (detail_quest_idx < 0 || detail_quest_idx >= QUEST_COUNT) return;
    Quest& q = quests[detail_quest_idx];
    if (obj_idx == 0 && q.obj1) q.obj1_done = !q.obj1_done;
    else if (obj_idx == 1 && q.obj2) q.obj2_done = !q.obj2_done;
    else if (obj_idx == 2 && q.obj3) q.obj3_done = !q.obj3_done;
    show_quest_detail(detail_quest_idx);
    populate_quests();
    // Re-highlight the selected quest row
    if (data_sel >= 0) {
        uint32_t cnt = lv_obj_get_child_cnt(quest_list);
        for (uint32_t i = 0; i < cnt; i++) {
            lv_obj_t* row = lv_obj_get_child(quest_list, i);
            if ((int)i == data_sel) {
                lv_obj_set_style_bg_color(row, CLR_SEL, 0);
                lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
                lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
                lv_obj_set_style_border_color(row, CLR_GREEN, 0);
                lv_obj_set_style_border_width(row, 2, 0);
            }
        }
    }
}

// ── Show quest detail ──
static void show_quest_detail(int quest_idx) {
    detail_quest_idx = quest_idx;
    const Quest& q = quests[quest_idx];

    // Tag
    if (q.main_quest) {
        lv_label_set_text(dq_tag, "MAIN QUEST");
        lv_obj_set_style_bg_color(lv_obj_get_parent(dq_tag), CLR_GOLDD, 0);
        lv_obj_set_style_text_color(dq_tag, lv_color_hex(0x1a0f00), 0);
    } else {
        lv_label_set_text(dq_tag, "SIDE QUEST");
        lv_obj_set_style_bg_color(lv_obj_get_parent(dq_tag), CLR_DARK, 0);
        lv_obj_set_style_text_color(dq_tag, CLR_DIM, 0);
    }

    // Name
    lv_label_set_text(dq_name, q.name);
    lv_obj_set_style_text_color(dq_name, q.main_quest ? CLR_GOLD : CLR_GREEN, 0);

    // Location
    lv_label_set_text(dq_loc, q.location);

    // Objectives
    lv_obj_clean(dq_objs);

    mk_label(dq_objs, "OBJECTIVES", &lv_font_unscii_8, CLR_DIM, 0, 0, 170);

    lv_obj_t* div = lv_obj_create(dq_objs);
    lv_obj_set_size(div, 168, 1);
    lv_obj_set_style_bg_color(div, CLR_DARK, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);

    const char* objs[] = {q.obj1, q.obj2, q.obj3};
    bool done[] = {q.obj1_done, q.obj2_done, q.obj3_done};

    for (int i = 0; i < 3; i++) {
        if (!objs[i]) continue;

        lv_obj_t* row = lv_obj_create(dq_objs);
        lv_obj_set_size(row, 168, 14);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, obj_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        // Checkbox
        lv_obj_t* chk = lv_obj_create(row);
        lv_obj_set_size(chk, 8, 8);
        lv_obj_set_pos(chk, 0, 2);
        lv_obj_set_style_border_color(chk, CLR_DARK, 0);
        lv_obj_set_style_border_width(chk, 1, 0);
        lv_obj_set_style_radius(chk, 0, 0);
        lv_obj_set_style_pad_all(chk, 0, 0);
        lv_obj_clear_flag(chk, LV_OBJ_FLAG_SCROLLABLE);

        if (done[i]) {
            lv_obj_set_style_bg_color(chk, CLR_DARK, 0);
            lv_obj_set_style_bg_opa(chk, LV_OPA_COVER, 0);
            lv_obj_t* ck = lv_label_create(chk);
            lv_label_set_text(ck, LV_SYMBOL_OK);
            lv_obj_set_style_text_font(ck, &lv_font_unscii_8, 0);
            lv_obj_set_style_text_color(ck, CLR_DIM, 0);
            lv_obj_align(ck, LV_ALIGN_CENTER, 0, 0);
        } else {
            lv_obj_set_style_bg_opa(chk, LV_OPA_TRANSP, 0);
        }

        // Text
        lv_obj_t* txt = mk_label(row, objs[i], &lv_font_unscii_8,
                                  done[i] ? CLR_DARK : CLR_GREEN,
                                  12, 1, 150);
        (void)txt;
    }

    // Reward
    char rwd[24];
    snprintf(rwd, sizeof(rwd), "%d XP", q.xp_reward);
    lv_label_set_text(dq_rwd, rwd);
}

// ── Quest click ──
static void quest_click_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    data_sel = idx;

    // Update selection highlighting
    uint32_t cnt = lv_obj_get_child_cnt(quest_list);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t* row = lv_obj_get_child(quest_list, i);
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

    if (idx >= 0 && idx < filtered_count) {
        show_quest_detail(filtered_indices[idx]);
    }
}

// ── Populate quest list ──
static void populate_quests(void) {
    lv_obj_clean(quest_list);
    data_sel = -1;
    build_filtered();

    char buf[12];
    snprintf(buf, sizeof(buf), "%d", filtered_count);
    lv_label_set_text(lbl_quest_count, buf);

    for (int fi = 0; fi < filtered_count; fi++) {
        int qi = filtered_indices[fi];
        const Quest& q = quests[qi];

        lv_obj_t* row = lv_obj_create(quest_list);
        lv_obj_set_size(row, 134, 22);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x005c22), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_left(row, 6, 0);
        lv_obj_set_style_pad_right(row, 3, 0);
        lv_obj_set_style_pad_top(row, 2, 0);
        lv_obj_set_style_pad_bottom(row, 2, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, quest_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)fi);

        if (q.done) {
            lv_obj_set_style_opa(row, 140, 0);
        }

        // Pip indicator
        lv_obj_t* pip = lv_obj_create(row);
        lv_obj_set_size(pip, 7, 7);
        lv_obj_set_pos(pip, 0, 3);
        lv_obj_set_style_radius(pip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(pip, 0, 0);
        lv_obj_clear_flag(pip, LV_OBJ_FLAG_SCROLLABLE);

        if (q.done) {
            lv_obj_set_style_bg_color(pip, CLR_DARK, 0);
            lv_obj_set_style_bg_opa(pip, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(pip, CLR_DARK, 0);
        } else if (q.main_quest) {
            lv_obj_set_style_bg_color(pip, CLR_GOLD, 0);
            lv_obj_set_style_bg_opa(pip, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(pip, CLR_GOLD, 0);
        } else {
            lv_obj_set_style_bg_color(pip, CLR_GREEN, 0);
            lv_obj_set_style_bg_opa(pip, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(pip, CLR_GREEN, 0);
        }
        lv_obj_set_style_border_width(pip, 1, 0);

        // Quest name
        lv_obj_t* name = mk_label(row, q.name, &lv_font_unscii_8,
                                   q.main_quest ? CLR_GOLD : CLR_GREEN,
                                   11, 0, 100);
        if (q.done) {
            lv_obj_set_style_text_decor(name, LV_TEXT_DECOR_STRIKETHROUGH, 0);
        }

        // Location
        mk_label(row, q.location, &lv_font_unscii_8, CLR_DIM, 11, 10, 100);
    }

    // Update XP bar
    int total = compute_total_xp();
    int earned = compute_earned_xp();
    snprintf(buf, sizeof(buf), "%d/%d", earned, total);
    lv_label_set_text(xp_lbl, buf);
    int pct = total > 0 ? (earned * 100 / total) : 0;
    lv_obj_set_width(xp_fill, (int)(124 * pct / 100));
}

// ── Filter click ──
static void fil_click_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    data_filter = idx;
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(fil_btns[i], i == idx ? CLR_SEL : CLR_HEADER, 0);
        lv_obj_set_style_text_color(fil_lbls[i], i == idx ? CLR_GREEN : CLR_DIM, 0);
    }
    populate_quests();
}

// ── Build screen ──
lv_obj_t* screen_data_create(void) {
    scr_data = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_data, CLR_BG, 0);
    lv_obj_set_style_bg_opa(scr_data, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr_data, 0, 0);
    lv_obj_set_style_border_width(scr_data, 0, 0);
    lv_obj_clear_flag(scr_data, LV_OBJ_FLAG_SCROLLABLE);

    // ═══ LEFT PANEL (136px) ═══
    lv_obj_t* left = lv_obj_create(scr_data);
    lv_obj_set_size(left, 136, 222);
    lv_obj_set_pos(left, 0, 18);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(left, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_color(left, CLR_DARK, 0);
    lv_obj_set_style_border_width(left, 1, 0);
    lv_obj_set_style_radius(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    // Filter tabs
    int fw = 136 / 3;
    for (int i = 0; i < 3; i++) {
        lv_obj_t* btn = lv_obj_create(left);
        lv_obj_set_size(btn, fw + (i == 2 ? 136 - fw * 3 : 0), 16);
        lv_obj_set_pos(btn, i * fw, 0);
        lv_obj_set_style_bg_color(btn, i == 0 ? CLR_SEL : CLR_HEADER, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(btn, CLR_DARK, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, fil_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        fil_btns[i] = btn;

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, FIL_NAMES[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(lbl, i == 0 ? CLR_GREEN : CLR_DIM, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        fil_lbls[i] = lbl;
    }

    // Header
    mk_label(left, "QUESTS", &lv_font_unscii_8, CLR_DIM, 5, 18, 70);
    lbl_quest_count = mk_label(left, "-", &lv_font_unscii_8, CLR_DIM, 90, 18, 44);
    lv_obj_set_style_text_align(lbl_quest_count, LV_TEXT_ALIGN_RIGHT, 0);

    // Divider
    lv_obj_t* hdiv = lv_obj_create(left);
    lv_obj_set_size(hdiv, 136, 1);
    lv_obj_set_pos(hdiv, 0, 28);
    lv_obj_set_style_bg_color(hdiv, CLR_DARK, 0);
    lv_obj_set_style_bg_opa(hdiv, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdiv, 0, 0);
    lv_obj_clear_flag(hdiv, LV_OBJ_FLAG_SCROLLABLE);

    // Quest list
    quest_list = lv_obj_create(left);
    lv_obj_set_size(quest_list, 136, 168);
    lv_obj_set_pos(quest_list, 0, 29);
    lv_obj_set_style_bg_opa(quest_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(quest_list, 0, 0);
    lv_obj_set_style_radius(quest_list, 0, 0);
    lv_obj_set_style_pad_all(quest_list, 0, 0);
    lv_obj_set_style_pad_row(quest_list, 0, 0);
    lv_obj_set_flex_flow(quest_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(quest_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(quest_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(quest_list, CLR_DARK, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(quest_list, LV_OPA_COVER, LV_PART_SCROLLBAR);

    // XP bar section
    lv_obj_t* xpw = lv_obj_create(left);
    lv_obj_set_size(xpw, 136, 24);
    lv_obj_set_pos(xpw, 0, 198);
    lv_obj_set_style_bg_opa(xpw, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(xpw, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(xpw, CLR_DARK, 0);
    lv_obj_set_style_border_width(xpw, 1, 0);
    lv_obj_set_style_radius(xpw, 0, 0);
    lv_obj_set_style_pad_all(xpw, 0, 0);
    lv_obj_clear_flag(xpw, LV_OBJ_FLAG_SCROLLABLE);

    mk_label(xpw, "EVENT XP", &lv_font_unscii_8, CLR_DIM, 5, 3, 60);
    xp_lbl = mk_label(xpw, "-/-", &lv_font_unscii_8, CLR_DIM, 70, 3, 62);
    lv_obj_set_style_text_align(xp_lbl, LV_TEXT_ALIGN_RIGHT, 0);

    // XP bar
    lv_obj_t* xbar = lv_obj_create(xpw);
    lv_obj_set_size(xbar, 126, 4);
    lv_obj_set_pos(xbar, 5, 14);
    lv_obj_set_style_bg_color(xbar, lv_color_hex(0x00a83e), 0);
    lv_obj_set_style_bg_opa(xbar, 20, 0);
    lv_obj_set_style_border_color(xbar, CLR_DIM, 0);
    lv_obj_set_style_border_width(xbar, 1, 0);
    lv_obj_set_style_radius(xbar, 0, 0);
    lv_obj_set_style_pad_all(xbar, 0, 0);
    lv_obj_clear_flag(xbar, LV_OBJ_FLAG_SCROLLABLE);

    xp_fill = lv_obj_create(xbar);
    lv_obj_set_size(xp_fill, 1, 2);
    lv_obj_set_pos(xp_fill, 0, 0);
    lv_obj_set_style_bg_color(xp_fill, CLR_GOLD, 0);
    lv_obj_set_style_bg_opa(xp_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(xp_fill, 0, 0);
    lv_obj_set_style_radius(xp_fill, 0, 0);
    lv_obj_clear_flag(xp_fill, LV_OBJ_FLAG_SCROLLABLE);

    // ═══ RIGHT PANEL (184px) ═══
    lv_obj_t* right = lv_obj_create(scr_data);
    lv_obj_set_size(right, 184, 222);
    lv_obj_set_pos(right, 136, 18);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 5, 0);
    lv_obj_set_style_radius(right, 0, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    // Quest tag
    lv_obj_t* tag_bg = lv_obj_create(right);
    lv_obj_set_size(tag_bg, LV_SIZE_CONTENT, 12);
    lv_obj_set_pos(tag_bg, 0, 0);
    lv_obj_set_style_bg_color(tag_bg, CLR_DARK, 0);
    lv_obj_set_style_bg_opa(tag_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tag_bg, 0, 0);
    lv_obj_set_style_radius(tag_bg, 0, 0);
    lv_obj_set_style_pad_hor(tag_bg, 4, 0);
    lv_obj_set_style_pad_ver(tag_bg, 1, 0);
    lv_obj_clear_flag(tag_bg, LV_OBJ_FLAG_SCROLLABLE);

    dq_tag = lv_label_create(tag_bg);
    lv_label_set_text(dq_tag, "SIDE QUEST");
    lv_obj_set_style_text_font(dq_tag, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(dq_tag, CLR_DIM, 0);

    // Quest name (single-line scroll to prevent overlap with location)
    dq_name = mk_label(right, "SELECT A QUEST", &lv_font_unscii_8, CLR_GREEN, 0, 16, 174);
    lv_label_set_long_mode(dq_name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(dq_name, 174);

    // Location
    dq_loc = mk_label(right, "-", &lv_font_unscii_8, CLR_DIM, 0, 28, 174);

    // Objectives box
    dq_objs = lv_obj_create(right);
    lv_obj_set_size(dq_objs, 174, 130);
    lv_obj_set_pos(dq_objs, 0, 40);
    lv_obj_set_style_bg_color(dq_objs, lv_color_hex(0x003410), 0);
    lv_obj_set_style_bg_opa(dq_objs, 80, 0);
    lv_obj_set_style_border_color(dq_objs, CLR_DARK, 0);
    lv_obj_set_style_border_width(dq_objs, 1, 0);
    lv_obj_set_style_radius(dq_objs, 0, 0);
    lv_obj_set_style_pad_all(dq_objs, 4, 0);
    lv_obj_set_style_pad_row(dq_objs, 2, 0);
    lv_obj_set_flex_flow(dq_objs, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(dq_objs, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(dq_objs, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(dq_objs, CLR_DARK, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(dq_objs, LV_OPA_COVER, LV_PART_SCROLLBAR);

    mk_label(dq_objs, "OBJECTIVES", &lv_font_unscii_8, CLR_DIM, 0, 0, 170);
    mk_label(dq_objs, "Choose a quest from the log.", &lv_font_unscii_8, CLR_DIM, 0, 0, 160);

    // Reward row
    lv_obj_t* rwd_row = lv_obj_create(right);
    lv_obj_set_size(rwd_row, 174, 14);
    lv_obj_set_pos(rwd_row, 0, 174);
    lv_obj_set_style_bg_opa(rwd_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(rwd_row, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(rwd_row, CLR_DARK, 0);
    lv_obj_set_style_border_width(rwd_row, 1, 0);
    lv_obj_set_style_radius(rwd_row, 0, 0);
    lv_obj_set_style_pad_all(rwd_row, 0, 0);
    lv_obj_clear_flag(rwd_row, LV_OBJ_FLAG_SCROLLABLE);

    mk_label(rwd_row, "REWARD", &lv_font_unscii_8, CLR_DIM, 0, 3, 60);
    dq_rwd = mk_label(rwd_row, "-", &lv_font_unscii_8, CLR_GOLD, 100, 3, 74);
    lv_obj_set_style_text_align(dq_rwd, LV_TEXT_ALIGN_RIGHT, 0);

    // Populate
    populate_quests();

    return scr_data;
}
