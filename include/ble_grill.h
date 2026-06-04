#pragma once
#include <stdint.h>

// ============================================================
// Vault 89 PDI-Boy -- BLE Grill Monitor
// Scan, connect, and read ThermoPro BLE thermometer probes
// BLE is only active while grill panel is open (saves RAM)
// ============================================================

#define GRILL_MAX_DEVICES  4
#define GRILL_MAX_PROBES   4

struct GrillDevice {
    char    name[24];
    char    addr[18];       // "AA:BB:CC:DD:EE:FF"
    int8_t  rssi;
    bool    connectable;
};

struct GrillProbe {
    int16_t temp_f;         // temperature in °F * 10 (e.g., 2010 = 201.0°F), -1 = disconnected
    bool    connected;      // probe physically plugged in
};

struct GrillState {
    bool        scanning;
    bool        ble_active;
    bool        connected;
    int         device_count;
    GrillDevice devices[GRILL_MAX_DEVICES];

    // Connected device data
    char        conn_name[24];
    GrillProbe  probes[GRILL_MAX_PROBES];
    int         probe_count;
    uint32_t    last_update_ms;
};

extern GrillState g_grill;
extern volatile bool grill_is_connecting(void);  // true while BG task runs

// API — call from main thread only
void grill_ble_start(void);       // init BLE + start scanning
void grill_ble_stop(void);        // disconnect + deinit BLE (free RAM)
void grill_scan(void);            // start/restart scan
void grill_connect(int idx);      // connect to device at index
void grill_disconnect(void);      // disconnect from current device
void grill_poll(void);            // call periodically to process events
