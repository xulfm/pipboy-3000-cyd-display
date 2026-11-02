// ============================================================
// Vault 89 PDI-Boy -- Day/Night Brightness Manager
// See brightness.h for the design notes.
// ============================================================
#include <Arduino.h>
#include <Preferences.h>
#include "brightness.h"
#include "clock_sync.h"

// Existing PWM config lives in main.cpp; we only need the channel
// number and the full-bright PWM value. Keep in sync with main.cpp.
#define BL_PWM_CHANNEL  0
#define BL_FULL_DAY     255
#define BL_IDLE_DAY     64    // matches main.cpp BL_DIM

// Defaults. Editable from Settings, persisted in the "bl" namespace.
static uint8_t s_dim_hour    = 18;  // 6pm
static uint8_t s_bright_hour = 7;   // 7am
// Night-time brightness cap (0-255). Boards can override the default
// from platformio.ini, e.g. -DBL_NIGHT_LEVEL_DEFAULT=153 for ~60%.
#ifndef BL_NIGHT_LEVEL_DEFAULT
#define BL_NIGHT_LEVEL_DEFAULT 16      // very dim at night
#endif
static uint8_t s_night_level = BL_NIGHT_LEVEL_DEFAULT;
// Mute sound effects during night mode? Boards can override the default
// from platformio.ini with -DBL_QUIET_DEFAULT=0.
#ifndef BL_QUIET_DEFAULT
#define BL_QUIET_DEFAULT 1
#endif
static bool    s_quiet_dim   = BL_QUIET_DEFAULT;

static bool    s_is_night    = false;
static bool    s_inited      = false;
static BacklightLevel s_current_level = BL_LEVEL_FULL;

static void write_pwm(uint8_t val) {
    ledcWrite(BL_PWM_CHANNEL, val);
}

// Reload night-mode flag from the local wall clock. Safe to call
// before the clock is set — we default to "day" until NTP or a
// mesh MSG_TIME delivers a real time.
static void recompute_night(void) {
    struct tm t;
    if (!clock_get(&t)) {
        s_is_night = false;
        return;
    }
    int h = t.tm_hour;
    // Schedule wraps around midnight when bright_hour < dim_hour
    // (the normal case: dim 18, bright 07).
    if (s_bright_hour < s_dim_hour) {
        s_is_night = (h >= s_dim_hour) || (h < s_bright_hour);
    } else if (s_bright_hour > s_dim_hour) {
        // Weird case: bright 10, dim 22 (daytime window is [10,22)).
        s_is_night = (h < s_bright_hour) || (h >= s_dim_hour);
    } else {
        // Equal -> never night
        s_is_night = false;
    }
}

void brightness_init(void) {
    Preferences p;
    if (p.begin("bl", true)) {
        s_dim_hour    = p.getUChar("dim_h",  18);
        s_bright_hour = p.getUChar("brt_h",  7);
        s_night_level = p.getUChar("n_lvl",  BL_NIGHT_LEVEL_DEFAULT);
        s_quiet_dim   = p.getBool("quiet",   BL_QUIET_DEFAULT);
        p.end();
    }
    if (s_dim_hour    > 23) s_dim_hour    = 18;
    if (s_bright_hour > 23) s_bright_hour = 7;
    s_inited = true;
    recompute_night();
    Serial.printf("[bl] init: dim@%02d bright@%02d night_lvl=%u quiet=%d\n",
                  s_dim_hour, s_bright_hour, s_night_level, (int)s_quiet_dim);
}

void brightness_poll(void) {
    if (!s_inited) return;
    // Recompute at most once per second
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 1000) return;
    last = now;
    bool was_night = s_is_night;
    recompute_night();
    if (was_night != s_is_night) {
        Serial.printf("[bl] night=%d (h=%02d)\n", (int)s_is_night,
                      [](){ struct tm t; clock_get(&t); return t.tm_hour; }());
        // Reapply the current logical level with the new cap
        brightness_apply(s_current_level);
    }
}

void brightness_apply(BacklightLevel level) {
    s_current_level = level;
    if (level == BL_LEVEL_OFF) {
        write_pwm(0);
        return;
    }
    if (s_is_night) {
        // Night caps everything at s_night_level
        write_pwm(level == BL_LEVEL_FULL ? s_night_level
                                         : (uint8_t)(s_night_level / 2));
    } else {
        write_pwm(level == BL_LEVEL_FULL ? BL_FULL_DAY : BL_IDLE_DAY);
    }
}

bool brightness_is_night(void) { return s_is_night; }

bool brightness_is_quiet(void) { return s_is_night && s_quiet_dim; }

void brightness_set_schedule(uint8_t dim_hour, uint8_t bright_hour) {
    if (dim_hour > 23 || bright_hour > 23) return;
    s_dim_hour    = dim_hour;
    s_bright_hour = bright_hour;
    Preferences p;
    if (p.begin("bl", false)) {
        p.putUChar("dim_h", s_dim_hour);
        p.putUChar("brt_h", s_bright_hour);
        p.end();
    }
    recompute_night();
    brightness_apply(s_current_level);
}

void brightness_get_schedule(uint8_t* dim_hour, uint8_t* bright_hour) {
    if (dim_hour)    *dim_hour    = s_dim_hour;
    if (bright_hour) *bright_hour = s_bright_hour;
}

void brightness_set_quiet_while_dim(bool on) {
    s_quiet_dim = on;
    Preferences p;
    if (p.begin("bl", false)) { p.putBool("quiet", on); p.end(); }
}

bool brightness_get_quiet_while_dim(void) { return s_quiet_dim; }

void brightness_set_night_level(uint8_t v) {
    s_night_level = v;
    Preferences p;
    if (p.begin("bl", false)) { p.putUChar("n_lvl", v); p.end(); }
    brightness_apply(s_current_level);
}

uint8_t brightness_get_night_level(void) { return s_night_level; }
