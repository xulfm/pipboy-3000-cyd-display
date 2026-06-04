// ============================================================
// Vault 89 PDI-Boy -- Sound Effects System
// LEDC hardware PWM on GPIO 26 (channel 2, channel 0 = backlight)
// Amplifier enable: GPIO 4, ACTIVE LOW
// ============================================================

#include <Arduino.h>
#include <Preferences.h>
#include "sound_fx.h"
#include "brightness.h"

#define SFX_PIN       26
#define SFX_AMP_PIN   4
#define SFX_LEDC_CH   2     // channel 0 = backlight, avoid it
#define SFX_MAX_NOTES 8

struct SfxNote {
    uint16_t freq;        // Hz (0 = silence/rest)
    uint16_t duration_ms;
};

struct SfxSequence {
    SfxNote notes[SFX_MAX_NOTES];
    uint8_t count;
};

// ── Predefined sound effects ──

static const SfxSequence s_sequences[] = {
    // SFX_MSG_IN — ascending two-tone chirp
    {{{800, 80}, {1200, 120}}, 2},

    // SFX_VAULT_ALERT — three urgent pulses
    {{{1000, 100}, {0, 50}, {1000, 100}, {0, 50}, {1000, 150}}, 5},

    // SFX_PEER_JOIN — friendly ascending triple
    {{{600, 60}, {800, 60}, {1000, 100}}, 3},

    // SFX_PEER_LEAVE — descending two-tone
    {{{800, 80}, {500, 120}}, 2},

    // SFX_CONNECT — success chime
    {{{800, 60}, {1000, 60}, {1200, 100}}, 3},

    // SFX_DISCONNECT — low warning
    {{{600, 100}, {400, 150}}, 2},

    // SFX_CLICK — short tick
    {{{1500, 50}}, 1},

    // SFX_ERROR — harsh low buzz
    {{{300, 100}, {0, 40}, {300, 150}}, 3},

    // SFX_BOOT — Pip-Boy startup jingle
    {{{600, 80}, {800, 80}, {1000, 80}, {1200, 80}, {1600, 150}}, 5},
};

// ── Playback state ──

static bool     s_inited   = false;
static bool     s_amp_on   = false;
static bool     s_playing  = false;
static bool     s_attached = false;

static SfxNote  s_queue[SFX_MAX_NOTES];
static uint8_t  s_queue_len  = 0;
static uint8_t  s_note_idx   = 0;
static uint32_t s_note_start_ms = 0;

// Volume: 0-100. Default is intentionally low — the CYD buzzer is PIERCING
// in a quiet room. User can crank it up from the settings screen.
static uint8_t  s_volume   = 5;
static uint8_t  s_duty     = 6;   // 128 * 5 / 100

// Click-tone preset selection (0..CLICK_TONE_COUNT-1)
static uint8_t  s_click_tone = 0;

// Alternative click sound presets — each replaces SFX_CLICK when selected.
// Kept short (<= 2 notes, <= 120ms total) so taps stay responsive.
static const SfxSequence s_click_presets[CLICK_TONE_COUNT] = {
    // 0: TICK — short high tick (original)
    {{{1500,  50}}, 1},
    // 1: BLIP — soft mid blip
    {{{ 900,  40}}, 1},
    // 2: CHIRP — rising two-tone
    {{{1200,  35}, {1800, 40}}, 2},
    // 3: THUD — low punchy tap
    {{{ 450,  60}}, 1},
};
static const char* s_click_tone_names[CLICK_TONE_COUNT] = {
    "TICK", "BLIP", "CHIRP", "THUD"
};

static void update_duty(void) {
    // Map volume 0-100 to duty 0-128 (128 = 50% on 8-bit = loudest square wave)
    s_duty = (uint8_t)((uint16_t)s_volume * 128 / 100);
}

// ── Amp + LEDC control ──

static void amp_on(void) {
    if (!s_amp_on) {
        digitalWrite(SFX_AMP_PIN, LOW);  // ACTIVE LOW
        s_amp_on = true;
    }
}

static void amp_off(void) {
    if (s_amp_on) {
        digitalWrite(SFX_AMP_PIN, HIGH);
        s_amp_on = false;
    }
}

static void ledc_start(uint16_t freq) {
    if (!s_attached) {
        ledcSetup(SFX_LEDC_CH, freq, 8);  // 8-bit resolution
        ledcAttachPin(SFX_PIN, SFX_LEDC_CH);
        s_attached = true;
    } else {
        ledcWriteTone(SFX_LEDC_CH, freq);
    }
    ledcWrite(SFX_LEDC_CH, s_duty);
}

static void ledc_silence(void) {
    if (s_attached) {
        ledcWrite(SFX_LEDC_CH, 0);  // 0 duty = silence, pin stays attached
    }
}

// Start playing the current note
static void play_note(const SfxNote& note) {
    s_note_start_ms = millis();
    if (note.freq > 0) {
        ledc_start(note.freq);
    } else {
        ledc_silence();
    }
}

// ── Public API ──

void sfx_init(void) {
    if (s_inited) return;
    pinMode(SFX_AMP_PIN, OUTPUT);
    digitalWrite(SFX_AMP_PIN, LOW);  // amp ON at startup for instant response
    s_amp_on = true;
    // Load persisted volume + click tone from NVS.
    // A schema "ver" key lets us force-reset the saved volume when the
    // default changes — older installs would otherwise keep the old 75.
    {
        const uint8_t SFX_SCHEMA_VER = 3;  // bump: lower default volume (15→5)
        Preferences p;
        p.begin("sfx", false);
        uint8_t ver = p.getUChar("ver", 0);
        if (ver != SFX_SCHEMA_VER) {
            // Migrate: drop the stored volume so the new default takes effect
            p.remove("vol");
            p.putUChar("ver", SFX_SCHEMA_VER);
            Serial.printf("[sfx] schema migrated %u -> %u (volume reset)\n",
                          ver, SFX_SCHEMA_VER);
        }
        s_volume     = (uint8_t)p.getUChar("vol", 5);
        s_click_tone = (uint8_t)p.getUChar("clk", 0);
        p.end();
        if (s_click_tone >= CLICK_TONE_COUNT) s_click_tone = 0;
    }
    update_duty();
    // Pre-attach LEDC so first sound plays instantly
    ledcSetup(SFX_LEDC_CH, 1000, 8);
    ledcAttachPin(SFX_PIN, SFX_LEDC_CH);
    ledcWrite(SFX_LEDC_CH, 0);  // silent
    s_attached = true;
    s_inited = true;
    Serial.printf("[sfx] init: vol=%u click=%s\n", s_volume, s_click_tone_names[s_click_tone]);
}

void sfx_set_volume(uint8_t vol) {
    if (vol > 100) vol = 100;
    s_volume = vol;
    update_duty();
    // Update live if currently playing
    if (s_playing && s_attached) {
        ledcWrite(SFX_LEDC_CH, s_duty);
    }
    // Persist (best-effort; Preferences is safe from any context)
    Preferences p;
    p.begin("sfx", false);
    p.putUChar("vol", s_volume);
    p.end();
}

uint8_t sfx_get_volume(void) {
    return s_volume;
}

void sfx_set_click_tone(uint8_t idx) {
    if (idx >= CLICK_TONE_COUNT) idx = 0;
    s_click_tone = idx;
    Preferences p;
    p.begin("sfx", false);
    p.putUChar("clk", s_click_tone);
    p.end();
}

uint8_t sfx_get_click_tone(void) {
    return s_click_tone;
}

const char* sfx_click_tone_name(uint8_t idx) {
    if (idx >= CLICK_TONE_COUNT) return "?";
    return s_click_tone_names[idx];
}

void sfx_play_click_preview(void) {
    if (!s_inited) sfx_init();
    const SfxSequence& seq = s_click_presets[s_click_tone];
    s_queue_len = seq.count;
    memcpy(s_queue, seq.notes, seq.count * sizeof(SfxNote));
    s_note_idx = 0;
    s_playing  = true;
    amp_on();
    play_note(s_queue[0]);
}

void sfx_play(SoundFX fx) {
    if (!s_inited) sfx_init();
    if (fx >= sizeof(s_sequences) / sizeof(s_sequences[0])) return;
    // Night quiet mode — suppress everything except nothing
    // (no exceptions right now; we could let SFX_ALARM through later).
    if (brightness_is_quiet()) return;

    // SFX_CLICK is user-selectable — swap in the current preset
    const SfxSequence& seq = (fx == SFX_CLICK)
                              ? s_click_presets[s_click_tone]
                              : s_sequences[fx];
    s_queue_len = seq.count;
    memcpy(s_queue, seq.notes, seq.count * sizeof(SfxNote));
    s_note_idx = 0;
    s_playing = true;

    amp_on();
    play_note(s_queue[0]);
}

void sfx_tone(uint16_t freq, uint16_t duration_ms) {
    if (!s_inited) sfx_init();

    s_queue[0] = {freq, duration_ms};
    s_queue_len = 1;
    s_note_idx = 0;
    s_playing = true;

    amp_on();
    play_note(s_queue[0]);
}

void sfx_stop(void) {
    ledc_silence();
    s_playing = false;
    s_queue_len = 0;
    // Keep amp on for instant tap response (draws minimal power when silent)
}

bool sfx_is_playing(void) {
    return s_playing;
}

void sfx_tick(void) {
    if (!s_playing || s_note_idx >= s_queue_len) {
        if (s_playing) sfx_stop();
        return;
    }

    const SfxNote& note = s_queue[s_note_idx];
    uint32_t elapsed = millis() - s_note_start_ms;

    // Safety watchdog: if ANY note has been playing longer than 2 seconds,
    // force-stop. This prevents stuck tones when the main loop freezes.
    if (elapsed > 2000) {
        Serial.println("[sfx] watchdog: force-stopping stuck tone");
        sfx_stop();
        return;
    }

    // Check if current note is finished
    if (elapsed >= note.duration_ms) {
        s_note_idx++;
        if (s_note_idx >= s_queue_len) {
            sfx_stop();
            return;
        }
        play_note(s_queue[s_note_idx]);
    }
}
