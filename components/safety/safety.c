#include "safety.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "ipc.h"
#include "sdkconfig.h"
#include <time.h>

static const char *TAG = "safety";

static void safety_task(void *arg)
{
    ESP_LOGI(TAG, "safety_task starting");
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    for (;;) {
        // This task is the highest priority and acts as the system's last-resort watchdog.
        // In a real-world application, it would monitor other critical metrics:
        // - Heap memory low water mark
        // - Other tasks' health via check-in mechanism
        // - Sensor readings for over-current or over-temperature conditions
        // If any of these checks fail, it would call safety_safe_shutdown()
        // and potentially log the event before a planned reset.

        ESP_ERROR_CHECK(esp_task_wdt_reset());
        vTaskDelay(pdMS_TO_TICKS(CONFIG_SAFETY_TASK_INTERVAL_MS));
    }
}

esp_err_t safety_init(void)
{
    // Initialize the Task Watchdog Timer unless IDF already does it via Kconfig
#if CONFIG_ESP_TASK_WDT_INIT
    ESP_LOGI(TAG, "Task WDT enabled by Kconfig (timeout=%ds); skipping init", CONFIG_ESP_TASK_WDT_TIMEOUT_S);
#else
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = CONFIG_SAFETY_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_init(&wdt_cfg));
#endif

    // Create the high-priority safety task
    BaseType_t r = xTaskCreate(safety_task, "safety_task", 2048, NULL, 7, NULL);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "Failed to create safety_task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Safety component initialized (WDT timeout: %ds)", CONFIG_SAFETY_WDT_TIMEOUT_S);
    return ESP_OK;
}

esp_err_t safety_safe_shutdown(void)
{
    ESP_LOGW(TAG, "Performing safe shutdown: disabling all actuators immediately.");

    control_cmd_t cmd = {
        .actor = ACTOR_SAFETY,
        .ts = time(NULL),
        .light_pct = 0,
        .pump_pct = 0,
        .ramp_ms = 0, // Apply instantly
    };

    // Use xQueueSendToFront to ensure this critical command is processed immediately.
    if (xQueueSendToFront(g_cmd_queue, &cmd, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to send shutdown command to control queue. Actuators may remain on.");
        return ESP_FAIL;
    }

    return ESP_OK;
}

