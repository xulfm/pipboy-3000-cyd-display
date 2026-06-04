/**
 * lv_conf.h — LVGL configuration for Vault 89 PDI-Boy
 * Place this file at: include/lv_conf.h
 *
 * Tuned for ESP32-2432S028R:
 *   - 320kB RAM available for LVGL after WiFi stack (~80kB)
 *   - No PSRAM on CYD, so we budget carefully
 *   - 240x320 display, 16-bit color (RGB565)
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* Color depth: 16-bit RGB565 matches the ILI9341 natively */
#define LV_COLOR_DEPTH 16

/* Swap bytes for ILI9341 big-endian requirement */
#define LV_COLOR_16_SWAP 0

/* ── Memory ──────────────────────────────────────────────────
   LVGL internal heap. 32kB is comfortable for our UI.
   Increase to 48kB if you see "out of memory" in console. */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE   (48 * 1024U)

/* ── Display buffer ──────────────────────────────────────────
   1/10th of screen = 240*32 pixels = 15kB per buffer.
   Double-buffered so DMA can push one while LVGL fills other. */
#define LV_DISP_DEF_REFR_PERIOD  16    /* ~60fps refresh */
#define LV_INDEV_DEF_READ_PERIOD 30    /* touch poll ms */

/* ── Rendering ───────────────────────────────────────────────*/
#define LV_DRAW_COMPLEX 1              /* shadows, gradients */
#define LV_SHADOW_CACHE_SIZE 0         /* save RAM */
#define LV_CIRCLE_CACHE_SIZE 4
#define LV_IMG_CACHE_DEF_SIZE 2        /* cache 2 images */

/* ── Fonts ───────────────────────────────────────────────────
   We use Unscii (8-bit monospace) as the base font —
   it has the retro terminal look of the Pip-Boy.
   Enable Montserrat for numbers if you want cleaner digits. */
#define LV_FONT_MONTSERRAT_8  1
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_UNSCII_8      1
#define LV_FONT_UNSCII_16     1
#define LV_FONT_DEFAULT       &lv_font_unscii_8

/* ── Theme ───────────────────────────────────────────────────
   We build a custom Pip-Boy theme in theme.h.
   Start with the basic theme as base. */
#define LV_USE_THEME_DEFAULT  1
#define LV_USE_THEME_BASIC    1
#define LV_USE_THEME_MONO     0

/* ── Widgets we use ──────────────────────────────────────────*/
#define LV_USE_ARC        1
#define LV_USE_BAR        1   /* HP/AP/XP bars */
#define LV_USE_BTN        1   /* tab buttons */
#define LV_USE_BTNMATRIX  1   /* relay switch grid */
#define LV_USE_CANVAS     1
#define LV_USE_CHART      0
#define LV_USE_CHECKBOX   1   /* quest objectives */
#define LV_USE_DROPDOWN   1   // required by lv_calendar_header_dropdown
#define LV_USE_IMG        1   /* bitmap assets from converter */
#define LV_USE_LABEL      1   /* text everywhere */
#define LV_USE_LINE       1   /* signal bars */
#define LV_USE_LIST       1   /* inventory, quest lists */
#define LV_USE_METER      0
#define LV_USE_MSGBOX     1   /* announcements overlay */
#define LV_USE_ROLLER     1   // required by some extra widgets
#define LV_USE_SLIDER     1   // required by some extra widgets
#define LV_USE_SPINBOX    0
#define LV_USE_SPINNER    1   /* loading indicator */
#define LV_USE_TABLE      1   // required by some extra widgets
#define LV_USE_TABVIEW    1   /* STAT/INV/DATA/MAP/RADIO tabs */
#define LV_USE_TEXTAREA   1
#define LV_USE_KEYBOARD   0
#define LV_USE_TILEVIEW   1   /* map pan */
#define LV_USE_WIN        0

/* ── Animations ──────────────────────────────────────────────*/
#define LV_USE_ANIMATION 1

/* ── Logging ─────────────────────────────────────────────────
   Set to LV_LOG_LEVEL_WARN in production to save Serial bandwidth */
#define LV_USE_LOG       1
#define LV_LOG_LEVEL     LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF    1

/* ── Performance ─────────────────────────────────────────────*/
#define LV_USE_PERF_MONITOR 0   /* set to 1 to see FPS on screen */
#define LV_USE_MEM_MONITOR  0

/* ── GPU ─────────────────────────────────────────────────────
   ESP32 has no GPU, but it has DMA. TFT_eSPI handles this. */
#define LV_USE_GPU_ESP32_S3_DMA 0

/* ── File system (for SPIFFS asset loading) ──────────────────*/
#define LV_USE_FS_STDIO 0
#define LV_USE_FS_POSIX 0

/* ── Tick source ─────────────────────────────────────────────
   We call lv_tick_inc() from a hardware timer ISR in main.cpp */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#endif /* LV_CONF_H */