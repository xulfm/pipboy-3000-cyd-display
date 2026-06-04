// ============================================================
// PIP-BOY -- MAP Screen
// Wasteland overworld — single full-screen stylized map.
// Tap map to set your position (broadcast in heartbeats).
// RADAR overlay shows mini-map with peer positions.
// The vault entrance is the radar center ("home").
// ============================================================

#include <Arduino.h>
#include <lvgl.h>
#include "pipboy_data.h"
#include "net_state.h"
#include "sound_fx.h"

#define CLR_ORANGE lv_color_hex(0xCC6600)
#define CLR_RED    lv_color_hex(0xCC0000)
#define CLR_YELLOW lv_color_hex(0xCCCC00)

static lv_obj_t* scr_map = NULL;

// ── Radar state ──
static lv_obj_t* radar_cont   = NULL;
static bool      radar_visible = false;
static lv_timer_t* radar_timer = NULL;
static uint16_t  sweep_angle   = 0;

#define RADAR_DIAM  160
#define RADAR_R     72
#define RADAR_CX    (RADAR_DIAM / 2)
#define RADAR_CY    (RADAR_DIAM / 2)
#define MAX_BLIPS   ESPNOW_MAX_PEERS

// Map coordinate space (matches map container layout)
// Stage is the center reference point
#define MAP_W       316
#define MAP_H       180

// LVGL objects for radar
static lv_obj_t* blip_dots[MAX_BLIPS];
static lv_obj_t* blip_labels[MAX_BLIPS];
static lv_obj_t* my_dot      = NULL;
static lv_obj_t* sweep_obj   = NULL;
static lv_obj_t* peer_cnt_lbl = NULL;
static lv_obj_t* radar_title = NULL;   // mode label at top of radar
static lv_obj_t* radar_list  = NULL;   // distance-list panel (DISTANCE mode)

// Position marker on main map
static lv_obj_t* pos_marker  = NULL;
static lv_obj_t* pos_label   = NULL;

// ── Helpers ──

static void box(lv_obj_t* par, const char* txt, int x, int y, int w, int h,
                lv_color_t border, const lv_font_t* f = &lv_font_unscii_8) {
    lv_obj_t* b = lv_obj_create(par);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, CLR_SEL, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, border, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 1, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);

    if (txt) {
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, f, 0);
        lv_obj_set_style_text_color(l, border, 0);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
    }
}

static void sq_marker(lv_obj_t* par, int x, int y, lv_color_t c, int sz = 6) {
    lv_obj_t* m = lv_obj_create(par);
    lv_obj_set_size(m, sz, sz);
    lv_obj_set_pos(m, x, y);
    lv_obj_set_style_bg_color(m, c, 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_radius(m, 0, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t* lbl(lv_obj_t* par, const char* txt, int x, int y,
                      lv_color_t c, const lv_font_t* f = &lv_font_unscii_8) {
    lv_obj_t* l = lv_label_create(par);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

// ── Map tap to set position ──
static void map_tap_cb(lv_event_t* e) {
    if (radar_visible) return;  // don't set position when radar is open

    lv_obj_t* map = lv_event_get_target(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);

    // Convert screen coords to map-relative coords
    lv_coord_t mx = lv_obj_get_x(map);
    lv_coord_t my_y = lv_obj_get_y(map);
    int16_t px = (int16_t)(p.x - mx);
    int16_t py = (int16_t)(p.y - my_y);

    // Clamp to map bounds
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px > MAP_W) px = MAP_W;
    if (py > MAP_H) py = MAP_H;

    g_my_pos_x = px;
    g_my_pos_y = py;

    // Update position marker
    if (pos_marker) {
        lv_obj_set_pos(pos_marker, px - 4, py - 4);
        lv_obj_clear_flag(pos_marker, LV_OBJ_FLAG_HIDDEN);
    }
    if (pos_label) {
        lv_obj_set_pos(pos_label, px + 6, py - 4);
        lv_label_set_text(pos_label, "YOU");
        lv_obj_clear_flag(pos_label, LV_OBJ_FLAG_HIDDEN);
    }

    sfx_play(SFX_CLICK);
    Serial.printf("[map] Position set: %d, %d\n", px, py);
}

// ── Radar helpers ──

// Convert map coordinates to radar pixel position (stage = center)
static void map_to_radar(int16_t mx, int16_t my, int* rx, int* ry) {
    // Offset from stage center
    float dx = (float)(mx - MAP_STAGE_X);
    float dy = (float)(my - MAP_STAGE_Y);

    // Scale: map is ~316 wide, radar is ~144 usable diameter
    // Scale factor to fit the map into the radar circle
    float scale = (float)RADAR_R / (float)(MAP_W / 2);

    *rx = RADAR_CX + (int)(dx * scale);
    *ry = RADAR_CY + (int)(dy * scale);

    // Clamp inside radar bounds
    if (*rx < 4) *rx = 4;
    if (*ry < 12) *ry = 12;
    if (*rx > RADAR_DIAM - 4) *rx = RADAR_DIAM - 4;
    if (*ry > RADAR_DIAM - 14) *ry = RADAR_DIAM - 14;
}

// Smooth a peer's RSSI (exponential moving average) and return it, so
// both the radar plot and the distance readouts use the same filtered
// value instead of the jittery raw sample.
static int peer_rssi_smoothed(int i) {
    int8_t raw = g_peers[i].rssi;
    if (g_peers[i].rssi_avg == 0) g_peers[i].rssi_avg = raw;
    else g_peers[i].rssi_avg = (int16_t)((g_peers[i].rssi_avg * 7 + raw) / 8);
    return g_peers[i].rssi_avg;
}

static void hide_all_blips(void) {
    for (int i = 0; i < MAX_BLIPS; i++) {
        if (blip_dots[i])   lv_obj_add_flag(blip_dots[i],   LV_OBJ_FLAG_HIDDEN);
        if (blip_labels[i]) lv_obj_add_flag(blip_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// The radar adapts to how many devices + anchors are present:
//   DEMO     — alone: simulate moving contacts so the radar isn't empty
//   DISTANCE — peers but not enough anchors: list RSSI distance per peer
//   POSITION — 3+ devices and >=2 anchors: plot weighted-centroid (x,y)
enum RadarMode { RM_DEMO, RM_DIST, RM_POS };

static void update_radar(void) {
    if (!radar_cont) return;

    // Sweep dot (advances every tick regardless of mode)
    if (sweep_obj) {
        float rad = sweep_angle * 3.14159f / 180.0f;
        int sx = RADAR_CX + (int)(RADAR_R * cosf(rad)) - 1;
        int sy = RADAR_CY - (int)(RADAR_R * sinf(rad)) - 1;
        lv_obj_set_pos(sweep_obj, sx, sy);
    }

    // ── Choose mode ──
    int peers_online = 0;
    for (int i = 0; i < g_peer_count; i++) if (g_peers[i].online) peers_online++;
    int active  = mesh_active_count();
    int anchors = mesh_anchor_count();
    RadarMode mode;
    if (peers_online == 0)                    mode = RM_DEMO;
    else if (active >= 3 && anchors >= 2)     mode = RM_POS;
    else                                       mode = RM_DIST;

    if (radar_title) {
        lv_label_set_text(radar_title,
            mode == RM_DEMO ? "DEMO" : mode == RM_POS ? "LOCATING" : "DISTANCE");
    }

    // ── DEMO: simulated contacts orbiting the vault ──
    if (mode == RM_DEMO) {
        if (radar_list) lv_obj_add_flag(radar_list, LV_OBJ_FLAG_HIDDEN);
        if (sweep_obj)  lv_obj_clear_flag(sweep_obj, LV_OBJ_FLAG_HIDDEN);
        if (my_dot) {
            lv_obj_set_pos(my_dot, RADAR_CX - 4, RADAR_CY - 4);
            lv_obj_clear_flag(my_dot, LV_OBJ_FLAG_HIDDEN);
        }
        static const char* dn[3] = {"RAIDER", "TRADER", "SCAV"};
        lv_color_t dc[3] = {CLR_RED, CLR_GREEN, CLR_YELLOW};
        float t = millis() / 1000.0f;
        for (int k = 0; k < MAX_BLIPS; k++) {
            if (k < 3) {
                float a  = t * (0.25f + 0.12f * k) + k * 2.1f;
                int   rr = 22 + 15 * k;
                int bx = RADAR_CX + (int)(rr * cosf(a)) - 3;
                int by = RADAR_CY - (int)(rr * sinf(a)) - 3;
                lv_obj_set_pos(blip_dots[k], bx, by);
                lv_obj_set_style_bg_color(blip_dots[k], dc[k], 0);
                lv_obj_clear_flag(blip_dots[k], LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(blip_labels[k], dn[k]);
                lv_obj_set_style_text_color(blip_labels[k], CLR_DIM, 0);
                lv_obj_set_pos(blip_labels[k], bx + 8, by - 2);
                lv_obj_clear_flag(blip_labels[k], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(blip_dots[k],   LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(blip_labels[k], LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (peer_cnt_lbl) lv_label_set_text(peer_cnt_lbl, "SIMULATED");
        return;
    }

    // ── DISTANCE: RSSI-estimated range to each peer (no true position) ──
    if (mode == RM_DIST) {
        if (sweep_obj) lv_obj_add_flag(sweep_obj, LV_OBJ_FLAG_HIDDEN);
        if (my_dot)    lv_obj_add_flag(my_dot,    LV_OBJ_FLAG_HIDDEN);
        hide_all_blips();
        char buf[256] = "";
        size_t used = 0;
        for (int i = 0; i < g_peer_count && used < sizeof(buf) - 1; i++) {
            if (!g_peers[i].online) continue;
            int rssi = peer_rssi_smoothed(i);
            int d = (int)(mesh_rssi_to_dist(rssi) + 0.5f);
            const char* nm = g_peers[i].name[0] ? g_peers[i].name : "?";
            int w = snprintf(buf + used, sizeof(buf) - used, "%-9.9s ~%dm\n", nm, d);
            if (w <= 0) break;
            used += w;
        }
        if (used == 0) snprintf(buf, sizeof(buf), "no peers in range");
        if (radar_list) {
            lv_label_set_text(radar_list, buf);
            lv_obj_clear_flag(radar_list, LV_OBJ_FLAG_HIDDEN);
        }
        if (peer_cnt_lbl) lv_label_set_text(peer_cnt_lbl, "RSSI EST");
        return;
    }

    // ── POSITION: plot true/estimated (x,y) on the map-aligned radar ──
    if (radar_list) lv_obj_add_flag(radar_list, LV_OBJ_FLAG_HIDDEN);
    if (sweep_obj)  lv_obj_clear_flag(sweep_obj, LV_OBJ_FLAG_HIDDEN);

    if (my_dot) {
        if (g_my_pos_x >= 0 && g_my_pos_y >= 0) {
            int rx, ry; map_to_radar(g_my_pos_x, g_my_pos_y, &rx, &ry);
            lv_obj_set_pos(my_dot, rx - 4, ry - 4);
        } else {
            lv_obj_set_pos(my_dot, RADAR_CX - 4, RADAR_CY - 4);
        }
        lv_obj_clear_flag(my_dot, LV_OBJ_FLAG_HIDDEN);
    }

    int shown = 0;
    for (int i = 0; i < MAX_BLIPS; i++) {
        if (i < g_peer_count && g_peers[i].online) {
            lv_color_t clr;
            if (g_peers[i].is_anchor)            clr = CLR_GOLD;   // anchors stand out
            else if (g_peers[i].sig_level >= 3)  clr = CLR_GREEN;
            else if (g_peers[i].sig_level >= 2)  clr = CLR_YELLOW;
            else                                  clr = CLR_RED;

            if (g_peers[i].pos_x >= 0 && g_peers[i].pos_y >= 0) {
                int rx, ry; map_to_radar(g_peers[i].pos_x, g_peers[i].pos_y, &rx, &ry);
                lv_obj_set_pos(blip_dots[i], rx - 3, ry - 3);
            } else {
                // Roamer without a fix yet — fall back to RSSI radius.
                int rssi = peer_rssi_smoothed(i);
                const int NEAR = -45, FAR = -90;
                float frac = (float)(NEAR - rssi) / (float)(NEAR - FAR);
                if (frac < 0.0f) frac = 0.0f;
                if (frac > 1.0f) frac = 1.0f;
                int rr = 16 + (int)(frac * (RADAR_R - 6 - 16));
                uint16_t ang = (uint16_t)((g_peers[i].mac[4] * 256 + g_peers[i].mac[5]) % 360);
                float a = ang * 3.14159f / 180.0f;
                lv_obj_set_pos(blip_dots[i], RADAR_CX + (int)(rr * cosf(a)) - 3,
                                             RADAR_CY - (int)(rr * sinf(a)) - 3);
            }

            lv_obj_set_style_bg_color(blip_dots[i], clr, 0);
            lv_obj_clear_flag(blip_dots[i], LV_OBJ_FLAG_HIDDEN);

            char short_name[8];
            if (g_peers[i].name[0]) {
                strncpy(short_name, g_peers[i].name, 7); short_name[7] = '\0';
            } else {
                snprintf(short_name, sizeof(short_name), "%02X%02X",
                         g_peers[i].mac[4], g_peers[i].mac[5]);
            }
            lv_label_set_text(blip_labels[i], short_name);
            lv_obj_set_style_text_color(blip_labels[i], clr, 0);

            lv_coord_t dx = lv_obj_get_x(blip_dots[i]);
            lv_coord_t dy = lv_obj_get_y(blip_dots[i]);
            int lx = dx + 8, ly = dy - 2;
            if (lx + 40 > RADAR_DIAM) lx = dx - 42;
            if (ly < 12) ly = 12;
            lv_obj_set_pos(blip_labels[i], lx, ly);
            lv_obj_clear_flag(blip_labels[i], LV_OBJ_FLAG_HIDDEN);
            shown++;
        } else {
            lv_obj_add_flag(blip_dots[i],   LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(blip_labels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (peer_cnt_lbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d PEERS", shown);
        lv_label_set_text(peer_cnt_lbl, buf);
    }
}

static void radar_timer_cb(lv_timer_t* t) {
    (void)t;
    sweep_angle = (sweep_angle + 6) % 360;
    update_radar();
}

static void radar_toggle_cb(lv_event_t* e) {
    (void)e;
    radar_visible = !radar_visible;
    if (radar_visible) {
        lv_obj_clear_flag(radar_cont, LV_OBJ_FLAG_HIDDEN);
        if (!radar_timer) {
            radar_timer = lv_timer_create(radar_timer_cb, 100, NULL);
        }
        sfx_play(SFX_CONNECT);
        update_radar();
    } else {
        lv_obj_add_flag(radar_cont, LV_OBJ_FLAG_HIDDEN);
        if (radar_timer) {
            lv_timer_del(radar_timer);
            radar_timer = NULL;
        }
    }
}

static void radar_close_cb(lv_event_t* e) {
    (void)e;
    radar_visible = false;
    lv_obj_add_flag(radar_cont, LV_OBJ_FLAG_HIDDEN);
    if (radar_timer) {
        lv_timer_del(radar_timer);
        radar_timer = NULL;
    }
}

// Docs/automation hook: open the radar overlay on the current MAP screen
// (no-op if the map isn't built). Lets the screenshot tool capture the
// radar without a touch.
void screen_map_force_radar(void) {
    if (!radar_cont || radar_visible) return;
    radar_visible = true;
    lv_obj_clear_flag(radar_cont, LV_OBJ_FLAG_HIDDEN);
    if (!radar_timer) radar_timer = lv_timer_create(radar_timer_cb, 100, NULL);
    update_radar();
}

// ── Build radar overlay ──
static void build_radar(lv_obj_t* parent) {
    radar_cont = lv_obj_create(parent);
    lv_obj_set_size(radar_cont, RADAR_DIAM + 8, RADAR_DIAM + 8);
    lv_obj_align(radar_cont, LV_ALIGN_CENTER, 0, -4);
    lv_obj_set_style_bg_color(radar_cont, lv_color_hex(0x080808), 0);
    lv_obj_set_style_bg_opa(radar_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(radar_cont, CLR_GREEN, 0);
    lv_obj_set_style_border_width(radar_cont, 1, 0);
    lv_obj_set_style_radius(radar_cont, 4, 0);
    lv_obj_set_style_pad_all(radar_cont, 4, 0);
    lv_obj_clear_flag(radar_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(radar_cont, LV_OBJ_FLAG_HIDDEN);

    lv_color_t ring_clr = lv_color_hex(0x1a3a1a);

    // Outer ring
    lv_obj_t* ring1 = lv_obj_create(radar_cont);
    lv_obj_set_size(ring1, RADAR_R * 2, RADAR_R * 2);
    lv_obj_align(ring1, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(ring1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring1, ring_clr, 0);
    lv_obj_set_style_border_width(ring1, 1, 0);
    lv_obj_set_style_radius(ring1, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(ring1, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Middle ring
    lv_obj_t* ring2 = lv_obj_create(radar_cont);
    int r2 = RADAR_R * 2 / 3;
    lv_obj_set_size(ring2, r2 * 2, r2 * 2);
    lv_obj_align(ring2, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(ring2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring2, ring_clr, 0);
    lv_obj_set_style_border_width(ring2, 1, 0);
    lv_obj_set_style_radius(ring2, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(ring2, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Inner ring
    lv_obj_t* ring3 = lv_obj_create(radar_cont);
    int r3 = RADAR_R / 3;
    lv_obj_set_size(ring3, r3 * 2, r3 * 2);
    lv_obj_align(ring3, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(ring3, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring3, ring_clr, 0);
    lv_obj_set_style_border_width(ring3, 1, 0);
    lv_obj_set_style_radius(ring3, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(ring3, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Vault marker at center (small square) — radar orients around home
    lv_obj_t* stage_mk = lv_obj_create(radar_cont);
    lv_obj_set_size(stage_mk, 8, 8);
    lv_obj_align(stage_mk, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(stage_mk, CLR_GOLD, 0);
    lv_obj_set_style_bg_opa(stage_mk, LV_OPA_50, 0);
    lv_obj_set_style_border_color(stage_mk, CLR_GOLD, 0);
    lv_obj_set_style_border_width(stage_mk, 1, 0);
    lv_obj_set_style_radius(stage_mk, 1, 0);
    lv_obj_clear_flag(stage_mk, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // "VLT" label next to center (the vault = home)
    lv_obj_t* stage_lbl = lv_label_create(radar_cont);
    lv_label_set_text(stage_lbl, "VLT");
    lv_obj_set_style_text_font(stage_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(stage_lbl, CLR_GOLD, 0);
    lv_obj_align(stage_lbl, LV_ALIGN_CENTER, 14, -4);

    // Sweep dot
    sweep_obj = lv_obj_create(radar_cont);
    lv_obj_set_size(sweep_obj, 4, 4);
    lv_obj_set_pos(sweep_obj, RADAR_CX + RADAR_R, RADAR_CY);
    lv_obj_set_style_bg_color(sweep_obj, CLR_GREEN, 0);
    lv_obj_set_style_bg_opa(sweep_obj, LV_OPA_60, 0);
    lv_obj_set_style_border_width(sweep_obj, 0, 0);
    lv_obj_set_style_radius(sweep_obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(sweep_obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Own position dot (bright green, larger)
    my_dot = lv_obj_create(radar_cont);
    lv_obj_set_size(my_dot, 8, 8);
    lv_obj_align(my_dot, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(my_dot, CLR_GREEN, 0);
    lv_obj_set_style_bg_opa(my_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(my_dot, lv_color_white(), 0);
    lv_obj_set_style_border_width(my_dot, 1, 0);
    lv_obj_set_style_radius(my_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(my_dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(my_dot, LV_OBJ_FLAG_HIDDEN);

    // Peer blips (pre-allocated, hidden)
    for (int i = 0; i < MAX_BLIPS; i++) {
        blip_dots[i] = lv_obj_create(radar_cont);
        lv_obj_set_size(blip_dots[i], 7, 7);
        lv_obj_set_pos(blip_dots[i], 0, 0);
        lv_obj_set_style_bg_color(blip_dots[i], CLR_GREEN, 0);
        lv_obj_set_style_bg_opa(blip_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(blip_dots[i], 0, 0);
        lv_obj_set_style_radius(blip_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(blip_dots[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(blip_dots[i], LV_OBJ_FLAG_HIDDEN);

        blip_labels[i] = lv_label_create(radar_cont);
        lv_label_set_text(blip_labels[i], "");
        lv_obj_set_style_text_font(blip_labels[i], &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(blip_labels[i], CLR_GREEN, 0);
        lv_obj_add_flag(blip_labels[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Title — shows the active radar mode (DEMO / DISTANCE / LOCATING)
    radar_title = lv_label_create(radar_cont);
    lv_label_set_text(radar_title, "RADAR");
    lv_obj_set_style_text_font(radar_title, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(radar_title, CLR_GREEN, 0);
    lv_obj_align(radar_title, LV_ALIGN_TOP_MID, 0, 0);

    // Distance-list panel (shown only in DISTANCE mode) — opaque so it
    // cleanly covers the rings/sweep when active.
    radar_list = lv_label_create(radar_cont);
    lv_label_set_text(radar_list, "");
    lv_obj_set_style_text_font(radar_list, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(radar_list, CLR_GREEN, 0);
    lv_obj_set_style_bg_color(radar_list, lv_color_hex(0x080808), 0);
    lv_obj_set_style_bg_opa(radar_list, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(radar_list, 4, 0);
    lv_obj_set_size(radar_list, RADAR_DIAM - 16, RADAR_DIAM - 36);
    lv_obj_align(radar_list, LV_ALIGN_CENTER, 0, 4);
    lv_obj_add_flag(radar_list, LV_OBJ_FLAG_HIDDEN);

    // Peer count
    peer_cnt_lbl = lv_label_create(radar_cont);
    lv_label_set_text(peer_cnt_lbl, "0 PEERS");
    lv_obj_set_style_text_font(peer_cnt_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(peer_cnt_lbl, CLR_DIM, 0);
    lv_obj_align(peer_cnt_lbl, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Close button
    lv_obj_t* close_btn = lv_btn_create(radar_cont);
    lv_obj_set_size(close_btn, 22, 18);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 4, -2);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0xCC0000), 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_set_style_radius(close_btn, 2, 0);
    lv_obj_set_style_pad_all(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, radar_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* x_lbl = lv_label_create(close_btn);
    lv_label_set_text(x_lbl, "X");
    lv_obj_set_style_text_font(x_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(x_lbl, lv_color_white(), 0);
    lv_obj_align(x_lbl, LV_ALIGN_CENTER, 0, 0);
}

// ── Screen delete cleanup ──
static void map_screen_delete_cb(lv_event_t* e) {
    if (radar_timer) { lv_timer_del(radar_timer); radar_timer = NULL; }
    radar_cont = NULL;
    sweep_obj = NULL;
    my_dot = NULL;
    peer_cnt_lbl = NULL;
    radar_title = NULL;
    radar_list = NULL;
    pos_marker = NULL;
    pos_label = NULL;
    for (int i = 0; i < MAX_BLIPS; i++) { blip_dots[i] = NULL; blip_labels[i] = NULL; }
    scr_map = NULL;
}

// ── Build screen ──
lv_obj_t* screen_map_create(void) {
    if (radar_timer) { lv_timer_del(radar_timer); radar_timer = NULL; }
    radar_visible = false;

    scr_map = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_map, CLR_BG, 0);
    lv_obj_set_style_bg_opa(scr_map, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr_map, 0, 0);
    lv_obj_set_style_border_width(scr_map, 0, 0);
    lv_obj_clear_flag(scr_map, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_map, map_screen_delete_cb, LV_EVENT_DELETE, NULL);

    // Main map container (tappable)
    lv_obj_t* map = lv_obj_create(scr_map);
    lv_obj_set_size(map, 320, 222);
    lv_obj_set_pos(map, 0, 18);
    lv_obj_set_style_bg_color(map, CLR_BG, 0);
    lv_obj_set_style_bg_opa(map, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(map, 0, 0);
    lv_obj_set_style_radius(map, 0, 0);
    lv_obj_set_style_pad_all(map, 2, 0);
    lv_obj_clear_flag(map, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(map, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(map, map_tap_cb, LV_EVENT_CLICKED, NULL);

    // ============================================================
    // WASTELAND OVERWORLD
    // Stylized Fallout-style map. VAULT entrance sits at the center
    // (MAP_STAGE_X/Y) so the radar overlay orients around "home".
    // Legend: green=settlement, gold=vault/objective, yellow=landmark,
    //         orange=rad hazard, red=hostile.
    // ============================================================

    // ── Northern reaches: ruins + rad storm ──
    box(map, "THE RUINS", 6, 2, 78, 14, CLR_YELLOW);
    sq_marker(map, 90, 6, CLR_ORANGE, 8);          // rad pocket
    box(map, "RAD ZONE", 150, 2, 64, 14, CLR_ORANGE);
    sq_marker(map, 222, 6, CLR_ORANGE, 8);
    box(map, "WATCHTOWER", 230, 2, 80, 14, CLR_YELLOW);

    // ── Settlements (west) ──
    box(map, "SANCTUARY", 6, 26, 80, 16, CLR_GREEN);
    lbl(map, "SAFE HAVEN", 8, 44, CLR_DIM);
    box(map, "RED ROCKET", 6, 60, 80, 16, CLR_YELLOW);

    // ── VAULT entrance (center / radar home) ──
    // Box centered on MAP_STAGE_X (160); wide enough for "VAULT 111".
    box(map, VAULT_ID, 120, 60, 80, 28, CLR_GOLD);
    lbl(map, "HOME", 146, 90, CLR_GOLD);

    // ── Trade + tech (east) ──
    box(map, "TRADE POST", 226, 30, 84, 16, CLR_GREEN);
    box(map, "POWER PLANT", 226, 58, 84, 16, CLR_YELLOW);
    sq_marker(map, 300, 80, CLR_ORANGE, 8);        // reactor leak

    // ── The river band ──
    lbl(map, "~ ~ ~  RIVER  ~ ~ ~", 96, 108, CLR_DIM);

    // ── Southern wastes: raiders, bunker, diner ──
    box(map, "RAIDER CAMP", 6, 122, 84, 16, CLR_RED);
    sq_marker(map, 96, 126, CLR_RED, 8);           // hostile contact
    box(map, "BUNKER", 132, 122, 56, 16, CLR_YELLOW);
    box(map, "DINER", 226, 122, 84, 16, CLR_GREEN);

    // ── Scrapyard + crater (far south) ──
    box(map, "SCRAPYARD", 6, 150, 84, 16, CLR_YELLOW);
    sq_marker(map, 150, 154, CLR_ORANGE, 8);
    lbl(map, "CRATER", 162, 152, CLR_ORANGE);
    box(map, "OUTPOST", 226, 150, 84, 16, CLR_GREEN);

    // ── Position marker (your location, hidden until set) ──
    pos_marker = lv_obj_create(map);
    lv_obj_set_size(pos_marker, 8, 8);
    lv_obj_set_style_bg_color(pos_marker, CLR_HI, 0);
    lv_obj_set_style_bg_opa(pos_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(pos_marker, lv_color_white(), 0);
    lv_obj_set_style_border_width(pos_marker, 1, 0);
    lv_obj_set_style_radius(pos_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(pos_marker, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    pos_label = lv_label_create(map);
    lv_label_set_text(pos_label, "YOU");
    lv_obj_set_style_text_font(pos_label, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(pos_label, CLR_HI, 0);

    // Show existing position or hide
    if (g_my_pos_x >= 0 && g_my_pos_y >= 0) {
        lv_obj_set_pos(pos_marker, g_my_pos_x - 4, g_my_pos_y - 4);
        lv_obj_set_pos(pos_label, g_my_pos_x + 6, g_my_pos_y - 4);
    } else {
        lv_obj_add_flag(pos_marker, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(pos_label, LV_OBJ_FLAG_HIDDEN);
    }

    // ── LEGEND ──
    int ly = 186;
    lv_obj_t* div = lv_obj_create(map);
    lv_obj_set_size(div, 316, 1);
    lv_obj_set_pos(div, 0, ly - 4);
    lv_obj_set_style_bg_color(div, CLR_DARK, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);

    sq_marker(map, 6, ly + 2, CLR_GREEN);
    lbl(map, "Town", 14, ly, CLR_DIM);
    sq_marker(map, 58, ly + 2, CLR_GOLD);
    lbl(map, "Vault", 66, ly, CLR_DIM);
    sq_marker(map, 110, ly + 2, CLR_YELLOW);
    lbl(map, "Site", 118, ly, CLR_DIM);
    sq_marker(map, 156, ly + 2, CLR_ORANGE);
    lbl(map, "Rad", 164, ly, CLR_DIM);
    sq_marker(map, 196, ly + 2, CLR_RED);
    lbl(map, "Foe", 204, ly, CLR_DIM);

    // Tap hint
    lbl(map, "TAP MAP TO SET POS", 6, ly + 10, CLR_DIM);

    // ── RADAR button ──
    lv_obj_t* rbtn = lv_btn_create(map);
    lv_obj_set_size(rbtn, 52, 18);
    lv_obj_set_pos(rbtn, 262, ly - 6);
    lv_obj_set_style_bg_color(rbtn, CLR_SEL, 0);
    lv_obj_set_style_bg_opa(rbtn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(rbtn, CLR_GREEN, 0);
    lv_obj_set_style_border_width(rbtn, 1, 0);
    lv_obj_set_style_radius(rbtn, 2, 0);
    lv_obj_set_style_pad_all(rbtn, 0, 0);
    lv_obj_add_event_cb(rbtn, radar_toggle_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* rbtn_lbl = lv_label_create(rbtn);
    lv_label_set_text(rbtn_lbl, "RADAR");
    lv_obj_set_style_text_font(rbtn_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(rbtn_lbl, CLR_GREEN, 0);
    lv_obj_align(rbtn_lbl, LV_ALIGN_CENTER, 0, 0);

    // Build radar overlay (hidden)
    build_radar(map);

    return scr_map;
}
