#pragma once
#include <stdint.h>
#include "net_config.h"

// ============================================================
// Vault 89 PDI-Boy -- Shared Network State
// Screens read from these — networking code writes to them.
// ============================================================

// Link mode — how this device is actually reaching the rest of the
// mesh / cloud. Drives the status indicator and the login "IN USE"
// visibility logic.
enum LinkMode : uint8_t {
    LINK_OFFLINE = 0,   // no wifi, no espnow peers heard
    LINK_MESH    = 1,   // offline from wifi, but locked onto mesh channel
    LINK_WIFI    = 2,   // direct wifi (and implicitly the cloud bridge)
};

struct NetState {
    bool    wifi_connected;
    int8_t  wifi_rssi;
    uint8_t wifi_channel;
    bool    ws_connected;
    bool    espnow_ready;
    uint8_t link_mode;     // one of LinkMode values, updated in net_poll()
};

// Gateway advert flag bits (see EspNowGatewayAdv in net_espnow.cpp)
#define GW_FLAG_HAS_CREDS    0x01   // peer has saved WiFi credentials
#define GW_FLAG_WIFI_CONN    0x02   // peer is currently connected to its AP
#define GW_FLAG_HAS_NTP      0x04   // peer's clock is NTP-synced
#define GW_FLAG_HAS_WEATHER  0x08   // peer has a valid g_weather snapshot

struct EspNowPeer {
    uint8_t  mac[6];
    char     name[20];
    char     role[20];
    char     last_msg[32];
    int8_t   rssi;
    uint8_t  sig_level;   // 0-4, computed from rssi
    int16_t  rssi_avg;    // smoothed RSSI (EMA) for radar distance; 0 = unset
    bool     is_anchor;   // peer is a fixed-position anchor (known x,y)
    bool     online;
    uint32_t last_seen_ms;
    int16_t  pos_x;       // map position (-1 = unset)
    int16_t  pos_y;

    // Gateway metadata — populated from MSG_GATEWAY_ADV frames. Used to
    // elect which device actually keeps a WiFi uplink in a fleet of
    // same-hotspot PDI-Boys. All peers listen; the one with the best AP
    // signal (or the one that's already connected) wins.
    uint8_t  gw_flags;        // GW_FLAG_* bitmask
    int8_t   gw_wifi_rssi;    // their RSSI to their AP (0 if not connected)
    uint32_t gw_last_adv_ms;  // millis() of last ADV rx (0 = never)
};

// Chat ring buffer
#define CHAT_MAX 8
#define CHAT_MSG_LEN 64

struct ChatMsg {
    char from[20];
    char text[CHAT_MSG_LEN];
    uint32_t timestamp;
    bool     is_dm;        // true = direct message, false = group
};

// Map coordinate system — the vault entrance is the center reference
// (radar orients around "home"). Map-pixel positions (0-316 x, 0-200 y).
#define MAP_STAGE_X   160
#define MAP_STAGE_Y   75

// Global state — defined in net_manager.cpp
extern NetState    g_net;
extern EspNowPeer  g_peers[ESPNOW_MAX_PEERS];
extern int         g_peer_count;
extern ChatMsg     g_chat[CHAT_MAX];
extern int         g_chat_head;
extern int         g_chat_count;
extern int16_t     g_my_pos_x;   // own map position (-1 = unset)
extern int16_t     g_my_pos_y;

// Anchor mode — this device acts as a fixed-position beacon at a known
// landmark, enabling RSSI multilateration of roaming peers. Set via the
// Settings screen, persisted to NVS.
extern bool        g_is_anchor;        // true = broadcast as a fixed anchor
extern int         g_anchor_landmark;  // index into LANDMARKS[] (-1 = none)
void anchor_load(void);                // restore anchor mode from NVS (boot)
void anchor_set(bool on, int landmark_idx);  // toggle + persist + set pos

// Mesh positioning (RSSI multilateration). localize_poll() runs from
// net_poll(); the rest are read by the map/radar UI to choose its mode.
void  localize_poll(void);
int   mesh_active_count(void);   // online devices incl. self
int   mesh_anchor_count(void);   // online anchors (incl. self if anchored)
float mesh_rssi_to_dist(int rssi); // rough meters from a (smoothed) RSSI

// Network API — called from main.cpp
void net_init(void);
void net_init_deferred(void);  // init WS subsystem only, WiFi handled by picker
void net_poll(void);

// WebSocket send helpers
void ws_send_login(int dweller_idx);
void ws_send_chat(const char* msg);
void ws_send_chat_as(const char* from, const char* text);  // relay on behalf of peer
void ws_send_relay(int relay_id, bool state);

// ESP-NOW chat helpers
void espnow_send_chat(const char* text);
void espnow_send_dm(int peer_idx, const char* text);
void espnow_relay_chat_foreign(const char* from, const char* text);  // broadcast someone else's msg
void espnow_request_sync(void);  // request chat history from peers

// Mesh ping: request the gateway to TCP-ping 1.1.1.1:80 on our behalf,
// and register a callback to receive the result.
void espnow_request_ping(void);
void espnow_set_mesh_ping_cb(void (*cb)(int ms));

// Settings > Refresh Now: broadcast a MSG_REFRESH_REQ over the mesh so
// whichever device currently owns the WiFi uplink will drop its weather
// and clock fresh latches. The originator also calls the local
// weather_force_refresh() / clock_force_ntp_sync() helpers directly so
// it works whether we're the gateway or a deferred peer.
void espnow_broadcast_refresh_req(void);

// WebSocket sync window control (implemented in net_websocket.cpp).
// Opens a bounded window during which ws_poll() is allowed to run and
// auto-reconnect. Outside the window ws.disconnect() is held so the
// library doesn't thrash reconnects against a down server. Called by
// net_force_refresh(), first WiFi-connect, and the periodic weather
// cascade so sync traffic rides the same cycle as weather/NTP.
void ws_open_sync_window(uint32_t window_ms);
bool ws_sync_window_open(void);

// Convenience: drop weather+clock+WS latches so the next net_poll cycle
// pulls everything fresh. Implemented in net_manager.cpp.
void net_force_refresh(void);

// Shared chat ring utilities (defined in net_manager.cpp)
bool chat_push_if_new(const char* from, const char* text, bool is_dm);

// Raw ESP-NOW send for subsystems (e.g. game_proto) that own their own
// wire format. Pass the broadcast MAC (FF:FF:FF:FF:FF:FF) or a unicast
// peer MAC; adds/reuses the peer entry transparently.
esp_err_t espnow_send_raw(const uint8_t* mac, const uint8_t* data, int len);

// Copy this device's STA MAC into the 6-byte buffer. Used by game_proto
// to stamp the host_mac field in GameHdr without reaching for the WiFi
// library from non-main modules.
void espnow_my_mac(uint8_t out[6]);

// Pre-WiFi.begin() helper: scan for `ssid`, broadcast a channel hint so
// mesh peers can follow us to the AP's channel before the radio flips.
// Returns the detected channel (1..13) or 0 if the SSID wasn't found.
// Blocks ~2-5s on the scan; only safe to call from setup / election.
uint8_t espnow_announce_wifi_channel(const char* ssid);

// True once the user has picked a dweller on the login screen. Gates
// ESP-NOW heartbeat broadcasts so the login screens of nearby devices
// don't show "team[0]" as IN USE before anyone has actually signed in.
extern bool g_logged_in;

// Human-readable link mode for status indicators ("WIFI"/"MESH"/"OFFLINE").
const char* link_mode_str(void);
