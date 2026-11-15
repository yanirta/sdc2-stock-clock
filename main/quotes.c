// Fetches quotes from Yahoo Finance's chart endpoint (no API key needed).
// One request per symbol: https://query1.finance.yahoo.com/v8/finance/chart/<SYM>?range=1d&interval=15m
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "gfx.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "quotes";

quote_t g_quotes[MAX_SYMS];
int g_nquotes;
SemaphoreHandle_t g_qmux;
volatile uint32_t g_quotes_gen;
static TaskHandle_t s_task;

#define RX_CAP (16 * 1024)
static char *s_rx;
static int s_rxlen;
static bool s_overflow;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0) {
        int n = e->data_len;
        if (s_rxlen + n >= RX_CAP) {
            n = RX_CAP - 1 - s_rxlen;
            s_overflow = true;
        }
        if (n > 0) {
            memcpy(s_rx + s_rxlen, e->data, n);
            s_rxlen += n;
        }
    }
    return ESP_OK;
}

static bool num_after(const char *buf, const char *key, double *out)
{
    const char *p = strstr(buf, key);
    if (!p) return false;
    p += strlen(key);
    if (!strncmp(p, "null", 4)) return false;
    char *end;
    double v = strtod(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

static void str_after(const char *buf, const char *key, char *out, size_t n)
{
    out[0] = 0;
    const char *p = strstr(buf, key);
    if (!p) return;
    p += strlen(key);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) {
        if (*p == '\\') {
            if (!strncmp(p, "\\u0026", 6)) { out[i++] = '&'; p += 6; continue; }
            if (p[1] == 'u') { p += 6; continue; }  // drop other unicode escapes
            p += 2;
            continue;
        }
        out[i++] = *p++;
    }
    out[i] = 0;
}

static int parse_closes(const char *buf, float *pts, int max)
{
    const char *p = strstr(buf, "\"close\":[");
    if (!p) return 0;
    p += 9;
    static float tmp[300];
    int n = 0;
    while (*p && *p != ']' && n < 300) {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']') break;
        if (!strncmp(p, "null", 4)) { p += 4; continue; }
        char *end;
        double v = strtod(p, &end);
        if (end == p) break;
        tmp[n++] = (float)v;
        p = end;
    }
    if (n <= max) {
        memcpy(pts, tmp, n * sizeof(float));
        return n;
    }
    // Downsample, always keeping the last point
    for (int i = 0; i < max; i++) {
        int src = (int)((long)i * (n - 1) / (max - 1));
        pts[i] = tmp[src];
    }
    return max;
}

static void url_encode_sym(const char *sym, char *out, size_t n)
{
    size_t j = 0;
    for (const char *p = sym; *p && j + 4 < n; p++) {
        if (isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == '_') {
            out[j++] = *p;
        } else {
            j += snprintf(out + j, n - j, "%%%02X", (unsigned char)*p);
        }
    }
    out[j] = 0;
}

void quotes_set_symbols(const char *csv)
{
    xSemaphoreTake(g_qmux, portMAX_DELAY);
    int nold = g_nquotes;
    quote_t *old = malloc(sizeof(quote_t) * (nold ? nold : 1));  // too big for task stacks
    if (old) memcpy(old, g_quotes, sizeof(quote_t) * nold);
    else nold = 0;
    memset(g_quotes, 0, sizeof(g_quotes));
    g_nquotes = 0;
    const char *p = csv;
    while (*p && g_nquotes < MAX_SYMS) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        char sym[20];
        int i = 0;
        while (*p && *p != ',' && *p != ' ' && i < 19) sym[i++] = toupper((unsigned char)*p++);
        sym[i] = 0;
        while (*p && *p != ',') p++;
        if (!i) continue;
        quote_t *q = &g_quotes[g_nquotes++];
        strcpy(q->sym, sym);
        for (int k = 0; k < nold; k++) {  // keep old data so the screen isn't blank
            if (!strcmp(old[k].sym, sym)) { *q = old[k]; break; }
        }
    }
    free(old);
    g_quotes_gen++;
    xSemaphoreGive(g_qmux);
}


// ---------------------------------------------------------------- price-move alerts

// Per-symbol alert memory, keyed by symbol so it survives list edits and re-fetches.
static struct {
    char sym[20];
    double prev;  // previous close this state belongs to (a new trading day re-arms)
    int8_t dir;   // direction already alerted: +1, -1, or 0
} s_al[MAX_SYMS + 4];
static int s_al_next;

static void check_move_alert(int idx, const char *sym, double price, double prev)
{
    if (prev <= 0) return;
    double pct = (price - prev) / prev * 100.0, thr = g_cfg.alert_move_x10 / 10.0;
    int n = sizeof(s_al) / sizeof(s_al[0]), e = -1;
    for (int i = 0; i < n; i++) if (!strcmp(s_al[i].sym, sym)) e = i;
    int dir = pct >= thr ? 1 : pct <= -thr ? -1 : 0;
    if (e < 0) {  // first time we see this symbol: remember silently, no beep at boot
        e = s_al_next++ % n;
        strlcpy(s_al[e].sym, sym, sizeof(s_al[e].sym));
        s_al[e].prev = prev;
        s_al[e].dir = thr > 0 ? dir : 0;
        return;
    }
    if (s_al[e].prev != prev) {  // new trading day
        s_al[e].prev = prev;
        s_al[e].dir = 0;
    }
    if (thr <= 0) return;
    if (s_al[e].dir && fabs(pct) < thr / 2) s_al[e].dir = 0;  // calmed down: re-arm
    if (dir && dir != s_al[e].dir) {
        s_al[e].dir = dir;
        ESP_LOGW(TAG, "%s moved %d.%d%%", sym, (int)pct, abs((int)(pct * 10)) % 10);
        alert(dir > 0 ? TUNE_UP_ID : TUNE_DOWN_ID, idx);
    }
}

static void fetch_one(esp_http_client_handle_t cli, int idx)
{
    char sym[20], enc[64], url[160];
    xSemaphoreTake(g_qmux, portMAX_DELAY);
    if (idx >= g_nquotes) { xSemaphoreGive(g_qmux); return; }
    strcpy(sym, g_quotes[idx].sym);
    xSemaphoreGive(g_qmux);

    url_encode_sym(sym, enc, sizeof(enc));
    snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=1d&interval=15m", enc);
    esp_http_client_set_url(cli, url);
    s_rxlen = 0;
    s_overflow = false;
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    s_rx[s_rxlen] = 0;

    quote_t q = {0};
    strcpy(q.sym, sym);
    if (err != ESP_OK || status != 200) {
        if (err != ESP_OK) snprintf(q.err, sizeof(q.err), "%s", esp_err_to_name(err));
        else snprintf(q.err, sizeof(q.err), "http %d", status);
        ESP_LOGW(TAG, "%s: %s", sym, q.err);
    } else {
        double price, prev = 0, mt = 0;
        if (num_after(s_rx, "\"regularMarketPrice\":", &price)) {
            if (!num_after(s_rx, "\"chartPreviousClose\":", &prev)) num_after(s_rx, "\"previousClose\":", &prev);
            num_after(s_rx, "\"regularMarketTime\":", &mt);
            q.price = price;
            q.prev = prev;
            q.mtime = (time_t)mt;
            str_after(s_rx, "\"shortName\":\"", q.name, sizeof(q.name));
            str_after(s_rx, "\"currency\":\"", q.cur, sizeof(q.cur));
            q.npts = parse_closes(s_rx, q.pts, MAX_PTS);
            if (q.npts > 0) q.pts[q.npts - 1] = (float)price;  // end chart at the live price
            q.ok = true;
            time(&q.fetched);
        } else {
            snprintf(q.err, sizeof(q.err), s_overflow ? "too big" : "no price");
            ESP_LOGW(TAG, "%s: %s (%d bytes)", sym, q.err, s_rxlen);
        }
    }

    xSemaphoreTake(g_qmux, portMAX_DELAY);
    if (idx < g_nquotes && !strcmp(g_quotes[idx].sym, sym)) {
        if (q.ok) {
            g_quotes[idx] = q;
        } else {
            strcpy(g_quotes[idx].err, q.err);  // keep last good data, note the error
        }
        g_quotes_gen++;
    }
    xSemaphoreGive(g_qmux);
    if (q.ok) check_move_alert(idx, sym, q.price, q.prev);
}


// ---------------------------------------------------------------- weather (Open-Meteo, plain HTTP, no key)

weather_t g_wx;
static int64_t s_wx_next;  // esp_timer time of the next weather fetch

void weather_kick(void)
{
    s_wx_next = 0;
    quotes_kick();
}

// Reads up to max numbers from the JSON array that follows key ("key":[1,2,null,...]).
static int num_array(const char *buf, const char *key, float *out, int max)
{
    const char *p = strstr(buf, key);
    if (!p) return 0;
    p = strchr(p + strlen(key), '[');
    if (!p) return 0;
    p++;
    int n = 0;
    while (*p && *p != ']' && n < max) {
        char *end;
        double v = strtod(p, &end);
        out[n++] = end == p ? 0 : (float)v;
        p = end == p ? p + 1 : end;
        while (*p && *p != ',' && *p != ']') p++;
        if (*p == ',') p++;
    }
    return n;
}

// Day of week for an ISO date (Sakamoto), 0 = Sunday.
static uint8_t iso_wday(const char *d)
{
    int y = atoi(d), m = atoi(d + 5), dd = atoi(d + 8);
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 1 || m > 12) return 0;
    if (m < 3) y--;
    return (uint8_t)((y + y / 4 - y / 100 + y / 400 + t[m - 1] + dd) % 7);
}

static void fetch_weather(void)
{
    char lat[16], lon[16], url[360];
    fmt_fixed(lat, sizeof(lat), g_cfg.wx_lat_e4 / 10000.0, 4, false);
    fmt_fixed(lon, sizeof(lon), g_cfg.wx_lon_e4 / 10000.0, 4, false);
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,is_day"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=4%s",
             lat, lon, g_cfg.wx_f ? "&temperature_unit=fahrenheit&wind_speed_unit=mph" : "");
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = on_http,
        .timeout_ms = 10000,
        .user_agent = "StockClock",
        .buffer_size = 1024,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return;
    s_rxlen = 0;
    s_overflow = false;
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    s_rx[s_rxlen] = 0;

    weather_t w = { 0 };
    const char *cur = strstr(s_rx, "\"current\":{");
    const char *daily = strstr(s_rx, "\"daily\":{");
    double v;
    if (err != ESP_OK || status != 200 || !cur || !daily) {
        snprintf(w.err, sizeof(w.err), err != ESP_OK ? "network error" : "HTTP %d", status);
    } else if (num_after(cur, "\"temperature_2m\":", &v)) {
        w.temp = v;
        if (num_after(cur, "\"apparent_temperature\":", &v)) w.feels = v;
        if (num_after(cur, "\"wind_speed_10m\":", &v)) w.wind = v;
        if (num_after(cur, "\"relative_humidity_2m\":", &v)) w.hum = (uint8_t)v;
        if (num_after(cur, "\"weather_code\":", &v)) w.code = (uint8_t)v;
        if (num_after(cur, "\"is_day\":", &v)) w.is_day = (uint8_t)v;
        float code[4], hi[4], lo[4];
        int n = num_array(daily, "\"weather_code\":", code, 4);
        if (num_array(daily, "\"temperature_2m_max\":", hi, 4) < n) n = 0;
        if (num_array(daily, "\"temperature_2m_min\":", lo, 4) < n) n = 0;
        const char *dates = strstr(daily, "\"time\":[");
        for (int i = 0; i < n; i++) {
            w.day[i] = (wx_day_t){ .hi = hi[i], .lo = lo[i], .code = (uint8_t)code[i] };
            const char *d = dates ? strchr(dates + 8, '"') : NULL;  // nth "yyyy-mm-dd"
            for (int k = 0; d && k < i; k++) d = strchr(strchr(d + 1, '"') + 1, '"');
            if (d) w.day[i].wday = iso_wday(d + 1);
        }
        w.ndays = n;
        w.ok = true;
        time(&w.fetched);
    } else {
        strcpy(w.err, "bad response");
    }
    xSemaphoreTake(g_qmux, portMAX_DELAY);
    if (w.ok || !g_wx.ok) g_wx = w;  // keep showing the last good reading on a failed refresh
    else strlcpy(g_wx.err, w.err, sizeof(g_wx.err));
    g_quotes_gen++;
    xSemaphoreGive(g_qmux);
    s_wx_next = esp_timer_get_time() + (w.ok ? 15 * 60 : 2 * 60) * 1000000LL;
}

static void quotes_task(void *arg)
{
    s_rx = heap_caps_malloc(RX_CAP, MALLOC_CAP_8BIT);
    if (!s_rx) {
        ESP_LOGE(TAG, "no memory for rx buffer");
        vTaskDelete(NULL);
    }
    for (;;) {
        if (g_net.state != NET_CONNECTED) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
            continue;
        }
        esp_http_client_config_t cfg = {
            .url = "https://query1.finance.yahoo.com/",
            .event_handler = on_http,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 10000,
            .user_agent = "Mozilla/5.0 (StockClock)",
            .keep_alive_enable = true,
            .buffer_size = 2048,
            .buffer_size_tx = 1024,
        };
        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        if (cli) {
            int n = g_nquotes;
            for (int i = 0; i < n && g_net.state == NET_CONNECTED; i++) {
                fetch_one(cli, i);
                vTaskDelay(pdMS_TO_TICKS(150));
            }
            esp_http_client_cleanup(cli);
        }
        if (!g_cfg.wx_off && g_net.state == NET_CONNECTED && esp_timer_get_time() >= s_wx_next) fetch_weather();
        ESP_LOGI(TAG, "refresh done, free heap %u, min %u", (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size());
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((g_cfg.refresh_s < 15 ? 15 : g_cfg.refresh_s) * 1000));
    }
}

void quotes_kick(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

void quotes_start(void)
{
    g_qmux = xSemaphoreCreateMutex();
    quotes_set_symbols(g_cfg.syms);
    xTaskCreate(quotes_task, "quotes", 8192, NULL, 4, &s_task);
}
