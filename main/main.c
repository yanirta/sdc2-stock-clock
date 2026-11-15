// Stock/forex ticker firmware for the ESP32-C2 "Smart Weather Clock".
//
// Safety net (there is no USB-serial on this clock, so a bad image must never strand it):
//  * The stock firmware stays untouched in the other app partition.
//  * A boot "fails" if the clock restarts before it has run healthily (Wi-Fi up for 30 s).
//    Once this build has proven itself, repeated failures are blamed on settings first:
//    3 failures -> restore the last settings that worked (or defaults), 6 -> defaults,
//    10 -> original firmware. A build that never proved itself goes back after 5.
//  * Until this build has once reached Wi-Fi, it switches back to stock after 10 minutes
//    without a connection (extended while someone is using the setup access point).
//  * If the bootloader has rollback enabled, the image is only marked valid once healthy.
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

static const char *TAG = "main";

#define MAX_FAILED_BOOTS_NEW 5       // unproven build -> stock (after trying the last good settings)
#define FAILS_RESTORE_SETTINGS 3     // proven build -> last good settings
#define FAILS_DEFAULT_SETTINGS 6     // -> factory defaults
#define FAILS_STOCK 10               // -> original firmware, last resort

char g_boot_note[96];
char g_reset_log[64];  // newest first, e.g. "SW PANIC PANIC PWR"

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "PWR";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INTWDT";
    case ESP_RST_TASK_WDT: return "TASKWDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "SLEEP";
    default: return "OTHER";
    }
}
#define FIRST_RUN_WIFI_DEADLINE_S 600
#define PORTAL_GRACE_S 900

const char *app_version(void) { return esp_app_get_description()->version; }

static const esp_partition_t *find_stock(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *found = NULL;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it) {
        const esp_partition_t *p = esp_partition_get(it);
        esp_app_desc_t d;
        if (p->address != run->address && esp_ota_get_partition_description(p, &d) == ESP_OK &&
            strcmp(d.project_name, PROJECT_NAME_ID) != 0) {
            found = p;
            break;
        }
        it = esp_partition_next(it);
    }
    if (it) esp_partition_iterator_release(it);
    return found;
}

bool stock_partition_available(char *label_out, size_t n, char *ver_out, size_t vn)
{
    const esp_partition_t *p = find_stock();
    if (!p) return false;
    esp_app_desc_t d;
    if (label_out) strlcpy(label_out, p->label, n);
    if (ver_out && esp_ota_get_partition_description(p, &d) == ESP_OK) {
        snprintf(ver_out, vn, "%s %s", d.project_name[0] ? d.project_name : "original", d.version);
    }
    return true;
}

esp_err_t revert_to_stock(void)
{
    const esp_partition_t *p = find_stock();
    if (!p) {
        ESP_LOGE(TAG, "no stock firmware partition found");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = esp_ota_set_boot_partition(p);  // verifies the image first
    ESP_LOGW(TAG, "revert to stock (%s): %s", p->label, esp_err_to_name(err));
    return err;
}

// True if something accepts TCP connections on our own port 80 (the web UI is our only way in).
static bool web_alive(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct timeval tv = { .tv_sec = 3 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(80) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0;
    close(fd);
    return ok;
}

static void restart_cb(void *arg) { esp_restart(); }

void schedule_restart(int ms)
{
    static esp_timer_handle_t t;
    if (!t) {
        esp_timer_create_args_t a = { .callback = restart_cb, .name = "restart" };
        esp_timer_create(&a, &t);
    }
    esp_timer_stop(t);
    esp_timer_start_once(t, (uint64_t)ms * 1000);
}

static void build_id(char *out, size_t n)
{
    esp_app_get_elf_sha256(out, n);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Stock Clock %s starting from %s", app_version(), esp_ota_get_running_partition()->label);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }

    // ---- crash-loop guard
    nvs_handle_t h = 0;
    uint8_t boots = 0;
    bool verified = false;
    char id[17], vid[17] = "";
    build_id(id, sizeof(id));
    if (err == ESP_OK && nvs_open("stk", NVS_READWRITE, &h) == ESP_OK) {
        // reset history, newest first
        char log[sizeof(g_reset_log)] = "";
        size_t len = sizeof(log);
        nvs_get_str(h, "rstlog", log, &len);
        strlcpy(g_reset_log, reset_name(esp_reset_reason()), sizeof(g_reset_log));
        strlcat(g_reset_log, " ", sizeof(g_reset_log));
        strlcat(g_reset_log, log, sizeof(g_reset_log));  // older entries fall off the end
        char *cut = strrchr(g_reset_log, ' ');  // keep whole words only
        if (strlen(g_reset_log) > sizeof(g_reset_log) - 12 && cut) *cut = 0;
        nvs_set_str(h, "rstlog", g_reset_log);

        nvs_get_u8(h, "boots", &boots);
        len = sizeof(vid);
        if (nvs_get_str(h, "verified", vid, &len) == ESP_OK) verified = !strcmp(vid, id);
        boots++;
        nvs_set_u8(h, "boots", boots);
        nvs_commit(h);
        ESP_LOGI(TAG, "boot attempt %u, build %s, verified=%d, resets: %s", boots, id, verified, g_reset_log);
        if (!verified && boots >= MAX_FAILED_BOOTS_NEW) {
            nvs_set_u8(h, "boots", 0);
            nvs_commit(h);
            if (revert_to_stock() == ESP_OK) esp_restart();
        }
        if (verified && boots >= FAILS_STOCK) {
            nvs_set_u8(h, "boots", 0);
            nvs_commit(h);
            if (revert_to_stock() == ESP_OK) esp_restart();
        }
    }

    settings_load();
    if (boots == FAILS_RESTORE_SETTINGS) {
        if (settings_restore_prev()) {
            settings_load();
            snprintf(g_boot_note, sizeof(g_boot_note), "Kept crashing after a settings change; restored the previous settings.");
        } else {
            settings_reset();
            snprintf(g_boot_note, sizeof(g_boot_note), "Kept crashing; reset all settings to defaults.");
        }
    } else if (verified && boots == FAILS_DEFAULT_SETTINGS) {
        settings_clear_prev();
        settings_reset();
        snprintf(g_boot_note, sizeof(g_boot_note), "Kept crashing; reset all settings to defaults.");
    }
    if (g_boot_note[0]) {
        ESP_LOGW(TAG, "%s", g_boot_note);
        if (h) {
            nvs_set_str(h, "note", g_boot_note);
            nvs_commit(h);
        }
    } else if (h) {
        size_t len = sizeof(g_boot_note);
        nvs_get_str(h, "note", g_boot_note, &len);  // keep showing the last recovery
    }
    setenv("TZ", g_cfg.tz, 1);
    tzset();

    quotes_start();  // creates the quote mutex used by the display
    display_start();
    buzzer_start();
    wifi_start();
    bool web_ok = web_start();
    int web_fails = 0;

    // ---- health watch
    bool healthy = false;
    int64_t deadline = FIRST_RUN_WIFI_DEADLINE_S;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t up = esp_timer_get_time() / 1000000;
        // The web UI is the only way to reach this clock: if it is down, this boot does not count as
        // healthy, and restarting (which the boot guard counts) is how we get out.
        if (up % 30 == 0) web_fails = web_ok && web_alive() ? 0 : web_fails + 1;
        if (web_fails >= 3) {
            ESP_LOGE(TAG, "web server not answering, restarting");
            esp_restart();
        }
        if (!healthy && web_ok && web_fails == 0 &&
            ((g_net.state == NET_CONNECTED && up > 30) || (verified && up > 60))) {
            healthy = true;
            if (h) {
                nvs_set_u8(h, "boots", 0);
                settings_clear_prev();  // the current settings work
                if (g_net.state == NET_CONNECTED && !verified) {
                    nvs_set_str(h, "verified", id);
                    verified = true;
                }
                nvs_commit(h);
            }
            esp_ota_img_states_t st;
            if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
                st == ESP_OTA_IMG_PENDING_VERIFY) {
                esp_ota_mark_app_valid_cancel_rollback();
            }
            ESP_LOGI(TAG, "healthy (verified=%d), heap %u", verified, (unsigned)esp_get_free_heap_size());
        }
        if (!verified && !g_net.ever_connected) {
            if (g_net.ap_clients > 0 && up + PORTAL_GRACE_S > deadline) deadline = up + PORTAL_GRACE_S;
            if (up > deadline) {
                ESP_LOGE(TAG, "never reached Wi-Fi on first run, going back to stock firmware");
                if (revert_to_stock() == ESP_OK) esp_restart();
                deadline = up + FIRST_RUN_WIFI_DEADLINE_S;  // nothing to revert to; keep trying
            }
        }
    }
}
