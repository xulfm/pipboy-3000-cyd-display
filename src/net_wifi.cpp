// ============================================================
// Vault 89 PDI-Boy -- WiFi Manager
// Connect to VAULT-89, reconnect on drop
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "net_config.h"
#include "net_state.h"

static unsigned long s_last_attempt = 0;
static bool          s_connecting   = false;
static bool          s_user_connected = false;  // true once user picks a network

void wifi_init(void) {
    // NOTE: WIFI_STA only. WIFI_AP_STA was tried for "ESP-NOW coexistence"
    // but actually makes the driver hand out a phantom softap channel that
    // breaks ESP-NOW RX whenever the STA isn't connected. Station-only is
    // fine for ESP-NOW as long as we pin the channel ourselves.
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    s_connecting = true;
    s_user_connected = true;
    s_last_attempt = millis();
    Serial.printf("[wifi] Connecting to %s ...\n", WIFI_SSID);
}

void wifi_poll(void) {
    wl_status_t st = WiFi.status();

    if (st == WL_CONNECTED) {
        if (!g_net.wifi_connected) {
            g_net.wifi_connected = true;
            g_net.wifi_channel   = WiFi.channel();
            g_net.wifi_rssi      = WiFi.RSSI();
            s_connecting = false;
            s_user_connected = true;
            // Enable power save — radio sleeps between AP beacons
            esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
            Serial.printf("[wifi] Connected! IP=%s ch=%d rssi=%d (PS on)\n",
                          WiFi.localIP().toString().c_str(),
                          g_net.wifi_channel, g_net.wifi_rssi);
        }
        // Periodically update RSSI
        static unsigned long last_rssi = 0;
        if (millis() - last_rssi > 5000) {
            g_net.wifi_rssi = WiFi.RSSI();
            last_rssi = millis();
        }
    } else {
        if (g_net.wifi_connected) {
            g_net.wifi_connected = false;
            Serial.println("[wifi] Disconnected");
        }
        // Only auto-reconnect if user previously connected to a network
        if (s_user_connected) {
            if (!s_connecting && millis() - s_last_attempt > WIFI_TIMEOUT_MS) {
                Serial.println("[wifi] Reconnecting...");
                WiFi.reconnect();
                s_connecting = true;
                s_last_attempt = millis();
            }
            if (s_connecting && millis() - s_last_attempt > WIFI_TIMEOUT_MS) {
                s_connecting = false;
                Serial.println("[wifi] Connect timeout, will retry");
            }
        }
    }
}

bool wifi_is_connected(void) {
    return g_net.wifi_connected;
}
