// ============================================================
// Freenove ESP32-S3 Display (FNK0104N) — speaker output
// See include/fn35_audio.h.
//
// ES8311 register sequence follows Espressif's es8311 driver
// (SPDX-License-Identifier: Apache-2.0, as shipped in Freenove's
// FNK0104 examples). Pins from Freenove's Sketch_07.1_Music.
// ============================================================
#if defined(BOARD_FREENOVE35)

#include <Arduino.h>
#include <Wire.h>
#include "driver/i2s.h"
#include "fn35_audio.h"

// ── Pins (FNK0104N) ──────────────────────────────────────────
#define AUD_MCLK      17
#define AUD_BCLK      18
#define AUD_WS        21
#define AUD_DOUT      15     // ESP32 -> codec (speaker)
#define AUD_AMP_EN    1      // speaker amplifier, LOW = on
#define AUD_I2C_SDA   38     // shared with the touch controller
#define AUD_I2C_SCL   39
#define ES8311_ADDR   0x18

#define AUD_PORT      I2S_NUM_0
#define AUD_RATE      16000  // plenty for beeps; MCLK = 256 x 16k = 4.096 MHz
#define AUD_FRAMES    256    // stereo frames per write

// ── Shared state (written by the UI task, read by the audio task) ──
static volatile uint16_t s_freq    = 0;
static volatile int16_t  s_target  = 0;   // target amplitude
static bool              s_ready   = false;

// ── ES8311 over Wire ─────────────────────────────────────────
static bool es_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static uint8_t es_read(uint8_t reg) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0;
    if (Wire.requestFrom((uint16_t)ES8311_ADDR, (size_t)1) != 1) return 0;
    return (uint8_t)Wire.read();
}

static bool es8311_setup(void) {
    // Reset, then power on
    if (!es_write(0x00, 0x1F)) return false;
    delay(20);
    es_write(0x00, 0x00);
    es_write(0x00, 0x80);

    // Clocks: MCLK from the MCLK pin, all internal clocks on
    es_write(0x01, 0x3F);
    es_write(0x06, es_read(0x06) & ~0x20);           // SCLK not inverted

    // Dividers for MCLK 4.096 MHz / 16 kHz
    // (pre_div 1, pre_multi 1x, adc/dac div 1, lrck 0x00FF, bclk_div 4, osr 0x10)
    es_write(0x02, es_read(0x02) & 0x07);
    es_write(0x03, 0x10);
    es_write(0x04, 0x10);
    es_write(0x05, 0x00);
    es_write(0x06, (es_read(0x06) & 0xE0) | (4 - 1));
    es_write(0x07, es_read(0x07) & 0xC0);
    es_write(0x08, 0xFF);

    // Slave mode, I2S format, 16-bit in and out
    es_write(0x00, es_read(0x00) & 0xBF);
    es_write(0x09, 0x0C);
    es_write(0x0A, 0x0C);

    // Power up analogue, DAC and output driver
    es_write(0x0D, 0x01);
    es_write(0x0E, 0x02);
    es_write(0x12, 0x00);
    es_write(0x13, 0x10);
    es_write(0x1C, 0x6A);
    es_write(0x37, 0x08);                            // bypass DAC EQ

    es_write(0x32, 0xBF);                            // DAC volume 0 dB (loudness set in software)
    es_write(0x31, es_read(0x31) & ~0x60);           // unmute
    return true;
}

// ── Tone generator task ──────────────────────────────────────
static void audio_task(void* arg) {
    (void)arg;
    static int16_t buf[AUD_FRAMES * 2];
    uint32_t phase = 0;
    int32_t  amp   = 0;                              // current amplitude (ramps to target)
    for (;;) {
        const uint16_t freq   = s_freq;
        const int32_t  target = (freq == 0) ? 0 : s_target;
        const uint32_t inc    = (uint32_t)(((uint64_t)freq << 32) / AUD_RATE);
        for (int i = 0; i < AUD_FRAMES; i++) {
            // Short ramp avoids clicks/pops at note starts and stops
            if (amp < target)      amp += 64;
            else if (amp > target) amp -= 64;
            if ((amp - target) < 64 && (target - amp) < 64) amp = target;
            int16_t v = 0;
            if (amp != 0) {
                phase += inc;
                v = (phase & 0x80000000u) ? (int16_t)amp : (int16_t)-amp;   // square wave, like the CYD buzzer
            }
            buf[2 * i]     = v;
            buf[2 * i + 1] = v;
        }
        size_t written = 0;
        i2s_write(AUD_PORT, buf, sizeof(buf), &written, portMAX_DELAY);
    }
}

// ── Public API ───────────────────────────────────────────────

void fn35_audio_amp(bool on) {
    digitalWrite(AUD_AMP_EN, on ? LOW : HIGH);
}

void fn35_audio_tone(uint16_t freq, uint8_t volume) {
    if (volume > 100) volume = 100;
    s_target = (int16_t)(volume * 300);              // 100 -> 30000 of 32767
    s_freq   = freq;
}

bool fn35_audio_init(void) {
    if (s_ready) return true;

    pinMode(AUD_AMP_EN, OUTPUT);
    digitalWrite(AUD_AMP_EN, HIGH);                  // amp off while we set up

    i2s_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate          = AUD_RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.dma_buf_count        = 4;
    cfg.dma_buf_len          = AUD_FRAMES;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = true;
    cfg.mclk_multiple        = I2S_MCLK_MULTIPLE_256;
    cfg.bits_per_chan        = I2S_BITS_PER_CHAN_16BIT;
    if (i2s_driver_install(AUD_PORT, &cfg, 0, NULL) != ESP_OK) {
        Serial.println("[audio] I2S install failed");
        return false;
    }
    i2s_pin_config_t pins;
    memset(&pins, 0, sizeof(pins));
    pins.mck_io_num   = AUD_MCLK;
    pins.bck_io_num   = AUD_BCLK;
    pins.ws_io_num    = AUD_WS;
    pins.data_out_num = AUD_DOUT;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;
    if (i2s_set_pin(AUD_PORT, &pins) != ESP_OK) {
        Serial.println("[audio] I2S pin setup failed");
        return false;
    }
    i2s_zero_dma_buffer(AUD_PORT);

    // The touch driver normally starts Wire first; begin() again is harmless.
    Wire.begin(AUD_I2C_SDA, AUD_I2C_SCL);
    if (!es8311_setup()) {
        Serial.println("[audio] ES8311 codec not responding at 0x18");
        return false;
    }

    xTaskCreatePinnedToCore(audio_task, "audio", 3072, NULL, 5, NULL, 0);
    fn35_audio_amp(true);
    s_ready = true;
    Serial.println("[audio] speaker ready");
    return true;
}

#endif  // BOARD_FREENOVE35
