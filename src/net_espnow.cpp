// ============================================================
// Vault 89 PDI-Boy -- ESP-NOW Peer Discovery
// Broadcast heartbeat with position, track peers, RSSI,
// chat (group + DM), and chat history sync
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <time.h>
#include "net_config.h"
#include "net_state.h"
#include "pipboy_data.h"
#include "sound_fx.h"
#include "clock_sync.h"
#include "weather.h"
#include "game_proto.h"

// ESP-NOW message types
enum MsgType : uint8_t {
    MSG_HEARTBEAT   = 1,
    MSG_CHAT        = 2,
    MSG_DM          = 3,
    MSG_SYNC_REQ    = 4,   // "I have N messages, send me what I'm missing"
    MSG_SYNC_ENTRY  = 5,   // one chat message from history
    MSG_TIME        = 6,   // broadcast UTC epoch so offline peers can set clock
    MSG_WEATHER     = 7,   // gateway device shares a fresh weather fetch
    MSG_GATEWAY_ADV = 8,   // "I can reach the internet, here's my AP signal"
    MSG_REFRESH_REQ = 9,   // Settings>Refresh Now: ask gateway to re-pull data
    MSG_PING_REQ    = 10,  // mesh peer asks gateway to TCP-ping 1.1.1.1
    MSG_PING_REPLY  = 11,  // gateway returns the result (ms or -1)
    // 12..19 reserved for game_proto (see include/game_proto.h).
    MSG_CHANNEL_HINT = 20, // "I'm about to associate on ch X — follow me"
};

struct __attribute__((packed)) EspNowChannelHint {
    uint8_t type;       // MSG_CHANNEL_HINT
    uint8_t target_ch;  // 1..13
    uint8_t _pad[2];
};

// Wire format — must fit in 250 bytes. All network frames begin with a
// uint8_t type so on_recv() can dispatch before committing to a struct.
struct __attribute__((packed)) EspNowMsg {
    uint8_t type;
    char    name[20];
    char    role[20];
    char    payload[32];
    int16_t pos_x;        // map position (heartbeat), or sync metadata
    int16_t pos_y;
    uint8_t flags;        // EN_FLAG_* (heartbeat); 0 for other msg types
};

// EspNowMsg.flags bits (heartbeat only)
#define EN_FLAG_ANCHOR  0x01   // sender is a fixed-position anchor

// The compact EspNowWeatherMsg wire-format struct for MSG_WEATHER now
// lives in include/weather.h so weather.cpp can memcpy directly into
// g_weather from the rx path without duplicating the layout.

// Gateway advertisement — broadcast every 10s by devices that either
// have saved WiFi credentials OR are actively connected to an AP. All
// peers listen and record gw_* fields on their EspNowPeer entry so a
// future deferral step can pick the best gateway without racing.
struct __attribute__((packed)) EspNowGatewayAdv {
    uint8_t  type;        // MSG_GATEWAY_ADV
    uint8_t  flags;       // GW_FLAG_* bitmask
    int8_t   wifi_rssi;   // our RSSI to our AP, 0 if not connected
    uint8_t  wifi_channel; // AP channel (1-13), 0 if unknown
    uint32_t uptime_ms;   // for tiebreaking when RSSIs tie
    char     name[20];    // dweller name (blank pre-login)
    char     ssid[33];    // AP name (debug / future display)
};

static unsigned long s_last_heartbeat = 0;
static unsigned long s_last_gw_adv    = 0;
static unsigned long s_init_ms        = 0;
static bool          s_boot_sync_done = false;
static uint8_t       s_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Deferred sound flags (set in ISR context, consumed in espnow_poll)
static volatile bool s_sfx_new_peer  = false;
static volatile bool s_sfx_chat_in   = false;
static volatile bool s_do_sync_reply = false;

// Deferred ping relay — on_recv() sets the flag, espnow_poll() does the
// TCP connect on the main core and broadcasts the reply. The requester's
// MAC is saved so we can route the reply (though we broadcast it anyway
// so any peer can see it).
static volatile bool s_ping_requested = false;
static uint8_t       s_ping_req_mac[6] = {};

// Deferred channel lock — set by on_recv() when a GW_ADV carries a
// wifi_channel. espnow_channel_lock_poll() picks it up on the main core
// and calls esp_wifi_set_channel(). This avoids calling the WiFi driver
// from the RX callback context.
static volatile uint8_t s_target_channel = 0;

// Best-gateway tracker for mesh-only devices. The first GW_ADV we hear
// doesn't necessarily come from the most useful gateway — a peer with
// only HAS_CREDS set (WiFi not yet up) scores worse than one that's
// actively serving NTP + weather. Track the incumbent so we only
// re-target the channel when a *better* gateway appears, or when the
// current one has gone silent long enough to be considered dead.
static uint8_t       s_best_gw_mac[6]   = {};
static uint8_t       s_best_gw_score    = 0;
static unsigned long s_best_gw_adv_ms   = 0;
#define GW_STALE_MS   45000UL

// Score a GW_ADV flags byte. WIFI_CONN dominates (without it, the
// gateway can't actually fetch anything on our behalf); NTP and
// WEATHER are tiebreakers. HAS_CREDS alone is worthless to a mesh-only
// peer — the advertiser might never succeed in connecting.
static uint8_t gw_score(uint8_t flags) {
    uint8_t s = 0;
    if (flags & GW_FLAG_WIFI_CONN)   s += 4;
    if (flags & GW_FLAG_HAS_NTP)     s += 2;
    if (flags & GW_FLAG_HAS_WEATHER) s += 1;
    return s;
}

// Mesh ping result callback — set by the ping screen so it can receive
// results from the gateway without a hard dependency between files.
static void (*s_mesh_ping_cb)(int ms) = NULL;

void espnow_set_mesh_ping_cb(void (*cb)(int ms)) {
    s_mesh_ping_cb = cb;
}

// Peer-event sound throttling. The CYD buzzer is piercing and peers can
// flap briefly due to WiFi power-save drops or brief channel hops. Keep
// a minimum gap between each kind of notification so the office doesn't
// turn into a beep factory.
#define SFX_PEER_MIN_MS   30000
static unsigned long s_last_peer_join_ms  = 0;
static unsigned long s_last_peer_leave_ms = 0;

// Deferred WS relay queue (messages we received via ESP-NOW that we want to
// forward up to the cloud so WS-only peers can see them)
#define WS_RELAY_QUEUE 4
static volatile bool s_ws_relay_pending[WS_RELAY_QUEUE] = {};
static char          s_ws_relay_from[WS_RELAY_QUEUE][20] = {};
static char          s_ws_relay_text[WS_RELAY_QUEUE][CHAT_MSG_LEN] = {};
static volatile int  s_ws_relay_head = 0;
static volatile int  s_ws_relay_tail = 0;

static void queue_ws_relay(const char* from, const char* text) {
    int next = (s_ws_relay_tail + 1) % WS_RELAY_QUEUE;
    if (next == s_ws_relay_head) return;  // full, drop
    strncpy((char*)s_ws_relay_from[s_ws_relay_tail], from, 19);
    s_ws_relay_from[s_ws_relay_tail][19] = '\0';
    strncpy((char*)s_ws_relay_text[s_ws_relay_tail], text, CHAT_MSG_LEN - 1);
    s_ws_relay_text[s_ws_relay_tail][CHAT_MSG_LEN - 1] = '\0';
    s_ws_relay_pending[s_ws_relay_tail] = true;
    s_ws_relay_tail = next;
}

// Chat sync state (main-core only)
static bool          s_syncing          = false;
static int           s_sync_idx         = 0;
static unsigned long s_sync_next_ms     = 0;
static unsigned long s_last_sync_req_ms = 0;   // cooldown to prevent loops
#define SYNC_INTERVAL_MS  40    // 40ms between sync packets (~25/sec)
#define SYNC_COOLDOWN_MS  5000  // don't re-request sync for 5 seconds

// Fixed RSSI display value — real per-packet RSSI would require
// promiscuous mode, which has turned out to be flaky with ESP-NOW RX.
static volatile int8_t s_last_rssi = -60;

static uint8_t rssi_to_sig(int8_t rssi) {
    if (rssi >= -50) return 4;
    if (rssi >= -60) return 3;
    if (rssi >= -70) return 2;
    if (rssi >= -80) return 1;
    return 0;
}

static int find_peer(const uint8_t* mac) {
    for (int i = 0; i < g_peer_count; i++) {
        if (memcmp(g_peers[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

// Forward decl so the send helper can re-arm ESP-NOW on NOT_INIT errors.
void espnow_init(void);

// Rate-limit the self-heal path. esp_now_deinit() + esp_now_init() are
// blocking calls into the WiFi driver and can stall the LVGL loop for
// 500-1500 ms. If we fire this on every 3 s heartbeat the user sees
// constant micro-freezes. Allow at most one self-heal per SELFHEAL_MIN_MS.
#define SELFHEAL_MIN_MS   15000
static unsigned long s_last_selfheal_ms = 0;

// Send helper that detects a driver teardown (ESP_ERR_ESPNOW_NOT_INIT,
// which ESP-IDF logs as "esp now not init!") and re-arms ESP-NOW on the
// fly so we recover from WiFi-stack state flips without needing a reboot.
static esp_err_t espnow_send_resilient(const uint8_t* mac, const uint8_t* buf, size_t len) {
    esp_err_t rc = esp_now_send(mac, buf, len);
    if (rc == ESP_ERR_ESPNOW_NOT_INIT) {
        unsigned long now = millis();
        if (s_last_selfheal_ms == 0 || (now - s_last_selfheal_ms) >= SELFHEAL_MIN_MS) {
            Serial.println("[espnow] send NOT_INIT -> re-initializing");
            s_last_selfheal_ms = now;
            // Force a fresh init — espnow_init() is idempotent.
            g_net.espnow_ready = true;   // let it take the deinit path
            espnow_init();
            if (g_net.espnow_ready) {
                rc = esp_now_send(mac, buf, len);
            }
        } else {
            // Silently drop — we'll recover on the next allowed heal window.
        }
    }
    return rc;
}

// Chat ring buffer push/dedup is provided by net_manager.cpp via
// chat_push_if_new() so the ESP-NOW and WS bridges can share dedup state.

// Saved WiFi creds accessor lives in main.cpp
extern bool wifi_creds_load(char* ssid, size_t ssid_len, char* pass, size_t pass_len);

// ── ESP-NOW channel discovery (hop → lock) ──
// ESP-NOW piggybacks on the single WiFi radio, which can only sit on one
// channel at a time. When THIS device is connected to an AP, the driver
// owns the channel (whatever the router uses). When THIS device is NOT
// connected, its radio floats on channel 1 by default — which means a
// mixed pair (one online device on the router's channel, one offline)
// never hears each other.
//
// The old fix was to scan for a saved SSID and pin to its channel, but
// that requires credentials to already match the exact hotspot the
// online device is on, which isn't always the case (hotspot naming,
// different saved networks, etc). Also, WiFi.scanNetworks() disturbs
// the radio in ways that can break ESP-NOW RX on some SDK builds.
//
// New approach: passive channel hopping. When offline and we haven't
// recently heard an ESP-NOW peer, cycle through the three common 2.4-
// GHz non-overlapping channels (1/6/11) every HOP_DWELL_MS. The hop
// dwell is tuned above the HEARTBEAT_MS (3s) so each visit is long
// enough to catch at least one broadcast from an online peer. As soon
// as on_recv fires and bumps any peer's last_seen_ms, the hopper
// freezes on that channel. If the peer later goes silent for a while,
// we resume hopping to find whatever channel they moved to.
static uint8_t s_locked_channel = 0;   // 0 = unknown / never set
static unsigned long s_last_hop_ms = 0;
static int s_hop_idx = 0;
static const uint8_t s_hop_channels[] = {1, 6, 11};
// Touched by on_recv() whenever ANY valid ESP-NOW frame is decoded so we
// can freeze channel hopping even when the other device is only sending
// MSG_TIME (e.g. pre-login), not heartbeats.
static volatile unsigned long s_last_any_rx_ms = 0;
#define HOP_DWELL_MS     4000   // >HEARTBEAT_MS so we catch at least one per hop
#define LOCK_RECENT_MS   40000  // TIME beacons are every 30s, stay a bit past that

// Return the most recent peer last_seen_ms (0 if we've never heard anyone).
static unsigned long newest_peer_ms(void) {
    unsigned long newest = 0;
    for (int i = 0; i < g_peer_count; i++) {
        if (g_peers[i].last_seen_ms > newest) newest = g_peers[i].last_seen_ms;
    }
    return newest;
}

void espnow_channel_lock_poll(void) {
    // When we're connected to a real AP, the driver owns the channel.
    if (g_net.wifi_connected) {
        uint8_t ch = WiFi.channel();
        if (ch != s_locked_channel) {
            s_locked_channel = ch;
            Serial.printf("[espnow] Channel tracked from AP: %u\n", ch);
        }
        s_target_channel = 0;  // don't apply stale hints
        return;
    }

    // WiFi.begin() was called but hasn't finished the 4-way handshake yet.
    // Calling esp_wifi_set_channel() during the association sequence breaks
    // the WPA handshake and the connection never completes. Back off and
    // let the driver do its thing until it either connects or times out.
    wl_status_t ws = WiFi.status();
    if (ws == WL_IDLE_STATUS || ws == WL_SCAN_COMPLETED) {
        // Driver is actively associating — hands off.
        return;
    }

    // Fast path: a GW_ADV told us exactly which channel the gateway's AP
    // uses. Switch immediately instead of waiting for the hopper to land
    // on it by chance. This is the normal path for mesh-only devices that
    // can overhear the gateway's broadcast (even cross-channel at low
    // RSSI) but need to be ON that channel for reliable data frames.
    uint8_t target = s_target_channel;
    if (target > 0 && target != s_locked_channel) {
        esp_err_t e = esp_wifi_set_channel(target, WIFI_SECOND_CHAN_NONE);
        if (e == ESP_OK) {
            s_locked_channel = target;
            s_last_hop_ms    = millis();
            Serial.printf("[espnow] ChLock -> ch %u (from GW_ADV)\n", target);
        } else {
            Serial.printf("[espnow] set_channel(%u) FAILED rc=%d\n",
                          target, (int)e);
        }
        s_target_channel = 0;
        return;
    }
    s_target_channel = 0;

    unsigned long now = millis();

    // Freeze the hopper only if we've recently heard a GATEWAY peer on
    // this channel — not generic mesh traffic. Otherwise a cluster of
    // mesh-only devices on ch=1 can trap us here even when the real
    // gateway is broadcasting on ch=11. The hopper keeps cycling until
    // it finds a GW_ADV; once it does, the fast path above locks us on.
    bool heard_gateway = false;
    for (int i = 0; i < g_peer_count; i++) {
        if (g_peers[i].gw_flags != 0 &&
            g_peers[i].gw_last_adv_ms != 0 &&
            (now - g_peers[i].gw_last_adv_ms) < LOCK_RECENT_MS) {
            heard_gateway = true;
            break;
        }
    }
    if (heard_gateway) {
        s_last_hop_ms = now;
        return;
    }

    // Otherwise hop every HOP_DWELL_MS looking for traffic.
    if (s_last_hop_ms != 0 && (now - s_last_hop_ms) < HOP_DWELL_MS) return;
    s_last_hop_ms = now;

    uint8_t next_ch = s_hop_channels[s_hop_idx];
    s_hop_idx = (s_hop_idx + 1) % (int)(sizeof(s_hop_channels));
    esp_err_t e = esp_wifi_set_channel(next_ch, WIFI_SECOND_CHAN_NONE);
    if (e == ESP_OK) {
        if (next_ch != s_locked_channel) {
            for (int i = 0; i < g_peer_count; i++) {
                g_peers[i].online    = false;
                g_peers[i].sig_level = 0;
            }
        }
        s_locked_channel = next_ch;
        Serial.printf("[espnow] ChHop -> ch %u\n", next_ch);
    } else {
        Serial.printf("[espnow] set_channel(%u) rc=%d\n", next_ch, (int)e);
    }
}

// ── Receive callback (WiFi task, core 0) ──

static void on_recv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len < 1) return;
    const uint8_t type = data[0];

    // Any valid frame on our current channel counts as activity — freeze
    // the channel hopper. Bumped before type dispatch so odd/new frame
    // types still count as "someone is here".
    s_last_any_rx_ms = millis();

    // Game protocol frames (MSG_GAME_INVITE..MSG_GAME_END = 12..18) have
    // their own GameHdr-prefixed layouts. Hand off to game_proto which
    // validates length per-subtype before touching the body.
    if (type >= MSG_GAME_INVITE && type <= MSG_GAME_END) {
        Serial.printf("[espnow] rx GAME type=%u len=%d from %02X:%02X:%02X:%02X:%02X:%02X ch=%u\n",
                      (unsigned)type, len,
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                      (unsigned)s_locked_channel);
        game_on_rx(mac, data, len);
        return;
    }

    // Channel hint: a peer is announcing the AP channel it's about to
    // associate on. Jump to that channel so we stay reachable without
    // waiting for the hop cycle to land on it by chance.
    if (type == MSG_CHANNEL_HINT) {
        if (len < (int)sizeof(EspNowChannelHint)) return;
        const EspNowChannelHint* h = (const EspNowChannelHint*)data;
        if (h->target_ch >= 1 && h->target_ch <= 13) {
            s_target_channel = h->target_ch;
            Serial.printf("[espnow] rx CHANNEL_HINT ch=%u (queued)\n",
                          (unsigned)h->target_ch);
        }
        return;
    }

    // MSG_WEATHER uses a different (larger) packed struct than the
    // legacy EspNowMsg, so it has to be dispatched before the legacy
    // size check rejects it. We hand the bytes off to weather.cpp which
    // applies them to g_weather if it's running on a mesh-only device.
    if (type == MSG_WEATHER) {
        if (len < (int)sizeof(EspNowWeatherMsg)) {
            Serial.printf("[espnow] rx short WX len=%d\n", len);
            return;
        }
        weather_apply_from_mesh((const uint8_t*)data, len);
        return;
    }

    // MSG_GATEWAY_ADV is also its own packed layout. Track the peer's
    // gateway metadata so we can later decide whether to defer our own
    // WiFi uplink to theirs.
    if (type == MSG_GATEWAY_ADV) {
        if (len < (int)sizeof(EspNowGatewayAdv)) {
            Serial.printf("[espnow] rx short GW_ADV len=%d\n", len);
            return;
        }
        const EspNowGatewayAdv* adv = (const EspNowGatewayAdv*)data;
        int idx = find_peer(mac);
        if (idx < 0 && g_peer_count < ESPNOW_MAX_PEERS) {
            idx = g_peer_count++;
            memcpy(g_peers[idx].mac, mac, 6);
            g_peers[idx].pos_x = -1;
            g_peers[idx].pos_y = -1;
        }
        if (idx >= 0) {
            g_peers[idx].gw_flags       = adv->flags;
            g_peers[idx].gw_wifi_rssi   = adv->wifi_rssi;
            g_peers[idx].gw_last_adv_ms = millis();
            g_peers[idx].last_seen_ms   = millis();
            g_peers[idx].online         = true;
            g_peers[idx].rssi           = s_last_rssi;
            g_peers[idx].sig_level      = rssi_to_sig(s_last_rssi);
            // Adopt name only if peer has published a real one AND we
            // don't already have a better one for this MAC.
            if (adv->name[0] && g_peers[idx].name[0] == '\0') {
                strncpy(g_peers[idx].name, adv->name, sizeof(g_peers[idx].name) - 1);
                g_peers[idx].name[sizeof(g_peers[idx].name) - 1] = '\0';
            }
            Serial.printf("[espnow] rx GW_ADV from %02X:%02X:%02X:%02X:%02X:%02X "
                          "flags=0x%02X rssi=%d ch=%u ssid='%s'\n",
                          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                          adv->flags, (int)adv->wifi_rssi,
                          (unsigned)adv->wifi_channel, adv->ssid);
            // If the gateway reports an AP channel and we're not on WiFi
            // ourselves, decide whether this gateway beats our current
            // incumbent. Prefer the one with the most "actually useful"
            // flags (WIFI_CONN > HAS_NTP > HAS_WEATHER). Re-target the
            // channel only on upgrade, or when the incumbent has gone
            // silent long enough to be considered dead.
            if (adv->wifi_channel > 0 && !g_net.wifi_connected) {
                uint8_t  score   = gw_score(adv->flags);
                bool     same_gw = (memcmp(mac, s_best_gw_mac, 6) == 0);
                bool     stale   = (s_best_gw_adv_ms == 0) ||
                                   ((millis() - s_best_gw_adv_ms) > GW_STALE_MS);
                bool     adopt   = stale || (score > s_best_gw_score);
                if (same_gw) {
                    // Refresh incumbent's score + timestamp; don't touch channel.
                    s_best_gw_score  = score;
                    s_best_gw_adv_ms = millis();
                } else if (adopt) {
                    memcpy(s_best_gw_mac, mac, 6);
                    s_best_gw_score  = score;
                    s_best_gw_adv_ms = millis();
                    s_target_channel = adv->wifi_channel;
                    Serial.printf("[espnow] GW adopt %02X:%02X:%02X:%02X:%02X:%02X "
                                  "score=%u ch=%u (%s)\n",
                                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                                  score, adv->wifi_channel,
                                  stale ? "incumbent stale" : "higher score");
                }
            }
        }
        return;
    }

    // All legacy message types share the fixed-size EspNowMsg layout.
    if (len < (int)sizeof(EspNowMsg)) {
        Serial.printf("[espnow] rx short frame len=%d from %02X:%02X:%02X:%02X:%02X:%02X\n",
                      len, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return;
    }
    const EspNowMsg* msg = (const EspNowMsg*)data;
    int8_t rssi = s_last_rssi;

    if (msg->type == MSG_HEARTBEAT) {
        int idx = find_peer(mac);
        if (idx < 0 && g_peer_count < ESPNOW_MAX_PEERS) {
            idx = g_peer_count++;
            memcpy(g_peers[idx].mac, mac, 6);
            g_peers[idx].pos_x = -1;
            g_peers[idx].pos_y = -1;
            Serial.printf("[espnow] New peer: %s (%02X:%02X:%02X:%02X:%02X:%02X)\n",
                          msg->name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            s_sfx_new_peer = true;
        }
        if (idx >= 0) {
            // Only adopt a NON-EMPTY name. A device sitting on the login
            // screen (or freshly rebooted) broadcasts discover-beacon
            // heartbeats with an empty name; without this guard those wipe
            // the good name peers already learned, making the radar / peer
            // list flicker to blank. Keep the last known good name instead.
            if (msg->name[0]) {
                strncpy(g_peers[idx].name, msg->name, sizeof(g_peers[idx].name) - 1);
                g_peers[idx].name[sizeof(g_peers[idx].name) - 1] = '\0';
            }
            strncpy(g_peers[idx].role, msg->role, sizeof(g_peers[idx].role) - 1);
            g_peers[idx].role[sizeof(g_peers[idx].role) - 1] = '\0';
            if (msg->payload[0]) {
                strncpy(g_peers[idx].last_msg, msg->payload, sizeof(g_peers[idx].last_msg) - 1);
                g_peers[idx].last_msg[sizeof(g_peers[idx].last_msg) - 1] = '\0';
            }
            g_peers[idx].rssi         = rssi;
            g_peers[idx].sig_level    = rssi_to_sig(rssi);
            g_peers[idx].online       = true;
            g_peers[idx].last_seen_ms = millis();
            g_peers[idx].pos_x        = msg->pos_x;
            g_peers[idx].pos_y        = msg->pos_y;
            g_peers[idx].is_anchor    = (msg->flags & EN_FLAG_ANCHOR) != 0;
        }
    } else if (msg->type == MSG_CHAT) {
        if (chat_push_if_new(msg->name, msg->payload, false)) {
            Serial.printf("[espnow] Chat from %s: %s\n", msg->name, msg->payload);
            s_sfx_chat_in = true;
            queue_ws_relay(msg->name, msg->payload);  // bridge to cloud
        }
    } else if (msg->type == MSG_DM) {
        if (chat_push_if_new(msg->name, msg->payload, true)) {
            Serial.printf("[espnow] DM from %s: %s\n", msg->name, msg->payload);
            s_sfx_chat_in = true;
        }
    } else if (msg->type == MSG_SYNC_REQ) {
        // Peer wants to sync — send ALL our messages so they can dedup
        if (g_chat_count > 0) {
            s_do_sync_reply = true;
            Serial.printf("[espnow] Sync request from %s, sending our %d msgs\n",
                          msg->name, g_chat_count);
        }
    } else if (msg->type == MSG_SYNC_ENTRY) {
        // Incoming sync message — add if we don't have it
        bool is_dm = (msg->pos_x != 0);
        if (chat_push_if_new(msg->name, msg->payload, is_dm)) {
            Serial.printf("[espnow] Sync'd: %s: %s\n", msg->name, msg->payload);
        }
    } else if (msg->type == MSG_TIME) {
        // Gateway device has NTP and is sharing its UTC epoch with us.
        // payload holds the epoch as a decimal ASCII string (fits in 32B
        // easily), pos_x carries the sender's timezone in hours so we
        // can show a matching local time. Only adopt if we don't already
        // have our own NTP sync — we don't want to fight a real NTP.
        if (!clock_has_ntp()) {
            uint32_t epoch = (uint32_t)strtoul(msg->payload, NULL, 10);
            if (epoch > 1700000000UL) {   // sanity: must be >= 2023
                clock_set_from_epoch(epoch, (int)msg->pos_x);
            }
        }
    } else if (msg->type == MSG_REFRESH_REQ) {
        Serial.printf("[espnow] Refresh request from %s\n",
                      msg->name[0] ? msg->name : "(peer)");
        if (g_net.wifi_connected) {
            weather_force_refresh();
            clock_force_ntp_sync();
        }
    } else if (msg->type == MSG_PING_REQ) {
        // A mesh peer wants us (the gateway) to TCP-ping 1.1.1.1 on
        // their behalf. Defer to espnow_poll() so we're not doing a
        // blocking TCP connect inside the WiFi RX callback.
        if (g_net.wifi_connected) {
            s_ping_requested = true;
            memcpy(s_ping_req_mac, mac, 6);
        }
    } else if (msg->type == MSG_PING_REPLY) {
        // The gateway sent back a ping result. Feed it to the ping
        // screen if it's currently open.
        int ms = (int)strtol(msg->payload, NULL, 10);
        Serial.printf("[espnow] Ping reply: %d ms\n", ms);
        if (s_mesh_ping_cb) s_mesh_ping_cb(ms);
    }
}

static void on_send(const uint8_t* mac, esp_now_send_status_t status) {
    // Could track delivery stats here
}

void espnow_init(void) {
    // Idempotent: if we were previously initialized and the underlying
    // driver state got torn down (e.g. by a WiFi mode change / reconnect
    // storm), deinit first so esp_now_init() returns cleanly instead of
    // ESP_ERR_ESPNOW_INTERNAL.
    if (g_net.espnow_ready) {
        esp_now_unregister_recv_cb();
        esp_now_unregister_send_cb();
        esp_now_deinit();
        g_net.espnow_ready = false;
    }

    esp_err_t ie = esp_now_init();
    if (ie != ESP_OK) {
        Serial.printf("[espnow] Init FAILED rc=%d\n", (int)ie);
        return;
    }
    esp_now_register_recv_cb(on_recv);
    esp_now_register_send_cb(on_send);

    // NOTE: promiscuous mode was previously enabled here to capture RSSI
    // for a nicer Radio tab display, but it appears to interfere with
    // ESP-NOW broadcast reception on some driver states (specifically,
    // after a WiFi.scanNetworks() or during WiFi connect handshake).
    // RSSI is cosmetic; reception is not. Drop promiscuous and fall
    // back to a static "OK" signal strength.
    s_last_rssi = -60;

    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, s_broadcast_mac, 6);
    peer_info.channel = 0;
    peer_info.encrypt = false;
    esp_err_t ae = esp_now_add_peer(&peer_info);
    Serial.printf("[espnow] add_peer(broadcast) rc=%d\n", (int)ae);

    g_net.espnow_ready = true;
    s_init_ms = millis();
    uint8_t my_mac[6] = {};
    esp_wifi_get_mac(WIFI_IF_STA, my_mac);
    Serial.printf("[espnow] Ready my_mac=%02X:%02X:%02X:%02X:%02X:%02X ch=%u\n",
                  my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5],
                  WiFi.channel());
}

void espnow_poll(void) {
    if (!g_net.espnow_ready) return;
    unsigned long now = millis();

    // Auto-request chat sync once peers have been discovered after boot
    if (!s_boot_sync_done && (now - s_init_ms) > 5000 && g_peer_count > 0) {
        Serial.println("[espnow] Auto sync on boot");
        espnow_request_sync();
        s_boot_sync_done = true;
    }

    // Connection-status sounds (peer-join) are intentionally silent.
    // The devices live on a quiet office desk and peer flap beeps are
    // disruptive. Chat-in still plays so you get notified of a real
    // incoming message; clicks still play via the UI touch handlers.
    if (s_sfx_new_peer) {
        s_sfx_new_peer = false;
        // (peer-join beep suppressed — office ambience)
    }
    if (s_sfx_chat_in) {
        s_sfx_chat_in = false;
        if (!sfx_is_playing()) sfx_play(SFX_MSG_IN);
    }

    // Drain WS relay queue (chat messages we received via ESP-NOW and want
    // to forward to the cloud for WS-only peers)
    while (s_ws_relay_head != s_ws_relay_tail) {
        int h = s_ws_relay_head;
        if (g_net.ws_connected) {
            ws_send_chat_as((const char*)s_ws_relay_from[h],
                            (const char*)s_ws_relay_text[h]);
        }
        s_ws_relay_pending[h] = false;
        s_ws_relay_head = (h + 1) % WS_RELAY_QUEUE;
    }

    // Start sync reply if requested — send ALL our messages, receiver deduplicates
    if (s_do_sync_reply && !s_syncing) {
        s_do_sync_reply = false;
        if (g_chat_count > 0) {
            s_syncing = true;
            s_sync_idx = 0;  // start from oldest message
            s_sync_next_ms = now;
            Serial.printf("[espnow] Syncing all %d messages to peers\n", g_chat_count);
        }
    }

    // Send sync entries one at a time (rate-limited)
    if (s_syncing && now >= s_sync_next_ms) {
        if (s_sync_idx < g_chat_count) {
            int ring_idx = (g_chat_head + s_sync_idx) % CHAT_MAX;
            const ChatMsg& cm = g_chat[ring_idx];

            EspNowMsg msg = {};
            msg.type = MSG_SYNC_ENTRY;
            strncpy(msg.name, cm.from, sizeof(msg.name) - 1);
            strncpy(msg.payload, cm.text, sizeof(msg.payload) - 1);
            msg.pos_x = cm.is_dm ? 1 : 0;
            msg.pos_y = 0;
            espnow_send_resilient(s_broadcast_mac, (uint8_t*)&msg, sizeof(msg));

            s_sync_idx++;
            s_sync_next_ms = now + SYNC_INTERVAL_MS;
        } else {
            s_syncing = false;
            Serial.println("[espnow] Sync complete");
        }
    }

    // Deferred mesh-ping relay. A mesh peer asked us to TCP-ping
    // 1.1.1.1:80 on their behalf. We do the blocking connect here on
    // the main core (not in the WiFi RX callback) and broadcast the
    // result so any peer on the channel can see it.
    if (s_ping_requested) {
        s_ping_requested = false;
        int ms = -1;
        if (g_net.wifi_connected) {
            WiFiClient c;
            c.setTimeout(2);
            uint32_t t0 = millis();
            bool ok = c.connect("1.1.1.1", 80);
            uint32_t dt = millis() - t0;
            if (ok) { c.stop(); ms = (int)dt; }
        }
        // Broadcast the reply so the requester (and everyone else) sees it
        EspNowMsg reply = {};
        reply.type = MSG_PING_REPLY;
        snprintf(reply.payload, sizeof(reply.payload), "%d", ms);
        espnow_send_resilient(s_broadcast_mac, (uint8_t*)&reply, sizeof(reply));
        Serial.printf("[espnow] Ping relay: %d ms\n", ms);
    }

    // Periodic gateway advert. Any device that either has saved WiFi
    // creds OR is currently connected broadcasts its availability so
    // the fleet can elect a single uplink holder. The payload is small
    // enough (~60B) that the 10s cadence is basically free.
    static bool s_gw_creds_checked = false;
    static bool s_gw_has_creds     = false;
    if (!s_gw_creds_checked) {
        char tssid[33] = {}, tpass[65] = {};
        s_gw_has_creds = wifi_creds_load(tssid, sizeof(tssid), tpass, sizeof(tpass));
        s_gw_creds_checked = true;
        Serial.printf("[espnow] GW creds check: has_creds=%d\n", (int)s_gw_has_creds);
    }
    #define GW_ADV_MS 10000UL
    if ((s_gw_has_creds || g_net.wifi_connected) &&
        (s_last_gw_adv == 0 || (now - s_last_gw_adv) >= GW_ADV_MS)) {
        s_last_gw_adv = now;
        EspNowGatewayAdv adv = {};
        adv.type = MSG_GATEWAY_ADV;
        uint8_t f = 0;
        if (s_gw_has_creds)        f |= GW_FLAG_HAS_CREDS;
        if (g_net.wifi_connected)  f |= GW_FLAG_WIFI_CONN;
        if (clock_has_ntp())       f |= GW_FLAG_HAS_NTP;
        if (g_weather.valid)       f |= GW_FLAG_HAS_WEATHER;
        adv.flags        = f;
        adv.wifi_rssi    = g_net.wifi_connected ? (int8_t)g_net.wifi_rssi : 0;
        adv.wifi_channel = g_net.wifi_connected ? (uint8_t)WiFi.channel() : 0;
        adv.uptime_ms    = now;
        if (g_logged_in) {
            strncpy(adv.name, user_display_name(), sizeof(adv.name) - 1);
        }
        if (g_net.wifi_connected) {
            String s = WiFi.SSID();
            strncpy(adv.ssid, s.c_str(), sizeof(adv.ssid) - 1);
        }
        esp_err_t se = espnow_send_resilient(s_broadcast_mac, (uint8_t*)&adv, sizeof(adv));
        Serial.printf("[espnow] GW_ADV TX flags=0x%02X rssi=%d ssid='%s' rc=%d\n",
                      adv.flags, (int)adv.wifi_rssi, adv.ssid, (int)se);
    }

    // Periodically re-broadcast the cached weather snapshot so mesh-only
    // peers that came online (or channel-hopped in) after the last actual
    // fetch don't have to wait the full 10-minute refresh interval for
    // the next MSG_WEATHER. Gated on wifi_connected so only the real
    // gateway broadcasts — mesh-only peers that copied a snapshot from
    // the gateway must not turn around and blast it back out, or two
    // devices with the same cached data double the airtime for nothing.
    // 60s cadence pairs nicely with the MSG_TIME broadcaster below.
    static unsigned long s_last_wx_bcast = 0;
    if (g_weather.valid && g_net.wifi_connected &&
        (now - s_last_wx_bcast >= 60000UL)) {
        s_last_wx_bcast = now;
        espnow_broadcast_weather();
    }

    // If we have a real NTP-synced clock, periodically broadcast the
    // current UTC epoch so offline peers on the same ESP-NOW channel
    // can set their own clocks. Every 30s is plenty — drift between
    // broadcasts is well under a second.
    static unsigned long s_last_time_bcast = 0;
    if (clock_has_ntp() && (now - s_last_time_bcast >= 30000)) {
        s_last_time_bcast = now;
        time_t epoch = time(NULL);
        if (epoch > 1700000000L) {
            EspNowMsg tmsg = {};
            tmsg.type = MSG_TIME;
            strncpy(tmsg.name, user_display_name(), sizeof(tmsg.name) - 1);
            snprintf(tmsg.payload, sizeof(tmsg.payload), "%lu",
                     (unsigned long)epoch);
            tmsg.pos_x = (int16_t)clock_get_tz();
            tmsg.pos_y = 0;
            espnow_send_resilient(s_broadcast_mac, (uint8_t*)&tmsg, sizeof(tmsg));
        }
    }

    // Broadcast heartbeat with position. Pre-login we still want to
    // advertise presence so the WiFi picker on a nearby device can
    // list us as a mesh peer — but we MUST leave msg.name empty so the
    // login screen's dweller-name strcmp doesn't false-match and mark
    // an unclaimed dweller as "IN USE". The Radio tab falls back to
    // "(peer)" when name is empty.
    if (now - s_last_heartbeat >= HEARTBEAT_MS) {
        s_last_heartbeat = now;
        EspNowMsg msg = {};
        msg.type = MSG_HEARTBEAT;
        if (g_logged_in) {
            const Dweller& d = team[g_dweller_idx];
            strncpy(msg.name, user_display_name(), sizeof(msg.name) - 1);
            strncpy(msg.role, d.title, sizeof(msg.role) - 1);
            snprintf(msg.payload, sizeof(msg.payload), "ONLINE");
            msg.pos_x = g_my_pos_x;
            msg.pos_y = g_my_pos_y;
            if (g_is_anchor) msg.flags |= EN_FLAG_ANCHOR;
        } else {
            // Anonymous discovery beacon. Name stays "" so the login
            // screen can't confuse it with any dweller.
            msg.name[0] = '\0';
            strncpy(msg.role, "BOOT", sizeof(msg.role) - 1);
            snprintf(msg.payload, sizeof(msg.payload), "DISCOVER");
            msg.pos_x = -1;
            msg.pos_y = -1;
        }
        esp_err_t se = espnow_send_resilient(s_broadcast_mac, (uint8_t*)&msg, sizeof(msg));
        static uint8_t dbg_cnt = 0;
        if (dbg_cnt++ < 5 || (dbg_cnt % 10) == 0) {
            Serial.printf("[espnow] HB TX '%s' ch=%u rc=%d peers=%d\n",
                          g_logged_in ? msg.name : "(discover)",
                          WiFi.channel(), (int)se, g_peer_count);
        }
    }

    // Sweep peers — mark offline. Peer-leave beep is suppressed for office
    // ambience; the UI indicator / Radio list still updates silently.
    for (int i = 0; i < g_peer_count; i++) {
        if (g_peers[i].online && now - g_peers[i].last_seen_ms > PEER_TIMEOUT_MS) {
            g_peers[i].online = false;
            g_peers[i].sig_level = 0;
            Serial.printf("[espnow] Peer offline: %s\n", g_peers[i].name);
            // (peer-leave beep suppressed — office ambience)
        }
    }
}

void espnow_send_chat(const char* text) {
    const char* me = user_display_name();
    // Always push to local ring (dedup safe)
    chat_push_if_new(me, text, false);

    if (g_net.espnow_ready) {
        EspNowMsg msg = {};
        msg.type = MSG_CHAT;
        strncpy(msg.name, me, sizeof(msg.name) - 1);
        strncpy(msg.payload, text, sizeof(msg.payload) - 1);
        msg.pos_x = g_my_pos_x;
        msg.pos_y = g_my_pos_y;
        espnow_send_resilient(s_broadcast_mac, (uint8_t*)&msg, sizeof(msg));
    }
    // Also push up to the cloud so WS-only peers see it
    if (g_net.ws_connected) {
        ws_send_chat(text);
    }
}

// Broadcast a chat message on behalf of another peer (used by the WS->ESP-NOW
// bridge when the cloud delivers a chat that originated from a WS-only peer).
// No push to local ring — caller should already have done that with dedup.
void espnow_relay_chat_foreign(const char* from, const char* text) {
    if (!g_net.espnow_ready) return;
    EspNowMsg msg = {};
    msg.type = MSG_CHAT;
    strncpy(msg.name, from, sizeof(msg.name) - 1);
    strncpy(msg.payload, text, sizeof(msg.payload) - 1);
    msg.pos_x = 0;
    msg.pos_y = 0;
    espnow_send_resilient(s_broadcast_mac, (uint8_t*)&msg, sizeof(msg));
}

// Broadcast the current g_weather snapshot over ESP-NOW so mesh-only peers
// can update their Clock / Weather tiles without needing WiFi. Called by
// weather.cpp right after fetch_forecast() succeeds on a gateway device.
void espnow_broadcast_weather(void) {
    if (!g_net.espnow_ready) return;
    if (!g_weather.valid) return;

    EspNowWeatherMsg wx = {};
    wx.type = MSG_WEATHER;
    time_t e = time(NULL);
    wx.fetched_epoch = (e > 1700000000L) ? (uint32_t)e : 0;
    strncpy(wx.city, g_weather.city, sizeof(wx.city) - 1);
    wx.current_f    = g_weather.current_f;
    wx.current_code = g_weather.current_code;

    wx.d0_hi   = g_weather.daily[0].hi_f;
    wx.d0_lo   = g_weather.daily[0].lo_f;
    wx.d0_code = g_weather.daily[0].code;
    wx.d0_sr_h = g_weather.daily[0].sr_h;
    wx.d0_sr_m = g_weather.daily[0].sr_m;
    wx.d0_ss_h = g_weather.daily[0].ss_h;
    wx.d0_ss_m = g_weather.daily[0].ss_m;

    wx.d1_hi   = g_weather.daily[1].hi_f;
    wx.d1_lo   = g_weather.daily[1].lo_f;
    wx.d1_code = g_weather.daily[1].code;

    wx.d2_hi   = g_weather.daily[2].hi_f;
    wx.d2_lo   = g_weather.daily[2].lo_f;
    wx.d2_code = g_weather.daily[2].code;

    wx.has_alert = g_weather.has_alert ? 1 : 0;
    strncpy(wx.alert, g_weather.alert, sizeof(wx.alert) - 1);

    esp_err_t rc = espnow_send_resilient(s_broadcast_mac, (uint8_t*)&wx, sizeof(wx));
    Serial.printf("[espnow] WX TX '%s' %dF code=%u len=%u rc=%d\n",
                  wx.city, (int)wx.current_f, (unsigned)wx.current_code,
                  (unsigned)sizeof(wx), (int)rc);
}

// Settings > Refresh Now fires this. If we happen to be an ACTIVE
// gateway, our own handler will pick this up via the recv path and
// force a local refresh; any other peer that happens to be ACTIVE
// does the same. Either way the user gets a re-fetch within a few
// seconds and the mesh gets the updated broadcast for free.
void espnow_broadcast_refresh_req(void) {
    if (!g_net.espnow_ready) return;
    EspNowMsg msg = {};
    msg.type = MSG_REFRESH_REQ;
    if (g_logged_in) {
        strncpy(msg.name, user_display_name(), sizeof(msg.name) - 1);
    }
    snprintf(msg.payload, sizeof(msg.payload), "REFRESH");
    msg.pos_x = 0;
    msg.pos_y = 0;
    esp_err_t rc = espnow_send_resilient(s_broadcast_mac, (uint8_t*)&msg, sizeof(msg));
    Serial.printf("[espnow] REFRESH_REQ TX rc=%d\n", (int)rc);
}

// Public wrappers for subsystems that own their own wire format (e.g.
// game_proto). espnow_send_raw routes through espnow_send_resilient so
// the NOT_INIT self-heal path runs for any sender.
esp_err_t espnow_send_raw(const uint8_t* mac, const uint8_t* data, int len) {
    if (!g_net.espnow_ready) return ESP_ERR_ESPNOW_NOT_INIT;
    return espnow_send_resilient(mac, data, (size_t)len);
}

void espnow_my_mac(uint8_t out[6]) {
    esp_read_mac(out, ESP_MAC_WIFI_STA);
}

// Before a device calls WiFi.begin() (which will yank the radio to the
// AP's channel), tell the mesh what channel to move to. This lets peers
// proactively follow instead of waiting for their hop cycle to catch a
// GW_ADV on the new channel. Best-effort: we send the hint 3x over ~250ms
// so a peer that's currently dwelling on this channel is likely to catch
// at least one frame. Returns the channel we detected, or 0 on failure.
uint8_t espnow_announce_wifi_channel(const char* ssid) {
    if (!ssid || !*ssid) return 0;
    // scanNetworks blocks ~2-5s and briefly disrupts ESP-NOW RX, so only
    // call this from the pre-connect path where that's acceptable.
    Serial.printf("[espnow] Scanning for '%s' to announce channel...\n", ssid);
    int n = WiFi.scanNetworks(false, false, false, 120);
    uint8_t found_ch = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(WiFi.SSID(i).c_str(), ssid) == 0) {
            found_ch = (uint8_t)WiFi.channel(i);
            break;
        }
    }
    WiFi.scanDelete();
    if (found_ch == 0) {
        Serial.printf("[espnow] Scan: SSID '%s' not found — skipping hint\n", ssid);
        return 0;
    }
    Serial.printf("[espnow] Scan: '%s' on ch=%u — broadcasting hint\n",
                  ssid, (unsigned)found_ch);
    EspNowChannelHint h = {};
    h.type      = MSG_CHANNEL_HINT;
    h.target_ch = found_ch;
    for (int i = 0; i < 3; i++) {
        espnow_send_resilient(s_broadcast_mac, (uint8_t*)&h, sizeof(h));
        delay(80);
    }
    return found_ch;
}

// Ask the gateway to TCP-ping on our behalf. Used by the Ping screen
// when this device has no WiFi.
void espnow_request_ping(void) {
    if (!g_net.espnow_ready) return;
    EspNowMsg msg = {};
    msg.type = MSG_PING_REQ;
    if (g_logged_in) {
        strncpy(msg.name, user_display_name(), sizeof(msg.name) - 1);
    }
    espnow_send_resilient(s_broadcast_mac, (uint8_t*)&msg, sizeof(msg));
}

void espnow_send_dm(int peer_idx, const char* text) {
    if (!g_net.espnow_ready) return;
    if (peer_idx < 0 || peer_idx >= g_peer_count) return;
    if (!g_peers[peer_idx].online) return;

    esp_now_peer_info_t pi = {};
    memcpy(pi.peer_addr, g_peers[peer_idx].mac, 6);
    pi.channel = 0;
    pi.encrypt = false;
    if (!esp_now_is_peer_exist(g_peers[peer_idx].mac)) {
        esp_now_add_peer(&pi);
    }

    EspNowMsg msg = {};
    msg.type = MSG_DM;
    const char* me = user_display_name();
    strncpy(msg.name, me, sizeof(msg.name) - 1);
    strncpy(msg.payload, text, sizeof(msg.payload) - 1);
    msg.pos_x = g_my_pos_x;
    msg.pos_y = g_my_pos_y;
    espnow_send_resilient(g_peers[peer_idx].mac, (uint8_t*)&msg, sizeof(msg));
    chat_push_if_new(me, text, true);
    Serial.printf("[espnow] DM to %s: %s\n", g_peers[peer_idx].name, text);
}

void espnow_request_sync(void) {
    if (!g_net.espnow_ready) {
        Serial.println("[espnow] Sync: ESP-NOW not ready");
        return;
    }
    unsigned long now = millis();
    s_last_sync_req_ms = now;

    Serial.printf("[espnow] SYNC button pressed. Peers=%d, local chat=%d\n",
                  g_peer_count, g_chat_count);
    for (int i = 0; i < g_peer_count; i++) {
        Serial.printf("[espnow]   peer[%d] %s online=%d rssi=%d\n",
                      i, g_peers[i].name, g_peers[i].online, g_peers[i].rssi);
    }

    // Broadcast sync request — peers will send all their messages
    EspNowMsg msg = {};
    msg.type = MSG_SYNC_REQ;
    strncpy(msg.name, user_display_name(), sizeof(msg.name) - 1);
    msg.pos_x = 0;
    msg.pos_y = 0;
    espnow_send_resilient(s_broadcast_mac, (uint8_t*)&msg, sizeof(msg));
    Serial.printf("[espnow] Sync requested (we have %d messages)\n", g_chat_count);

    // Also send our own messages so it's bidirectional
    if (g_chat_count > 0 && !s_syncing) {
        s_syncing = true;
        s_sync_idx = 0;
        s_sync_next_ms = now + 200;  // slight delay so request goes first
        Serial.printf("[espnow] Also sending our %d messages\n", g_chat_count);
    }
}
