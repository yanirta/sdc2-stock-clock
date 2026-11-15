// ST7789 240x240 over SPI via esp_lcd, rendered in horizontal bands.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gfx.h"

static const char *TAG = "display";

#define BAND_H 16
#define LCD_HOST SPI2_HOST

volatile bool g_display_ok;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_buf[2];
static SemaphoreHandle_t s_sem;
static volatile bool s_dirty = true;

// Palette
#define C_BG rgb(0, 0, 0)
#define C_CARD rgb(20, 23, 30)
#define C_TEXT rgb(240, 242, 245)
#define C_DIM rgb(130, 136, 148)
#define C_FAINT rgb(55, 60, 70)
#define C_UP rgb(30, 210, 120)
#define C_DOWN rgb(255, 85, 85)
#define C_ACCENT rgb(80, 160, 255)

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ed, void *ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_sem, &hp);
    return hp == pdTRUE;
}

void display_set_brightness(uint8_t pct)
{
    if (g_cfg.pin_bl < 0) return;
    if (pct > 100) pct = 100;
    uint32_t duty = (uint32_t)pct * 255 / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void display_redraw(void) { s_dirty = true; }

static volatile int s_focus = -1;  // page requested by an alert (symbol index or FOCUS_CLAUDE)
void display_focus(int focus)
{
    s_focus = focus;
    s_dirty = true;
}

static void backlight_init(void)
{
    if (g_cfg.pin_bl < 0) return;
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t ch = {
        .gpio_num = g_cfg.pin_bl,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .flags.output_invert = g_cfg.bl_inverted ? 1 : 0,
    };
    ledc_channel_config(&ch);
}

static esp_err_t lcd_init(void)
{
    spi_bus_config_t bus = {
        .sclk_io_num = g_cfg.pin_sclk,
        .mosi_io_num = g_cfg.pin_mosi,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SCREEN_W * BAND_H * 2 + 8,
    };
    esp_err_t err = spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = g_cfg.pin_dc,
        .cs_gpio_num = g_cfg.pin_cs,
        .pclk_hz = (uint32_t)(g_cfg.spi_mhz ? g_cfg.spi_mhz : 40) * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = g_cfg.spi_mode,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_trans_done,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io);
    if (err != ESP_OK) return err;

    esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = g_cfg.pin_rst,
        .rgb_ele_order = g_cfg.bgr ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io, &pc, &s_panel);
    if (err != ESP_OK) return err;
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, g_cfg.invert);
    if (g_cfg.rot180) {
        esp_lcd_panel_mirror(s_panel, true, true);
        esp_lcd_panel_set_gap(s_panel, 0, 80);  // 240x240 window inside the 240x320 controller RAM
    }
    esp_lcd_panel_disp_on_off(s_panel, true);
    return ESP_OK;
}

// ---------------------------------------------------------------- formatting

static int price_decimals(double p)
{
    double a = fabs(p);
    if (a >= 10000) return 0;
    if (a >= 1) return 2;  // stocks and forex: at most 2 decimals
    return 4;              // sub-1 prices (some crypto) would otherwise round to nothing
}

static void fmt_price(char *out, size_t n, double p)
{
    int d = price_decimals(p);
    char tmp[32];
    fmt_fixed(tmp, sizeof(tmp), p, d, false);
    // thousands separators on the integer part
    char *dot = strchr(tmp, '.');
    int intlen = dot ? (int)(dot - tmp) : (int)strlen(tmp);
    int neg = tmp[0] == '-';
    size_t j = 0;
    for (int i = 0; tmp[i] && j + 1 < n; i++) {
        if (i > neg && i < intlen && (intlen - i) % 3 == 0) out[j++] = ',';
        if (j + 1 < n) out[j++] = tmp[i];
    }
    out[j] = 0;
}

static void display_name(const quote_t *q, char *out, size_t n)
{
    const char *s = q->sym;
    size_t len = strlen(s);
    if (len == 8 && !strcmp(s + 6, "=X")) {  // EURUSD=X -> EUR/USD
        snprintf(out, n, "%.3s/%.3s", s, s + 3);
    } else if (s[0] == '^' && q->name[0]) {
        snprintf(out, n, "%.12s", q->name);
    } else if (len > 4 && !strcmp(s + len - 4, "-USD")) {
        snprintf(out, n, "%.*s", (int)(len - 4), s);
    } else if (len > 2 && s[len - 2] == '=') {
        snprintf(out, n, "%.*s", (int)(len - 2), s);
    } else {
        snprintf(out, n, "%s", s);
    }
}

static void fmt_clock(char *out, size_t n, time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    if (g_cfg.h24) strftime(out, n, "%H:%M", &tm);
    else {
        strftime(out, n, "%I:%M", &tm);
        if (out[0] == '0') memmove(out, out + 1, strlen(out));
    }
}

static bool time_valid(void)
{
    time_t now;
    time(&now);
    return now > 1700000000;
}

// ---------------------------------------------------------------- pages

typedef struct {
    int page;  // -1 status, 0 overview (if enabled), then symbols
    quote_t q;
    int nq;
    quote_t *all;
} frame_t;

static quote_t s_snap[MAX_SYMS];

static void draw_header(canvas_t *c, const char *title, uint16_t title_color)
{
    gfx_text(c, &font_m, 8, 5, title, title_color, ALIGN_LEFT);
    if (time_valid()) {
        char clk[12];
        time_t now;
        time(&now);
        fmt_clock(clk, sizeof(clk), now);
        gfx_text(c, &font_m, 232, 5, clk, C_DIM, ALIGN_RIGHT);
    }
}

static void draw_status(canvas_t *c)
{
    gfx_fill(c, C_BG);
    gfx_text(c, &font_l, 120, 40, "Stock Clock", C_TEXT, ALIGN_CENTER);
    gfx_text(c, &font_s, 120, 76, app_version(), C_DIM, ALIGN_CENTER);
    char line[64];
    switch (g_net.state) {
    case NET_STARTING:
    case NET_CONNECTING:
        gfx_text(c, &font_m, 120, 118, "Connecting to Wi-Fi", C_TEXT, ALIGN_CENTER);
        if (g_net.sta_ssid[0]) gfx_text(c, &font_s, 120, 146, g_net.sta_ssid, C_DIM, ALIGN_CENTER);
        break;
    case NET_PORTAL:
        gfx_text(c, &font_m, 120, 104, "Wi-Fi setup", C_ACCENT, ALIGN_CENTER);
        gfx_text(c, &font_s, 120, 134, "Join network:", C_DIM, ALIGN_CENTER);
        gfx_text(c, &font_m, 120, 154, g_net.ap_ssid, C_TEXT, ALIGN_CENTER);
        gfx_text(c, &font_s, 120, 184, "then open http://192.168.4.1", C_DIM, ALIGN_CENTER);
        break;
    case NET_CONNECTED:
        gfx_text(c, &font_m, 120, 110, "Connected", C_UP, ALIGN_CENTER);
        snprintf(line, sizeof(line), "http://%s", g_net.ip);
        gfx_text(c, &font_m, 120, 140, line, C_TEXT, ALIGN_CENTER);
        gfx_text(c, &font_s, 120, 176, "Loading quotes...", C_DIM, ALIGN_CENTER);
        break;
    }
}

static void draw_change_pill(canvas_t *c, int x, int y, int w, int h, const quote_t *q)
{
    if (!q->ok || q->prev == 0) {
        gfx_round_rect(c, x, y, w, h, 5, C_CARD);
        gfx_text(c, &font_s, x + w / 2, y + (h - font_s.height) / 2, "--", C_DIM, ALIGN_CENTER);
        return;
    }
    double pct = (q->price - q->prev) / q->prev * 100.0;
    uint16_t col = pct >= 0 ? C_UP : C_DOWN;
    gfx_round_rect(c, x, y, w, h, 5, gfx_mix(C_BG, col, 70));
    char s[16];
    fmt_fixed(s, sizeof(s) - 1, pct, 2, true);
    strcat(s, "%");
    gfx_text(c, &font_s, x + w / 2, y + (h - font_s.height) / 2, s, rgb(255, 255, 255), ALIGN_CENTER);
}

static void draw_overview(canvas_t *c, quote_t *qs, int n)
{
    gfx_fill(c, C_BG);
    draw_header(c, "MARKETS", C_ACCENT);
    int top = 32;
    int rh = n > 0 ? (SCREEN_H - top - 2) / n : 26;
    if (rh > 34) rh = 34;
    for (int i = 0; i < n; i++) {
        const quote_t *q = &qs[i];
        int y = top + i * rh;
        if (y > c->y0 + c->h || y + rh < c->y0) continue;
        if (i) gfx_hline(c, 8, y, 224, rgb(28, 31, 38));
        int ty = y + (rh - font_m.height) / 2 + 1;
        char name[24], price[24];
        display_name(q, name, sizeof(name));
        gfx_text(c, &font_m, 8, ty, name, C_TEXT, ALIGN_LEFT);
        if (q->ok) fmt_price(price, sizeof(price), q->price);
        else strcpy(price, q->err[0] ? "err" : "...");
        gfx_text(c, &font_m, 166, ty, price, q->ok ? C_TEXT : C_DIM, ALIGN_RIGHT);
        draw_change_pill(c, 172, y + 4, 62, rh - 7, q);
    }
    if (n == 0) gfx_text(c, &font_m, 120, 110, "No symbols set", C_DIM, ALIGN_CENTER);
}

static void draw_chart(canvas_t *c, const quote_t *q, int X0, int Y0, int W, int H, uint16_t col)
{
    int n = q->npts;
    if (n < 2) {
        gfx_text(c, &font_s, X0 + W / 2, Y0 + H / 2 - 8, "no intraday data", C_FAINT, ALIGN_CENTER);
        return;
    }
    float mn = q->pts[0], mx = q->pts[0];
    for (int i = 1; i < n; i++) {
        if (q->pts[i] < mn) mn = q->pts[i];
        if (q->pts[i] > mx) mx = q->pts[i];
    }
    if (q->prev > 0) {
        if (q->prev < mn) mn = q->prev;
        if (q->prev > mx) mx = q->prev;
    }
    float pad = (mx - mn) * 0.08f;
    if (pad == 0) pad = mx * 0.001f + 0.0001f;
    mn -= pad;
    mx += pad;
#define YOF(v) (Y0 + H - 1 - (int)(((v) - mn) / (mx - mn) * (H - 1)))

    // area fill, fading towards the bottom
    for (int x = X0; x < X0 + W; x++) {
        float t = (float)(x - X0) * (n - 1) / (W - 1);
        int i = (int)t;
        if (i >= n - 1) i = n - 2;
        float f = t - i;
        float v = q->pts[i] + (q->pts[i + 1] - q->pts[i]) * f;
        int yl = YOF(v);
        for (int y = yl; y < Y0 + H; y++) {
            if (y < c->y0 || y >= c->y0 + c->h) continue;
            uint8_t a = (uint8_t)(70 - 65 * (y - yl) / (Y0 + H - yl + 1));
            gfx_blend_pixel(c, x, y, col, a);
        }
    }
    // previous close, dashed
    if (q->prev > 0) {
        int yp = YOF(q->prev);
        for (int x = X0; x < X0 + W; x += 6) gfx_hline(c, x, yp, 3, C_DIM);
    }
    // the line itself (2px)
    for (int i = 0; i < n - 1; i++) {
        int x0 = X0 + i * (W - 1) / (n - 1), x1 = X0 + (i + 1) * (W - 1) / (n - 1);
        int y0 = YOF(q->pts[i]), y1 = YOF(q->pts[i + 1]);
        gfx_line(c, x0, y0, x1, y1, col);
        gfx_line(c, x0, y0 - 1, x1, y1 - 1, col);
    }
    // last point dot
    int lx = X0 + W - 1, ly = YOF(q->pts[n - 1]);
    gfx_round_rect(c, lx - 3, ly - 3, 7, 7, 3, col);
#undef YOF
}

static void draw_detail(canvas_t *c, const quote_t *q, int page, int pages)
{
    gfx_fill(c, C_BG);
    char name[24];
    display_name(q, name, sizeof(name));
    draw_header(c, name, C_TEXT);
    if (q->name[0] && strcmp(q->name, name)) {
        char sub[30];
        snprintf(sub, sizeof(sub), "%.26s", q->name);
        gfx_text(c, &font_s, 8, 30, sub, C_DIM, ALIGN_LEFT);
    }
    if (!q->ok) {
        gfx_text(c, &font_m, 120, 100, q->err[0] ? q->err : "Loading...", C_DIM, ALIGN_CENTER);
        return;
    }
    double chg = q->price - q->prev;
    double pct = q->prev ? chg / q->prev * 100.0 : 0;
    uint16_t col = chg >= 0 ? C_UP : C_DOWN;

    char price[24];
    fmt_price(price, sizeof(price), q->price);
    const font_t *pf = gfx_text_width(&font_xl, price) <= 228 ? &font_xl : &font_l;
    gfx_text(c, pf, 120, 50 + (pf == &font_l ? 10 : 0), price, C_TEXT, ALIGN_CENTER);

    char line[48];
    int d = price_decimals(q->price);
    char cs[20], ps[16];
    fmt_fixed(cs, sizeof(cs), chg, d, true);
    fmt_fixed(ps, sizeof(ps), pct, 2, true);
    snprintf(line, sizeof(line), "%s %s  %s%%", chg >= 0 ? GLYPH_UP : GLYPH_DOWN, cs, ps);
    gfx_text(c, &font_m, 120, 106, line, col, ALIGN_CENTER);

    draw_chart(c, q, 8, 136, 224, 82, col);

    // footer: currency + market time + page dots
    char foot[48], tbuf[12] = "";
    if (q->mtime) fmt_clock(tbuf, sizeof(tbuf), q->mtime);
    snprintf(foot, sizeof(foot), "%s" GLYPH_DOT "%s", q->cur[0] ? q->cur : "", tbuf);
    gfx_text(c, &font_s, 8, 222, foot, C_DIM, ALIGN_LEFT);
    int dots = pages > 12 ? 12 : pages;
    int dx = 232 - dots * 8;
    for (int i = 0; i < dots; i++)
        gfx_round_rect(c, dx + i * 8, 228, 4, 4, 2, i == page ? C_TEXT : C_FAINT);
}


// ---------------------------------------------------------------- weather page

#define C_SUN rgb(255, 190, 40)
#define C_CLOUD rgb(205, 212, 222)
#define C_CLOUD2 rgb(120, 130, 146)
#define C_RAIN rgb(90, 170, 255)

static weather_t s_wx;

typedef enum { WX_CLEAR, WX_FEW, WX_PARTLY, WX_CLOUDY, WX_FOG, WX_DRIZZLE, WX_RAIN, WX_SNOW, WX_THUNDER } wx_kind_t;

static wx_kind_t wx_kind(uint8_t code)
{
    if (code == 0) return WX_CLEAR;
    if (code == 1) return WX_FEW;
    if (code == 2) return WX_PARTLY;
    if (code == 45 || code == 48) return WX_FOG;
    if (code >= 51 && code <= 57) return WX_DRIZZLE;
    if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return WX_RAIN;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return WX_SNOW;
    if (code >= 95) return WX_THUNDER;
    return WX_CLOUDY;
}

static const char *wx_text(uint8_t code)
{
    switch (code) {
    case 0: return "Clear";
    case 1: return "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunder, hail";
    default: return "Cloudy";
    }
}

static void draw_sun(canvas_t *c, float cx, float cy, float r, bool day)
{
    if (!day) {  // crescent moon
        gfx_disc(c, cx, cy, r, rgb(235, 232, 210));
        gfx_disc(c, cx + r * 0.5f, cy - r * 0.35f, r * 0.85f, C_BG);
        return;
    }
    static const float dir[8][2] = { { 1, 0 }, { .707f, .707f }, { 0, 1 }, { -.707f, .707f },
                                     { -1, 0 }, { -.707f, -.707f }, { 0, -1 }, { .707f, -.707f } };
    for (int i = 0; i < 8; i++) {
        int x0 = cx + dir[i][0] * r * 1.35f, y0 = cy + dir[i][1] * r * 1.35f;
        int x1 = cx + dir[i][0] * r * 1.75f, y1 = cy + dir[i][1] * r * 1.75f;
        gfx_line(c, x0, y0, x1, y1, C_SUN);
        if (r > 8) gfx_line(c, x0 + 1, y0, x1 + 1, y1, C_SUN);
    }
    gfx_disc(c, cx, cy, r, C_SUN);
}

static void draw_cloud(canvas_t *c, float cx, float cy, float s, uint16_t col)
{
    gfx_disc(c, cx - 0.32f * s, cy + 0.08f * s, 0.30f * s, col);
    gfx_disc(c, cx + 0.06f * s, cy - 0.12f * s, 0.40f * s, col);
    gfx_disc(c, cx + 0.40f * s, cy + 0.12f * s, 0.26f * s, col);
    gfx_rect(c, cx - 0.32f * s, cy + 0.08f * s, 0.72f * s, 0.30f * s, col);
}

// s is the icon's size in pixels (roughly its width)
static void draw_wx_icon(canvas_t *c, float cx, float cy, float s, uint8_t code, bool day)
{
    if (cy - s > c->y0 + c->h || cy + s < c->y0) return;
    wx_kind_t k = wx_kind(code);
    switch (k) {
    case WX_CLEAR:
        draw_sun(c, cx, cy, 0.3f * s, day);
        break;
    case WX_FEW:
    case WX_PARTLY:
        draw_sun(c, cx - 0.18f * s, cy - 0.16f * s, 0.24f * s, day);
        draw_cloud(c, cx + 0.12f * s, cy + 0.14f * s, k == WX_FEW ? 0.62f * s : 0.8f * s, C_CLOUD);
        break;
    case WX_CLOUDY:
        draw_cloud(c, cx + 0.16f * s, cy - 0.12f * s, 0.62f * s, C_CLOUD2);
        draw_cloud(c, cx - 0.04f * s, cy + 0.08f * s, 0.9f * s, C_CLOUD);
        break;
    case WX_FOG:
        draw_cloud(c, cx, cy - 0.14f * s, 0.8f * s, C_CLOUD2);
        for (int i = 0; i < 3; i++)
            gfx_round_rect(c, cx - 0.42f * s + (i & 1) * 0.08f * s, cy + 0.2f * s + i * 0.13f * s, 0.8f * s,
                           s > 40 ? 4 : 2, 2, C_CLOUD);
        break;
    default:
        draw_cloud(c, cx, cy - 0.16f * s, 0.9f * s, k == WX_THUNDER ? C_CLOUD2 : C_CLOUD);
        for (int i = -1; i <= 1; i++) {
            float x = cx + i * 0.26f * s, y = cy + 0.3f * s;
            if (k == WX_SNOW) {
                gfx_disc(c, x, y + (i & 1) * 0.1f * s, s > 40 ? 3.5f : 1.8f, rgb(255, 255, 255));
            } else if (k != WX_THUNDER) {
                float len = (k == WX_RAIN ? 0.26f : 0.14f) * s;
                gfx_line(c, x + 0.05f * s, y, x - 0.05f * s, y + len, C_RAIN);
                if (s > 40) gfx_line(c, x + 0.05f * s + 1, y, x - 0.05f * s + 1, y + len, C_RAIN);
            }
        }
        if (k == WX_THUNDER) {
            float x = cx, y = cy + 0.2f * s;
            for (int d = 0; d < (s > 40 ? 2 : 1); d++) {
                gfx_line(c, x + 0.06f * s + d, y, x - 0.08f * s + d, y + 0.2f * s, C_SUN);
                gfx_line(c, x - 0.08f * s + d, y + 0.2f * s, x + 0.07f * s + d, y + 0.2f * s, C_SUN);
                gfx_line(c, x + 0.07f * s + d, y + 0.2f * s, x - 0.06f * s + d, y + 0.42f * s, C_SUN);
            }
        }
        break;
    }
}

static void fmt_deg(char *out, size_t n, float v)
{
    fmt_fixed(out, n - 2, v, 0, false);
    strcat(out, GLYPH_DEG);
}

static void draw_weather(canvas_t *c, const weather_t *w)
{
    gfx_fill(c, C_BG);
    char name[32];
    strlcpy(name, g_cfg.wx_name[0] ? g_cfg.wx_name : "Weather", sizeof(name));
    while (strlen(name) > 3 && gfx_text_width(&font_m, name) > 160) name[strlen(name) - 1] = 0;
    draw_header(c, name, C_ACCENT);
    if (!w->ok) {
        gfx_text(c, &font_m, 120, 110, w->err[0] ? w->err : "Loading weather...", C_DIM, ALIGN_CENTER);
        return;
    }
    draw_wx_icon(c, 60, 82, 84, w->code, w->is_day);

    char t[16], t2[16], line[64];
    fmt_deg(t, sizeof(t), w->temp);
    gfx_text(c, &font_xl, 172, 42, t, C_TEXT, ALIGN_CENTER);
    const char *cond = wx_text(w->code);
    const font_t *cf = gfx_text_width(&font_m, cond) <= 128 ? &font_m : &font_s;
    gfx_text(c, cf, 172, 102, cond, C_TEXT, ALIGN_CENTER);

    if (w->ndays) {
        fmt_deg(t, sizeof(t), w->day[0].hi);
        fmt_deg(t2, sizeof(t2), w->day[0].lo);
        snprintf(line, sizeof(line), "H %s   L %s", t, t2);
        gfx_text(c, &font_m, 120, 136, line, C_TEXT, ALIGN_CENTER);
    }
    char ws[12];
    fmt_deg(t, sizeof(t), w->feels);
    fmt_fixed(ws, sizeof(ws), w->wind, 0, false);
    snprintf(line, sizeof(line), "Feels %s  " GLYPH_DOT "  %u%%  " GLYPH_DOT "  %s %s", t, w->hum, ws,
             g_cfg.wx_f ? "mph" : "km/h");
    gfx_text(c, &font_s, 120, 162, line, C_DIM, ALIGN_CENTER);

    gfx_hline(c, 8, 186, 224, rgb(28, 31, 38));
    static const char *wd[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    for (int i = 1; i < w->ndays && i < 4; i++) {
        int x0 = (i - 1) * 80;
        const wx_day_t *d = &w->day[i];
        draw_wx_icon(c, x0 + 18, 212, 30, d->code, true);
        gfx_text(c, &font_s, x0 + 36, 192, wd[d->wday % 7], C_DIM, ALIGN_LEFT);
        fmt_deg(t, sizeof(t), d->hi);
        fmt_deg(t2, sizeof(t2), d->lo);
        int x = gfx_text(c, &font_s, x0 + 36, 212, t, C_TEXT, ALIGN_LEFT);
        if (x + 3 + gfx_text_width(&font_s, t2) <= x0 + 80) gfx_text(c, &font_s, x + 3, 212, t2, C_DIM, ALIGN_LEFT);
    }
}


// ---------------------------------------------------------------- Claude usage page

// Claude's light theme: ivory background, near-black text, terracotta accent.
#define CL_BG rgb(250, 249, 245)
#define CL_TRACK rgb(232, 230, 220)
#define CL_TEXT rgb(20, 20, 19)
#define CL_DIM rgb(115, 114, 108)
#define CL_ORANGE rgb(217, 119, 87)
#define CL_AMBER rgb(214, 150, 40)
#define CL_RED rgb(191, 64, 47)
static claude_t s_claude;

static bool claude_fresh(void)
{
    time_t now;
    time(&now);
    return s_claude.updated && time_valid() && now - s_claude.updated < CLAUDE_MAX_AGE_S;
}

// The Claude spark: a burst of rounded, tapering rays of slightly uneven length.
static void draw_spark(canvas_t *c, float cx, float cy, float R, uint16_t col)
{
    if (cy - R > c->y0 + c->h || cy + R < c->y0) return;
    static const float len[12] = { 1.0f, .82f, .94f, .78f, .97f, .85f, .92f, .8f, 1.0f, .84f, .9f, .79f };
    for (int i = 0; i < 12; i++) {
        float a = i * 0.5236f + 0.12f, dx = cosf(a), dy = sinf(a), L = R * len[i];
        for (float t = 0; t <= L; t += 0.6f) {
            float w = R * 0.14f * (1.0f - 0.45f * t / L);
            gfx_disc(c, cx + dx * t, cy + dy * t, w, col);
        }
    }
}

static void fmt_reset(char *out, size_t n, time_t reset, time_t now)
{
    long d = (long)(reset - now);
    if (!reset) out[0] = 0;
    else if (d <= 0) snprintf(out, n, "Resetting now");
    else if (d < 3600) snprintf(out, n, "Resets in %ld min", (d + 59) / 60);
    else if (d < 24 * 3600) snprintf(out, n, "Resets in %ldh %02ldm", d / 3600, (d % 3600) / 60);
    else {
        struct tm tm;
        localtime_r(&reset, &tm);
        char clk[12];
        fmt_clock(clk, sizeof(clk), reset);
        static const char *wd[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        snprintf(out, n, "Resets %s %s", wd[tm.tm_wday], clk);
    }
}

static void draw_limit(canvas_t *c, int y, const char *label, int pct, time_t reset, time_t now)
{
    if (reset && reset <= now) pct = 0;  // window already rolled over since the last push
    char t[32];
    gfx_text(c, &font_m, 12, y + 8, label, CL_TEXT, ALIGN_LEFT);
    snprintf(t, sizeof(t), "%d%%", pct);
    uint16_t col = pct >= 90 ? CL_RED : pct >= 70 ? CL_AMBER : CL_ORANGE;
    gfx_text(c, &font_l, 228, y, t, col, ALIGN_RIGHT);
    gfx_round_rect(c, 12, y + 36, 216, 14, 7, CL_TRACK);
    int w = 216 * pct / 100;
    if (w > 0) gfx_round_rect(c, 12, y + 36, w < 14 ? 14 : w, 14, 7, col);
    fmt_reset(t, sizeof(t), reset, now);
    gfx_text(c, &font_s, 12, y + 56, t, CL_DIM, ALIGN_LEFT);
}

static void draw_claude(canvas_t *c)
{
    gfx_fill(c, CL_BG);
    draw_spark(c, 21, 15, 11, CL_ORANGE);
    gfx_text(c, &font_m, 38, 5, "Claude", CL_TEXT, ALIGN_LEFT);
    time_t now;
    time(&now);
    char clk[12];
    fmt_clock(clk, sizeof(clk), now);
    gfx_text(c, &font_m, 232, 5, clk, CL_DIM, ALIGN_RIGHT);
    draw_limit(c, 34, "5-hour limit", s_claude.s_pct, s_claude.s_reset, now);
    draw_limit(c, 118, "Weekly limit", s_claude.w_pct, s_claude.w_reset, now);
    char foot[48], plan[16] = "";
    long age = (long)(now - s_claude.updated);
    if (s_claude.plan[0]) {
        snprintf(plan, sizeof(plan), "%s", s_claude.plan);
        plan[0] = toupper((unsigned char)plan[0]);
    }
    const char *sep = plan[0] ? "  " GLYPH_DOT "  " : "";
    if (age < 120) snprintf(foot, sizeof(foot), "%s%supdated just now", plan, sep);
    else if (age < 7200) snprintf(foot, sizeof(foot), "%s%supdated %ld min ago", plan, sep, age / 60);
    else snprintf(foot, sizeof(foot), "%s%supdated %ldh ago", plan, sep, age / 3600);
    gfx_text(c, &font_s, 120, 216, foot, age > 1800 ? CL_AMBER : CL_DIM, ALIGN_CENTER);
}

// ---------------------------------------------------------------- push pages

static push_t s_push[MAX_PUSH];

// Greedy word wrap; returns the y below the last row drawn.
static int draw_wrapped(canvas_t *c, const font_t *f, int x, int y, int maxw, const char *s, uint16_t col, int rows)
{
    char row[64];
    while (*s && rows-- > 0) {
        while (*s == ' ') s++;
        int n = 0, cut = 0;
        while (s[n] && n < (int)sizeof(row) - 1) {
            row[n] = s[n];
            row[n + 1] = 0;
            if (gfx_text_width(f, row) > maxw) break;
            n++;
            if (s[n] == ' ' || !s[n]) cut = n;
        }
        if (!s[n]) cut = n;                 // everything left fits
        if (!cut) cut = n ? n : 1;          // one long word: hard cut
        if (rows == 0 && s[cut]) {          // last row and more text: end with "..."
            while (cut > 0 && (row[cut] = 0, gfx_text_width(f, row) + gfx_text_width(f, "...") > maxw)) cut--;
            row[cut] = 0;
            strlcat(row, "...", sizeof(row));
        } else {
            memcpy(row, s, cut);
            row[cut] = 0;
        }
        gfx_text(c, f, x, y, row, col, ALIGN_LEFT);
        y += f->height - 2;
        s += cut;
    }
    return y;
}

static void fmt_age(char *out, size_t n, long age)
{
    if (age < 90) snprintf(out, n, "just now");
    else if (age < 5400) snprintf(out, n, "%ld min ago", (age + 30) / 60);
    else snprintf(out, n, "%ldh ago", age / 3600);
}

static void draw_push(canvas_t *c, const push_t *m)
{
    bool cl = m->theme == 1;
    uint16_t bg = cl ? CL_BG : C_BG, fg = cl ? CL_TEXT : C_TEXT, dim = cl ? CL_DIM : C_DIM;
    uint16_t acc = m->color ? m->color : cl ? CL_ORANGE : C_ACCENT;
    gfx_fill(c, bg);
    if (cl) draw_spark(c, 21, 15, 11, acc);
    else gfx_disc(c, 18, 15, 6, acc);
    char title[28];
    strlcpy(title, m->title[0] ? m->title : m->id, sizeof(title));
    while (strlen(title) > 3 && gfx_text_width(&font_m, title) > 194) title[strlen(title) - 1] = 0;
    gfx_text(c, &font_m, 38, 5, title, fg, ALIGN_LEFT);  // full-width title; the time goes in the footer
    time_t now;
    time(&now);
    char t[24], foot[48];
    if (m->alert) gfx_rect(c, 0, 30, SCREEN_W, 3, acc);
    int y = 44;
    if (m->line[0][0]) y = draw_wrapped(c, &font_l, 12, y, 216, m->line[0], m->alert ? acc : fg, 2) + 8;
    if (m->line[1][0]) y = draw_wrapped(c, &font_m, 12, y, 216, m->line[1], fg, 4) + 6;
    if (m->line[2][0]) draw_wrapped(c, &font_s, 12, y, 216, m->line[2], dim, 2);
    fmt_clock(foot, sizeof(foot), now);
    strlcat(foot, "  " GLYPH_DOT "  ", sizeof(foot));
    fmt_age(t, sizeof(t), (long)(now - m->updated));
    strlcat(foot, t, sizeof(foot));
    gfx_text(c, &font_s, 120, 216, foot, dim, ALIGN_CENTER);
}

// ---------------------------------------------------------------- night clock

#define C_NIGHT rgb(255, 150, 70)

static void draw_night(canvas_t *c)
{
    gfx_fill(c, C_BG);
    if (!time_valid()) return;
    time_t now;
    time(&now);
    char t[24];
    fmt_clock(t, sizeof(t), now);
    gfx_text(c, &font_xxl, 120, 52, t, C_NIGHT, ALIGN_CENTER);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(t, sizeof(t), "%a %d %b", &tm);
    gfx_text(c, &font_m, 120, 148, t, gfx_mix(C_BG, C_NIGHT, 150), ALIGN_CENTER);
    if (s_wx.ok && !g_cfg.wx_off) {
        char line[40], deg[12];
        fmt_fixed(deg, sizeof(deg) - 2, s_wx.temp, 0, false);
        strcat(deg, GLYPH_DEG);
        snprintf(line, sizeof(line), "%s  %s", deg, wx_text(s_wx.code));
        gfx_text(c, &font_s, 120, 180, line, gfx_mix(C_BG, C_NIGHT, 110), ALIGN_CENTER);
    }
}

// ---------------------------------------------------------------- task

#define SHOW_STATUS -1
#define SHOW_NIGHT -2
#define SHOW_PUSH(k) (-10 - (k))  // a push slot shown out of rotation (alert)

static void render(int show, int pages, int nq, const int *pidx, int np)
{
    for (int y = 0, bi = 0; y < SCREEN_H; y += BAND_H, bi ^= 1) {
        xSemaphoreTake(s_sem, portMAX_DELAY);
        canvas_t c = { .buf = s_buf[bi], .y0 = y, .h = BAND_H };
        // rotation: [overview] [weather] [claude] [push pages] symbols...
        int p = show;
        if (show == SHOW_STATUS) draw_status(&c);
        else if (show == SHOW_NIGHT) draw_night(&c);
        else if (show <= SHOW_PUSH(0)) draw_push(&c, &s_push[SHOW_PUSH(0) - show]);
        else if (g_cfg.overview && p-- == 0) draw_overview(&c, s_snap, nq);
        else if (!g_cfg.wx_off && p-- == 0) draw_weather(&c, &s_wx);
        else if (claude_fresh() && p-- == 0) draw_claude(&c);
        else if (p >= 0 && p < np) draw_push(&c, &s_push[pidx[p]]);
        else if ((p -= np) >= 0 && p < nq) draw_detail(&c, &s_snap[p], show, pages);
        else gfx_fill(&c, C_BG);
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, SCREEN_W, y + BAND_H, c.buf);
    }
}

static void display_task(void *arg)
{
    int page = 0, last_show = -100, last_min = -1, hold_push = -1, last_night = -1;
    bool alt = false;  // alternate turn for an alerting push page
    uint32_t last_gen = 0;
    net_state_t last_state = -1;
    int64_t page_t = esp_timer_get_time(), hold_until = 0;
    int64_t connected_seen = 0;
    for (;;) {
        bool redraw = s_dirty;
        s_dirty = false;
        int nq;
        bool have_data = false;
        xSemaphoreTake(g_qmux, portMAX_DELAY);
        nq = g_nquotes;
        for (int i = 0; i < nq; i++) have_data |= g_quotes[i].ok;
        if (g_quotes_gen != last_gen) {
            memcpy(s_snap, g_quotes, sizeof(quote_t) * nq);
            s_wx = g_wx;
            s_claude = g_claude;
            memcpy(s_push, g_push, sizeof(s_push));
            last_gen = g_quotes_gen;
            redraw = true;
        }
        xSemaphoreGive(g_qmux);

        time_t t;
        time(&t);
        int pidx[MAX_PUSH], np = 0, alert_slot = -1;
        for (int i = 0; i < MAX_PUSH; i++) {
            if (!s_push[i].id[0] || s_push[i].expires <= t) continue;
            pidx[np++] = i;
            if (s_push[i].alert && (alert_slot < 0 || s_push[i].updated > s_push[alert_slot].updated)) alert_slot = i;
        }
        bool night = quiet_now();
        if (night != last_night) {  // dim for the night, restore in the morning
            last_night = night;
            display_set_brightness(night ? NIGHT_BRIGHT(&g_cfg) : g_cfg.bright);
            redraw = true;
        }
        bool night_clock = night && !g_cfg.night_pages;

        int pages = nq + (g_cfg.overview ? 1 : 0) + (g_cfg.wx_off ? 0 : 1) + (claude_fresh() ? 1 : 0) + np;
        if (pages == 0) pages = 1;
        int64_t now = esp_timer_get_time();
        int64_t cycle = (int64_t)(g_cfg.cycle_s ? g_cfg.cycle_s : 8) * 1000000LL;
        if (g_net.state == NET_CONNECTED && !connected_seen) connected_seen = now;
        if (g_net.state != NET_CONNECTED) connected_seen = 0;

        int show;
        // status screen until connected, and briefly after connecting until data arrives
        if (!night_clock && (g_net.state != NET_CONNECTED || (!have_data && now - connected_seen < 30000000LL) ||
                             now - connected_seen < 4000000LL)) {
            show = SHOW_STATUS;
        } else {
            int f = s_focus;
            if (f >= 0) {  // an alert: jump to its page and hold it for three page times
                s_focus = -1;
                if (f >= FOCUS_PUSH && f < FOCUS_PUSH + MAX_PUSH) {
                    hold_push = f - FOCUS_PUSH;
                    hold_until = now + 3 * cycle;
                } else if (!night_clock) {  // price / Claude-limit alerts don't wake the night clock
                    int base = (g_cfg.overview ? 1 : 0) + (g_cfg.wx_off ? 0 : 1);
                    int target = f == FOCUS_CLAUDE ? (claude_fresh() ? base : -1)
                                                   : (f < nq ? base + (claude_fresh() ? 1 : 0) + np + f : -1);
                    if (target >= 0 && target < pages) {
                        page = target;
                        page_t = now + 2 * cycle;
                        alt = false;
                    }
                }
            }
            if (hold_push >= 0 && (now > hold_until || !s_push[hold_push].id[0])) hold_push = -1;
            if (now - page_t > cycle) {
                page_t = now;
                if (alert_slot >= 0 && !alt) alt = true;  // every other turn goes to the alert
                else {
                    alt = false;
                    if (!night_clock) page = (page + 1) % pages;
                }
            }
            if (page >= pages) page = 0;
            if (hold_push >= 0) show = SHOW_PUSH(hold_push);
            else if (alt && alert_slot >= 0) show = SHOW_PUSH(alert_slot);
            else if (night_clock) show = SHOW_NIGHT;
            else show = page;
        }
        int minute = (int)(t / 60);
        if (show != last_show || minute != last_min || g_net.state != last_state) redraw = true;
        if (redraw) {
            render(show, pages, nq, pidx, np);
            last_show = show;
            last_min = minute;
            last_state = g_net.state;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void display_start(void)
{
    backlight_init();
    s_sem = xSemaphoreCreateCounting(2, 2);
    for (int i = 0; i < 2; i++) {
        s_buf[i] = heap_caps_malloc(SCREEN_W * BAND_H * 2, MALLOC_CAP_DMA);
        if (!s_buf[i]) {
            ESP_LOGE(TAG, "no DMA memory for band buffers");
            return;
        }
    }
    esp_err_t err = lcd_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD init failed: %s", esp_err_to_name(err));
        return;
    }
    g_display_ok = true;
    xTaskCreate(display_task, "display", 4096, NULL, 3, NULL);
    vTaskDelay(pdMS_TO_TICKS(60));  // let the first frame land before lighting up
    display_set_brightness(g_cfg.bright);
}
