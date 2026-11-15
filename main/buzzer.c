// Piezo buzzer on GPIO7 (passive, PWM driven; same wiring as the original firmware).
// Tunes are queued and played by a small task so callers never block.
#include "app.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/queue.h"

#define BUZZER_GPIO 7
#define BZ_TIMER LEDC_TIMER_1
#define BZ_CHANNEL LEDC_CHANNEL_1

typedef struct {
    uint16_t hz;  // 0 = rest
    uint16_t ms;
} note_t;

static const note_t TUNE_UP[] = { { 1047, 90 }, { 0, 30 }, { 1319, 90 }, { 0, 30 }, { 1568, 160 }, { 0, 0 } };
static const note_t TUNE_DOWN[] = { { 1568, 90 }, { 0, 30 }, { 1319, 90 }, { 0, 30 }, { 1047, 160 }, { 0, 0 } };
static const note_t TUNE_CLAUDE[] = { { 1760, 140 }, { 0, 90 }, { 1760, 140 }, { 0, 350 },
                                      { 1760, 140 }, { 0, 90 }, { 1760, 140 }, { 0, 0 } };
static const note_t *const TUNES[] = { [TUNE_UP_ID] = TUNE_UP, [TUNE_DOWN_ID] = TUNE_DOWN, [TUNE_CLAUDE_ID] = TUNE_CLAUDE };

static QueueHandle_t s_q;
static const uint8_t VOLUME_DUTY[4] = { 0, 3, 20, 128 };  // 8-bit duty; 50% is loudest for a piezo

static void tone(uint16_t hz)
{
    uint8_t duty = hz ? VOLUME_DUTY[g_cfg.volume & 3] : 0;
    if (hz) ledc_set_freq(LEDC_LOW_SPEED_MODE, BZ_TIMER, hz);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BZ_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BZ_CHANNEL);
}

static void buzzer_task(void *arg)
{
    uint8_t id;
    for (;;) {
        if (xQueueReceive(s_q, &id, portMAX_DELAY) != pdTRUE || id >= sizeof(TUNES) / sizeof(TUNES[0])) continue;
        for (const note_t *n = TUNES[id]; n->ms; n++) {
            tone(n->hz);
            vTaskDelay(pdMS_TO_TICKS(n->ms));
        }
        tone(0);
    }
}

void buzzer_start(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = BZ_TIMER,
        .freq_hz = 2000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t ch = {
        .gpio_num = BUZZER_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BZ_CHANNEL,
        .timer_sel = BZ_TIMER,
        .duty = 0,
    };
    if (ledc_timer_config(&t) != ESP_OK || ledc_channel_config(&ch) != ESP_OK) {
        ESP_LOGE("buzzer", "LEDC setup failed");
        return;
    }
    s_q = xQueueCreate(4, 1);
    xTaskCreate(buzzer_task, "buzzer", 2048, NULL, 2, NULL);
}

void buzzer_play(tune_t tune)
{
    uint8_t id = tune;
    if (s_q) xQueueSend(s_q, &id, 0);
}

bool quiet_now(void)
{
    if (g_cfg.quiet_from == g_cfg.quiet_to) return false;
    time_t now;
    time(&now);
    if (now < 1700000000) return false;
    struct tm tm;
    localtime_r(&now, &tm);
    int h = tm.tm_hour, a = g_cfg.quiet_from, b = g_cfg.quiet_to;
    return a < b ? (h >= a && h < b) : (h >= a || h < b);
}

void alert(tune_t tune, int focus)
{
    display_focus(focus);
    if (g_cfg.volume && !quiet_now()) buzzer_play(tune);
}
