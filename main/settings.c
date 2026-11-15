#include <string.h>
#include "app.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";
settings_t g_cfg;

void settings_defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->version = SETTINGS_VERSION;
    strcpy(s->syms, "AAPL,MSFT,NVDA,SPY,EURUSD=X,GBPUSD=X,USDJPY=X,USDCAD=X");
    strcpy(s->tz, "EST5EDT,M3.2.0,M11.1.0");  // Toronto / New York
    s->refresh_s = 60;
    s->cycle_s = 8;
    s->bright = 80;
    s->h24 = 1;
    s->overview = 1;
    // JUZIPi SDC2 wiring, read from its stock firmware (TFT_eSPI): GPIO18 is the button, GPIO7 the buzzer
    s->pin_sclk = 1;
    s->pin_mosi = 2;
    s->pin_cs = -1;  // tied to GND on the board
    s->pin_dc = 0;
    s->pin_rst = 3;
    s->pin_bl = 5;   // PWM, active low
    s->bl_inverted = 1;
    s->spi_mode = 3;
    s->invert = 1;   // confirmed from photos of the real screen: inversion on, RGB order
    s->bgr = 0;
    s->rot180 = 0;
    s->spi_mhz = 40;
    s->wx_off = 0;
    s->wx_f = 0;
    s->wx_lat_e4 = 436532;   // Toronto, to match the default time zone
    s->wx_lon_e4 = -793832;
    strcpy(s->wx_name, "Toronto");
    s->alert_move_x10 = 30;  // 3.0 %
    s->alert_claude = 90;
    s->quiet_from = 22;
    s->quiet_to = 8;
    s->volume = 2;
}

// Settings saved by an older build are a prefix of today's struct: keep the new fields' defaults.
static bool load_blob(nvs_handle_t h, const char *key, settings_t *out)
{
    size_t len = 0;
    if (nvs_get_blob(h, key, NULL, &len) != ESP_OK || len < sizeof(uint32_t) || len > sizeof(*out)) return false;
    settings_t tmp;
    settings_defaults(&tmp);
    if (nvs_get_blob(h, key, &tmp, &len) != ESP_OK || tmp.version != SETTINGS_VERSION) return false;
    tmp.syms[sizeof(tmp.syms) - 1] = 0;
    tmp.tz[sizeof(tmp.tz) - 1] = 0;
    tmp.wx_name[sizeof(tmp.wx_name) - 1] = 0;
    *out = tmp;
    return true;
}

void settings_load(void)
{
    settings_defaults(&g_cfg);
    nvs_handle_t h;
    if (nvs_open("stk", NVS_READONLY, &h) != ESP_OK) return;
    if (load_blob(h, "cfg", &g_cfg)) ESP_LOGI(TAG, "loaded settings");
    nvs_close(h);
}

esp_err_t settings_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("stk", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, "cfg", &g_cfg, sizeof(g_cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void settings_keep_prev(const settings_t *old)
{
    nvs_handle_t h;
    if (nvs_open("stk", NVS_READWRITE, &h) != ESP_OK) return;
    size_t len = 0;
    if (nvs_get_blob(h, "cfg_ok", NULL, &len) != ESP_OK) {
        nvs_set_blob(h, "cfg_ok", old, sizeof(*old));
    }
    nvs_erase_key(h, "note");  // a new change supersedes the last recovery message
    nvs_commit(h);
    nvs_close(h);
}

bool settings_restore_prev(void)
{
    nvs_handle_t h;
    if (nvs_open("stk", NVS_READWRITE, &h) != ESP_OK) return false;
    settings_t tmp;
    bool ok = load_blob(h, "cfg_ok", &tmp);
    if (ok) nvs_set_blob(h, "cfg", &tmp, sizeof(tmp));
    nvs_erase_key(h, "cfg_ok");
    nvs_commit(h);
    nvs_close(h);
    return ok;
}

void settings_clear_prev(void)
{
    nvs_handle_t h;
    if (nvs_open("stk", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_erase_key(h, "cfg_ok") == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

void settings_reset(void)
{
    settings_defaults(&g_cfg);
    settings_save();
}
