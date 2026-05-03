#include "sdkconfig.h"
#include "display.h"

#ifdef CONFIG_OLED_ENABLED

#include "wifi.h"
#include "stream_stats.h"
#include "tasks.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"

#include <string.h>
#include <stdio.h>

#include "gnss.h"

static const char *TAG = "DISPLAY";

// ── SSD1306 constants ─────────────────────────────────────────────────────────
#define OLED_I2C_ADDR   0x3C
#define OLED_WIDTH      128
#define OLED_PAGES      4        // 32px tall / 8px per page

// ── Font geometry ─────────────────────────────────────────────────────────────
#define FONT_WIDTH      5
#define GLYPH_WIDTH     6        // 5px glyph + 1px spacing
#define COLS            (OLED_WIDTH / GLYPH_WIDTH)  // 21 chars per row

// ── I2C handles ───────────────────────────────────────────────────────────────
static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t i2c_dev;

// ── Framebuffer and transmit buffer ──────────────────────────────────────────
static uint8_t fb[OLED_PAGES][OLED_WIDTH];
static uint8_t tx_buf[1 + OLED_PAGES * OLED_WIDTH];  // 0x40 + 512 data bytes

// ── 5×8 font, ASCII 0x20–0x7E ────────────────────────────────────────────────
// Each entry: 5 column bytes, bit0 = topmost pixel
static const uint8_t font5x8[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // 0x20 ' '
    {0x00,0x00,0x5F,0x00,0x00}, // 0x21 '!'
    {0x00,0x07,0x00,0x07,0x00}, // 0x22 '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // 0x23 '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // 0x24 '$'
    {0x23,0x13,0x08,0x64,0x62}, // 0x25 '%'
    {0x36,0x49,0x55,0x22,0x50}, // 0x26 '&'
    {0x00,0x05,0x03,0x00,0x00}, // 0x27 '\''
    {0x00,0x1C,0x22,0x41,0x00}, // 0x28 '('
    {0x00,0x41,0x22,0x1C,0x00}, // 0x29 ')'
    {0x14,0x08,0x3E,0x08,0x14}, // 0x2A '*'
    {0x08,0x08,0x3E,0x08,0x08}, // 0x2B '+'
    {0x00,0x50,0x30,0x00,0x00}, // 0x2C ','
    {0x08,0x08,0x08,0x08,0x08}, // 0x2D '-'
    {0x00,0x60,0x60,0x00,0x00}, // 0x2E '.'
    {0x20,0x10,0x08,0x04,0x02}, // 0x2F '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // 0x30 '0'
    {0x00,0x42,0x7F,0x40,0x00}, // 0x31 '1'
    {0x42,0x61,0x51,0x49,0x46}, // 0x32 '2'
    {0x21,0x41,0x45,0x4B,0x31}, // 0x33 '3'
    {0x18,0x14,0x12,0x7F,0x10}, // 0x34 '4'
    {0x27,0x45,0x45,0x45,0x39}, // 0x35 '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // 0x36 '6'
    {0x01,0x71,0x09,0x05,0x03}, // 0x37 '7'
    {0x36,0x49,0x49,0x49,0x36}, // 0x38 '8'
    {0x06,0x49,0x49,0x29,0x1E}, // 0x39 '9'
    {0x00,0x36,0x36,0x00,0x00}, // 0x3A ':'
    {0x00,0x56,0x36,0x00,0x00}, // 0x3B ';'
    {0x08,0x14,0x22,0x41,0x00}, // 0x3C '<'
    {0x14,0x14,0x14,0x14,0x14}, // 0x3D '='
    {0x00,0x41,0x22,0x14,0x08}, // 0x3E '>'
    {0x02,0x01,0x51,0x09,0x06}, // 0x3F '?'
    {0x32,0x49,0x79,0x41,0x3E}, // 0x40 '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 0x41 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 0x42 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 0x43 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 0x44 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 0x45 'E'
    {0x7F,0x09,0x09,0x09,0x01}, // 0x46 'F'
    {0x3E,0x41,0x49,0x49,0x7A}, // 0x47 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 0x48 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 0x49 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 0x4A 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 0x4B 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 0x4C 'L'
    {0x7F,0x02,0x0C,0x02,0x7F}, // 0x4D 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 0x4E 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 0x4F 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 0x50 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 0x51 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 0x52 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 0x53 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 0x54 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 0x55 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 0x56 'V'
    {0x3F,0x40,0x38,0x40,0x3F}, // 0x57 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 0x58 'X'
    {0x07,0x08,0x70,0x08,0x07}, // 0x59 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 0x5A 'Z'
    {0x00,0x7F,0x41,0x41,0x00}, // 0x5B '['
    {0x02,0x04,0x08,0x10,0x20}, // 0x5C '\'
    {0x00,0x41,0x41,0x7F,0x00}, // 0x5D ']'
    {0x04,0x02,0x01,0x02,0x04}, // 0x5E '^'
    {0x40,0x40,0x40,0x40,0x40}, // 0x5F '_'
    {0x00,0x01,0x02,0x04,0x00}, // 0x60 '`'
    {0x20,0x54,0x54,0x54,0x78}, // 0x61 'a'
    {0x7F,0x48,0x44,0x44,0x38}, // 0x62 'b'
    {0x38,0x44,0x44,0x44,0x20}, // 0x63 'c'
    {0x38,0x44,0x44,0x48,0x7F}, // 0x64 'd'
    {0x38,0x54,0x54,0x54,0x18}, // 0x65 'e'
    {0x08,0x7E,0x09,0x01,0x02}, // 0x66 'f'
    {0x0C,0x52,0x52,0x52,0x3E}, // 0x67 'g'
    {0x7F,0x08,0x04,0x04,0x78}, // 0x68 'h'
    {0x00,0x44,0x7D,0x40,0x00}, // 0x69 'i'
    {0x20,0x40,0x44,0x3D,0x00}, // 0x6A 'j'
    {0x7F,0x10,0x28,0x44,0x00}, // 0x6B 'k'
    {0x00,0x41,0x7F,0x40,0x00}, // 0x6C 'l'
    {0x7C,0x04,0x18,0x04,0x78}, // 0x6D 'm'
    {0x7C,0x08,0x04,0x04,0x78}, // 0x6E 'n'
    {0x38,0x44,0x44,0x44,0x38}, // 0x6F 'o'
    {0x7C,0x14,0x14,0x14,0x08}, // 0x70 'p'
    {0x08,0x14,0x14,0x18,0x7C}, // 0x71 'q'
    {0x7C,0x08,0x04,0x04,0x08}, // 0x72 'r'
    {0x48,0x54,0x54,0x54,0x20}, // 0x73 's'
    {0x04,0x3F,0x44,0x40,0x20}, // 0x74 't'
    {0x3C,0x40,0x40,0x20,0x7C}, // 0x75 'u'
    {0x1C,0x20,0x40,0x20,0x1C}, // 0x76 'v'
    {0x3C,0x40,0x30,0x40,0x3C}, // 0x77 'w'
    {0x44,0x28,0x10,0x28,0x44}, // 0x78 'x'
    {0x0C,0x50,0x50,0x50,0x3C}, // 0x79 'y'
    {0x44,0x64,0x54,0x4C,0x44}, // 0x7A 'z'
    {0x00,0x08,0x36,0x41,0x00}, // 0x7B '{'
    {0x00,0x00,0x7F,0x00,0x00}, // 0x7C '|'
    {0x00,0x41,0x36,0x08,0x00}, // 0x7D '}'
    {0x10,0x08,0x08,0x10,0x08}, // 0x7E '~'
};

// ── Framebuffer helpers ───────────────────────────────────────────────────────

static void fb_clear(void) {
    memset(fb, 0, sizeof(fb));
}

static void fb_putchar(int row, int col, char c) {
    if (c < 0x20 || c > 0x7E) c = ' ';
    int x = col * GLYPH_WIDTH;
    if (x + FONT_WIDTH > OLED_WIDTH) return;
    const uint8_t *g = font5x8[(uint8_t)c - 0x20];
    for (int i = 0; i < FONT_WIDTH; i++) fb[row][x + i] = g[i];
    // spacing column already 0 from fb_clear
}

static void fb_puts(int row, int col, const char *s) {
    while (*s && col < COLS) fb_putchar(row, col++, *s++);
}

// Write a fixed-width field: print str left-aligned, pad to width with spaces
static void fb_puts_padded(int row, int col, int width, const char *s) {
    int start = col;
    while (*s && col < start + width && col < COLS) fb_putchar(row, col++, *s++);
    while (col < start + width && col < COLS) fb_putchar(row, col++, ' ');
}

// ── SSD1306 driver ────────────────────────────────────────────────────────────

static esp_err_t oled_write_cmds(const uint8_t *cmds, size_t len) {
    // Prepend control byte 0x00 (command stream) inline using a small stack buffer.
    // Largest call below is 20 bytes; 32-byte stack buffer is safe.
    uint8_t buf[32];
    if (len + 1 > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    buf[0] = 0x00;
    memcpy(buf + 1, cmds, len);
    return i2c_master_transmit(i2c_dev, buf, len + 1, -1);
}

static esp_err_t oled_init(void) {
    const uint8_t seq[] = {
        0xAE,       // display off
        0xD5, 0x80, // clock divide ratio / osc freq
        0xA8, 0x1F, // multiplex ratio = 32 (0x1F = 31)
        0xD3, 0x00, // display offset = 0
        0x40,       // display start line = 0
        0x8D, 0x14, // charge pump: enabled
        0x20, 0x00, // memory addressing: horizontal
        0xA1,       // segment remap: col127 = SEG0
        0xC8,       // COM scan: remapped (COM[N-1]→COM0), gives upright image
        0xDA, 0x02, // COM pins: sequential, no left/right remap (required for 32px)
        0x81, 0x8F, // contrast
        0xD9, 0xF1, // pre-charge period
        0xDB, 0x40, // VCOMH deselect level
        0xA4,       // display from RAM
        0xA6,       // normal polarity (not inverted)
        0xAF,       // display on
    };
    return oled_write_cmds(seq, sizeof(seq));
}

static esp_err_t oled_flush(void) {
    // Set full column + page range so horizontal addressing wraps correctly
    const uint8_t addr[] = { 0x21, 0x00, 127, 0x22, 0x00, OLED_PAGES - 1 };
    esp_err_t err = oled_write_cmds(addr, sizeof(addr));
    if (err != ESP_OK) return err;

    // Send entire framebuffer in one transaction
    tx_buf[0] = 0x40;  // data stream control byte
    for (int p = 0; p < OLED_PAGES; p++)
        memcpy(tx_buf + 1 + p * OLED_WIDTH, fb[p], OLED_WIDTH);
    return i2c_master_transmit(i2c_dev, tx_buf, sizeof(tx_buf), -1);
}

// ── Status formatting helpers ─────────────────────────────────────────────────

// Format a byte rate into at most 8 chars: "1234B/s" or "12.3kB/s"
static void fmt_rate(char *buf, size_t len, uint32_t bps) {
    if (bps < 1000) {
        snprintf(buf, len, "%uB/s", (unsigned)bps);
    } else {
        snprintf(buf, len, "%u.%ukB/s", (unsigned)(bps / 1000), (unsigned)((bps % 1000) / 100));
    }
}

// Format uptime seconds: "45s", "12m34s", "3h14m", "2d14h"
static void fmt_uptime(char *buf, size_t len, uint32_t s) {
    uint32_t d = s / 86400, h = (s % 86400) / 3600;
    uint32_t m = (s % 3600) / 60, sec = s % 60;
    if (d > 0)      snprintf(buf, len, "%ud%02uh",  (unsigned)d, (unsigned)h);
    else if (h > 0) snprintf(buf, len, "%uh%02um",  (unsigned)h, (unsigned)m);
    else if (m > 0) snprintf(buf, len, "%um%02us",  (unsigned)m, (unsigned)sec);
    else            snprintf(buf, len, "%us",        (unsigned)sec);
}

// ── Display update ────────────────────────────────────────────────────────────

static void render(void) {
    fb_clear();

    char line[COLS + 1];
    char tmp[16];
    char tmp2[16];

    // ── Row 0: WiFi + GNSS Sats ──────────────────────────────────────────────
    wifi_sta_status_t sta;
    wifi_sta_status(&sta);
    gnss_status_t gnss;
    gnss_get_status(&gnss);

    if (sta.active && sta.connected) {
        esp_ip4addr_ntoa((const esp_ip4_addr_t *)&sta.ip4_addr, tmp, sizeof(tmp));
        snprintf(line, sizeof(line), "W:%s S:%d", tmp, gnss.satellites);
    } else {
        wifi_ap_status_t ap;
        wifi_ap_status(&ap);
        if (ap.active) {
            esp_ip4addr_ntoa((const esp_ip4_addr_t *)&ap.ip4_addr, tmp, sizeof(tmp));
            snprintf(line, sizeof(line), "AP:%s S:%d", tmp, gnss.satellites);
        } else {
            snprintf(line, sizeof(line), "No WiFi   S:%d", gnss.satellites);
        }
    }
    fb_puts_padded(0, 0, COLS, line);

    // ── Row 1: GNSS Fix Quality ──────────────────────────────────────────────
    const char *fix_str = "No Fix";
    switch (gnss.fix_quality) {
        case 1: fix_str = "GPS Fix"; break;
        case 2: fix_str = "DGPS Fix"; break;
        case 4: fix_str = "RTK Fixed"; break;
        case 5: fix_str = "RTK Float"; break;
    }
    snprintf(line, sizeof(line), "GNSS: %s", fix_str);
    fb_puts_padded(1, 0, COLS, line);

    // ── Row 2: NTRIP Rates ───────────────────────────────────────────────────
    stream_stats_handle_t s1 = stream_stats_get("ntrip_server");
    stream_stats_handle_t s2 = stream_stats_get("ntrip_server_2");
    stream_stats_handle_t sc = stream_stats_get("ntrip_client");

    uint32_t r1 = 0, r2 = 0, rc = 0;
    if (s1) { stream_stats_values_t v; stream_stats_values(s1, &v); r1 = v.rate_out; }
    if (s2) { stream_stats_values_t v; stream_stats_values(s2, &v); r2 = v.rate_out; }
    if (sc) { stream_stats_values_t v; stream_stats_values(sc, &v); rc = v.rate_in; }

    if (sc && (r1 || r2)) {
        fmt_rate(tmp, sizeof(tmp), rc);
        fmt_rate(tmp2, sizeof(tmp2), r1 + r2);
        snprintf(line, sizeof(line), "C:%s S:%s", tmp, tmp2);
    } else if (r1 || r2) {
        fmt_rate(tmp, sizeof(tmp), r1);
        fmt_rate(tmp2, sizeof(tmp2), r2);
        snprintf(line, sizeof(line), "N1:%s N2:%s", tmp, tmp2);
    } else if (sc) {
        fmt_rate(tmp, sizeof(tmp), rc);
        snprintf(line, sizeof(line), "Client: %s", tmp);
    } else {
        snprintf(line, sizeof(line), "NTRIP: idle");
    }
    fb_puts_padded(2, 0, COLS, line);

    // ── Row 3: uptime + heap ─────────────────────────────────────────────────
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t heap_k   = esp_get_free_heap_size() / 1024;
    fmt_uptime(tmp, sizeof(tmp), uptime_s);
    snprintf(line, sizeof(line), "Up:%-8s Heap:%3uk", tmp, (unsigned)heap_k);
    fb_puts_padded(3, 0, COLS, line);
}

// ── Display task ──────────────────────────────────────────────────────────────

static void display_task(void *arg) {
    while (true) {
        render();
        if (oled_flush() != ESP_OK) {
            ESP_LOGW(TAG, "Flush failed");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void display_init(void) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port            = I2C_NUM_0,
        .sda_io_num          = CONFIG_OLED_SDA_PIN,
        .scl_io_num          = CONFIG_OLED_SCL_PIN,
        .clk_source          = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt   = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &i2c_bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed");
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = OLED_I2C_ADDR,
        .scl_speed_hz    = 400000,
    };
    if (i2c_master_bus_add_device(i2c_bus, &dev_cfg, &i2c_dev) != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed");
        return;
    }

    if (oled_init() != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306 init failed – check wiring on SDA=GPIO%d SCL=GPIO%d",
                 CONFIG_OLED_SDA_PIN, CONFIG_OLED_SCL_PIN);
        return;
    }

    ESP_LOGI(TAG, "SSD1306 128x32 ready on SDA=GPIO%d SCL=GPIO%d",
             CONFIG_OLED_SDA_PIN, CONFIG_OLED_SCL_PIN);

    xTaskCreate(display_task, "display", 4096, NULL, TASK_PRIORITY_STATUS_LED, NULL);
}

#else // CONFIG_OLED_ENABLED not set

void display_init(void) {}

#endif // CONFIG_OLED_ENABLED
