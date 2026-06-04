// ============================================================
// Vault 89 PDI-Boy -- Day/Night Brightness Manager
// Time-of-day based backlight control with an optional
// "quiet while dim" feature that mutes network SFX at night.
//
// The existing idle-dim (touch -> bright, 30s no touch -> dim)
// still runs during the day; night mode overrides both by
// capping the maximum backlight level to a lower value and
// marking the device as "quiet" so background sounds are
// suppressed. Screen taps still wake / play clicks as normal.
// ============================================================
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Backlight level enum — caller-facing; the brightness module
// translates to 0-255 PWM based on day/night state.
typedef enum {
    BL_LEVEL_FULL  = 0,   // active touch / daytime
    BL_LEVEL_IDLE  = 1,   // daytime idle (30s no touch)
    BL_LEVEL_OFF   = 2,   // fully off (deep sleep etc)
} BacklightLevel;

void brightness_init(void);

// Call every loop — re-evaluates the current time-of-day state.
// Cheap (just reads localtime + compares hour).
void brightness_poll(void);

// Apply a logical level. The module picks the actual PWM value
// taking day/night into account. Duplicate calls are cheap.
void brightness_apply(BacklightLevel level);

// True if it's currently "night" (between dim_hour and bright_hour).
bool brightness_is_night(void);

// True if sound should be suppressed right now. This is the
// night flag AND-ed with the "quiet while dim" setting.
bool brightness_is_quiet(void);

// Schedule config (24h clock, hours 0..23). Persisted to NVS.
void brightness_set_schedule(uint8_t dim_hour, uint8_t bright_hour);
void brightness_get_schedule(uint8_t* dim_hour, uint8_t* bright_hour);

// Quiet-while-dim toggle. Persisted.
void brightness_set_quiet_while_dim(bool on);
bool brightness_get_quiet_while_dim(void);

// Max brightness caps at night vs day. Persisted.
void brightness_set_night_level(uint8_t pwm_0_255);
uint8_t brightness_get_night_level(void);

#ifdef __cplusplus
}
#endif
