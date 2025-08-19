/*
 * Main firmware for esp32_light_and_pump
 * Implements: WiFi (STA), MQTT client, I2C AHT10 reads, PWM control (LEDC), NVS storage,
 * hourly sensor logging, schedule-based light/pump control, OTA trigger via MQTT.
 *
 * NOTE: BLE commissioning is left as a TODO (see README). For now, WiFi/MQTT credentials
 * can be stored via NVS using the simple CLI helper in storage.c or via MQTT provisioning
 * topic when the device first connects.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_sntp.h"

#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "aht10.h"
#include "pwm_ctrl.h"
#include "storage.h"
#include "scheduler.h"
#include "ota_manager.h"

static const char *TAG = "main";

void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronized");
}

static void sntp_init_and_wait(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    sntp_init();

    /* wait for time to be set */
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 10;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    time(&now);
    localtime_r(&now, &timeinfo);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "Initializing storage");
    storage_init();

    ESP_LOGI(TAG, "Initializing PWM controller");
    pwm_ctrl_init();

    ESP_LOGI(TAG, "Initializing I2C and AHT10 sensor");
    aht10_init();

    ESP_LOGI(TAG, "Starting WiFi manager");
    wifi_manager_init();

    /* Wait until WiFi is connected or timeout */
    if (wifi_manager_wait_connected(pdMS_TO_TICKS(15000))) {
        ESP_LOGI(TAG, "Connected to WiFi, init SNTP and MQTT");
        sntp_init_and_wait();
        mqtt_manager_init();
    } else {
        ESP_LOGW(TAG, "WiFi not connected - MQTT and SNTP will be delayed until connection");
    }

    ESP_LOGI(TAG, "Initializing scheduler (default schedule 07:00-21:00)");
    scheduler_init();

    ESP_LOGI(TAG, "Initializing OTA manager");
    ota_manager_init();

    /* Sensor task: read AHT10 hourly and store */
    xTaskCreatePinnedToCore(aht10_hourly_task, "aht10_task", 4 * 1024, NULL, 5, NULL, tskNO_AFFINITY);

    /* Scheduler task manages scheduled on/off for light and pump */
    xTaskCreatePinnedToCore(scheduler_task, "scheduler", 4 * 1024, NULL, 5, NULL, tskNO_AFFINITY);

    /* MQTT task runs inside mqtt_manager; it will publish sensor data when ready */

    ESP_LOGI(TAG, "Firmware initialization complete");
}
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/ledc.h"
#include "driver/i2c.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "lwip/sockets.h"

static const char *TAG = "grow_firmware";

// Configurable pins - change if needed for your board
#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_OUTPUT_IO_LIGHT    (18) // PWM pin for grow light
#define LEDC_OUTPUT_IO_PUMP     (19) // PWM pin for pump
#define LEDC_CHANNEL_LIGHT      LEDC_CHANNEL_0
#define LEDC_CHANNEL_PUMP       LEDC_CHANNEL_1
#define LEDC_DUTY_RES           LEDC_TIMER_13_BIT
#define LEDC_FREQUENCY          (5000) // 5 kHz

// I2C config for AHT10
#define I2C_MASTER_NUM          I2C_NUM_0
#define I2C_SDA_IO              21
#define I2C_SCL_IO              22
#define I2C_FREQ_HZ             100000
#define AHT10_ADDR              0x38

// Storage keys and limits
#define NVS_NAMESPACE           "storage"
#define MAX_ENTRIES             256

typedef struct {
    uint32_t timestamp;
    float temperature_c;
    float humidity_rh;
} sensor_entry_t;

// Forward declarations
static esp_err_t i2c_master_init(void);
static esp_err_t aht10_read(float *temperature, float *humidity);
static void pwm_init(void);
static void set_light_duty(uint32_t duty);
static void set_pump_duty(uint32_t duty);
static void hourly_sensor_task(void *arg);
static void wifi_connect_from_nvs(void);
static void time_sync(void);
static void scheduler_task(void *arg);
static void tcp_control_task(void *arg);

// --- I2C AHT10 implementation ---
static esp_err_t i2c_master_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = I2C_SCL_IO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) return err;
    return i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

// AHT10 measurement: send 0xAC 0x33 0x00, wait ~80ms, then read 6 bytes
static esp_err_t aht10_read(float *temperature, float *humidity)
{
    if (!temperature || !humidity) return ESP_ERR_INVALID_ARG;
    uint8_t cmd[3] = {0xAC, 0x33, 0x00};
    i2c_cmd_handle_t handle = i2c_cmd_link_create();
    i2c_master_start(handle);
    i2c_master_write_byte(handle, (AHT10_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(handle, cmd, sizeof(cmd), true);
    i2c_master_stop(handle);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, handle, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AHT10: trigger command failed: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(80));

    uint8_t data[6] = {0};
    handle = i2c_cmd_link_create();
    i2c_master_start(handle);
    i2c_master_write_byte(handle, (AHT10_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(handle, data, 6, I2C_MASTER_LAST_NACK);
    i2c_master_stop(handle);
    ret = i2c_master_cmd_begin(I2C_MASTER_NUM, handle, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AHT10: read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Parse data: 20-bit humidity, 20-bit temperature
    uint32_t raw_h = ((uint32_t)(data[1] & 0x0F) << 16) | ((uint32_t)data[2] << 8) | data[3];
    uint32_t raw_t = ((uint32_t)(data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];
    *humidity = ((float)raw_h) * 100.0f / 1048576.0f; // 2^20 = 1048576
    *temperature = ((float)raw_t) * 200.0f / 1048576.0f - 50.0f;
    return ESP_OK;
}

// --- PWM (LEDC) ---
static void pwm_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz = LEDC_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t light_channel = {
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL_LIGHT,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LEDC_OUTPUT_IO_LIGHT,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&light_channel);

    ledc_channel_config_t pump_channel = {
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL_PUMP,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LEDC_OUTPUT_IO_PUMP,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&pump_channel);
}

static void set_light_duty(uint32_t duty)
{
    if (duty > ((1 << LEDC_DUTY_RES) - 1)) duty = (1 << LEDC_DUTY_RES) - 1;
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_LIGHT, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_LIGHT);
}

static void set_pump_duty(uint32_t duty)
{
    if (duty > ((1 << LEDC_DUTY_RES) - 1)) duty = (1 << LEDC_DUTY_RES) - 1;
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_PUMP, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_PUMP);
}

// --- NVS storage helpers (ring buffer) ---
static esp_err_t nvs_store_entry(sensor_entry_t *entry)
{
    if (!entry) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    uint32_t next = 0;
    err = nvs_get_u32(handle, "entry_next", &next);
    if (err == ESP_ERR_NVS_NOT_FOUND) next = 0;
    else if (err != ESP_OK) { nvs_close(handle); return err; }

    char key[32];
    snprintf(key, sizeof(key), "entry_%u", next);
    err = nvs_set_blob(handle, key, entry, sizeof(sensor_entry_t));
    if (err != ESP_OK) { nvs_close(handle); return err; }

    next = (next + 1) % MAX_ENTRIES;
    nvs_set_u32(handle, "entry_next", next);
    uint32_t count = 0;
    err = nvs_get_u32(handle, "entry_count", &count);
    if (err == ESP_ERR_NVS_NOT_FOUND) count = 0;
    else if (err != ESP_OK) { nvs_close(handle); return err; }
    if (count < MAX_ENTRIES) count++;
    nvs_set_u32(handle, "entry_count", count);

    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

// --- Hourly sensor task ---
static void hourly_sensor_task(void *arg)
{
    ESP_LOGI(TAG, "Hourly sensor task started");
    for (;;) {
        float temp = 0, hum = 0;
        if (aht10_read(&temp, &hum) == ESP_OK) {
            sensor_entry_t e = {0};
            e.timestamp = (uint32_t)time(NULL);
            e.temperature_c = temp;
            e.humidity_rh = hum;
            if (nvs_store_entry(&e) == ESP_OK) {
                ESP_LOGI(TAG, "Stored sensor reading: t=%.2fC h=%.2f%%", temp, hum);
            } else {
                ESP_LOGW(TAG, "Failed to store sensor reading");
            }
        } else {
            ESP_LOGW(TAG, "Failed to read AHT10 sensor");
        }
        // Sleep for one hour
        const TickType_t delay_ticks = pdMS_TO_TICKS(60 * 60 * 1000);
        vTaskDelay(delay_ticks);
    }
}

// --- WiFi connect from saved NVS credentials ---
static EventGroupHandle_t s_wifi_event_group;
const int CONNECTED_BIT = BIT0;

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
        ESP_LOGI(TAG, "Disconnected. Reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
        ESP_LOGI(TAG, "Got IP address");
        time_sync();
    }
}

static void wifi_connect_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed for WiFi creds: %s", esp_err_to_name(err));
        return;
    }
    size_t ssid_len = 0, pass_len = 0;
    err = nvs_get_str(handle, "wifi_ssid", NULL, &ssid_len);
    if (err != ESP_OK) { ESP_LOGW(TAG, "No WiFi SSID stored"); nvs_close(handle); return; }
    char *ssid = malloc(ssid_len);
    nvs_get_str(handle, "wifi_ssid", ssid, &ssid_len);
    nvs_get_str(handle, "wifi_pass", NULL, &pass_len);
    char *pass = NULL;
    if (pass_len > 0) {
        pass = malloc(pass_len);
        nvs_get_str(handle, "wifi_pass", pass, &pass_len);
    }
    nvs_close(handle);

    esp_netif_init();
    s_wifi_event_group = xEventGroupCreate();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

    wifi_config_t wifi_config = { 0 };
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid)-1);
    if (pass) strncpy((char*)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password)-1);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid);

    // free
    free(ssid);
    if (pass) free(pass);
}

// --- SNTP time sync ---
static void time_sync(void)
{
    // read timezone from NVS
    nvs_handle_t handle;
    char tz[64] = "UTC";
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t tz_len = sizeof(tz);
        if (nvs_get_str(handle, "timezone", tz, &tz_len) != ESP_OK) {
            strncpy(tz, "UTC", sizeof(tz));
        }
        nvs_close(handle);
    }
    setenv("TZ", tz, 1);
    tzset();

    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    ESP_LOGI(TAG, "SNTP initialized, timezone=%s", tz);
}

// --- Scheduler: default on 7:00 off 21:00 ---
static void scheduler_task(void *arg)
{
    int on_h = 7, on_m = 0, off_h = 21, off_m = 0;
    // load from nvs if available
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_i32(handle, "on_hour", &on_h);
        nvs_get_i32(handle, "on_min", &on_m);
        nvs_get_i32(handle, "off_hour", &off_h);
        nvs_get_i32(handle, "off_min", &off_m);
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "Scheduler on %02d:%02d off %02d:%02d", on_h, on_m, off_h, off_m);

    for (;;) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        if ((tm.tm_hour > on_h || (tm.tm_hour == on_h && tm.tm_min >= on_m)) &&
            (tm.tm_hour < off_h || (tm.tm_hour == off_h && tm.tm_min < off_m))) {
            // within on window -> ensure lights on
            set_light_duty((1 << LEDC_DUTY_RES) - 1);
        } else {
            // off
            set_light_duty(0);
        }
        // run every 30 seconds
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

// --- Simple TCP control server ---
static void tcp_control_task(void *arg)
{
    const int port = 3333;
    struct sockaddr_in server_addr;
    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket");
        vTaskDelete(NULL);
        return;
    }
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Socket bind failed");
        closesocket(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listen_sock, 1) < 0) {
        ESP_LOGE(TAG, "Listen failed");
        closesocket(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "TCP control server listening on %d", port);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        ESP_LOGI(TAG, "Client connected");
        char buf[128];
        int len = recv(client_sock, buf, sizeof(buf)-1, 0);
        if (len > 0) {
            buf[len] = '\0';
            ESP_LOGI(TAG, "Received: %s", buf);
            // simple parser: SET LIGHT <duty>, SET PUMP <duty>
            if (strncmp(buf, "SET LIGHT", 9) == 0) {
                int value = atoi(buf + 9);
                uint32_t duty = (uint32_t)value;
                set_light_duty(duty);
                send(client_sock, "OK\n", 3, 0);
            } else if (strncmp(buf, "SET PUMP", 8) == 0) {
                int value = atoi(buf + 8);
                uint32_t duty = (uint32_t)value;
                set_pump_duty(duty);
                send(client_sock, "OK\n", 3, 0);
            } else if (strncmp(buf, "GET STATUS", 10) == 0) {
                char out[128];
                snprintf(out, sizeof(out), "STATUS OK\n");
                send(client_sock, out, strlen(out), 0);
            } else {
                send(client_sock, "ERR Unknown command\n", 21, 0);
            }
        }
        closesocket(client_sock);
    }
}

// --- Application entry point ---
void app_main(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "Starting grow controller firmware");

    // init nvs
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // init i2c
    ESP_ERROR_CHECK(i2c_master_init());

    // init pwm
    pwm_init();

    // start WiFi (if creds present)
    wifi_connect_from_nvs();

    // start tasks
    xTaskCreate(hourly_sensor_task, "hourly_sensor", 4096, NULL, 5, NULL);
    xTaskCreate(scheduler_task, "scheduler", 4096, NULL, 5, NULL);
    xTaskCreate(tcp_control_task, "tcp_control", 4096, NULL, 5, NULL);

    // Placeholder: OTA and BLE provisioning should be implemented by app author.
    ESP_LOGI(TAG, "Initialization complete");
}
