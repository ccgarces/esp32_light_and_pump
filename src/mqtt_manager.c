#include "mqtt_manager.h"
#include "esp_log.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "pwm_ctrl.h"
#include "storage.h"

static const char *TAG = "mqtt_manager";
static esp_mqtt_client_handle_t client = NULL;

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            esp_mqtt_client_subscribe(client, "device/cmd/#", 0);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT topic=%.*s data=%.*s", event->topic_len, event->topic, event->data_len, event->data);
            // very simple command parsing
            if (strncmp(event->topic, "device/cmd/light", event->topic_len) == 0) {
                int val = atoi(event->data);
                pwm_set_light_duty((uint8_t)val);
            } else if (strncmp(event->topic, "device/cmd/pump", event->topic_len) == 0) {
                int val = atoi(event->data);
                pwm_set_pump_duty((uint8_t)val);
            } else if (strncmp(event->topic, "device/cmd/provision", event->topic_len) == 0) {
                // payload: ssid;pass
                char buf[128];
                snprintf(buf, sizeof(buf), "%.*s", event->data_len, event->data);
                char *sep = strchr(buf, ';');
                if (sep) {
                    *sep = '\0';
                    storage_set_wifi_credentials(buf, sep + 1);
                }
            } else if (strncmp(event->topic, "device/cmd/ota", event->topic_len) == 0) {
                // payload could be URL - for now trigger internal OTA manager to check broker
                ota_manager_request_update();
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

void mqtt_manager_init(void)
{
    esp_mqtt_client_config_t cfg = {0};
    /* Use broker.address.uri which some esp-idf versions expect */
    cfg.broker.address.uri = "mqtt://broker.hivemq.com:1883";
    client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, (esp_event_handler_t)mqtt_event_handler_cb, NULL);
    esp_mqtt_client_start(client);
}

void mqtt_publish_sensor(int64_t timestamp, float temperature, float humidity)
{
    if (!client) return;
    char payload[128];
    snprintf(payload, sizeof(payload), "{\"ts\":%lld,\"t\":%.2f,\"h\":%.2f}", timestamp, temperature, humidity);
    esp_mqtt_client_publish(client, "device/sensor", payload, 0, 1, 0);
}
