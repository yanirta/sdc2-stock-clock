// Wi-Fi: reuse credentials already stored in NVS (by the stock firmware or by us);
// fall back to an open setup access point when they're missing or don't work.
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/timers.h"

static const char *TAG = "wifi";
net_status_t g_net;

static esp_netif_t *s_sta, *s_ap;
static bool s_portal, s_have_creds, s_sntp;
static int64_t s_start_us, s_last_try_us;
static TimerHandle_t s_tick;

int8_t wifi_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

static void start_portal(void)
{
    if (s_portal) return;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(g_net.ap_ssid, sizeof(g_net.ap_ssid), "StockClock-%02X%02X", mac[4], mac[5]);
    wifi_config_t ap = { 0 };
    strcpy((char *)ap.ap.ssid, g_net.ap_ssid);
    ap.ap.ssid_len = strlen(g_net.ap_ssid);
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 3;
    ap.ap.channel = 6;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    s_portal = true;
    if (g_net.state != NET_CONNECTED) g_net.state = NET_PORTAL;
    ESP_LOGW(TAG, "setup AP up: %s (http://192.168.4.1)", g_net.ap_ssid);
}

static void stop_portal(void)
{
    if (!s_portal) return;
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_portal = false;
    g_net.ap_clients = 0;
    ESP_LOGI(TAG, "setup AP stopped");
}

static void try_connect(void)
{
    if (!s_have_creds) return;
    s_last_try_us = esp_timer_get_time();
    esp_wifi_connect();
}

static void tick(TimerHandle_t t)
{
    int64_t now = esp_timer_get_time();
    if (g_net.state == NET_CONNECTED) {
        // keep the portal a little while after connecting so the setup page can show the new IP
        if (s_portal && now - g_net.connected_at_us > 60 * 1000000LL && g_net.ap_clients == 0) stop_portal();
        return;
    }
    if (!s_portal && (!s_have_creds || now - s_start_us > 25 * 1000000LL)) start_portal();
    // While the portal is up, retry less often: STA scans hop channels and disturb AP clients.
    int64_t gap = s_portal ? 30 * 1000000LL : 5 * 1000000LL;
    if (s_have_creds && now - s_last_try_us > gap) try_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            try_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = data;
            ESP_LOGW(TAG, "disconnected, reason %d", d->reason);
            g_net.ip[0] = 0;
            g_net.state = s_portal ? NET_PORTAL : NET_CONNECTING;
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            g_net.ap_clients++;
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (g_net.ap_clients > 0) g_net.ap_clients--;
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(g_net.ip, sizeof(g_net.ip), IPSTR, IP2STR(&e->ip_info.ip));
        g_net.connected_at_us = esp_timer_get_time();
        g_net.ever_connected = true;
        g_net.state = NET_CONNECTED;
        ESP_LOGI(TAG, "got IP %s", g_net.ip);
        if (!s_sntp) {
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&cfg);
            s_sntp = true;
        }
        quotes_kick();
    }
}

esp_err_t wifi_set_credentials(const char *ssid, const char *pass)
{
    wifi_config_t c = { 0 };
    strlcpy((char *)c.sta.ssid, ssid, sizeof(c.sta.ssid));
    strlcpy((char *)c.sta.password, pass, sizeof(c.sta.password));
    c.sta.threshold.authmode = WIFI_AUTH_OPEN;
    c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    c.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &c);
    if (err != ESP_OK) return err;
    strlcpy(g_net.sta_ssid, ssid, sizeof(g_net.sta_ssid));
    s_have_creds = ssid[0] != 0;
    s_start_us = esp_timer_get_time();
    g_net.state = s_portal ? NET_PORTAL : NET_CONNECTING;
    try_connect();
    return ESP_OK;
}

void wifi_start(void)
{
    s_start_us = esp_timer_get_time();
    g_net.state = NET_STARTING;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, "stock-clock");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    // Credentials saved in NVS by esp_wifi (the stock firmware is ESP-IDF too, so this
    // normally picks up the network the clock was already on).
    wifi_config_t sta = { 0 };
    esp_wifi_get_config(WIFI_IF_STA, &sta);
    s_have_creds = sta.sta.ssid[0] != 0;
    strlcpy(g_net.sta_ssid, (char *)sta.sta.ssid, sizeof(g_net.sta_ssid));
    if (s_have_creds) {
        // Accept any security mode the stored network uses
        sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
        sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        esp_wifi_set_config(WIFI_IF_STA, &sta);
        ESP_LOGI(TAG, "stored network: %s", g_net.sta_ssid);
    } else {
        ESP_LOGW(TAG, "no stored Wi-Fi credentials");
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    g_net.state = NET_CONNECTING;
    ESP_ERROR_CHECK(esp_wifi_start());
    if (!s_have_creds) start_portal();

    s_tick = xTimerCreate("wifitick", pdMS_TO_TICKS(1000), pdTRUE, NULL, tick);
    xTimerStart(s_tick, 0);
}
