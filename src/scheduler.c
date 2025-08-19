#include "scheduler.h"
#include "pwm_ctrl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>

static const char *TAG = "scheduler";
static int on_hour = 7;
static int on_min = 0;
static int off_hour = 21;
static int off_min = 0;

void scheduler_init(void)
{
    // load schedule from NVS in future; keep defaults
}

static bool is_time_between_now(int sh, int sm, int eh, int em)
{
    time_t now = time(NULL);
    struct tm tmnow;
    localtime_r(&now, &tmnow);
    int cur = tmnow.tm_hour * 60 + tmnow.tm_min;
    int start = sh * 60 + sm;
    int end = eh * 60 + em;
    if (start <= end) {
        return cur >= start && cur < end;
    } else {
        // overnight schedule
        return cur >= start || cur < end;
    }
}

void scheduler_task(void *arg)
{
    while (1) {
        bool on = is_time_between_now(on_hour, on_min, off_hour, off_min);
        if (on) {
            // set default light to 100 and pump to 100 - they can be changed via MQTT
            pwm_set_light_duty(100);
            pwm_set_pump_duty(100);
        } else {
            pwm_set_light_duty(0);
            pwm_set_pump_duty(0);
        }
    vTaskDelay(pdMS_TO_TICKS(60 * 1000)); // check every minute
    }
}

*** End Patch
