// ============================================================
// Vault 89 PDI-Boy -- WebSocket Client (Board A)
// Sessions, chat relay, data sync
// ============================================================

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include "net_config.h"
#include "net_state.h"
#include "pipboy_data.h"
#include "sound_fx.h"

static WebSocketsClient ws;
static bool             s_ws_started     = false;
static bool             s_was_connected  = false;  // for state-transition sfx
static unsigned long    s_last_try       = 0;

// Sync-window gating. The WebSocketsClient library auto-reconnects on
// every ws.loop() call, which means when the cloud server is down the
// device spams TCP SYNs and WStype_DISCONNECTED events every few seconds
// forever. That's wasted airtime and CPU on an office device that's
// mostly using the ESP-NOW mesh for realtime stuff anyway. Gate
// ws.loop() inside a periodic ~60s window aligned with the weather /
// clock sync cycle: any cloud data (announcements, presence) either
// lands during the window or rides the next one 10 minutes later.
#define WS_WINDOW_DEFAULT_MS   (60UL * 1000UL)
#define WS_WINDOW_INTERVAL_MS  (10UL * 60UL * 1000UL)
static unsigned long s_window_until_ms    = 0;
static unsigned long s_last_window_open_ms = 0;

void ws_open_sync_window(uint32_t window_ms) {
    if (window_ms == 0) window_ms = WS_WINDOW_DEFAULT_MS;
    unsigned long now = millis();
    s_window_until_ms    = now + window_ms;
    s_last_window_open_ms = now;
    Serial.printf("[ws] Sync window open (%lums)\n", (unsigned long)window_ms);
}

bool ws_sync_window_open(void) {
    return (long)(millis() - s_window_until_ms) < 0;
}

// Forward declarations for internal handlers
static void handle_server_msg(const char* payload, size_t len);

static void ws_event(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            g_net.ws_connected = true;
            Serial.printf("[ws] Connected to %s:%d\n", WS_HOST, WS_PORT);
            if (!s_was_connected) {
                sfx_play(SFX_CONNECT);
                s_was_connected = true;
            }
            break;
        case WStype_DISCONNECTED:
            // The WebSocketsClient library fires this on EVERY failed reconnect
            // attempt (every ~5-10s when the server is unreachable). Only play
            // the disconnect sound on the true connected→disconnected edge so
            // the device doesn't beep periodically when the server is down.
            if (s_was_connected) {
                Serial.println("[ws] Disconnected");
                sfx_play(SFX_DISCONNECT);
                s_was_connected = false;
            }
            g_net.ws_connected = false;
            break;
        case WStype_TEXT:
            handle_server_msg((const char*)payload, length);
            break;
        default:
            break;
    }
}

// Send a JSON document over WebSocket
static void ws_send(JsonDocument& doc) {
    if (!g_net.ws_connected) return;
    char buf[256];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    ws.sendTXT(buf, n);
}

void ws_init(void) {
    // Don't connect yet — wait for WiFi
}

void ws_poll(void) {
    if (!g_net.wifi_connected) {
        // No uplink — nothing to do. If we were previously connected the
        // library will fire the DISCONNECTED event next time we actually
        // call loop() inside a sync window, which handles the beep + flag.
        return;
    }

    unsigned long now = millis();

    // Auto-open a fresh window every WS_WINDOW_INTERVAL_MS so cloud
    // traffic still rides the same 10-min cadence as the weather pull.
    // First-ever call also opens a window so the device can do an
    // initial login handshake as soon as WiFi comes up.
    if (s_last_window_open_ms == 0 ||
        (now - s_last_window_open_ms) >= WS_WINDOW_INTERVAL_MS) {
        ws_open_sync_window(WS_WINDOW_DEFAULT_MS);
    }

    if (!ws_sync_window_open()) {
        // Outside window: if we were connected, hang up so the library
        // stops retrying. The server will see a clean disconnect and we
        // won't burn TCP syns against it every 5 seconds.
        if (s_ws_started && g_net.ws_connected) {
            ws.disconnect();
            g_net.ws_connected = false;
            s_was_connected = false;
            Serial.println("[ws] Sync window closed -> disconnecting");
        }
        return;
    }

    if (!s_ws_started) {
        ws.begin(WS_HOST, WS_PORT, WS_PATH);
        ws.onEvent(ws_event);
        ws.setReconnectInterval(WS_RECONNECT_MS);
        s_ws_started = true;
        Serial.printf("[ws] Connecting to ws://%s:%d%s\n", WS_HOST, WS_PORT, WS_PATH);
    }

    ws.loop();
}

// ── Send helpers ──

void ws_send_login(int dweller_idx) {
    if (!g_net.ws_connected) return;
    JsonDocument doc;
    doc["type"]        = "login";
    doc["dweller_idx"] = dweller_idx;
    doc["name"]        = user_display_name();

    // Include MAC address for identification
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    doc["mac"] = mac_str;

    ws_send(doc);
    Serial.printf("[ws] Login sent: %s (idx=%d)\n", user_display_name(), dweller_idx);
}

void ws_send_chat(const char* msg) {
    ws_send_chat_as(user_display_name(), msg);
}

// Relay a chat message on behalf of another peer (used when we received
// the message via ESP-NOW and want to forward it to the cloud).
void ws_send_chat_as(const char* from, const char* text) {
    if (!g_net.ws_connected) return;
    JsonDocument doc;
    doc["type"]      = "chat";
    doc["from_init"] = from;
    doc["to_init"]   = "";
    doc["text"]      = text;
    ws_send(doc);
}

void ws_send_relay(int relay_id, bool state) {
    if (!g_net.ws_connected) return;
    JsonDocument doc;
    doc["type"]     = "relay";
    doc["relay_id"] = relay_id;
    doc["state"]    = state;
    ws_send(doc);
}

// ── Handle incoming server messages ──

static void handle_server_msg(const char* payload, size_t len) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, len);
    if (err) {
        Serial.printf("[ws] JSON parse error: %s\n", err.c_str());
        return;
    }

    const char* type = doc["type"];
    if (!type) return;

    if (strcmp(type, "login_ack") == 0) {
        Serial.printf("[ws] Login acknowledged: %s\n",
                      doc["status"].as<const char*>());
    }
    else if (strcmp(type, "welcome") == 0) {
        Serial.println("[ws] Welcome received");
    }
    else if (strcmp(type, "message") == 0 || strcmp(type, "chat") == 0) {
        // Server broadcasts {type:"message", from_init, to_init, text, ts}
        // Accept either field naming for forward-compatibility.
        const char* from = doc["from_init"] | doc["from"] | "???";
        const char* text = doc["text"]      | doc["msg"]  | "";
        const char* to   = doc["to_init"]   | "";
        bool is_dm = (to && to[0] != '\0');

        // Skip echoes of our own messages (server bounces chat to all clients)
        if (strcmp(from, user_display_name()) == 0) return;

        if (chat_push_if_new(from, text, is_dm)) {
            Serial.printf("[ws] Chat from %s: %s\n", from, text);
            sfx_play(SFX_MSG_IN);
            // Bridge to ESP-NOW mesh so local peers without WiFi can see it
            if (!is_dm) {
                espnow_relay_chat_foreign(from, text);
            }
        }
    }
    else if (strcmp(type, "announcement") == 0) {
        const char* text = doc["text"] | "";
        Serial.printf("[ws] Announcement: %s\n", text);
        sfx_play(SFX_VAULT_ALERT);
    }
    else if (strcmp(type, "sync") == 0) {
        Serial.println("[ws] Data sync received");
        // Future: update DwellerLive data from server
    }
}
