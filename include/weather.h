// ============================================================
// Vault 89 PDI-Boy -- Weather Module
// Fetches forecast from Open-Meteo (free, no key) and exposes
// a shared state struct that screens can read. Location is
// auto-detected on first boot via ip-api.com and persisted to
// NVS. Refreshes every 10 minutes in a background task.
// ============================================================
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define WEATHER_HOURLY_SLOTS 12
#define WEATHER_DAILY_SLOTS  3
#define WEATHER_ALERT_LEN    48
#define WEATHER_CITY_LEN     28

struct WeatherDay {
    int16_t  hi_f;
    int16_t  lo_f;
    uint8_t  code;      // WMO weather code
    uint8_t  sr_h;
    uint8_t  sr_m;
    uint8_t  ss_h;
    uint8_t  ss_m;
};

// ── Mesh wire format ─────────────────────────────────────────────────────────
// Compact weather snapshot shared between devices over ESP-NOW. Carries
// the subset of WeatherState the Clock / Weather screens actually display
// (no hourly curve) so mesh-only peers can render a live weather tile
// without ever touching WiFi themselves. ~105 bytes on the wire, well
// under the 250-byte ESP-NOW MTU. Defined here (not in net_espnow.cpp)
// so weather.cpp can memcpy directly into g_weather from the rx path.
struct __attribute__((packed)) EspNowWeatherMsg {
    uint8_t  type;                        // must equal MSG_WEATHER (7)
    uint32_t fetched_epoch;               // UTC seconds of gateway fetch
    char     city[WEATHER_CITY_LEN];
    int16_t  current_f;
    uint8_t  current_code;
    int16_t  d0_hi;
    int16_t  d0_lo;
    uint8_t  d0_code;
    uint8_t  d0_sr_h, d0_sr_m, d0_ss_h, d0_ss_m;
    int16_t  d1_hi;
    int16_t  d1_lo;
    uint8_t  d1_code;
    int16_t  d2_hi;
    int16_t  d2_lo;
    uint8_t  d2_code;
    uint8_t  has_alert;
    char     alert[WEATHER_ALERT_LEN];
};

struct WeatherState {
    // Location (persisted)
    float    lat;
    float    lon;
    char     city[WEATHER_CITY_LEN];
    bool     located;

    // Status
    bool     valid;                // true once we have a successful fetch
    bool     fetch_in_progress;    // non-blocking task running
    uint32_t last_update_ms;       // millis() of last successful fetch
    uint32_t last_fail_ms;         // millis() of last failed fetch

    // Current conditions
    int16_t  current_f;
    uint8_t  current_code;
    float    current_precip_mm;

    // Next 12 hours starting from the current hour
    int16_t  hourly_temp[WEATHER_HOURLY_SLOTS];
    uint8_t  hourly_precip_pct[WEATHER_HOURLY_SLOTS];
    uint8_t  hourly_code[WEATHER_HOURLY_SLOTS];
    float    hourly_precip_mm[WEATHER_HOURLY_SLOTS];

    // Today + next 2 days
    WeatherDay daily[WEATHER_DAILY_SLOTS];

    // Precipitation alert banner (empty = none)
    char     alert[WEATHER_ALERT_LEN];
    bool     has_alert;
};

extern WeatherState g_weather;

// Load persisted location (if any) and prime state. Safe to call before WiFi.
void weather_init(void);

// Call from main loop / net_poll. Will kick off a background fetch task when
// WiFi is available and the refresh interval has elapsed. Non-blocking.
void weather_poll(void);

// Short human label for a WMO weather code (6 chars max, uppercase)
const char* weather_code_short(uint8_t code);

// Longer human label for a WMO weather code
const char* weather_code_long(uint8_t code);

// Returns true if g_weather has ever had a successful fetch
bool weather_has_data(void);

// Format a 24h HH/MM pair into dst according to the current clock format
// setting (12h / 24h). 12h output is "H:MMa" / "H:MMp" style, no leading
// zero on hour, to keep the label short on the weather / clock screens.
// Returns the number of chars written (excluding null), capped at dst_len-1.
int  weather_format_time(char* dst, size_t dst_len, uint8_t h, uint8_t m);

// ── Mesh plumbing ────────────────────────────────────────────────────────────
// Called by net_espnow.cpp when a MSG_WEATHER frame arrives. Copies the
// packed fields into g_weather, preferring a recent local fetch over a
// mesh broadcast when this device has its own working internet.
void weather_apply_from_mesh(const uint8_t* data, int len);

// Implemented in net_espnow.cpp. Broadcasts the current g_weather snapshot
// on the shared ESP-NOW channel so mesh-only peers can see it. Called by
// weather.cpp right after a successful internet fetch.
void espnow_broadcast_weather(void);

// Drop the "valid" latch so the next weather_poll() fires a fresh fetch
// on the next cycle. Called by the Settings > Refresh Now button and by
// the mesh refresh-request handler. Does not block.
void weather_force_refresh(void);

// millis() since the last successful weather fetch (local or mesh). Used
// by the Settings screen to render "WX: 4m ago". UINT32_MAX if never
// fetched.
uint32_t weather_last_update_age_ms(void);
