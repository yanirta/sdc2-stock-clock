// Shared state for the stock clock firmware.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PROJECT_NAME_ID "stock_clock"

// ---------- settings (NVS namespace "stk", blob "cfg") ----------
#define SETTINGS_VERSION 2  // bumped in 1.0.2 to drop settings that crashed 1.0.1
typedef struct {
    uint32_t version;
    char syms[192];      // comma separated Yahoo symbols
    char tz[64];         // POSIX TZ string
    uint16_t refresh_s;  // quote refresh period
    uint8_t cycle_s;     // seconds per page
    uint8_t bright;      // 0..100
    uint8_t h24;         // 24h clock
    uint8_t overview;    // show overview page in the cycle
    // display wiring (defaults = ESP32-C2 smart weather clock board)
    int8_t pin_sclk, pin_mosi, pin_cs, pin_dc, pin_rst, pin_bl;
    uint8_t bl_inverted;
    uint8_t spi_mode;
    uint8_t invert;   // colour inversion (IPS panels need it)
    uint8_t bgr;      // red/blue order
    uint8_t rot180;   // rotate 180 degrees
    uint8_t spi_mhz;
    // ---- added in 1.0.4 (appended: older stored blobs load with defaults for these)
    uint8_t wx_off;       // hide the weather page (0 = shown; lands in old blobs' zeroed padding)
    uint8_t wx_f;         // Fahrenheit / mph
    int32_t wx_lat_e4;    // latitude * 10000
    int32_t wx_lon_e4;
    char wx_name[32];     // place name shown on the page
    // ---- added in 1.0.7 (beyond older blobs, so they get these defaults)
    uint8_t alert_move_x10;  // beep when a symbol's daily move crosses this many % x10 (0 = off)
    uint8_t alert_claude;    // beep when a Claude limit reaches this % (0 = off)
    uint8_t quiet_from, quiet_to;  // no sound from..to (hours, local); equal = never quiet
    uint8_t volume;          // 0 off, 1 low, 2 medium, 3 high
    // ---- added in 1.0.8; they land in 1.0.7's zeroed tail padding, so 0 must mean "default"
    uint8_t night_bright_p1; // night brightness + 1 (0 = default 10%, 1 = screen off)
    uint8_t night_pages;     // 1 = keep the normal pages at night (0 = big clock, the default)
} settings_t;
#define NIGHT_BRIGHT(c) ((c)->night_bright_p1 ? (c)->night_bright_p1 - 1 : 10)

extern settings_t g_cfg;
void settings_defaults(settings_t *s);
void settings_load(void);
esp_err_t settings_save(void);
// Last known-good copy ("cfg_ok"), kept while a reboot-requiring change is on trial.
void settings_keep_prev(const settings_t *old);  // no-op if a trial is already running
bool settings_restore_prev(void);                 // true if there was one; saves it as current
void settings_clear_prev(void);                   // the current settings proved good
void settings_reset(void);                        // back to defaults and save
// Why the firmware restarted / what the boot guard did (for /api/state)
extern char g_boot_note[96];
extern char g_reset_log[64];

// ---------- quotes ----------
#define MAX_SYMS 10
#define MAX_PTS 100
typedef struct {
    char sym[20];
    char name[32];
    char cur[8];
    double price, prev;
    float pts[MAX_PTS];
    uint8_t npts;
    bool ok;
    time_t mtime;    // market time of last price
    time_t fetched;  // local fetch time
    char err[24];
} quote_t;

extern quote_t g_quotes[MAX_SYMS];
extern int g_nquotes;
extern SemaphoreHandle_t g_qmux;
extern volatile uint32_t g_quotes_gen;  // bumps on every update
void quotes_start(void);
void quotes_kick(void);  // refetch now (e.g. after settings change)
void quotes_set_symbols(const char *csv);

// ---------- weather (Open-Meteo, fetched by the quotes task; guarded by g_qmux) ----------
typedef struct {
    float hi, lo;
    uint8_t code;  // WMO weather code
    uint8_t wday;  // 0 = Sunday
} wx_day_t;
typedef struct {
    bool ok;
    float temp, feels, wind;
    uint8_t hum, code, is_day;
    uint8_t ndays;
    wx_day_t day[4];  // day[0] = today
    time_t fetched;
    char err[24];
} weather_t;
extern weather_t g_wx;
void weather_kick(void);  // refetch now (location or units changed)

// ---------- Claude plan usage, pushed by a script on the user's computer (POST /api/claude) ----------
typedef struct {
    uint8_t s_pct, w_pct;        // 5-hour session and weekly limit used, 0..100
    time_t s_reset, w_reset;     // when each window resets (epoch)
    time_t updated;              // 0 = never
    char plan[12];
} claude_t;
extern claude_t g_claude;        // guarded by g_qmux; changes bump g_quotes_gen
#define CLAUDE_MAX_AGE_S (24 * 3600)  // hide the page when the data is older than this

// ---------- push pages: anything a computer on the network wants shown (POST /api/push) ----------
#define MAX_PUSH 4
typedef struct {
    char id[16];          // "" = free slot
    char title[28];
    char line[3][64];     // big line, normal line, small dim line (wrapped on screen)
    uint16_t color;       // accent (RGB565); 0 = theme default
    uint8_t theme;        // 0 dark, 1 Claude (ivory + spark)
    uint8_t alert;        // keeps coming back every other page until cleared or expired
    time_t updated, expires;
} push_t;
extern push_t g_push[MAX_PUSH];  // guarded by g_qmux; changes bump g_quotes_gen
#define FOCUS_PUSH 200           // display_focus(FOCUS_PUSH + slot)

// ---------- network / status ----------
typedef enum { NET_STARTING, NET_CONNECTING, NET_PORTAL, NET_CONNECTED } net_state_t;
typedef struct {
    volatile net_state_t state;
    volatile bool ever_connected;
    volatile int ap_clients;
    char ip[16];
    char ap_ssid[33];
    char sta_ssid[33];
    volatile int64_t connected_at_us;
} net_status_t;
extern net_status_t g_net;
void wifi_start(void);
esp_err_t wifi_set_credentials(const char *ssid, const char *pass);
int8_t wifi_rssi(void);

// ---------- display ----------
void display_start(void);
void display_set_brightness(uint8_t pct);
void display_redraw(void);
extern volatile bool g_display_ok;

// ---------- web ----------
bool web_start(void);

// ---------- buzzer + alerts ----------
typedef enum { TUNE_UP_ID, TUNE_DOWN_ID, TUNE_CLAUDE_ID } tune_t;
#define FOCUS_CLAUDE 100  // display_focus() target for the Claude page; 0..MAX_SYMS-1 = a symbol
void buzzer_start(void);
void buzzer_play(tune_t tune);
bool quiet_now(void);
void alert(tune_t tune, int focus);  // jump to that page, and beep unless muted or in quiet hours
void display_focus(int focus);  // false if the web server could not start

// ---------- safety / partitions ----------
const char *app_version(void);
bool stock_partition_available(char *label_out, size_t n, char *ver_out, size_t vn);
esp_err_t revert_to_stock(void);  // sets boot partition to stock firmware; caller restarts
void schedule_restart(int ms);
