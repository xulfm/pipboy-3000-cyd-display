// ============================================================
// Vault 89 PDI-Boy -- Weather Module
// Background fetch from Open-Meteo + ip-api.com geolocation.
// Runs in a one-shot FreeRTOS task so the UI thread never blocks.
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_mac.h>
#include "weather.h"
#include "net_state.h"
#include "clock_sync.h"

WeatherState g_weather = {};

#define WEATHER_REFRESH_MS  (10UL * 60UL * 1000UL)   // 10 min
#define WEATHER_RETRY_MS    (60UL  * 1000UL)         // 1 min after failure
#define WEATHER_GEO_RETRY_MS (120UL * 1000UL)        // 2 min if geolocation failed

static volatile bool s_fetch_running = false;
static bool          s_tried_geo     = false;
static int           s_consec_fails  = 0;      // consecutive fetch failures
#define MAX_CONSEC_FAILS  5  // after 5 fails, back off to full refresh interval
// MAC-derived offset (0..60s) applied to the 10-min fetch cadence so two
// ACTIVE gateways on different uplinks in the same mesh don't hammer the
// Open-Meteo API at the exact same wall-clock instant. Set lazily in
// weather_poll() on first run. Deferred / mesh-only devices never reach
// the fetch path so the stagger is only paid by real uplinks.
static unsigned long s_fetch_stagger_ms = 0;
static bool          s_stagger_seeded   = false;

// ── Helpers ──────────────────────────────────────────────────────────────────

static int16_t f_round(float f) {
    if (f > 0) return (int16_t)(f + 0.5f);
    return (int16_t)(f - 0.5f);
}

// Public: format a 24h hour/minute pair respecting the user's clock format
// preference. 12h output uses "H:MMa" / "H:MMp" (no leading zero on hour)
// to keep sunrise/sunset labels on the clock + weather screens compact.
int weather_format_time(char* dst, size_t dst_len, uint8_t h, uint8_t m) {
    if (!dst || dst_len == 0) return 0;
    if (clock_get_24h()) {
        return snprintf(dst, dst_len, "%02u:%02u", (unsigned)h, (unsigned)m);
    }
    uint8_t h12 = h % 12;
    if (h12 == 0) h12 = 12;
    const char* suf = (h >= 12) ? "p" : "a";
    return snprintf(dst, dst_len, "%u:%02u%s", (unsigned)h12, (unsigned)m, suf);
}

// Parse "YYYY-MM-DDTHH:MM" and return the HH component (0..23). -1 on fail.
static int parse_hour(const char* iso) {
    if (!iso || strlen(iso) < 13) return -1;
    if (iso[11] < '0' || iso[11] > '9') return -1;
    if (iso[12] < '0' || iso[12] > '9') return -1;
    return (iso[11] - '0') * 10 + (iso[12] - '0');
}

static bool parse_hm(const char* iso, uint8_t* h, uint8_t* m) {
    int hh = parse_hour(iso);
    if (hh < 0 || strlen(iso) < 16) return false;
    int mm = (iso[14] - '0') * 10 + (iso[15] - '0');
    if (mm < 0 || mm > 59) return false;
    *h = (uint8_t)hh;
    *m = (uint8_t)mm;
    return true;
}

// Compare the "YYYY-MM-DD" prefix of two ISO timestamps.
static bool same_date(const char* a, const char* b) {
    return a && b && strncmp(a, b, 10) == 0;
}

// ── WMO code labels ──────────────────────────────────────────────────────────

const char* weather_code_short(uint8_t code) {
    switch (code) {
        case 0:  return "CLEAR";
        case 1:  return "M.SUN";
        case 2:  return "P.CLDY";
        case 3:  return "CLOUDY";
        case 45: case 48: return "FOG";
        case 51: case 53: case 55: return "DRIZZL";
        case 56: case 57: return "FR.DRZ";
        case 61: case 63: case 65: return "RAIN";
        case 66: case 67: return "FR.RN";
        case 71: case 73: case 75: return "SNOW";
        case 77: return "SNOW";
        case 80: case 81: case 82: return "SHWRS";
        case 85: case 86: return "SNSHWR";
        case 95: return "T.STRM";
        case 96: case 99: return "HAIL";
        default: return "?";
    }
}

const char* weather_code_long(uint8_t code) {
    switch (code) {
        case 0:  return "Clear sky";
        case 1:  return "Mainly clear";
        case 2:  return "Partly cloudy";
        case 3:  return "Overcast";
        case 45: case 48: return "Fog";
        case 51: return "Light drizzle";
        case 53: return "Drizzle";
        case 55: return "Heavy drizzle";
        case 56: case 57: return "Freezing drizzle";
        case 61: return "Light rain";
        case 63: return "Rain";
        case 65: return "Heavy rain";
        case 66: case 67: return "Freezing rain";
        case 71: return "Light snow";
        case 73: return "Snow";
        case 75: return "Heavy snow";
        case 77: return "Snow grains";
        case 80: return "Rain showers";
        case 81: return "Rain showers";
        case 82: return "Heavy showers";
        case 85: case 86: return "Snow showers";
        case 95: return "Thunderstorm";
        case 96: case 99: return "T-storm w/ hail";
        default: return "Unknown";
    }
}

static bool code_is_frozen(uint8_t code) {
    return (code >= 71 && code <= 77) || code == 85 || code == 86 ||
           code == 66 || code == 67;
}

// ── NVS persistence ──────────────────────────────────────────────────────────

static void load_location(void) {
    Preferences p;
    if (!p.begin("weather", true)) return;
    g_weather.lat = p.getFloat("lat", 0.0f);
    g_weather.lon = p.getFloat("lon", 0.0f);
    String city = p.getString("city", "");
    strncpy(g_weather.city, city.c_str(), sizeof(g_weather.city) - 1);
    g_weather.city[sizeof(g_weather.city) - 1] = '\0';
    p.end();
    if (g_weather.lat != 0.0f || g_weather.lon != 0.0f) {
        g_weather.located = true;
        Serial.printf("[weather] Loaded location: %s (%.3f, %.3f)\n",
                      g_weather.city, g_weather.lat, g_weather.lon);
    }
}

static void save_location(void) {
    Preferences p;
    if (!p.begin("weather", false)) return;
    p.putFloat("lat", g_weather.lat);
    p.putFloat("lon", g_weather.lon);
    p.putString("city", g_weather.city);
    p.end();
}

// ── ip-api.com geolocation ───────────────────────────────────────────────────

static bool fetch_geolocation(void) {
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(6000);
    if (!http.begin(client,
                    "http://ip-api.com/json/?fields=status,lat,lon,city")) {
        return false;
    }
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[weather] geoloc HTTP %d\n", code);
        http.end();
        return false;
    }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    const char* status = doc["status"] | "";
    if (strcmp(status, "success") != 0) return false;

    g_weather.lat = doc["lat"] | 0.0f;
    g_weather.lon = doc["lon"] | 0.0f;
    const char* city = doc["city"] | "Unknown";
    strncpy(g_weather.city, city, sizeof(g_weather.city) - 1);
    g_weather.city[sizeof(g_weather.city) - 1] = '\0';
    g_weather.located = true;
    save_location();
    Serial.printf("[weather] Geolocated: %s (%.3f, %.3f)\n",
                  g_weather.city, g_weather.lat, g_weather.lon);
    return true;
}

// ── Open-Meteo forecast fetch ────────────────────────────────────────────────

static bool fetch_forecast(void) {
    if (!g_weather.located) return false;

    char url[512];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast"
             "?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code,precipitation"
             "&hourly=temperature_2m,precipitation,precipitation_probability,weather_code"
             "&daily=temperature_2m_max,temperature_2m_min,sunrise,sunset,weather_code"
             "&forecast_days=3&timezone=auto&temperature_unit=fahrenheit"
             "&wind_speed_unit=mph",
             g_weather.lat, g_weather.lon);

    WiFiClientSecure sclient;
    sclient.setInsecure();   // Open-Meteo is public; no cert pin needed
    sclient.setTimeout(10);

    HTTPClient http;
    http.setTimeout(10000);
    http.setReuse(false);
    if (!http.begin(sclient, url)) {
        Serial.println("[weather] http.begin failed");
        return false;
    }
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[weather] forecast HTTP %d\n", code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("[weather] JSON parse err: %s\n", err.c_str());
        return false;
    }

    JsonObject current = doc["current"];
    JsonObject hourly  = doc["hourly"];
    JsonObject daily   = doc["daily"];
    if (current.isNull() || hourly.isNull() || daily.isNull()) {
        Serial.println("[weather] missing sections");
        return false;
    }

    // --- current ---
    g_weather.current_f         = f_round(current["temperature_2m"] | 0.0f);
    g_weather.current_code      = (uint8_t)(current["weather_code"] | 0);
    g_weather.current_precip_mm = current["precipitation"] | 0.0f;
    const char* curr_time       = current["time"] | "";
    int curr_hour               = parse_hour(curr_time);
    if (curr_hour < 0) curr_hour = 0;

    // --- hourly — align to current hour ---
    JsonArray h_time  = hourly["time"];
    JsonArray h_temp  = hourly["temperature_2m"];
    JsonArray h_prec  = hourly["precipitation"];
    JsonArray h_pprob = hourly["precipitation_probability"];
    JsonArray h_code  = hourly["weather_code"];

    int start = 0;
    int n = h_time.size();
    // Find the hourly index whose date+hour matches current.time
    for (int i = 0; i < n; i++) {
        const char* t = h_time[i];
        if (!t) continue;
        if (same_date(t, curr_time) && parse_hour(t) == curr_hour) {
            start = i;
            break;
        }
    }

    for (int i = 0; i < WEATHER_HOURLY_SLOTS; i++) {
        int j = start + i;
        if (j < n) {
            g_weather.hourly_temp[i]        = f_round(h_temp[j] | 0.0f);
            g_weather.hourly_precip_mm[i]   = h_prec[j] | 0.0f;
            g_weather.hourly_precip_pct[i]  = (uint8_t)(h_pprob[j] | 0);
            g_weather.hourly_code[i]        = (uint8_t)(h_code[j] | 0);
        } else {
            g_weather.hourly_temp[i]        = 0;
            g_weather.hourly_precip_mm[i]   = 0;
            g_weather.hourly_precip_pct[i]  = 0;
            g_weather.hourly_code[i]        = 0;
        }
    }

    // --- daily (today + next 2) ---
    JsonArray d_hi   = daily["temperature_2m_max"];
    JsonArray d_lo   = daily["temperature_2m_min"];
    JsonArray d_sr   = daily["sunrise"];
    JsonArray d_ss   = daily["sunset"];
    JsonArray d_code = daily["weather_code"];
    for (int i = 0; i < WEATHER_DAILY_SLOTS; i++) {
        WeatherDay& d = g_weather.daily[i];
        d.hi_f = f_round(d_hi[i] | 0.0f);
        d.lo_f = f_round(d_lo[i] | 0.0f);
        d.code = (uint8_t)(d_code[i] | 0);
        d.sr_h = d.sr_m = d.ss_h = d.ss_m = 0;
        const char* sr = d_sr[i];
        const char* ss = d_ss[i];
        if (sr) parse_hm(sr, &d.sr_h, &d.sr_m);
        if (ss) parse_hm(ss, &d.ss_h, &d.ss_m);
    }

    // --- precipitation alert (next 2 hours) ---
    g_weather.has_alert = false;
    g_weather.alert[0]  = '\0';
    const float RAIN_THRESH = 0.2f;

    bool frozen_now = code_is_frozen(g_weather.current_code);
    const char* type_label = frozen_now ? "SNOW" : "RAIN";

    if (g_weather.current_precip_mm >= RAIN_THRESH) {
        snprintf(g_weather.alert, sizeof(g_weather.alert),
                 ">> %s FALLING NOW <<", type_label);
        g_weather.has_alert = true;
    } else if (g_weather.hourly_precip_mm[1] >= RAIN_THRESH) {
        bool frozen = code_is_frozen(g_weather.hourly_code[1]);
        snprintf(g_weather.alert, sizeof(g_weather.alert),
                 ">> %s WITHIN 1 HR <<", frozen ? "SNOW" : "RAIN");
        g_weather.has_alert = true;
    } else if (g_weather.hourly_precip_mm[2] >= RAIN_THRESH) {
        bool frozen = code_is_frozen(g_weather.hourly_code[2]);
        snprintf(g_weather.alert, sizeof(g_weather.alert),
                 ">> %s IN ABOUT 2 HR <<", frozen ? "SNOW" : "RAIN");
        g_weather.has_alert = true;
    }

    Serial.printf("[weather] OK: %dF %s  hi=%d lo=%d  sr=%02u:%02u ss=%02u:%02u\n",
                  g_weather.current_f, weather_code_short(g_weather.current_code),
                  g_weather.daily[0].hi_f, g_weather.daily[0].lo_f,
                  g_weather.daily[0].sr_h, g_weather.daily[0].sr_m,
                  g_weather.daily[0].ss_h, g_weather.daily[0].ss_m);
    if (g_weather.has_alert) {
        Serial.printf("[weather] Alert: %s\n", g_weather.alert);
    }

    g_weather.valid = true;
    g_weather.last_update_ms = millis();

    // We're the gateway that just pulled fresh data — push it out to
    // any mesh-only peers so they can update their tiles without ever
    // touching WiFi themselves.
    espnow_broadcast_weather();
    return true;
}

// ── Mesh receive ─────────────────────────────────────────────────────────────
// Called from on_recv() in net_espnow.cpp when a MSG_WEATHER frame lands.
// Copies the packed fields into g_weather. If this device has its own
// recent internet fetch, the mesh broadcast is ignored so a stale gateway
// can't stomp fresher local data.
void weather_apply_from_mesh(const uint8_t* data, int len) {
    if (!data || len < (int)sizeof(EspNowWeatherMsg)) return;
    const EspNowWeatherMsg* m = (const EspNowWeatherMsg*)data;

    unsigned long now = millis();
    if (g_weather.valid && g_net.wifi_connected &&
        (now - g_weather.last_update_ms) < WEATHER_REFRESH_MS) {
        // We have our own fresh fetch — keep ours.
        return;
    }

    // Sanity-check temperature values. A peer with uninitialised weather
    // data (all zeros, or garbage from a stale struct) can set valid=true
    // and stamp random numbers on our clock strip. Reject anything outside
    // a plausible Earth temperature range.
    if (m->current_f < -60 || m->current_f > 140 ||
        m->d0_hi < -60 || m->d0_hi > 140 ||
        m->d0_lo < -80 || m->d0_lo > 140) {
        Serial.printf("[weather] mesh rx REJECTED (out of range: %d / %d / %d)\n",
                      (int)m->current_f, (int)m->d0_hi, (int)m->d0_lo);
        return;
    }

    // Only adopt the gateway's city label if we haven't geolocated ourselves.
    if (!g_weather.located || g_weather.city[0] == '\0') {
        strncpy(g_weather.city, m->city, sizeof(g_weather.city) - 1);
        g_weather.city[sizeof(g_weather.city) - 1] = '\0';
    }

    g_weather.current_f         = m->current_f;
    g_weather.current_code      = m->current_code;
    g_weather.current_precip_mm = 0.0f;

    g_weather.daily[0].hi_f = m->d0_hi;
    g_weather.daily[0].lo_f = m->d0_lo;
    g_weather.daily[0].code = m->d0_code;
    g_weather.daily[0].sr_h = m->d0_sr_h;
    g_weather.daily[0].sr_m = m->d0_sr_m;
    g_weather.daily[0].ss_h = m->d0_ss_h;
    g_weather.daily[0].ss_m = m->d0_ss_m;

    g_weather.daily[1].hi_f = m->d1_hi;
    g_weather.daily[1].lo_f = m->d1_lo;
    g_weather.daily[1].code = m->d1_code;
    g_weather.daily[1].sr_h = 0;
    g_weather.daily[1].sr_m = 0;
    g_weather.daily[1].ss_h = 0;
    g_weather.daily[1].ss_m = 0;

    g_weather.daily[2].hi_f = m->d2_hi;
    g_weather.daily[2].lo_f = m->d2_lo;
    g_weather.daily[2].code = m->d2_code;
    g_weather.daily[2].sr_h = 0;
    g_weather.daily[2].sr_m = 0;
    g_weather.daily[2].ss_h = 0;
    g_weather.daily[2].ss_m = 0;

    // Hourly curve isn't shipped over the mesh. Zero the slots so stale
    // data from an earlier local fetch doesn't linger on the forecast
    // row if this device just lost its WiFi uplink.
    for (int i = 0; i < WEATHER_HOURLY_SLOTS; i++) {
        g_weather.hourly_temp[i]       = 0;
        g_weather.hourly_precip_mm[i]  = 0;
        g_weather.hourly_precip_pct[i] = 0;
        g_weather.hourly_code[i]       = 0;
    }

    g_weather.has_alert = (m->has_alert != 0);
    strncpy(g_weather.alert, m->alert, sizeof(g_weather.alert) - 1);
    g_weather.alert[sizeof(g_weather.alert) - 1] = '\0';

    g_weather.valid          = true;
    g_weather.last_update_ms = now;

    Serial.printf("[weather] mesh rx: %s %dF %s\n",
                  g_weather.city, g_weather.current_f,
                  weather_code_short(g_weather.current_code));
}

// ── Background fetch task ────────────────────────────────────────────────────

static void weather_task(void* arg) {
    (void)arg;
    bool need_geo = !g_weather.located;
    bool ok = true;
    if (need_geo) {
        ok = fetch_geolocation();
    }
    if (ok && g_weather.located) {
        ok = fetch_forecast();
    }
    if (!ok) {
        g_weather.last_fail_ms = millis();
        s_consec_fails++;
        Serial.printf("[weather] Fetch failed (%d consecutive)\n", s_consec_fails);
    } else {
        s_consec_fails = 0;
    }
    s_fetch_running = false;
    g_weather.fetch_in_progress = false;
    vTaskDelete(NULL);
}

// ── Public API ───────────────────────────────────────────────────────────────

void weather_init(void) {
    memset(&g_weather, 0, sizeof(g_weather));
    load_location();
    Serial.println("[weather] init");
}

void weather_poll(void) {
    if (s_fetch_running) return;
    if (!g_net.wifi_connected) return;

    // Lazily seed a per-device fetch stagger (0..60s) derived from the
    // base MAC so the Refresh cadence of two ACTIVE gateways on the same
    // mesh doesn't line up byte-for-byte. Cheap XOR hash — we just need
    // some entropy across devices.
    if (!s_stagger_seeded) {
        uint8_t mac[6] = {};
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
            uint32_t h = 0;
            for (int i = 0; i < 6; i++) h = (h * 131) ^ mac[i];
            s_fetch_stagger_ms = (h % 60000UL);
        }
        s_stagger_seeded = true;
        Serial.printf("[weather] Fetch stagger offset: %lums\n",
                      (unsigned long)s_fetch_stagger_ms);
    }

    unsigned long now = millis();

    // Need to fetch if we've never fetched, refresh interval elapsed, or
    // previous attempt failed and retry interval elapsed. Back off
    // exponentially after repeated failures so a busted SSL stack doesn't
    // spin-loop and exhaust heap. After MAX_CONSEC_FAILS, wait the full
    // refresh interval (10 min) before trying again.
    bool due = false;
    if (!g_weather.valid) {
        if (g_weather.last_fail_ms == 0) {
            due = true;   // First-ever attempt
        } else {
            // Exponential backoff: 60s, 120s, 240s, 480s, then clamp to
            // WEATHER_REFRESH_MS (600s) after MAX_CONSEC_FAILS.
            unsigned long backoff = WEATHER_RETRY_MS;
            for (int i = 1; i < s_consec_fails && i < MAX_CONSEC_FAILS; i++)
                backoff = (backoff < WEATHER_REFRESH_MS / 2) ? backoff * 2
                                                              : WEATHER_REFRESH_MS;
            if (s_consec_fails >= MAX_CONSEC_FAILS) backoff = WEATHER_REFRESH_MS;
            if (now - g_weather.last_fail_ms > backoff) due = true;
        }
    } else if (now - g_weather.last_update_ms >
               (WEATHER_REFRESH_MS + s_fetch_stagger_ms)) {
        due = true;
    }
    if (!due) return;

    s_fetch_running = true;
    g_weather.fetch_in_progress = true;
    // Pin to core 0 (WiFi driver core). The TLS handshake for Open-Meteo
    // runs mbedtls crypto bursts that don't yield cooperatively; if we put
    // this on core 1 at priority 1 alongside the Arduino loopTask, it
    // starves sfx_tick() long enough for the sound watchdog to fire,
    // which is what caused the ~2s beep-on-login / beep-on-pomo-press
    // bug. Core 0 already hosts the WiFi driver so the crypto work lands
    // on the same core as the network I/O. 12KB stack is enough for
    // HTTPClient + ArduinoJson.
    BaseType_t r = xTaskCreatePinnedToCore(
        weather_task, "weatherFetch", 12288, NULL, 1, NULL, 0);
    if (r != pdPASS) {
        Serial.println("[weather] task create FAILED");
        s_fetch_running = false;
        g_weather.fetch_in_progress = false;
        g_weather.last_fail_ms = millis();
    }
}

bool weather_has_data(void) {
    return g_weather.valid;
}

void weather_force_refresh(void) {
    // Drop the fresh-data guard so the next weather_poll() iteration
    // fires a new fetch even if the 10-min interval hasn't elapsed. We
    // keep the existing g_weather contents so the UI stays populated
    // while the background task runs; only the timers get reset.
    g_weather.last_update_ms = 0;
    g_weather.last_fail_ms   = 0;
    s_consec_fails           = 0;
    Serial.println("[weather] Forced refresh requested");
}

uint32_t weather_last_update_age_ms(void) {
    if (!g_weather.valid || g_weather.last_update_ms == 0) return UINT32_MAX;
    unsigned long now = millis();
    if (now < g_weather.last_update_ms) return 0;
    return (uint32_t)(now - g_weather.last_update_ms);
}
