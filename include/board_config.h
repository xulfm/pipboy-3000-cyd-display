// ============================================================
// Board selection — pins and features that differ per board.
//
//   env:cyd / env:cyd_st7789  -> ESP32-2432S028R "Cheap Yellow Display"
//   env:freenove35            -> Freenove ESP32-S3 Display 3.5" (FNK0104N)
//
// The build environment in platformio.ini sets BOARD_FREENOVE35 for
// the Freenove board; everything else defaults to the CYD.
// ============================================================
#pragma once

#if defined(BOARD_FREENOVE35)
  // Freenove FNK0104N: ESP32-S3 (16MB flash, 8MB OPI PSRAM),
  // 3.5" 320x480 ST77922 over QSPI, capacitive touch on I2C.
  #define BOARD_LABEL       "ESP32-S3"
  #define LCD_BL_PIN        41      // backlight (PWM)
  #define BATT_ADC_PIN      8       // battery sense, 100K/100K divider
  #define BOARD_HAS_BUZZER  0
  #define BOARD_HAS_I2S_AUDIO 1     // ES8311 codec + speaker (src/fn35_audio.cpp)
#else
  // Classic CYD (ESP32-2432S028R)
  #define BOARD_CYD         1
  #define BOARD_LABEL       "ESP32"
  #define LCD_BL_PIN        21
  #define BATT_ADC_PIN      34
  #define BOARD_HAS_BUZZER  1       // LEDC tone on GPIO 26, amp enable GPIO 4
  #define BOARD_HAS_I2S_AUDIO 0
#endif
