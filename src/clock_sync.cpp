// ============================================================
// Vault 89 PDI-Boy -- Clock + NTP Sync
// ============================================================
#include <Arduino.h>
#include <time.h>
#include <sys/time.h>
#include <WiFi.h>
#include <Preferences.h>
#include "clock_sync.h"
#include "net_state.h"

static int  s_tz_hours    = -6;   // default to CST (Texas/BBQ time)
static bool s_use_24h     = false;
static bool s_ntp_started = false;
static bool s_ntp_synced  = false;
static bool s_manual_set  = false;
static unsigned long s_last_try_ms  = 0;
static unsigned long s_last_sync_ms = 0;  // when the clock was last known-good

static void apply_timezone(void) {
    char tz_buf[16];
    snprintf(tz_buf, sizeof(tz_buf), "UTC%+d", -s_tz_hours);  // POSIX: sign flipped
    setenv("TZ", tz_buf, 1);
    tzset();
}

void clock_init(void) {
    Preferences prefs;
    prefs.begin("clock", true);
    s_tz_hours = prefs.getInt("tz", -6);
    s_use_24h  = prefs.getBool("h24", false);
    prefs.end();
    apply_timezone();
    Serial.printf("[clock] Init, TZ=UTC%+d, %s\n", s_tz_hours, s_use_24h ? "24h" : "12h");
}

void clock_poll(void) {
    // Kick off NTP once WiFi is connected
    if (g_net.wifi_connected && !s_ntp_started) {
        Serial.println("[clock] WiFi up — starting NTP sync");
        configTime(s_tz_hours * 3600, 0, "pool.ntp.org", "time.nist.gov");
        s_ntp_started = true;
        s_last_try_ms = millis();
    }

    // Re-check every 5 seconds until synced
    if (s_ntp_started && !s_ntp_synced && (millis() - s_last_try_ms) > 5000) {
        s_last_try_ms = millis();
        struct tm t;
        if (getLocalTime(&t, 0) && t.tm_year > (2024 - 1900)) {
            s_ntp_synced = true;
            s_last_sync_ms = millis();
            Serial.printf("[clock] NTP synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                          t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                          t.tm_hour, t.tm_min, t.tm_sec);
        }
    }
}

void clock_force_ntp_sync(void) {
    // Drop the "synced" latch so clock_poll() re-runs configTime() next
    // iteration. If WiFi isn't up yet we just arm the start flag; the
    // existing poll path will pick it up once g_net.wifi_connected flips.
    s_ntp_started = false;
    s_ntp_synced  = false;
    s_last_try_ms = 0;
    Serial.println("[clock] Forced NTP resync requested");
}

uint32_t clock_last_sync_ms(void) {
    return s_last_sync_ms;
}

bool clock_is_set(void) {
    return s_ntp_synced || s_manual_set;
}

bool clock_get(struct tm* out) {
    if (!out) return false;
    if (!clock_is_set()) return false;
    return getLocalTime(out, 0);
}

void clock_set_from_epoch(uint32_t utc_epoch, int tz_hours) {
    // Adopt the sender's timezone if it's within sane range so the
    // offline device's clock face matches the gateway.
    if (tz_hours >= -12 && tz_hours <= 14 && tz_hours != s_tz_hours) {
        s_tz_hours = tz_hours;
        apply_timezone();
    }
    struct timeval tv = { (time_t)utc_epoch, 0 };
    settimeofday(&tv, NULL);
    s_manual_set = true;
    s_last_sync_ms = millis();
    struct tm t;
    if (getLocalTime(&t, 0)) {
        Serial.printf("[clock] ESP-NOW time: %04d-%02d-%02d %02d:%02d:%02d (tz=%+d)\n",
                      t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                      t.tm_hour, t.tm_min, t.tm_sec, s_tz_hours);
    }
}

bool clock_has_ntp(void) {
    return s_ntp_synced;
}

void clock_set_manual(int year, int month, int day, int h, int m, int s) {
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = day;
    t.tm_hour = h;
    t.tm_min  = m;
    t.tm_sec  = s;
    t.tm_isdst = -1;
    time_t epoch = mktime(&t);
    struct timeval tv = { epoch, 0 };
    settimeofday(&tv, NULL);
    s_manual_set = true;
    s_last_sync_ms = millis();
    Serial.printf("[clock] Manual set: %04d-%02d-%02d %02d:%02d:%02d\n",
                  year, month, day, h, m, s);
}

void clock_set_tz(int hours) {
    if (hours < -12 || hours > 14) return;
    s_tz_hours = hours;
    Preferences prefs;
    prefs.begin("clock", false);
    prefs.putInt("tz", hours);
    prefs.end();
    apply_timezone();
    // Re-sync NTP with new offset
    if (g_net.wifi_connected) {
        configTime(s_tz_hours * 3600, 0, "pool.ntp.org", "time.nist.gov");
    }
}

int clock_get_tz(void) {
    return s_tz_hours;
}

void clock_set_24h(bool on) {
    s_use_24h = on;
    Preferences prefs;
    prefs.begin("clock", false);
    prefs.putBool("h24", on);
    prefs.end();
}

bool clock_get_24h(void) {
    return s_use_24h;
}
