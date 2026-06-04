// ============================================================
// Vault 89 PDI-Boy -- Clock + NTP Sync
// Simple time-of-day module: NTP over WiFi, manual fallback.
// ESP32 internal RTC keeps time while powered.
// ============================================================
#pragma once
#include <stdint.h>
#include <time.h>

// Call from setup() once
void clock_init(void);

// Call periodically (from main loop) — triggers NTP sync once WiFi is up
void clock_poll(void);

// Returns true if internal time is believed to be correct
bool clock_is_set(void);

// Get current local time; returns true on success
bool clock_get(struct tm* out);

// Manually set local time (hour 0-23, min 0-59, sec 0-59, y/m/d full date)
void clock_set_manual(int year, int month, int day, int h, int m, int s);

// Set local time from a UTC epoch (seconds since 1970). Used by the
// ESP-NOW time bridge so an offline device can pick up the time from
// another device that has NTP. Also adopts the sender's timezone if tz
// is in the valid range, so the display matches the gateway.
void clock_set_from_epoch(uint32_t utc_epoch, int tz_hours);

// True if the clock has been set via NTP (not manual/ESP-NOW). Used by
// the ESP-NOW time broadcaster to decide whether this device is
// authoritative enough to share time with peers.
bool clock_has_ntp(void);

// Force a fresh NTP sync on the next clock_poll() call. Used by the
// Settings > Refresh Now button and by the MSG_REFRESH_REQ handler
// when a mesh peer asks us (the gateway) to re-sync everything. If
// WiFi isn't up, this just arms the flag and the next successful
// WiFi connect will pick it up.
void clock_force_ntp_sync(void);

// millis() of the last successful clock set (NTP, manual, or
// ESP-NOW mesh time). 0 if never set. Used by the Settings screen to
// show "LAST SYNC: 2m ago".
uint32_t clock_last_sync_ms(void);

// Set/get timezone offset in hours (e.g. -5 for EST, -8 for PST)
// Saved to preferences.
void clock_set_tz(int hours);
int  clock_get_tz(void);

// 12-hour vs 24-hour display format (persisted)
void clock_set_24h(bool on);
bool clock_get_24h(void);
