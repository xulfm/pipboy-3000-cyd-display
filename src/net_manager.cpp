// ============================================================
// Vault 89 PDI-Boy -- Network Manager (Orchestrator)
// Init sequence: WiFi → ESP-NOW → WebSocket
// ============================================================

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include "net_config.h"
#include "net_state.h"
#include "weather.h"
#include "clock_sync.h"
#include "landmarks.h"

// Global state definitions
NetState    g_net       = {};
EspNowPeer  g_peers[ESPNOW_MAX_PEERS] = {};
int         g_peer_count = 0;
ChatMsg     g_chat[CHAT_MAX] = {};
int         g_chat_head  = 0;
int         g_chat_count = 0;
int16_t     g_my_pos_x   = -1;
int16_t     g_my_pos_y   = -1;
bool        g_logged_in  = false;
bool        g_is_anchor       = false;
int         g_anchor_landmark = -1;

// Apply the current anchor selection to our broadcast position.
static void anchor_apply_pos(void) {
    if (g_is_anchor && g_anchor_landmark >= 0 &&
        g_anchor_landmark < LANDMARK_COUNT) {
        g_my_pos_x = LANDMARKS[g_anchor_landmark].x;
        g_my_pos_y = LANDMARKS[g_anchor_landmark].y;
    }
}

void anchor_load(void) {
    Preferences p;
    if (p.begin("anchor", true)) {
        g_is_anchor       = p.getBool("on", false);
        g_anchor_landmark = p.getInt("lm", -1);
        p.end();
    }
    if (g_anchor_landmark < 0 || g_anchor_landmark >= LANDMARK_COUNT) {
        g_anchor_landmark = 0;   // default to VAULT if unset/invalid
    }
    anchor_apply_pos();
    Serial.printf("[anchor] load on=%d lm=%d (%s)\n",
                  g_is_anchor, g_anchor_landmark,
                  LANDMARKS[g_anchor_landmark].name);
}

void anchor_set(bool on, int landmark_idx) {
    g_is_anchor = on;
    if (landmark_idx >= 0 && landmark_idx < LANDMARK_COUNT)
        g_anchor_landmark = landmark_idx;
    Preferences p;
    if (p.begin("anchor", false)) {
        p.putBool("on", g_is_anchor);
        p.putInt("lm", g_anchor_landmark);
        p.end();
    }
    if (g_is_anchor) {
        anchor_apply_pos();
    } else {
        // Leaving anchor mode — clear our fixed position so we go back to
        // being a roaming/estimated peer.
        g_my_pos_x = -1;
        g_my_pos_y = -1;
    }
    Serial.printf("[anchor] set on=%d lm=%d (%s) pos=(%d,%d)\n",
                  g_is_anchor, g_anchor_landmark,
                  LANDMARKS[g_anchor_landmark].name, g_my_pos_x, g_my_pos_y);
}

const char* link_mode_str(void) {
    switch (g_net.link_mode) {
        case LINK_WIFI:    return "WIFI";
        case LINK_MESH:    return "MESH";
        default:           return "OFFLINE";
    }
}

// Forward declarations for subsystem functions
extern void wifi_init(void);
extern void wifi_poll(void);
extern void espnow_init(void);
extern void espnow_poll(void);
extern void espnow_channel_lock_poll(void);
extern void ws_init(void);
extern void ws_poll(void);

static bool s_espnow_started = false;

void net_init(void) {
    Serial.println("[net] Initializing...");
    wifi_init();
    ws_init();
    // ESP-NOW init is deferred until WiFi connects (need channel)
}

void net_init_deferred(void) {
    // Called when WiFi picker handles connection — skip wifi_init()
    Serial.println("[net] Deferred init (WiFi managed by picker)...");
    ws_init();
}

// ── Shared chat ring helper ──
// Returns true if the message was new (and was inserted), false if it was a
// duplicate. Used by both the ESP-NOW and WebSocket layers so the bridge can
// avoid infinite echo loops when cross-relaying.
bool chat_push_if_new(const char* from, const char* text, bool is_dm) {
    if (!from || !text) return false;
    // Dedup by {from, text} against the whole ring
    for (int i = 0; i < g_chat_count; i++) {
        int idx = (g_chat_head + i) % CHAT_MAX;
        if (strcmp(g_chat[idx].from, from) == 0 &&
            strcmp(g_chat[idx].text, text) == 0) {
            return false;
        }
    }
    int idx = (g_chat_head + g_chat_count) % CHAT_MAX;
    if (g_chat_count >= CHAT_MAX) {
        g_chat_head = (g_chat_head + 1) % CHAT_MAX;
    } else {
        g_chat_count++;
    }
    strncpy(g_chat[idx].from, from, sizeof(g_chat[idx].from) - 1);
    g_chat[idx].from[sizeof(g_chat[idx].from) - 1] = '\0';
    strncpy(g_chat[idx].text, text, sizeof(g_chat[idx].text) - 1);
    g_chat[idx].text[sizeof(g_chat[idx].text) - 1] = '\0';
    g_chat[idx].timestamp = millis();
    g_chat[idx].is_dm     = is_dm;
    return true;
}

// ── Mesh positioning helpers ──────────────────────────────────
// Rough RSSI -> distance via the log-distance path-loss model.
// ref = expected RSSI at 1m, n = environment path-loss exponent.
// Result is in "meters-ish" — good for relative weighting, not survey.
float mesh_rssi_to_dist(int rssi) {
    const float ref = -45.0f;   // dBm at ~1m
    const float n   = 2.5f;     // indoor-ish path loss
    if (rssi == 0) rssi = -80;  // unknown -> assume far
    float d = powf(10.0f, (ref - (float)rssi) / (10.0f * n));
    if (d < 0.3f) d = 0.3f;
    if (d > 200.0f) d = 200.0f;
    return d;
}

int mesh_active_count(void) {
    int n = g_logged_in ? 1 : 0;   // count self only once we're a real node
    for (int i = 0; i < g_peer_count; i++) if (g_peers[i].online) n++;
    return n;
}

int mesh_anchor_count(void) {
    int n = (g_is_anchor ? 1 : 0);
    for (int i = 0; i < g_peer_count; i++)
        if (g_peers[i].online && g_peers[i].is_anchor) n++;
    return n;
}

// Periodically estimate our own position by weighted-centroid of the
// anchors we hear (closer/stronger anchors pull harder), then let the
// heartbeat broadcast it so every device can plot us. Only runs when
// there are enough devices + anchors; otherwise leaves manual taps alone.
static bool          s_auto_pos      = false;
static unsigned long s_last_localize = 0;

void localize_poll(void) {
    unsigned long now = millis();
    if (now - s_last_localize < 1500) return;
    s_last_localize = now;

    if (g_is_anchor) return;   // anchors keep their fixed landmark position

    bool positioning_ok = (mesh_active_count() >= 3) && (mesh_anchor_count() >= 2);
    if (!positioning_ok) {
        // Drop any stale auto-estimate so we don't broadcast a ghost
        // position; preserve a manual map tap (s_auto_pos == false).
        if (s_auto_pos) { g_my_pos_x = -1; g_my_pos_y = -1; s_auto_pos = false; }
        return;
    }

    float sw = 0, sx = 0, sy = 0;
    int used = 0;
    for (int i = 0; i < g_peer_count; i++) {
        const EspNowPeer& p = g_peers[i];
        if (!p.online || !p.is_anchor || p.pos_x < 0) continue;
        int rssi = p.rssi_avg ? p.rssi_avg : p.rssi;
        float d = mesh_rssi_to_dist(rssi);
        float w = 1.0f / (d * d + 1.0f);
        sw += w; sx += w * p.pos_x; sy += w * p.pos_y; used++;
    }
    if (used >= 2 && sw > 0) {
        g_my_pos_x = (int16_t)(sx / sw);
        g_my_pos_y = (int16_t)(sy / sw);
        s_auto_pos = true;
    }
}

void net_poll(void) {
    wifi_poll();

    // Start ESP-NOW as soon as WiFi mode is set (doesn't need connection)
    if (!s_espnow_started) {
        espnow_init();
        s_espnow_started = true;
    }

    if (s_espnow_started) {
        espnow_poll();
        // Keep our radio on the same channel as the saved AP when we're
        // offline, so mixed (online+offline) peers can still hear each
        // other and the WS<->ESP-NOW chat bridge works.
        espnow_channel_lock_poll();
    }

    ws_poll();

    // Compute our link-mode classification for UI indicators. WIFI wins
    // outright (we have real internet); otherwise if we have any online
    // mesh peer the link is MESH (we can still get chat / time via ESP-
    // NOW relay); otherwise we're OFFLINE.
    if (g_net.wifi_connected) {
        g_net.link_mode = LINK_WIFI;
    } else {
        bool any_online = false;
        for (int i = 0; i < g_peer_count; i++) {
            if (g_peers[i].online) { any_online = true; break; }
        }
        g_net.link_mode = any_online ? LINK_MESH : LINK_OFFLINE;
    }

    // Estimate our position from anchors (when enough are present) so the
    // heartbeat can broadcast it for everyone's radar.
    localize_poll();

    // Kick off weather fetches once WiFi is up (non-blocking)
    weather_poll();
}

// Settings > Refresh Now entry point. Drop every fresh-data latch so the
// next net_poll() cycle re-pulls weather + NTP, opens a WS sync window,
// and broadcasts a REFRESH_REQ for any peer that happens to currently
// own the WiFi uplink.
void net_force_refresh(void) {
    Serial.println("[net] Force refresh");
    weather_force_refresh();
    clock_force_ntp_sync();
    ws_open_sync_window(60UL * 1000UL);
    espnow_broadcast_refresh_req();
}
