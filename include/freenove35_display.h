// ============================================================
// Freenove ESP32-S3 Display 3.5" (FNK0104N) — display + touch driver
//
// The Pip-Boy UI is laid out for a 320x240 screen. This driver keeps
// LVGL at 320x240 and scales every frame up to the 480x320 panel, so
// all existing screens work unchanged. Touch is scaled back down to
// the same 320x240 coordinates.
//
// Build-time options (set in platformio.ini):
//   FN35_ROTATION  1 or 3   — landscape, USB on one side or the other.
//                             Swap if the picture is upside down.
//   FN35_FILL      1        — stretch to fill the whole 480x320 panel
//                  0        — keep the original shape (427x320 picture
//                             with thin black bars left and right)
// ============================================================
#pragma once
#include <lvgl.h>

#ifndef FN35_ROTATION
#define FN35_ROTATION 1
#endif
#ifndef FN35_FILL
#define FN35_FILL 1
#endif
// 1 = show a touch status line and a red dot where you touch, and print
//     [touch] lines to the serial monitor. Add -DFN35_TOUCH_DEBUG=1 to the
//     freenove35 build_flags to turn it on for troubleshooting.
#ifndef FN35_TOUCH_DEBUG
#define FN35_TOUCH_DEBUG 0
#endif

// Bring up the panel, clear it to black and start the touch controller.
// Backlight is NOT touched here — main.cpp drives it with LEDC PWM.
bool fn35_init(void);

// LVGL flush: area is in 320x240 UI coordinates.
void fn35_flush(const lv_area_t* area, const lv_color_t* color_p);

// Touch: returns true while pressed and fills x/y in 320x240 UI coordinates.
bool fn35_touch_read(int* x, int* y);

// One-line touch status for the on-screen debug readout.
void fn35_touch_status(char* buf, size_t len);
