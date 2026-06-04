#pragma once
// ============================================================
// Vault 89 PDI-Boy -- Sound Effects System
// Non-blocking tone generation via dacWrite on GPIO 26
// Amplifier enable: GPIO 4, ACTIVE LOW
// ============================================================

// Sound effect IDs
enum SoundFX : uint8_t {
    SFX_MSG_IN,        // incoming chat / ESP-NOW message
    SFX_VAULT_ALERT,   // vault announcement from server
    SFX_PEER_JOIN,     // new ESP-NOW peer discovered
    SFX_PEER_LEAVE,    // peer went offline
    SFX_CONNECT,       // WiFi / WS connected
    SFX_DISCONNECT,    // WiFi / WS disconnected
    SFX_CLICK,         // UI button tap
    SFX_ERROR,         // error beep
    SFX_BOOT,          // boot-up jingle
};

// Initialize the sound system (call once in setup)
void sfx_init(void);

// Play a predefined sound effect (non-blocking, queues the sequence)
void sfx_play(SoundFX fx);

// Play a custom tone: freq in Hz, duration in ms
void sfx_tone(uint16_t freq, uint16_t duration_ms);

// Set volume 0-100 (persists across plays, saved to NVS)
void sfx_set_volume(uint8_t vol);
uint8_t sfx_get_volume(void);

// Beep tone preset selection for the tap/click sound (0..CLICK_TONE_COUNT-1)
// Preset 0 is the default short high tick.
#define CLICK_TONE_COUNT 4
void        sfx_set_click_tone(uint8_t idx);
uint8_t     sfx_get_click_tone(void);
const char* sfx_click_tone_name(uint8_t idx);

// Preview the currently selected click tone (for settings UI)
void sfx_play_click_preview(void);

// Stop any currently playing sound
void sfx_stop(void);

// Must be called from loop() — drives the tone generation
void sfx_tick(void);

// Returns true if a sound is currently playing
bool sfx_is_playing(void);
