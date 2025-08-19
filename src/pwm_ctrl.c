#include "pwm_ctrl.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "pwm_ctrl";

#define LIGHT_GPIO 25
#define PUMP_GPIO 26

void pwm_ctrl_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t light_channel = {
        .gpio_num = LIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&light_channel);

    ledc_channel_config_t pump_channel = {
        .gpio_num = PUMP_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&pump_channel);
    ESP_LOGI(TAG, "PWM initialized (light GPIO=%d pump GPIO=%d)", LIGHT_GPIO, PUMP_GPIO);
}

void pwm_set_light_duty(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t duty = (percent * ((1 << 13) - 1)) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void pwm_set_pump_duty(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t duty = (percent * ((1 << 13) - 1)) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}
