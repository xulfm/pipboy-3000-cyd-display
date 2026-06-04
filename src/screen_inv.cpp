// ============================================================
// Vault 89 PDI-Boy -- INV Screen
// Matches pipboy_v3.html #screen-inv layout
//
// Layout (320x222 usable):
//   Left panel  : 148px wide
//     - Category tabs: TOOLS / PROVS / DOCS (16px h)
//     - List header (item count)
//     - Scrollable item list
//     - Carry weight bar
//   Right panel : 172px wide
//     - Item icon (48x48)
//     - Item name + type
//     - Stats box (VALUE, WEIGHT, COND, BONUS)
//     - Description
//     - EQUIP/CONSUME button
// ============================================================

#include <Arduino.h>
#include <lvgl.h>
#include "pipboy_data.h"

// ── Forward helpers from screen_stat.cpp ──
// We redefine local helpers to keep this self-contained

static lv_obj_t* scr_inv = NULL;

// Current state
static int  inv_cat = 0;   // 0=TOOLS, 1=PROVS, 2=DOCS
static int  inv_sel = -1;  // selected item index

// UI pointers
static lv_obj_t* cat_btns[3]    = {};
static lv_obj_t* cat_lbls[3]    = {};
static lv_obj_t* lbl_list_title = NULL;
static lv_obj_t* lbl_list_count = NULL;
static lv_obj_t* item_list      = NULL;
static lv_obj_t* carry_lbl      = NULL;
static lv_obj_t* carry_bar_fill = NULL;

// Detail panel
static lv_obj_t* d_icon    = NULL;
static lv_obj_t* d_name    = NULL;
static lv_obj_t* d_type    = NULL;
static lv_obj_t* d_val     = NULL;
static lv_obj_t* d_wt      = NULL;
static lv_obj_t* d_cond    = NULL;
static lv_obj_t* d_bonus   = NULL;
static lv_obj_t* d_desc    = NULL;
static lv_obj_t* d_action  = NULL;
static lv_obj_t* d_act_bg  = NULL;

// ── Category data ──
static const char* CAT_NAMES[] = {"TOOLS", "PROVS", "DOCS"};
static const char* CAT_TITLES[] = {"TOOLS & GEAR", "PROVISIONS", "DOCUMENTS"};

static InvItem* cat_items[] = {inv_tools, inv_provs, inv_docs};
static const int cat_counts[] = {TOOLS_COUNT, PROVS_COUNT, DOCS_COUNT};

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

// ── Show item detail ──
static void show_detail(const InvItem* item) {
    if (!item) return;
    lv_label_set_text(d_icon, item->icon);
    lv_label_set_text(d_name, item->name);
    lv_label_set_text(d_type, item->type);
    lv_label_set_text(d_cond, item->condition);
    lv_label_set_text(d_bonus, item->bonus);
    lv_label_set_text(d_desc, item->desc);

    // Weight: just show "1 lb" placeholder
    lv_label_set_text(d_wt, "—");
    lv_label_set_text(d_val, "—");

    // Action button
    if (item->equipped) {
        lv_label_set_text(d_action, "[ UNEQUIP ]");
        if (d_act_bg) lv_obj_set_style_bg_color(d_act_bg, CLR_GREEN, 0);
        lv_obj_set_style_text_color(d_action, CLR_BG, 0);
    } else if (inv_cat == 1) {
        lv_label_set_text(d_action, "[ CONSUME ]");
        if (d_act_bg) lv_obj_set_style_bg_color(d_act_bg, CLR_GREEN, 0);
        lv_obj_set_style_text_color(d_action, CLR_BG, 0);
    } else {
        lv_label_set_text(d_action, "[ EQUIP ]");
        if (d_act_bg) lv_obj_set_style_bg_color(d_act_bg, CLR_DARK, 0);
        lv_obj_set_style_text_color(d_action, CLR_DIM, 0);
    }
}

// ── Item click callback ──
static void item_click_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    inv_sel = idx;
    const InvItem* items = cat_items[inv_cat];
    show_detail(&items[idx]);

    // Update selection highlighting
    uint32_t cnt = lv_obj_get_child_cnt(item_list);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t* row = lv_obj_get_child(item_list, i);
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
}

// ── Populate item list ──
static void populate_list(void) {
    // Clear old children
    lv_obj_clean(item_list);
    inv_sel = -1;

    const InvItem* items = cat_items[inv_cat];
    int count = cat_counts[inv_cat];

    // Update header
    lv_label_set_text(lbl_list_title, CAT_TITLES[inv_cat]);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d items", count);
    lv_label_set_text(lbl_list_count, buf);

    // Update carry bar
    lv_label_set_text(carry_lbl, "47/200 lbs");
    lv_obj_set_width(carry_bar_fill, (int)(136 * 47 / 200));

    for (int i = 0; i < count; i++) {
        const InvItem* item = &items[i];

        lv_obj_t* row = lv_obj_create(item_list);
        lv_obj_set_size(row, 146, 16);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x005c22), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_left(row, 5, 0);
        lv_obj_set_style_pad_right(row, 3, 0);
        lv_obj_set_style_pad_top(row, 1, 0);
        lv_obj_set_style_pad_bottom(row, 1, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, item_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        // Icon
        lv_obj_t* ico = lv_label_create(row);
        lv_label_set_text(ico, item->icon);
        lv_obj_set_style_text_font(ico, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(ico, CLR_DIM, 0);
        lv_obj_set_pos(ico, 0, 2);

        // Name
        lv_obj_t* name = lv_label_create(row);
        lv_label_set_text(name, item->name);
        lv_obj_set_style_text_font(name, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(name, item->equipped ? CLR_GREEN : CLR_DIM, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(name, 95);
        lv_obj_set_pos(name, 12, 2);

        // Equipped indicator
        if (item->equipped) {
            lv_obj_t* eq = lv_label_create(row);
            lv_label_set_text(eq, "[E]");
            lv_obj_set_style_text_font(eq, &lv_font_unscii_8, 0);
            lv_obj_set_style_text_color(eq, CLR_GOLD, 0);
            lv_obj_set_pos(eq, 108, 2);
        }

        // Qty
        if (item->qty > 1) {
            char qbuf[8];
            snprintf(qbuf, sizeof(qbuf), "x%d", item->qty);
            lv_obj_t* qlbl = lv_label_create(row);
            lv_label_set_text(qlbl, qbuf);
            lv_obj_set_style_text_font(qlbl, &lv_font_unscii_8, 0);
            lv_obj_set_style_text_color(qlbl, CLR_DIM, 0);
            lv_obj_set_pos(qlbl, 122, 2);
        }
    }

    // Reset detail
    lv_label_set_text(d_icon, "?");
    lv_label_set_text(d_name, "SELECT AN ITEM");
    lv_label_set_text(d_type, "-");
    lv_label_set_text(d_val, "-");
    lv_label_set_text(d_wt, "-");
    lv_label_set_text(d_cond, "-");
    lv_label_set_text(d_bonus, "-");
    lv_label_set_text(d_desc, "Select an item to view details.");
    lv_label_set_text(d_action, "[ EQUIP ]");
    if (d_act_bg) lv_obj_set_style_bg_color(d_act_bg, CLR_DARK, 0);
    lv_obj_set_style_text_color(d_action, CLR_DIM, 0);
}

// ── Category tab click ──
static void cat_click_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    inv_cat = idx;
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(cat_btns[i], i == idx ? lv_color_hex(0x002a10) : CLR_HEADER, 0);
        lv_obj_set_style_text_color(cat_lbls[i], i == idx ? CLR_GREEN : CLR_DIM, 0);
    }
    populate_list();
}

// ── Equip/consume click ──
static void equip_click_cb(lv_event_t* e) {
    (void)e;
    if (inv_sel < 0 || inv_sel >= cat_counts[inv_cat]) return;
    int saved_sel = inv_sel;
    InvItem* items = cat_items[inv_cat];
    InvItem* item = &items[saved_sel];
    item->equipped = !item->equipped;
    populate_list();        // rebuilds list (resets inv_sel to -1)
    inv_sel = saved_sel;    // restore selection
    // Re-highlight the selected row
    uint32_t cnt = lv_obj_get_child_cnt(item_list);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t* row = lv_obj_get_child(item_list, i);
        if ((int)i == inv_sel) {
            lv_obj_set_style_bg_color(row, CLR_SEL, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_color(row, CLR_GREEN, 0);
            lv_obj_set_style_border_width(row, 2, 0);
        }
    }
    show_detail(item);
}

// ── Build screen ──
lv_obj_t* screen_inv_create(void) {
    scr_inv = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_inv, CLR_BG, 0);
    lv_obj_set_style_bg_opa(scr_inv, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr_inv, 0, 0);
    lv_obj_set_style_border_width(scr_inv, 0, 0);
    lv_obj_clear_flag(scr_inv, LV_OBJ_FLAG_SCROLLABLE);

    // ═══ LEFT PANEL (148px) ═══
    lv_obj_t* left = lv_obj_create(scr_inv);
    lv_obj_set_size(left, 148, 222);
    lv_obj_set_pos(left, 0, 18);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(left, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_color(left, CLR_DARK, 0);
    lv_obj_set_style_border_width(left, 1, 0);
    lv_obj_set_style_radius(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    // Category tabs bar (16px h)
    int catw = 148 / 3;
    for (int i = 0; i < 3; i++) {
        lv_obj_t* btn = lv_obj_create(left);
        lv_obj_set_size(btn, catw + (i == 2 ? 148 - catw * 3 : 0), 16);
        lv_obj_set_pos(btn, i * catw, 0);
        lv_obj_set_style_bg_color(btn, i == 0 ? CLR_SEL : CLR_HEADER, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(btn, CLR_DARK, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, cat_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        cat_btns[i] = btn;

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, CAT_NAMES[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(lbl, i == 0 ? CLR_GREEN : CLR_DIM, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        cat_lbls[i] = lbl;
    }

    // List header
    lbl_list_title = mk_label(left, "TOOLS & GEAR", &lv_font_unscii_8, CLR_DIM, 5, 18, 90);
    lbl_list_count = mk_label(left, "8 items", &lv_font_unscii_8, CLR_DIM, 100, 18, 46);
    lv_obj_set_style_text_align(lbl_list_count, LV_TEXT_ALIGN_RIGHT, 0);

    // Divider under header
    lv_obj_t* hdiv = lv_obj_create(left);
    lv_obj_set_size(hdiv, 148, 1);
    lv_obj_set_pos(hdiv, 0, 28);
    lv_obj_set_style_bg_color(hdiv, CLR_DARK, 0);
    lv_obj_set_style_bg_opa(hdiv, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdiv, 0, 0);
    lv_obj_clear_flag(hdiv, LV_OBJ_FLAG_SCROLLABLE);

    // Scrollable item list
    item_list = lv_obj_create(left);
    lv_obj_set_size(item_list, 148, 168);
    lv_obj_set_pos(item_list, 0, 29);
    lv_obj_set_style_bg_opa(item_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(item_list, 0, 0);
    lv_obj_set_style_radius(item_list, 0, 0);
    lv_obj_set_style_pad_all(item_list, 0, 0);
    lv_obj_set_style_pad_row(item_list, 0, 0);
    lv_obj_set_flex_flow(item_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(item_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(item_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(item_list, CLR_DARK, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(item_list, LV_OPA_COVER, LV_PART_SCROLLBAR);

    // Carry weight section
    lv_obj_t* cw = lv_obj_create(left);
    lv_obj_set_size(cw, 148, 24);
    lv_obj_set_pos(cw, 0, 198);
    lv_obj_set_style_bg_opa(cw, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(cw, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(cw, CLR_DARK, 0);
    lv_obj_set_style_border_width(cw, 1, 0);
    lv_obj_set_style_radius(cw, 0, 0);
    lv_obj_set_style_pad_all(cw, 0, 0);
    lv_obj_clear_flag(cw, LV_OBJ_FLAG_SCROLLABLE);

    mk_label(cw, "CARRY WT", &lv_font_unscii_8, CLR_DIM, 5, 3, 65);
    carry_lbl = mk_label(cw, "47/200 lbs", &lv_font_unscii_8, CLR_DIM, 75, 3, 68);
    lv_obj_set_style_text_align(carry_lbl, LV_TEXT_ALIGN_RIGHT, 0);

    // Carry bar outer
    lv_obj_t* cbar = lv_obj_create(cw);
    lv_obj_set_size(cbar, 138, 4);
    lv_obj_set_pos(cbar, 5, 14);
    lv_obj_set_style_bg_color(cbar, lv_color_hex(0x00a83e), 0);
    lv_obj_set_style_bg_opa(cbar, 20, 0);
    lv_obj_set_style_border_color(cbar, CLR_DIM, 0);
    lv_obj_set_style_border_width(cbar, 1, 0);
    lv_obj_set_style_radius(cbar, 0, 0);
    lv_obj_set_style_pad_all(cbar, 0, 0);
    lv_obj_clear_flag(cbar, LV_OBJ_FLAG_SCROLLABLE);

    carry_bar_fill = lv_obj_create(cbar);
    lv_obj_set_size(carry_bar_fill, (int)(136 * 47 / 200), 2);
    lv_obj_set_pos(carry_bar_fill, 0, 0);
    lv_obj_set_style_bg_color(carry_bar_fill, CLR_DIM, 0);
    lv_obj_set_style_bg_opa(carry_bar_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(carry_bar_fill, 0, 0);
    lv_obj_set_style_radius(carry_bar_fill, 0, 0);
    lv_obj_clear_flag(carry_bar_fill, LV_OBJ_FLAG_SCROLLABLE);

    // ═══ RIGHT PANEL (172px) ═══
    lv_obj_t* right = lv_obj_create(scr_inv);
    lv_obj_set_size(right, 172, 222);
    lv_obj_set_pos(right, 148, 18);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 6, 0);
    lv_obj_set_style_radius(right, 0, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    // Detail icon box (48x48)
    lv_obj_t* icon_box = lv_obj_create(right);
    lv_obj_set_size(icon_box, 48, 48);
    lv_obj_set_pos(icon_box, 56, 0);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(0x003410), 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(icon_box, CLR_DARK, 0);
    lv_obj_set_style_border_width(icon_box, 1, 0);
    lv_obj_set_style_radius(icon_box, 0, 0);
    lv_obj_set_style_pad_all(icon_box, 0, 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);

    d_icon = lv_label_create(icon_box);
    lv_label_set_text(d_icon, "?");
    lv_obj_set_style_text_font(d_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(d_icon, CLR_GREEN, 0);
    lv_obj_align(d_icon, LV_ALIGN_CENTER, 0, 0);

    // Item name
    d_name = mk_label(right, "SELECT AN ITEM", &lv_font_unscii_8, CLR_GREEN, 0, 52, 160);
    lv_obj_set_style_text_align(d_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(d_name, 160);

    // Item type
    d_type = mk_label(right, "-", &lv_font_unscii_8, CLR_DIM, 0, 63, 160);
    lv_obj_set_style_text_align(d_type, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(d_type, 160);

    // Stats box
    lv_obj_t* sbox = lv_obj_create(right);
    lv_obj_set_size(sbox, 160, 52);
    lv_obj_set_pos(sbox, 0, 75);
    lv_obj_set_style_bg_color(sbox, lv_color_hex(0x003410), 0);
    lv_obj_set_style_bg_opa(sbox, 80, 0);
    lv_obj_set_style_border_color(sbox, CLR_DARK, 0);
    lv_obj_set_style_border_width(sbox, 1, 0);
    lv_obj_set_style_radius(sbox, 0, 0);
    lv_obj_set_style_pad_all(sbox, 4, 0);
    lv_obj_clear_flag(sbox, LV_OBJ_FLAG_SCROLLABLE);

    auto mk_ds_row = [&](const char* key, int y) -> lv_obj_t* {
        mk_label(sbox, key, &lv_font_unscii_8, CLR_DIM, 0, y, 50);
        return mk_label(sbox, "-", &lv_font_unscii_8, CLR_GREEN, 55, y, 95);
    };

    d_val   = mk_ds_row("VALUE",  0);
    d_wt    = mk_ds_row("WEIGHT", 11);
    d_cond  = mk_ds_row("COND.",  22);
    d_bonus = mk_ds_row("BONUS",  33);
    lv_obj_set_style_text_color(d_bonus, CLR_GOLD, 0);

    // Description
    d_desc = mk_label(right, "Select an item to view details.", &lv_font_unscii_8, CLR_DIM, 0, 133, 160);
    lv_label_set_long_mode(d_desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(d_desc, 160);

    // Action button
    lv_obj_t* act_bg = lv_obj_create(right);
    lv_obj_set_size(act_bg, 160, 16);
    lv_obj_set_pos(act_bg, 0, 196);
    lv_obj_set_style_bg_color(act_bg, CLR_DARK, 0);
    lv_obj_set_style_bg_opa(act_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(act_bg, 0, 0);
    lv_obj_set_style_radius(act_bg, 0, 0);
    lv_obj_set_style_pad_all(act_bg, 0, 0);
    lv_obj_clear_flag(act_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(act_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(act_bg, equip_click_cb, LV_EVENT_CLICKED, NULL);
    d_act_bg = act_bg;

    lv_obj_t* act_lbl = lv_label_create(act_bg);
    lv_label_set_text(act_lbl, "[ EQUIP ]");
    lv_obj_set_style_text_font(act_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(act_lbl, CLR_DIM, 0);
    lv_obj_align(act_lbl, LV_ALIGN_CENTER, 0, 0);
    d_action = act_lbl;  // point to label for text updates

    // Populate initial list
    populate_list();

    return scr_inv;
}
