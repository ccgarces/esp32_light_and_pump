#include "aht10.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "storage.h"
#include <string.h>

static const char *TAG = "aht10";
#define I2C_MASTER_SCL_IO 22
#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_FREQ_HZ 100000
#define AHT10_ADDR 0x38

void aht10_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_NUM_0, &conf);
    i2c_driver_install(I2C_NUM_0, conf.mode, 0, 0, 0);
    ESP_LOGI(TAG, "I2C initialized for AHT10");
}

static esp_err_t aht10_read_raw(uint8_t *data)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (AHT10_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, 0xE1, true); // soft reset
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    vTaskDelay(pdMS_TO_TICKS(50));

    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (AHT10_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, 0xAC, true); // measure
    i2c_master_write_byte(cmd, 0x33, true);
    i2c_master_write_byte(cmd, 0x00, true);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    vTaskDelay(pdMS_TO_TICKS(80));

    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (AHT10_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, data, 6, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

bool aht10_read(float *temperature, float *humidity)
{
    uint8_t data[6];
    if (aht10_read_raw(data) != ESP_OK) return false;
    // parse
    uint32_t raw_h = ((uint32_t)(data[1] & 0x0F) << 16) | ((uint32_t)data[2] << 8) | data[3];
    uint32_t raw_t = ((uint32_t)(data[3] & 0xF0) << 12) | ((uint32_t)data[4] << 8) | data[5];
    float h = (raw_h * 100.0f) / 1048576.0f;
    float t = ((raw_t * 200.0f) / 1048576.0f) - 50.0f;
    *temperature = t;
    *humidity = h;
    ESP_LOGI(TAG, "AHT10 t=%.2fC h=%.2f%%", t, h);
    return true;
}

void aht10_hourly_task(void *arg)
{
    while (1) {
        float t = 0, h = 0;
        if (aht10_read(&t, &h)) {
            int64_t now = esp_timer_get_time() / 1000000;
            storage_log_sensor_reading(now, t, h);
            mqtt_publish_sensor(now, t, h);
        }
        vTaskDelay(pdMS_TO_TICKS(60 * 60 * 1000)); // 1 hour
    }
}
