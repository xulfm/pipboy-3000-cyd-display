// ============================================================
// Vault 89 PDI-Boy -- BLE Grill Monitor
// ThermoPro BLE thermometer scanning and connection
// Uses NimBLE (lightweight BLE stack)
// Connection runs on background FreeRTOS task to avoid UI freeze
// ============================================================

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "ble_grill.h"

GrillState g_grill = {};

static NimBLEScan*   s_scan   = NULL;
static NimBLEClient* s_client = NULL;
static unsigned long s_scan_start_ms = 0;
#define SCAN_DURATION_MS 5500

// Background connect task state
static volatile bool s_connect_pending = false;
static volatile bool s_connecting      = false;
static int           s_connect_idx     = -1;
static TaskHandle_t  s_connect_task    = NULL;

// Known ThermoPro service/characteristic UUIDs
static NimBLEUUID CHR_NOTIFY_1("00001002-0000-1000-8000-00805f9b34fb");
static NimBLEUUID CHR_NOTIFY_2("0000fff1-0000-1000-8000-00805f9b34fb");
static NimBLEUUID CHR_NOTIFY_3("0000ffe1-0000-1000-8000-00805f9b34fb");
static NimBLEUUID CHR_NOTIFY_4("0000fff4-0000-1000-8000-00805f9b34fb");

// Track which characteristic sends real temp data
static NimBLERemoteCharacteristic* s_temp_chr = NULL;

// Helper: check if raw 0.1C value is plausible temperature (-40C to 400C)
static bool is_plausible_temp(int16_t raw) {
    return (raw >= -400 && raw <= 4000) && raw != 0;
}

// Helper: check if a value is a "disconnected probe" marker
static bool is_disconnect_marker(int16_t raw) {
    uint16_t u = (uint16_t)raw;
    return (raw == 0 || raw == -1 || u == 0xFFFF || u == 0x7FFF ||
            u == 0x7FFE || u == 0x8000 || u == 0x8001 ||
            raw > 6000 || raw < -500);
}

// ── Parse temperature notification data ──
static void parse_temp_data(const uint8_t* data, size_t len, bool from_temp_chr) {
    Serial.printf("[grill] Notify (%d bytes, %s):", len,
                  from_temp_chr ? "TEMP" : "other");
    for (size_t i = 0; i < len && i < 24; i++) Serial.printf(" %02X", data[i]);
    Serial.println();

    // Need at least 2 bytes per probe; reject tiny or huge packets
    if (len < 2 || len > 40) return;

    // Try to detect the data format by scanning for plausible temperatures
    // ThermoPro formats seen in the wild:
    //   Format A: [temp1_lo, temp1_hi, temp2_lo, temp2_hi, ...] (raw pairs)
    //   Format B: [header, temp1_lo, temp1_hi, temp2_lo, temp2_hi, ...]
    //   Format C: [header1, header2, temp1_lo, temp1_hi, ...]
    // Temps are in 0.1°C (e.g., 180 = 18.0°C)

    // Try offset 0, 1, 2 — pick whichever yields the most plausible probes
    int best_offset = -1;
    int best_plausible = 0;

    for (int try_off = 0; try_off <= 2 && try_off + 1 < (int)len; try_off++) {
        int plausible = 0;
        int checked = 0;
        for (int p = 0; p < GRILL_MAX_PROBES && (try_off + p * 2 + 1) < (int)len; p++) {
            int16_t raw = (int16_t)(data[try_off + p * 2] | (data[try_off + p * 2 + 1] << 8));
            checked++;
            if (is_plausible_temp(raw)) plausible++;
            else if (is_disconnect_marker(raw)) plausible++;  // valid "no probe"
        }
        if (checked > 0 && plausible >= checked && plausible > best_plausible) {
            best_plausible = plausible;
            best_offset = try_off;
        }
    }

    // If no offset looks good, this isn't a temperature packet — skip it
    if (best_offset < 0) {
        Serial.println("[grill]   -> Not a temp packet, skipping");
        return;
    }

    // If this is the first packet that looks like temps, lock onto this characteristic
    if (!from_temp_chr && !s_temp_chr) {
        Serial.printf("[grill]   -> Auto-detected temp data at offset %d\n", best_offset);
    }

    g_grill.last_update_ms = millis();

    int probes_found = 0;
    for (int i = 0; i < GRILL_MAX_PROBES && (best_offset + i * 2 + 1) < (int)len; i++) {
        int16_t raw = (int16_t)(data[best_offset + i * 2] | (data[best_offset + i * 2 + 1] << 8));

        if (is_disconnect_marker(raw)) {
            g_grill.probes[i].connected = false;
            g_grill.probes[i].temp_f = -1;
        } else if (is_plausible_temp(raw)) {
            g_grill.probes[i].connected = true;
            // Convert 0.1°C to 0.1°F: F = C * 9/5 + 32
            g_grill.probes[i].temp_f = (int16_t)(raw * 9 / 5 + 320);
            probes_found++;
        } else {
            g_grill.probes[i].connected = false;
            g_grill.probes[i].temp_f = -1;
        }
    }

    if (probes_found > g_grill.probe_count) g_grill.probe_count = probes_found;

    for (int i = 0; i < GRILL_MAX_PROBES; i++) {
        if (g_grill.probes[i].connected) {
            Serial.printf("[grill]   P%d: %d.%d F (raw=%d)\n", i + 1,
                          g_grill.probes[i].temp_f / 10,
                          abs(g_grill.probes[i].temp_f % 10),
                          (int)(data[best_offset + i * 2] | (data[best_offset + i * 2 + 1] << 8)));
        } else {
            Serial.printf("[grill]   P%d: disconnected\n", i + 1);
        }
    }
}

// ── Notification callback ──
static void notify_cb(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify) {
    bool is_temp = (s_temp_chr != NULL && chr == s_temp_chr);
    parse_temp_data(data, len, is_temp);
    // If we haven't locked a temp characteristic yet, and this packet was valid, lock it
    if (!s_temp_chr && g_grill.last_update_ms == millis()) {
        s_temp_chr = chr;
        Serial.printf("[grill] Locked temp characteristic: %s\n",
                      chr->getUUID().toString().c_str());
    }
}

// ── Scan callback ──
class GrillScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        std::string name = dev->getName();
        if (name.length() == 0) return;
        if (g_grill.device_count >= GRILL_MAX_DEVICES) return;

        // Dedup
        std::string addr = dev->getAddress().toString();
        for (int i = 0; i < g_grill.device_count; i++) {
            if (strcmp(g_grill.devices[i].addr, addr.c_str()) == 0) return;
        }

        int idx = g_grill.device_count++;
        strncpy(g_grill.devices[idx].name, name.c_str(), sizeof(g_grill.devices[idx].name) - 1);
        g_grill.devices[idx].name[sizeof(g_grill.devices[idx].name) - 1] = '\0';
        strncpy(g_grill.devices[idx].addr, addr.c_str(), sizeof(g_grill.devices[idx].addr) - 1);
        g_grill.devices[idx].addr[sizeof(g_grill.devices[idx].addr) - 1] = '\0';
        g_grill.devices[idx].rssi = dev->getRSSI();
        g_grill.devices[idx].connectable = true;

        Serial.printf("[grill] Found: %s (%s) RSSI=%d\n",
                      g_grill.devices[idx].name,
                      g_grill.devices[idx].addr,
                      g_grill.devices[idx].rssi);
    }
};

static GrillScanCallbacks s_scan_cbs;

// ── Background connect task (runs on core 0 to avoid blocking UI) ──
static void connect_task(void* param) {
    int idx = (int)(intptr_t)param;
    s_connecting = true;

    Serial.printf("[grill] BG: Connecting to %s (%s)...\n",
                  g_grill.devices[idx].name, g_grill.devices[idx].addr);

    if (!s_client) {
        s_client = NimBLEDevice::createClient();
    }
    s_client->setConnectTimeout(5);  // 5 second timeout

    NimBLEAddress addr(g_grill.devices[idx].addr);

    // Try both public and random address types
    bool ok = s_client->connect(addr, true);  // true = use public address
    if (!ok) {
        Serial.println("[grill] BG: Public addr failed, trying random...");
        ok = s_client->connect(addr, false);  // false = random address
    }

    if (!ok) {
        Serial.println("[grill] BG: Connect FAILED");
        s_connecting = false;
        s_connect_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    Serial.println("[grill] BG: Connected! Discovering services...");
    g_grill.connected = true;
    strncpy(g_grill.conn_name, g_grill.devices[idx].name, sizeof(g_grill.conn_name) - 1);

    // Reset probes
    for (int i = 0; i < GRILL_MAX_PROBES; i++) {
        g_grill.probes[i].temp_f = -1;
        g_grill.probes[i].connected = false;
    }
    g_grill.probe_count = 0;

    // Discover all services
    std::vector<NimBLERemoteService*>* svcs = s_client->getServices(true);
    if (svcs) {
        Serial.printf("[grill] BG: Found %d services\n", svcs->size());
        for (auto svc : *svcs) {
            Serial.printf("[grill]   Svc: %s\n", svc->getUUID().toString().c_str());

            // Enumerate all characteristics
            std::vector<NimBLERemoteCharacteristic*>* chrs = svc->getCharacteristics(true);
            if (chrs) {
                for (auto chr : *chrs) {
                    Serial.printf("[grill]     Chr: %s [%s%s%s%s]\n",
                                  chr->getUUID().toString().c_str(),
                                  chr->canRead() ? "R" : "",
                                  chr->canWrite() ? "W" : "",
                                  chr->canNotify() ? "N" : "",
                                  chr->canIndicate() ? "I" : "");

                    // Subscribe to any notify characteristic
                    if (chr->canNotify()) {
                        if (chr->subscribe(true, notify_cb)) {
                            Serial.println("[grill]     -> Subscribed OK");
                        } else {
                            Serial.println("[grill]     -> Subscribe failed");
                        }
                    }

                    // Try reading (log only, don't parse — wait for notifications)
                    if (chr->canRead()) {
                        std::string val = chr->readValue();
                        if (val.length() > 0) {
                            Serial.printf("[grill]     Read (%d):", val.length());
                            for (size_t i = 0; i < val.length() && i < 20; i++) {
                                Serial.printf(" %02X", (uint8_t)val[i]);
                            }
                            Serial.println();
                        }
                    }
                }
            }
        }
    }

    Serial.println("[grill] BG: Discovery complete");
    s_connecting = false;
    s_connect_task = NULL;
    vTaskDelete(NULL);
}

// ── Public API ──

volatile bool grill_is_connecting(void) { return s_connecting; }

void grill_ble_start(void) {
    // Initialize NimBLE once, reuse on subsequent opens
    if (!NimBLEDevice::getInitialized()) {
        Serial.println("[grill] Initializing NimBLE...");
        NimBLEDevice::init("PipBoy");
        NimBLEDevice::setPower(ESP_PWR_LVL_P9);
        Serial.println("[grill] NimBLE ready");
    }

    g_grill.ble_active = true;
    g_grill.device_count = 0;
    g_grill.connected = false;
    g_grill.scanning = false;
    g_grill.last_update_ms = 0;

    for (int i = 0; i < GRILL_MAX_PROBES; i++) {
        g_grill.probes[i].temp_f = -1;
        g_grill.probes[i].connected = false;
    }

    grill_scan();
}

void grill_ble_stop(void) {
    // NOTE: We do NOT deinit NimBLE here anymore, because it blocks the main
    // thread for several seconds (especially if a connect is in progress).
    // Instead: stop scan, disconnect, and leave NimBLE initialized.
    // BLE stays active until reboot. RAM cost is acceptable.
    if (!g_grill.ble_active) return;

    Serial.println("[grill] Stopping BLE activity (keeping stack alive)...");

    // Stop scan non-blocking
    if (s_scan) {
        s_scan->stop();
    }
    g_grill.scanning = false;
    g_grill.device_count = 0;

    // Disconnect if connected — this is fast
    if (s_client && s_client->isConnected()) {
        s_client->disconnect();
    }
    s_temp_chr = NULL;
    g_grill.connected = false;
    g_grill.conn_name[0] = '\0';
    for (int i = 0; i < GRILL_MAX_PROBES; i++) {
        g_grill.probes[i].temp_f = -1;
        g_grill.probes[i].connected = false;
    }

    Serial.println("[grill] BLE stopped (stack still alive)");
}

void grill_scan(void) {
    if (!g_grill.ble_active) return;
    if (g_grill.connected || s_connecting) return;

    g_grill.device_count = 0;
    g_grill.scanning = true;

    s_scan = NimBLEDevice::getScan();
    s_scan->setAdvertisedDeviceCallbacks(&s_scan_cbs);
    s_scan->setActiveScan(true);
    s_scan->setInterval(100);
    s_scan->setWindow(99);
    s_scan->start(5, false);
    s_scan_start_ms = millis();

    Serial.println("[grill] Scanning 5s...");
}

void grill_connect(int idx) {
    if (s_connecting || g_grill.connected) return;
    if (idx < 0 || idx >= g_grill.device_count) return;

    // Stop scan before connecting
    if (s_scan) s_scan->stop();
    g_grill.scanning = false;

    // Launch connection on background task (4KB stack on core 0)
    s_connect_pending = false;
    xTaskCreatePinnedToCore(connect_task, "ble_conn", 4096,
                            (void*)(intptr_t)idx, 1, &s_connect_task, 0);
}

void grill_disconnect(void) {
    if (s_client && s_client->isConnected()) {
        s_client->disconnect();
        Serial.println("[grill] Disconnected");
    }
    s_temp_chr = NULL;
    g_grill.connected = false;
    g_grill.conn_name[0] = '\0';
    for (int i = 0; i < GRILL_MAX_PROBES; i++) {
        g_grill.probes[i].temp_f = -1;
        g_grill.probes[i].connected = false;
    }
}

void grill_poll(void) {
    if (!g_grill.ble_active) return;

    // Check scan completion
    if (g_grill.scanning && millis() - s_scan_start_ms > SCAN_DURATION_MS) {
        g_grill.scanning = false;
        if (s_scan) s_scan->stop();
        Serial.printf("[grill] Scan done, %d devices\n", g_grill.device_count);
    }

    // Check connection health
    if (g_grill.connected && s_client && !s_client->isConnected()) {
        Serial.println("[grill] Connection lost");
        g_grill.connected = false;
    }
}
