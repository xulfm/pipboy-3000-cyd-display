#pragma once
// ============================================================
// Vault 89 PDI-Boy -- Network Configuration
// ============================================================

// WiFi
#define WIFI_SSID       "VAULT-89"
#define WIFI_PASS       ""           // open network, or set password
#define WIFI_TIMEOUT_MS 15000

// WebSocket — Board A
#define WS_HOST         "192.168.89.10"
#define WS_PORT         8080
#define WS_PATH         "/ws"
#define WS_RECONNECT_MS 5000

// ESP-NOW
#define ESPNOW_MAX_PEERS  6
#define HEARTBEAT_MS      3000       // broadcast heartbeat interval
#define PEER_TIMEOUT_MS   10000      // mark offline after silence

// Icecast — Board B (future audio stream)
#define ICECAST_HOST    "192.168.89.11"
#define ICECAST_PORT    8000
