// ============================================================
// Freenove ESP32-S3 Display (FNK0104N) — speaker output
//
// ES8311 codec on I2C (shared with touch) + I2S, with the speaker
// amplifier switched by GPIO 1. Plays simple tones so the existing
// Pip-Boy sound effects (clicks, chimes, boot jingle) work unchanged.
// ============================================================
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Start I2S, set up the codec and the background tone task.
bool fn35_audio_init(void);

// Play a continuous tone. freq = 0 is silence. volume is 0-100.
void fn35_audio_tone(uint16_t freq, uint8_t volume);

// Speaker amplifier on/off.
void fn35_audio_amp(bool on);
