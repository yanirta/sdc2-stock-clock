// Settings web UI, JSON API, firmware upload and revert-to-stock.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "gfx.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_err_t h_index(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, index_html_start, index_html_end - index_html_start);
}

// --- tiny JSON writer helpers
typedef struct {
    char *buf;
    size_t n, len;
} jw_t;

static void jw(jw_t *w, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (w->len < w->n) {
        int k = vsnprintf(w->buf + w->len, w->n - w->len, fmt, ap);
        if (k > 0) w->len += k;
        if (w->len > w->n) w->len = w->n;
    }
    va_end(ap);
}

static void jw_str(jw_t *w, const char *s)
{
    jw(w, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') jw(w, "\\%c", *s);
        else if ((unsigned char)*s < 0x20) jw(w, " ");
        else jw(w, "%c", *s);
    }
    jw(w, "\"");
}

static esp_err_t h_state(httpd_req_t *r)
{
    size_t cap = 6144;
    char *buf = malloc(cap);
    if (!buf) return httpd_resp_send_500(r);
    jw_t w = { buf, cap, 0 };
    char stock_label[17] = "", stock_ver[33] = "";
    bool stock = stock_partition_available(stock_label, sizeof(stock_label), stock_ver, sizeof(stock_ver));
    jw(&w, "{\"version\":");
    jw_str(&w, app_version());
    jw(&w, ",\"running\":\"%s\",\"stock\":%s,\"stock_label\":", esp_ota_get_running_partition()->label,
       stock ? "true" : "false");
    jw_str(&w, stock_label);
    jw(&w, ",\"stock_version\":");
    jw_str(&w, stock_ver);
    jw(&w, ",\"heap\":%u,\"heap_min\":%u,\"uptime\":%lu,\"rssi\":%d,\"display_ok\":%s,",
       (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
       (unsigned long)(esp_timer_get_time() / 1000000), wifi_rssi(), g_display_ok ? "true" : "false");
    jw(&w, "\"boot_note\":");
    jw_str(&w, g_boot_note);
    jw(&w, ",\"resets\":");
    jw_str(&w, g_reset_log);
    jw(&w, ",\"wifi\":{\"state\":%d,\"ssid\":", g_net.state);
    jw_str(&w, g_net.sta_ssid);
    jw(&w, ",\"ip\":\"%s\",\"ap\":", g_net.ip);
    jw_str(&w, g_net.ap_ssid);
    jw(&w, "},\"cfg\":{\"syms\":");
    jw_str(&w, g_cfg.syms);
    jw(&w, ",\"tz\":");
    jw_str(&w, g_cfg.tz);
    jw(&w,
       ",\"refresh_s\":%u,\"cycle_s\":%u,\"bright\":%u,\"h24\":%u,\"overview\":%u,"
       "\"pin_sclk\":%d,\"pin_mosi\":%d,\"pin_cs\":%d,\"pin_dc\":%d,\"pin_rst\":%d,\"pin_bl\":%d,"
       "\"bl_inverted\":%u,\"spi_mode\":%u,\"invert\":%u,\"bgr\":%u,\"rot180\":%u,\"spi_mhz\":%u,",
       g_cfg.refresh_s, g_cfg.cycle_s, g_cfg.bright, g_cfg.h24, g_cfg.overview, g_cfg.pin_sclk, g_cfg.pin_mosi,
       g_cfg.pin_cs, g_cfg.pin_dc, g_cfg.pin_rst, g_cfg.pin_bl, g_cfg.bl_inverted, g_cfg.spi_mode, g_cfg.invert,
       g_cfg.bgr, g_cfg.rot180, g_cfg.spi_mhz);
    {
        char la[16], lo[16];
        fmt_fixed(la, sizeof(la), g_cfg.wx_lat_e4 / 10000.0, 4, false);
        fmt_fixed(lo, sizeof(lo), g_cfg.wx_lon_e4 / 10000.0, 4, false);
        jw(&w, "\"wx_on\":%u,\"wx_f\":%u,\"wx_lat\":\"%s\",\"wx_lon\":\"%s\",\"wx_name\":", !g_cfg.wx_off,
           g_cfg.wx_f, la, lo);
        jw_str(&w, g_cfg.wx_name);
        char am[8];
        fmt_fixed(am, sizeof(am), g_cfg.alert_move_x10 / 10.0, 1, false);
        jw(&w, ",\"alert_move\":%s,\"alert_claude\":%u,\"quiet_from\":%u,\"quiet_to\":%u,\"volume\":%u", am,
           g_cfg.alert_claude, g_cfg.quiet_from, g_cfg.quiet_to, g_cfg.volume);
        jw(&w, ",\"night_bright\":%u,\"night_pages\":%u", NIGHT_BRIGHT(&g_cfg), g_cfg.night_pages);
        xSemaphoreTake(g_qmux, portMAX_DELAY);
        weather_t wx = g_wx;
        xSemaphoreGive(g_qmux);
        char tt[16], wd[16];
        fmt_fixed(tt, sizeof(tt), wx.temp, 1, false);
        fmt_fixed(wd, sizeof(wd), wx.wind, 0, false);
        jw(&w, "},\"wx\":{\"ok\":%s,\"temp\":%s,\"hum\":%u,\"wind\":%s,\"code\":%u,\"err\":",
           wx.ok ? "true" : "false", tt, wx.hum, wd, wx.code);
        jw_str(&w, wx.err);
        jw(&w, "},\"push\":[");
        time_t tn;
        time(&tn);
        xSemaphoreTake(g_qmux, portMAX_DELAY);
        for (int i = 0, n = 0; i < MAX_PUSH; i++) {
            const push_t *m = &g_push[i];
            if (!m->id[0] || m->expires <= tn) continue;
            jw(&w, "%s{\"id\":", n++ ? "," : "");
            jw_str(&w, m->id);
            jw(&w, ",\"title\":");
            jw_str(&w, m->title);
            jw(&w, ",\"l1\":");
            jw_str(&w, m->line[0]);
            jw(&w, ",\"alert\":%s,\"expires_in\":%ld}", m->alert ? "true" : "false", (long)(m->expires - tn));
        }
        xSemaphoreGive(g_qmux);
        jw(&w, "],");
    }
    jw(&w, "\"quotes\":[");
    xSemaphoreTake(g_qmux, portMAX_DELAY);
    for (int i = 0; i < g_nquotes; i++) {
        quote_t *q = &g_quotes[i];
        jw(&w, "%s{\"sym\":", i ? "," : "");
        jw_str(&w, q->sym);
        jw(&w, ",\"name\":");
        jw_str(&w, q->name);
        jw(&w, ",\"cur\":");
        jw_str(&w, q->cur);
        char ps[24], pv[24];
        fmt_fixed(ps, sizeof(ps), q->price, 6, false);
        fmt_fixed(pv, sizeof(pv), q->prev, 6, false);
        jw(&w, ",\"ok\":%s,\"price\":%s,\"prev\":%s,\"mtime\":%lu,\"err\":", q->ok ? "true" : "false", ps, pv,
           (unsigned long)q->mtime);
        jw_str(&w, q->err);
        jw(&w, "}");
    }
    xSemaphoreGive(g_qmux);
    jw(&w, "]}");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(r, buf, w.len);
    free(buf);
    return err;
}

// Reads an x-www-form-urlencoded body into buf.
static int read_body(httpd_req_t *r, char *buf, size_t n)
{
    if (r->content_len >= n) return -1;
    int got = 0;
    while (got < r->content_len) {
        int k = httpd_req_recv(r, buf + got, r->content_len - got);
        if (k <= 0) return -1;
        got += k;
    }
    buf[got] = 0;
    return got;
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

static bool field(const char *body, const char *key, char *out, size_t n)
{
    if (httpd_query_key_value(body, key, out, n) != ESP_OK) return false;
    url_decode(out);
    return true;
}

static bool field_int(const char *body, const char *key, int *v)
{
    char t[12];
    if (!field(body, key, t, sizeof(t))) return false;
    *v = atoi(t);
    return true;
}

static esp_err_t h_settings(httpd_req_t *r)
{
    char body[768], t[200];
    if (read_body(r, body, sizeof(body)) < 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body");
    bool need_reboot = false;
    settings_t old = g_cfg;
    int v;
    if (field(body, "syms", t, sizeof(t))) {
        strlcpy(g_cfg.syms, t, sizeof(g_cfg.syms));
        quotes_set_symbols(g_cfg.syms);
        quotes_kick();
    }
    if (field(body, "tz", t, sizeof(t)) && t[0]) {
        strlcpy(g_cfg.tz, t, sizeof(g_cfg.tz));
        setenv("TZ", g_cfg.tz, 1);
        tzset();
    }
    if (field_int(body, "refresh_s", &v) && v >= 15 && v <= 3600) g_cfg.refresh_s = v;
    if (field_int(body, "cycle_s", &v) && v >= 2 && v <= 120) g_cfg.cycle_s = v;
    if (field_int(body, "bright", &v) && v >= 0 && v <= 100) {
        g_cfg.bright = v;
        display_set_brightness(v);
    }
    if (field_int(body, "h24", &v)) g_cfg.h24 = !!v;
    if (field_int(body, "overview", &v)) g_cfg.overview = !!v;
#define PIN(name)                                                                \
    if (field_int(body, #name, &v) && v >= -1 && v <= 20 && (v < 11 || v > 17) && v != g_cfg.name) { \
        g_cfg.name = v;                                                         \
        need_reboot = true;                                                     \
    }
    PIN(pin_sclk) PIN(pin_mosi) PIN(pin_cs) PIN(pin_dc) PIN(pin_rst) PIN(pin_bl)
#undef PIN
#define FLAG(name, maxv)                                                           \
    if (field_int(body, #name, &v) && v >= 0 && v <= maxv && v != g_cfg.name) { \
        g_cfg.name = v;                                                           \
        need_reboot = true;                                                       \
    }
    FLAG(bl_inverted, 1) FLAG(spi_mode, 3) FLAG(invert, 1) FLAG(bgr, 1) FLAG(rot180, 1) FLAG(spi_mhz, 80)
#undef FLAG
    if (field(body, "alert_move", t, sizeof(t)) && t[0]) {
        double d = strtod(t, NULL);
        if (d >= 0 && d <= 25) g_cfg.alert_move_x10 = (uint8_t)(d * 10 + 0.5);
    }
    if (field_int(body, "alert_claude", &v) && v >= 0 && v <= 100) g_cfg.alert_claude = v;
    if (field_int(body, "quiet_from", &v) && v >= 0 && v <= 23) g_cfg.quiet_from = v;
    if (field_int(body, "quiet_to", &v) && v >= 0 && v <= 23) g_cfg.quiet_to = v;
    if (field_int(body, "volume", &v) && v >= 0 && v <= 3) g_cfg.volume = v;
    if (field_int(body, "night_bright", &v) && v >= 0 && v <= 100) {
        g_cfg.night_bright_p1 = v + 1;
        display_redraw();
    }
    if (field_int(body, "night_pages", &v)) g_cfg.night_pages = !!v;
    bool wx_changed = false;
    if (field(body, "wx_name", t, sizeof(t))) {
        strlcpy(g_cfg.wx_name, t, sizeof(g_cfg.wx_name));
        wx_changed = true;
    }
    if (field(body, "wx_lat", t, sizeof(t)) && t[0]) {
        double d = strtod(t, NULL);
        if (d >= -90 && d <= 90) g_cfg.wx_lat_e4 = (int32_t)(d * 10000 + (d < 0 ? -0.5 : 0.5));
        wx_changed = true;
    }
    if (field(body, "wx_lon", t, sizeof(t)) && t[0]) {
        double d = strtod(t, NULL);
        if (d >= -180 && d <= 180) g_cfg.wx_lon_e4 = (int32_t)(d * 10000 + (d < 0 ? -0.5 : 0.5));
        wx_changed = true;
    }
    if (field_int(body, "wx_on", &v)) {
        g_cfg.wx_off = !v;
        wx_changed = true;
    }
    if (field_int(body, "wx_f", &v)) {
        g_cfg.wx_f = !!v;
        wx_changed = true;
    }
    if (wx_changed) {
        xSemaphoreTake(g_qmux, portMAX_DELAY);
        memset(&g_wx, 0, sizeof(g_wx));
        g_quotes_gen++;
        xSemaphoreGive(g_qmux);
        weather_kick();
    }
    if (need_reboot) settings_keep_prev(&old);  // boot guard restores this if the new wiring crashes
    esp_err_t err = settings_save();
    display_redraw();
    httpd_resp_set_type(r, "application/json");
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":%s,\"reboot\":%s}", err == ESP_OK ? "true" : "false",
             need_reboot ? "true" : "false");
    httpd_resp_sendstr(r, resp);
    if (need_reboot) schedule_restart(800);
    return ESP_OK;
}

static esp_err_t h_wifi(httpd_req_t *r)
{
    char body[256], ssid[33] = "", pass[65] = "";
    if (read_body(r, body, sizeof(body)) < 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body");
    field(body, "ssid", ssid, sizeof(ssid));
    field(body, "pass", pass, sizeof(pass));
    if (!ssid[0]) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "ssid required");
    httpd_resp_sendstr(r, "{\"ok\":true}");
    wifi_set_credentials(ssid, pass);
    return ESP_OK;
}

// Raw firmware upload: POST /ota with the .bin as the request body.
static esp_err_t h_ota(httpd_req_t *r)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
    esp_ota_handle_t h;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    char *buf = malloc(2048);
    int remaining = r->content_len, total = 0;
    bool first = true;
    while (remaining > 0) {
        int k = httpd_req_recv(r, buf, remaining < 2048 ? remaining : 2048);
        if (k == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (k <= 0) { err = ESP_FAIL; break; }
        if (first && (uint8_t)buf[0] != 0xE9) { err = ESP_ERR_INVALID_ARG; break; }
        first = false;
        err = esp_ota_write(h, buf, k);
        if (err != ESP_OK) break;
        remaining -= k;
        total += k;
    }
    free(buf);
    if (err != ESP_OK) {
        esp_ota_abort(h);
        ESP_LOGE(TAG, "OTA failed after %d bytes: %s", total, esp_err_to_name(err));
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
    }
    err = esp_ota_end(h);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
    ESP_LOGI(TAG, "OTA ok (%d bytes) -> %s, rebooting", total, part->label);
    httpd_resp_sendstr(r, "{\"ok\":true}");
    schedule_restart(1000);
    return ESP_OK;
}

static esp_err_t h_revert(httpd_req_t *r)
{
    esp_err_t err = revert_to_stock();
    if (err != ESP_OK) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
    httpd_resp_sendstr(r, "{\"ok\":true}");
    schedule_restart(1000);
    return ESP_OK;
}



static esp_err_t h_reboot(httpd_req_t *r)
{
    httpd_resp_sendstr(r, "{\"ok\":true}");
    schedule_restart(500);
    return ESP_OK;
}

// Captive-portal style: anything unknown goes to the settings page.
static esp_err_t h_404(httpd_req_t *r, httpd_err_code_t e)
{
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "/");
    return httpd_resp_send(r, NULL, 0);
}


claude_t g_claude;

// POST /api/claude  s=<5h %>&sr=<5h reset epoch>&w=<week %>&wr=<week reset epoch>&plan=<name>
static esp_err_t h_claude(httpd_req_t *r)
{
    char body[200], t[16];
    if (read_body(r, body, sizeof(body)) < 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body");
    int v;
    claude_t c = { 0 };
    if (!field_int(body, "s", &v) || v < 0 || v > 100) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "s");
    c.s_pct = v;
    if (!field_int(body, "w", &v) || v < 0 || v > 100) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "w");
    c.w_pct = v;
    if (field(body, "sr", t, sizeof(t))) c.s_reset = (time_t)strtoll(t, NULL, 10);
    if (field(body, "wr", t, sizeof(t))) c.w_reset = (time_t)strtoll(t, NULL, 10);
    field(body, "plan", c.plan, sizeof(c.plan));
    time(&c.updated);
    // Beep when either limit crosses the threshold; re-arm once it is back under (i.e. after a reset).
    // The first push after boot only sets the state, so a reboot never beeps.
    static bool seen, fired_s, fired_w;
    int thr = g_cfg.alert_claude;
    bool hit_s = thr && c.s_pct >= thr, hit_w = thr && c.w_pct >= thr;
    bool beep = seen && ((hit_s && !fired_s) || (hit_w && !fired_w));
    fired_s = hit_s;
    fired_w = hit_w;
    seen = true;
    if (beep) alert(TUNE_CLAUDE_ID, FOCUS_CLAUDE);
    xSemaphoreTake(g_qmux, portMAX_DELAY);
    g_claude = c;
    g_quotes_gen++;
    xSemaphoreGive(g_qmux);
    httpd_resp_sendstr(r, "{\"ok\":true}");
    return ESP_OK;
}

// POST /api/beep?t=0|1|2 plays a tune now (ignores quiet hours, uses the current volume)
static esp_err_t h_beep(httpd_req_t *r)
{
    char q[16], t[4] = "0";
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) httpd_query_key_value(q, "t", t, sizeof(t));
    int id = atoi(t);
    if (id < 0 || id > TUNE_CLAUDE_ID) id = 0;
    buzzer_play((tune_t)id);
    httpd_resp_sendstr(r, "{\"ok\":true}");
    return ESP_OK;
}


push_t g_push[MAX_PUSH];

// Like field() but keeps a value that was too long for out (it is cut, not dropped).
static bool field_cut(const char *body, const char *key, char *out, size_t n)
{
    esp_err_t e = httpd_query_key_value(body, key, out, n);
    if (e != ESP_OK && e != ESP_ERR_HTTPD_RESULT_TRUNC) return false;
    url_decode(out);
    return true;
}

// POST /api/push  id=<slot name>  [clear=1]  title=  l1=  l2=  l3=  color=RRGGBB  theme=dark|claude
//                 alert=0|1  ttl=<seconds, default 3600>
static esp_err_t h_push(httpd_req_t *r)
{
    char body[640], id[16] = "", t[16];
    if (read_body(r, body, sizeof(body)) < 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body");
    if (!field_cut(body, "id", id, sizeof(id)) || !id[0]) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "id");
    int v;
    bool clear = field_int(body, "clear", &v) && v;
    push_t m = { 0 };
    if (!clear) {
        strlcpy(m.id, id, sizeof(m.id));
        field_cut(body, "title", m.title, sizeof(m.title));
        field_cut(body, "l1", m.line[0], sizeof(m.line[0]));
        field_cut(body, "l2", m.line[1], sizeof(m.line[1]));
        field_cut(body, "l3", m.line[2], sizeof(m.line[2]));
        if (field(body, "color", t, sizeof(t))) {
            unsigned long c = strtoul(t[0] == '#' ? t + 1 : t, NULL, 16);
            m.color = rgb(c >> 16, c >> 8, c);
        }
        if (field(body, "theme", t, sizeof(t))) m.theme = !strcmp(t, "claude");
        m.alert = field_int(body, "alert", &v) && v;
        int ttl = field_int(body, "ttl", &v) && v > 0 ? v : 3600;
        if (ttl > 7 * 86400) ttl = 7 * 86400;
        time(&m.updated);
        m.expires = m.updated + ttl;
    }
    int slot = -1, alerted = 0;
    xSemaphoreTake(g_qmux, portMAX_DELAY);
    for (int i = 0; i < MAX_PUSH; i++) if (!strcmp(g_push[i].id, id)) slot = i;
    if (clear) {
        if (slot >= 0) memset(&g_push[slot], 0, sizeof(push_t));
    } else {
        if (slot < 0) {  // free or expired slot, else the oldest
            slot = 0;
            for (int i = 0; i < MAX_PUSH; i++) {
                if (!g_push[i].id[0] || g_push[i].expires <= m.updated) { slot = i; break; }
                if (g_push[i].updated < g_push[slot].updated) slot = i;
            }
        }
        alerted = m.alert && !(g_push[slot].alert && !strcmp(g_push[slot].id, id) &&
                               !strcmp(g_push[slot].title, m.title) && !strcmp(g_push[slot].line[1], m.line[1]));
        g_push[slot] = m;
    }
    g_quotes_gen++;
    xSemaphoreGive(g_qmux);
    if (alerted) alert(TUNE_CLAUDE_ID, FOCUS_PUSH + slot);
    httpd_resp_sendstr(r, "{\"ok\":true}");
    return ESP_OK;
}

bool web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 15;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 15;
    httpd_handle_t s;
    if (httpd_start(&s, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return false;
    }
    const httpd_uri_t uris[] = {
        { "/", HTTP_GET, h_index, NULL },
        { "/api/state", HTTP_GET, h_state, NULL },
        { "/api/settings", HTTP_POST, h_settings, NULL },
        { "/api/wifi", HTTP_POST, h_wifi, NULL },
        { "/api/reboot", HTTP_POST, h_reboot, NULL },
        { "/api/revert", HTTP_POST, h_revert, NULL },
        { "/ota", HTTP_POST, h_ota, NULL },
        { "/api/claude", HTTP_POST, h_claude, NULL },
        { "/api/beep", HTTP_POST, h_beep, NULL },
        { "/api/push", HTTP_POST, h_push, NULL },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s, &uris[i]);
    httpd_register_err_handler(s, HTTPD_404_NOT_FOUND, h_404);
    return true;
}
