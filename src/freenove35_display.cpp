// ============================================================
// Freenove ESP32-S3 Display 3.5" (FNK0104N) — display + touch driver
// See include/freenove35_display.h for what this does and the options.
//
// The ST77922 init sequence, QSPI command format and touch register map
// come from Freenove's FNK0104 example code
// (github.com/Freenove/Freenove_ESP32_S3_Display, CC BY-NC-SA 3.0).
// ============================================================
#if defined(BOARD_FREENOVE35)

#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include "driver/spi_master.h"
#include "freenove35_display.h"

// ── Panel geometry ───────────────────────────────────────────
// The glass is portrait (320 wide, 480 tall). We draw landscape.
#define PANEL_W   320
#define PANEL_H   480
#define LAND_W    480       // landscape width  (= PANEL_H)
#define LAND_H    320       // landscape height (= PANEL_W)
#define UI_W      320       // LVGL / Pip-Boy UI resolution
#define UI_H      240

// UI -> landscape scaling. Vertical is always 240 -> 320 (x4/3).
#define SY_NUM    4
#define SY_DEN    3
#if FN35_FILL
  #define SX_NUM  3         // 320 -> 480 (x3/2): fills the panel
  #define SX_DEN  2
  #define ACT_W   480
#else
  #define SX_NUM  4         // 320 -> 427 (x4/3): same shape as the CYD
  #define SX_DEN  3
  #define ACT_W   427
#endif
#define OFF_X     ((LAND_W - ACT_W) / 2)

// ── ST77922 over QSPI (pins from the FNK0104N schematic) ─────
#define LCD_CS      10
#define QSPI_SCLK   12
#define QSPI_D0     11
#define QSPI_D1     13
#define QSPI_D2     14
#define QSPI_D3     9
#define QSPI_HOST   SPI2_HOST
#define QSPI_FREQ   80000000
#define QSPI_1W_CMD 0x02
#define QSPI_4W_CMD 0x32
#define WR_RAM_CONT 0x3C
#define CMD_CASET   0x2A
#define CMD_RASET   0x2B

// Pixels pushed per SPI transaction (internal DMA RAM: 2 bytes each)
#define PUSH_BUF_PX 8192

// ── Touch controller (I2C, shares the bus with the audio codec) ──
#define TOUCH_ADDR   0x55
#define TOUCH_SDA    38
#define TOUCH_SCL    39
#define TOUCH_RST    48
#define TOUCH_INT    47
#define TREG_STATUS  0x0001
#define TREG_INFO    0x0010
#define TREG_POINT0  0x0014
#define TOUCH_POLL_MS     15   // don't hammer I2C every loop

// Init sequence as a flat byte stream: cmd, len, delay/10ms, data...
static const uint8_t k_init[] = {
    0xF1, 1, 0, 0x00,
    0x60, 3, 0, 0x00, 0x00, 0x00,
    0x65, 1, 0, 0x80,
    0x79, 1, 0, 0x06,
    0x7B, 3, 0, 0x00, 0x08, 0x08,
    0x80, 11, 0, 0x55, 0x62, 0x2F, 0x17, 0xF0, 0x52, 0x70, 0xD2, 0x52, 0x62, 0xEA,
    0x81, 4, 0, 0x26, 0x52, 0x72, 0x27,
    0x84, 2, 0, 0x92, 0x25,
    0x87, 6, 0, 0x10, 0x10, 0x58, 0x00, 0x02, 0x3A,
    0x88, 15, 0, 0x00, 0x00, 0x2C, 0x10, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x06,
    0x89, 3, 0, 0x00, 0x00, 0x00,
    0x8A, 11, 0, 0x13, 0x00, 0x2C, 0x00, 0x00, 0x2C, 0x10, 0x10, 0x00, 0x3E, 0x19,
    0x8B, 9, 0, 0x15, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x97, 0x8E,
    0x8C, 13, 0, 0x1D, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x50, 0x0F, 0x01, 0xC5, 0x12, 0x09,
    0x8D, 1, 0, 0x0C,
    0x8E, 6, 0, 0x33, 0x01, 0x0C, 0x13, 0x01, 0x01,
    0xB3, 2, 0, 0x00, 0x30,
    0xF1, 1, 0, 0x00,
    0x71, 1, 0, 0xD0,
    0x66, 2, 0, 0x02, 0x3F,
    0xBE, 3, 0, 0x26, 0x00, 0x9D,
    0x70, 12, 0, 0x01, 0xA0, 0x11, 0x40, 0xE0, 0x00, 0x11, 0x69, 0x11, 0x00, 0x00, 0x1A,
    0x90, 9, 0, 0x04, 0x04, 0x55, 0x74, 0x00, 0x40, 0x43, 0x27, 0x27,
    0x91, 9, 0, 0x04, 0x04, 0x55, 0x75, 0x00, 0x40, 0x42, 0x27, 0x27,
    0x92, 10, 0, 0x04, 0x44, 0x55, 0xC0, 0x06, 0x00, 0x07, 0x05, 0x90, 0x27,
    0x93, 10, 0, 0x04, 0x43, 0x11, 0x00, 0x00, 0x00, 0x00, 0x05, 0x90, 0x27,
    0x94, 6, 0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x95, 5, 0, 0x96, 0x16, 0x00, 0x00, 0xFF,
    0x96, 12, 0, 0x44, 0x53, 0x03, 0x12, 0x23, 0x24, 0x06, 0x05, 0x94, 0x27, 0x00, 0x44,
    0x97, 12, 0, 0x44, 0x53, 0x47, 0x56, 0x20, 0x20, 0x02, 0x01, 0x94, 0x27, 0x00, 0x44,
    0xBA, 5, 0, 0x55, 0x94, 0x2D, 0x94, 0x27,
    0x9A, 7, 0, 0x40, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00,
    0x9B, 7, 0, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00,
    0x9C, 13, 0, 0x5C, 0x12, 0x00, 0x00, 0x10, 0x12, 0x00, 0x00, 0x10, 0x02, 0x00, 0x00, 0x00,
    0x9D, 8, 0, 0x8A, 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01,
    0x9E, 7, 0, 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01,
    0xB4, 12, 0, 0x1D, 0x1C, 0x1E, 0x0B, 0x14, 0x02, 0x13, 0x09, 0x1E, 0x00, 0x1E, 0x10,
    0xB5, 12, 0, 0x1D, 0x1C, 0x1E, 0x0A, 0x15, 0x03, 0x11, 0x08, 0x1E, 0x01, 0x1E, 0x12,
    0xB6, 7, 0, 0x77, 0x77, 0x00, 0x0A, 0xFF, 0x0A, 0xFF,
    0x86, 14, 0, 0xCD, 0x04, 0xB1, 0x02, 0x58, 0x12, 0x58, 0x0C, 0x13, 0x01, 0xA5, 0x00, 0xA5, 0xA5,
    0xB7, 16, 0, 0x07, 0x0A, 0x0E, 0x06, 0x05, 0x03, 0x2B, 0x03, 0x03, 0x42, 0x07, 0x10, 0x10, 0x2E, 0x3F, 0x0D,
    0xB8, 16, 0, 0x07, 0x0A, 0x0D, 0x05, 0x05, 0x02, 0x2B, 0x02, 0x03, 0x42, 0x06, 0x10, 0x0F, 0x2E, 0x3F, 0x0D,
    0xB9, 2, 0, 0x23, 0x23,
    0xBF, 6, 0, 0x10, 0x14, 0x14, 0x0B, 0x0B, 0x0B,
    0xF2, 1, 0, 0x00,
    0x73, 5, 0, 0x04, 0xDA, 0x12, 0x54, 0x47,
    0x77, 5, 0, 0x6B, 0x5B, 0xFD, 0xC3, 0xC5,
    0x7A, 2, 0, 0x15, 0x27,
    0x7B, 2, 0, 0x04, 0x57,
    0x7E, 2, 0, 0x01, 0x0E,
    0xBF, 1, 0, 0x36,
    0xE3, 2, 0, 0x40, 0x40,
    0xF0, 1, 0, 0x00,
    0xD0, 1, 0, 0x00,
    0x2A, 4, 0, 0x00, 0x00, 0x01, 0x3F,
    0x2B, 4, 0, 0x00, 0x00, 0x01, 0xDF,
    0x21, 0, 0,
    0x11, 0, 12,
    0x29, 0, 0,
    0x2C, 0, 0,
    0x3A, 1, 0, 0x01,
    0x36, 1, 0, 0x00,
    0x35, 1, 2, 0x01,
};

static spi_device_handle_t s_qspi     = nullptr;
static uint16_t*           s_fb       = nullptr;   // PANEL_W x PANEL_H, panel byte order
static uint16_t*           s_push     = nullptr;   // DMA bounce buffer
static bool                s_ready    = false;

// ── Low-level QSPI ───────────────────────────────────────────

static void lcd_cmd(uint8_t cmd, const uint8_t* data, uint8_t len) {
    spi_transaction_ext_t t;
    memset(&t, 0, sizeof(t));
    t.base.flags     = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR;
    t.base.cmd       = QSPI_1W_CMD;
    t.base.addr      = (uint32_t)cmd << 8;
    t.command_bits   = 8;
    t.address_bits   = 24;
    t.base.tx_buffer = len ? data : nullptr;
    t.base.length    = 8 * len;
    spi_device_polling_transmit(s_qspi, (spi_transaction_t*)&t);
}

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    // inclusive coordinates in panel (portrait) space
    const uint8_t xd[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
    const uint8_t yd[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
    lcd_cmd(CMD_CASET, xd, 4);
    lcd_cmd(CMD_RASET, yd, 4);
}

// Push a rectangle of the framebuffer (panel coordinates, inclusive).
// The ST77922 wants windows aligned to 4 pixels; callers guarantee that.
static void lcd_push_rect(int x0, int y0, int x1, int y1) {
    const int pw = x1 - x0 + 1;
    int rows_per_band = PUSH_BUF_PX / pw;
    if (rows_per_band < 1) rows_per_band = 1;

    lcd_set_window(x0, y0, x1, y1);

    bool first = true;
    for (int y = y0; y <= y1; y += rows_per_band) {
        int rows = y1 - y + 1;
        if (rows > rows_per_band) rows = rows_per_band;
        for (int r = 0; r < rows; r++) {
            memcpy(s_push + r * pw, s_fb + (y + r) * PANEL_W + x0, pw * sizeof(uint16_t));
        }
        spi_transaction_ext_t t;
        memset(&t, 0, sizeof(t));
        t.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR;
        if (!first) t.base.flags |= SPI_TRANS_VARIABLE_DUMMY;
        t.base.cmd       = QSPI_4W_CMD;
        t.base.addr      = (uint32_t)WR_RAM_CONT << 8;
        t.command_bits   = 8;
        t.address_bits   = 24;
        t.base.tx_buffer = s_push;
        t.base.length    = (size_t)rows * pw * 16;
        spi_device_polling_transmit(s_qspi, (spi_transaction_t*)&t);
        first = false;
    }
}

// ── Coordinate mapping ───────────────────────────────────────
// landscape (X,Y) -> framebuffer index in panel (portrait) space
static inline int land_to_fb(int X, int Y) {
#if FN35_ROTATION == 3
    return (PANEL_H - 1 - X) * PANEL_W + Y;
#else
    return X * PANEL_W + (PANEL_W - 1 - Y);
#endif
}

static inline int ceil_div(int a, int b) { return (a + b - 1) / b; }

// ── Public API ───────────────────────────────────────────────

static bool touch_init(void);
static void panel_init_sequence(void);

bool fn35_init(void) {
    s_fb   = (uint16_t*)heap_caps_calloc(PANEL_W * PANEL_H, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_push = (uint16_t*)heap_caps_malloc(PUSH_BUF_PX * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_fb || !s_push) {
        Serial.println("[fn35] out of memory — is PSRAM enabled for this build?");
        return false;
    }

    spi_bus_config_t bus;
    memset(&bus, 0, sizeof(bus));
    bus.data0_io_num    = QSPI_D0;
    bus.data1_io_num    = QSPI_D1;
    bus.sclk_io_num     = QSPI_SCLK;
    bus.data2_io_num    = QSPI_D2;
    bus.data3_io_num    = QSPI_D3;
    bus.data4_io_num    = -1;
    bus.data5_io_num    = -1;
    bus.data6_io_num    = -1;
    bus.data7_io_num    = -1;
    bus.max_transfer_sz = PUSH_BUF_PX * 2 + 8;
    bus.flags           = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_IOMUX_PINS | SPICOMMON_BUSFLAG_QUAD;
    if (spi_bus_initialize(QSPI_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        Serial.println("[fn35] QSPI bus init failed");
        return false;
    }

    spi_device_interface_config_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.mode           = 0;
    dev.clock_speed_hz = QSPI_FREQ;
    dev.spics_io_num   = LCD_CS;
    dev.flags          = SPI_DEVICE_HALFDUPLEX;
    dev.queue_size     = 4;
    if (spi_bus_add_device(QSPI_HOST, &dev, &s_qspi) != ESP_OK) {
        Serial.println("[fn35] QSPI device add failed");
        return false;
    }

    panel_init_sequence();

    s_ready = true;
    lcd_push_rect(0, 0, PANEL_W - 1, PANEL_H - 1);   // clear to black

    // Start touch. Its reset line may also reset the display on this
    // TDDI panel, so set the panel up again afterwards and redraw.
    if (!touch_init()) Serial.println("[fn35] touch controller not responding");
    panel_init_sequence();
    lcd_push_rect(0, 0, PANEL_W - 1, PANEL_H - 1);

    Serial.printf("[fn35] panel ready, rotation %d, %s\n", FN35_ROTATION,
                  FN35_FILL ? "fill 480x320" : "keep aspect 427x320");
    return true;
}

static void panel_init_sequence(void) {
    // Freenove's examples run the init sequence twice; do the same.
    for (int pass = 0; pass < 2; pass++) {
        size_t i = 0;
        while (i < sizeof(k_init)) {
            const uint8_t cmd = k_init[i], len = k_init[i + 1], dly = k_init[i + 2];
            lcd_cmd(cmd, len ? &k_init[i + 3] : nullptr, len);
            if (dly) delay(dly * 10);
            i += 3 + len;
        }
    }
}

void fn35_flush(const lv_area_t* area, const lv_color_t* color_p) {
    if (!s_ready) return;
    const int x1 = area->x1, y1 = area->y1, x2 = area->x2, y2 = area->y2;
    const int w  = x2 - x1 + 1;

    // Landscape pixels covered by this UI area (end exclusive).
    const int X0 = OFF_X + ceil_div(x1 * SX_NUM, SX_DEN);
    const int X1 = OFF_X + ceil_div((x2 + 1) * SX_NUM, SX_DEN);
    const int Y0 = ceil_div(y1 * SY_NUM, SY_DEN);
    const int Y1 = ceil_div((y2 + 1) * SY_NUM, SY_DEN);

    // Nearest-neighbour scale into the framebuffer, swapping to panel byte order.
    for (int Y = Y0; Y < Y1; Y++) {
        const lv_color_t* row = color_p + (size_t)((Y * SY_DEN) / SY_NUM - y1) * w;
        for (int X = X0; X < X1; X++) {
            const int lx = ((X - OFF_X) * SX_DEN) / SX_NUM;
            s_fb[land_to_fb(X, Y)] = __builtin_bswap16(row[lx - x1].full);
        }
    }

    // Same rectangle in panel space, widened to 4-pixel alignment.
#if FN35_ROTATION == 3
    int px0 = Y0,                 px1 = Y1 - 1;
    int py0 = PANEL_H - X1,       py1 = PANEL_H - 1 - X0;
#else
    int px0 = PANEL_W - Y1,       px1 = PANEL_W - 1 - Y0;
    int py0 = X0,                 py1 = X1 - 1;
#endif
    px0 &= ~3; py0 &= ~3;
    px1 |= 3;  py1 |= 3;
    lcd_push_rect(px0, py0, px1, py1);
}

// ── Touch ────────────────────────────────────────────────────

// Behaviour of this touch controller, as found when bringing up the same
// board in the SquachWatch-CYD project (github.com/skizzophrenic/SquachWatch-CYD):
//   * the "new report" flag (TOUCH_INFO bit 3) only clears once the WHOLE
//     report — 7 bytes for every supported point — has been read. Reading
//     just the first point leaves the first tap stuck "down" forever.
//   * point reads are refused for roughly 1-2 s after reset while it settles.
//   * between reports the finger is where the last one put it; when reports
//     stop arriving the finger has lifted.

#if FN35_TOUCH_DEBUG
  #define TDBG(...) Serial.printf(__VA_ARGS__)
#else
  #define TDBG(...) ((void)0)
#endif

#define TREG_FW_VER      0x0000
#define TREG_MAX_TOUCH   0x0009
#define TOUCH_MAX_POINTS 10
#define TOUCH_GONE_MS    100   // no report for this long = finger lifted
#define TOUCH_BACKOFF_MS 50    // after a refused read, leave the chip alone
#define TOUCH_WARMUP_MS  3000  // refused reads this soon after boot are expected

static bool     s_touch_ok      = false;   // controller answered on I2C
static uint8_t  s_max_points    = 1;
static bool     s_touch_down    = false;
static int      s_touch_x       = 0;
static int      s_touch_y       = 0;
static uint32_t s_last_report   = 0;
static uint32_t s_next_poll     = 0;
static uint32_t s_last_retry    = 0;
static uint32_t s_touch_boot    = 0;
static uint32_t s_read_errors   = 0;
static uint32_t s_last_err_log  = 0;
static uint32_t s_taps          = 0;
static uint32_t s_reports       = 0;
static uint8_t  s_fw[2]         = {0, 0};
static int      s_raw_x         = -1;
static int      s_raw_y         = -1;

static bool touch_read_reg(uint16_t reg, uint8_t* buf, size_t len) {
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((uint16_t)TOUCH_ADDR, (size_t)len) != len) return false;
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)Wire.read();
    return true;
}

static bool touch_probe(void) {
    Wire.beginTransmission(TOUCH_ADDR);
    return Wire.endTransmission() == 0;
}

static void i2c_scan(void) {
    Serial.print("[touch] I2C devices found:");
    int n = 0;
    for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", a); n++; }
    }
    Serial.println(n ? "" : " none");
}

static bool touch_init(void) {
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    Wire.setClock(100000);
    pinMode(TOUCH_INT, INPUT);
    pinMode(TOUCH_RST, OUTPUT);
    digitalWrite(TOUCH_RST, HIGH);
    delay(10);
    digitalWrite(TOUCH_RST, LOW);
    delay(100);
    digitalWrite(TOUCH_RST, HIGH);
    delay(100);
    s_touch_boot = millis();

    // Wait (bounded) for the status register's low nibble to clear.
    uint8_t status = 0xFF;
    for (int i = 0; i < 50; i++) {
        if (touch_read_reg(TREG_STATUS, &status, 1) && (status & 0x0F) == 0) break;
        delay(10);
    }
    uint8_t* fw   = s_fw;
    uint8_t  maxp = 0;
    const bool id_ok = touch_read_reg(TREG_FW_VER, fw, 2) && touch_read_reg(TREG_MAX_TOUCH, &maxp, 1);
    s_max_points = (maxp >= 1 && maxp <= TOUCH_MAX_POINTS) ? maxp : TOUCH_MAX_POINTS;
    s_touch_ok   = id_ok || touch_probe();

    if (s_touch_ok) {
        Serial.printf("[touch] controller found: fw %u.%u, %u points (reported %u), status 0x%02X\n",
                      fw[0], fw[1], s_max_points, maxp, status);
    } else {
        Serial.println("[touch] controller NOT FOUND at 0x55");
        i2c_scan();
    }
    return s_touch_ok;
}

bool fn35_touch_read(int* x, int* y) {
    const uint32_t now = millis();

    // If the controller wasn't found at boot, keep looking once a second.
    if (!s_touch_ok) {
        if (now - s_last_retry >= 1000) {
            s_last_retry = now;
            if (touch_probe()) {
                s_touch_ok = true;
                Serial.println("[touch] controller appeared — touch enabled");
            }
        }
        if (!s_touch_ok) return false;
    }

    if ((int32_t)(now - s_next_poll) >= 0) {
        s_next_poll = now + TOUCH_POLL_MS;
        uint8_t info = 0;
        if (!touch_read_reg(TREG_INFO, &info, 1)) {
            s_next_poll = now + TOUCH_BACKOFF_MS;
            s_read_errors++;
            if (now - s_touch_boot > TOUCH_WARMUP_MS && now - s_last_err_log > 2000) {
                s_last_err_log = now;
                TDBG("[touch] read refused (%lu so far)\n", (unsigned long)s_read_errors);
            }
        } else if (info & 0x08) {
            // Read the entire report so the chip clears its new-report flag.
            uint8_t rep[7 * TOUCH_MAX_POINTS];
            if (!touch_read_reg(TREG_POINT0, rep, (size_t)7 * s_max_points)) {
                s_next_poll = now + TOUCH_BACKOFF_MS;
                s_read_errors++;
            } else {
                s_last_report = now;
                s_reports++;
                const bool was_down = s_touch_down;
                const int rx = ((rep[0] & 0x3F) << 8) | rep[1];   // 0..319, panel short side
                const int ry = ((rep[2] & 0x3F) << 8) | rep[3];   // 0..479, panel long side
                s_touch_down = (rep[0] & 0x80) && rx < PANEL_W && ry < PANEL_H;
                if (s_touch_down) {
#if FN35_ROTATION == 3
                    const int X = PANEL_H - 1 - ry, Y = rx;
#else
                    const int X = ry,               Y = PANEL_W - 1 - rx;
#endif
                    s_touch_x = constrain(((X - OFF_X) * SX_DEN) / SX_NUM, 0, UI_W - 1);
                    s_touch_y = constrain((Y * SY_DEN) / SY_NUM,          0, UI_H - 1);
                    s_raw_x = rx;
                    s_raw_y = ry;
                    if (!was_down) {
                        s_taps++;
                        TDBG("[touch] down raw=(%d,%d) ui=(%d,%d)\n", rx, ry, s_touch_x, s_touch_y);
                    }
                } else if (was_down) {
                    TDBG("[touch] up\n");
                }
            }
        } else if (s_touch_down && now - s_last_report > TOUCH_GONE_MS) {
            s_touch_down = false;   // reports stopped — finger lifted
            TDBG("[touch] up (reports stopped)\n");
        }
    }
    if (s_touch_down) {
        *x = s_touch_x;
        *y = s_touch_y;
    }
    return s_touch_down;
}

void fn35_touch_status(char* buf, size_t len) {
    if (!s_touch_ok) {
        snprintf(buf, len, "TOUCH: NOT FOUND @0x55  (retrying)");
        return;
    }
    snprintf(buf, len, "TOUCH fw%u.%u pts%u | %s raw %d,%d | taps %lu rep %lu err %lu",
             s_fw[0], s_fw[1], s_max_points,
             s_touch_down ? "DOWN" : "up", s_raw_x, s_raw_y,
             (unsigned long)s_taps, (unsigned long)s_reports, (unsigned long)s_read_errors);
}

#endif  // BOARD_FREENOVE35
