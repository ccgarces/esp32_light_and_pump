#include "control.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "esp_task_wdt.h"
#include "sdkconfig.h"
#include "ipc.h"

static const char *TAG = "control";

// Kconfig defaults
#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_SPEED_MODE         LEDC_LOW_SPEED_MODE
#define LEDC_RESOLUTION         LEDC_TIMER_8_BIT
#define LEDC_FREQ_HZ            CONFIG_CONTROL_LEDC_FREQ
#define LEDC_LIGHT_CHANNEL      LEDC_CHANNEL_0
#define LEDC_LIGHT_GPIO         CONFIG_CONTROL_LIGHT_GPIO
#define LEDC_PUMP_CHANNEL       LEDC_CHANNEL_1
#define LEDC_PUMP_GPIO          CONFIG_CONTROL_PUMP_GPIO

// Mutex for thread-safe access to LEDC hardware and state
static SemaphoreHandle_t s_ledc_mutex = NULL;
// Last applied control state
static control_state_t s_current_state = {0, 0};

// Forward declaration
static void control_task(void *arg);

// internal helpers
static void ledc_init_hw(void)
{
    // Configure timer
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_SPEED_MODE,
        .duty_resolution = LEDC_RESOLUTION,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));

    // Configure channels for light and pump
    ledc_channel_config_t ch_conf = {
        .speed_mode = LEDC_SPEED_MODE,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };

    ch_conf.channel = LEDC_LIGHT_CHANNEL;
    ch_conf.gpio_num = LEDC_LIGHT_GPIO;
    ESP_ERROR_CHECK(ledc_channel_config(&ch_conf));

    ch_conf.channel = LEDC_PUMP_CHANNEL;
    ch_conf.gpio_num = LEDC_PUMP_GPIO;
    ESP_ERROR_CHECK(ledc_channel_config(&ch_conf));

    // Fade functions allow smooth transitions
    ledc_fade_func_install(0);
}

// Convert percent (0-100) to duty (0..255 for 8-bit)
static inline uint32_t pct_to_duty(uint8_t pct)
{
    if (pct > 100) pct = 100;
    return (uint32_t)((pct * ((1 << LEDC_RESOLUTION) - 1)) / 100);
}

static void apply_duty_locked(uint8_t light_pct, uint8_t pump_pct, uint32_t ramp_ms)
{
    // This function must be called with s_ledc_mutex held
    uint32_t light_duty = pct_to_duty(light_pct);
    uint32_t pump_duty = pct_to_duty(pump_pct);

    ESP_LOGI(TAG, "apply_duty: light=%u%% pump=%u%% ramp=%ums", light_pct, pump_pct, ramp_ms);

    ledc_set_fade_with_time(LEDC_SPEED_MODE, LEDC_LIGHT_CHANNEL, light_duty, ramp_ms);
    ledc_fade_start(LEDC_SPEED_MODE, LEDC_LIGHT_CHANNEL, LEDC_FADE_NO_WAIT);

    ledc_set_fade_with_time(LEDC_SPEED_MODE, LEDC_PUMP_CHANNEL, pump_duty, ramp_ms);
    ledc_fade_start(LEDC_SPEED_MODE, LEDC_PUMP_CHANNEL, LEDC_FADE_NO_WAIT);

    // Update snapshot of the state
    s_current_state.light_pct = light_pct;
    s_current_state.pump_pct = pump_pct;
}

static void control_task(void *arg)
{
    ESP_LOGI(TAG, "control_task starting");
    // Register WDT for this task
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    control_cmd_t cmd;
    for (;;) {
        // Wait for a command from the global queue
        if (xQueueReceive(g_cmd_queue, &cmd, portMAX_DELAY) == pdPASS) {
            ESP_LOGI(TAG, "control_task got cmd: actor=%u seq=%u light=%u pump=%u ramp=%u",
                     cmd.actor, cmd.seq, cmd.light_pct, cmd.pump_pct, cmd.ramp_ms);

            // Clamp values to safe range
            if (cmd.light_pct > 100) cmd.light_pct = 100;
            if (cmd.pump_pct > 100) cmd.pump_pct = 100;

            // Atomically update LEDC outputs
            xSemaphoreTake(s_ledc_mutex, portMAX_DELAY);
            apply_duty_locked(cmd.light_pct, cmd.pump_pct, cmd.ramp_ms);
            xSemaphoreGive(s_ledc_mutex);

            // The ramp is handled by hardware, but for very long ramps,
            // we might need to periodically reset the WDT.
            // A simple delay matching the ramp duration is fine here.
            if (cmd.ramp_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(cmd.ramp_ms));
            }
        }
        // Pet the watchdog to indicate task is alive
        ESP_ERROR_CHECK(esp_task_wdt_reset());
    }
}

esp_err_t control_init(void)
{
    if (s_ledc_mutex != NULL) {
        return ESP_OK; // already initialized
    }

    s_ledc_mutex = xSemaphoreCreateMutex();
    if (!s_ledc_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    // Init LEDC hardware, set safe defaults (OFF)
    ledc_init_hw();
    xSemaphoreTake(s_ledc_mutex, portMAX_DELAY);
    apply_duty_locked(0, 0, 0);
    xSemaphoreGive(s_ledc_mutex);

    // Start control task
    BaseType_t r = xTaskCreatePinnedToCore(control_task, "control_task", 4096, NULL, 6, NULL, tskNO_AFFINITY);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "Failed to create control_task");
        vSemaphoreDelete(s_ledc_mutex);
        s_ledc_mutex = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "control initialized");
    return ESP_OK;
}

esp_err_t control_get_state(control_state_t *out_state)
{
    if (!out_state) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ledc_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_ledc_mutex, portMAX_DELAY);
    memcpy(out_state, &s_current_state, sizeof(s_current_state));
    xSemaphoreGive(s_ledc_mutex);

    return ESP_OK;
}

// Utility for tests: number of steps for a ramp given step_ms granularity
uint32_t control_calc_step_count(uint32_t ramp_ms, uint32_t step_ms)
{
    if (step_ms == 0) return 0;
    return (ramp_ms + step_ms - 1) / step_ms; // ceil
}
