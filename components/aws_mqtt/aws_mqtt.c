#include "aws_mqtt.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_tls.h"
#include "storage.h"
#include "secure_part.h"
#include "sdkconfig.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "ipc.h"

static const char *TAG = "aws_mqtt";

#ifndef CONFIG_AWS_IOT_ENDPOINT
#define CONFIG_AWS_IOT_ENDPOINT "example-ats.iot.region.amazonaws.com"
#endif
#ifndef CONFIG_AWS_CLIENT_ID
#define CONFIG_AWS_CLIENT_ID "esp_grow_controller"
#endif

static esp_mqtt_client_handle_t s_client = NULL;
static mbedtls_x509_crt s_signer_cert;
static bool s_signer_loaded = false;
// cached PEM blob read from esp_secure_cert partition (kept for mqtt TLS pointers)
static uint8_t *s_pem_blob = NULL;
static size_t s_pem_blob_len = 0;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "mqtt connected, subscribing to jobs topic");
        xEventGroupSetBits(g_net_state_event_group, NET_BIT_MQTT_UP);
        // subscribe to jobs accepted topic for this thing
        {
            char topic[256];
            snprintf(topic, sizeof(topic), "$aws/things/%s/jobs/+/notify-next", CONFIG_AWS_CLIENT_ID);
            esp_mqtt_client_subscribe(s_client, topic, 1);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "mqtt disconnected");
        xEventGroupClearBits(g_net_state_event_group, NET_BIT_MQTT_UP);
        break;
    case MQTT_EVENT_DATA: {
        if (event->topic_len > 0) {
            ESP_LOGI(TAG, "mqtt data topic=%.*s", event->topic_len, event->topic);
            // parse JSON payload
            char *payload = strndup(event->data, event->data_len);
            if (!payload) break;
            cJSON *root = cJSON_Parse(payload);
            if (!root) {
                ESP_LOGW(TAG, "job payload not json");
                free(payload);
                break;
            }
            // If job contains a 'manifest' object, treat as manifest-based OTA
            cJSON *manifest = cJSON_GetObjectItem(root, "manifest");
            if (manifest) {
                char *mstr = cJSON_PrintUnformatted(manifest);
                if (mstr) {
                    ESP_LOGI(TAG, "job contains manifest, applying manifest");
                    extern esp_err_t ota_trigger_update(const char *manifest_json);
                    ota_trigger_update(mstr);
                    free(mstr);
                }
            } else {
                // Strict schema: require jobId (string), ota_url (string), signature (base64 string)
                cJSON *jobId = cJSON_GetObjectItem(root, "jobId");
                cJSON *ota = cJSON_GetObjectItem(root, "ota_url");
                cJSON *sig = cJSON_GetObjectItem(root, "signature");
                if (!cJSON_IsString(jobId) || !cJSON_IsString(ota) || !cJSON_IsString(sig)) {
                    ESP_LOGW(TAG, "job payload missing required fields");
                } else {
                    const char *url = ota->valuestring;
                    // base64 decode signature
                    size_t req_len = 0;
                    if (mbedtls_base64_decode(NULL, 0, &req_len, (const unsigned char*)sig->valuestring, strlen(sig->valuestring)) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
                        ESP_LOGW(TAG, "invalid base64 signature length");
                    } else {
                        unsigned char *sig_bin = malloc(req_len);
                        if (!sig_bin) { ESP_LOGW(TAG, "oom decoding signature"); }
                        else {
                            size_t sig_len = req_len;
                            if (mbedtls_base64_decode(sig_bin, req_len, &sig_len, (const unsigned char*)sig->valuestring, strlen(sig->valuestring))==0) {
                                bool sig_ok = false;
                                if (s_signer_loaded) {
                                    unsigned char hash[32];
                                    mbedtls_sha256((const unsigned char*)url, strlen(url), hash, 0);
                                    if (mbedtls_pk_verify(&s_signer_cert.pk, MBEDTLS_MD_SHA256, hash, 0, sig_bin, sig_len) == 0) {
                                        sig_ok = true;
                                    }
                                } else {
                                    ESP_LOGW(TAG, "no signer cert available to verify job");
                                }
                                if (sig_ok) {
                                    ESP_LOGI(TAG, "job %s ota_url verified: %s", jobId->valuestring, url);
                                    // If needed, extend OTA API to support URL+sig; for now, handled via manifest flow.
                                } else {
                                    ESP_LOGW(TAG, "job signature verification failed for job=%s", jobId->valuestring);
                                }
                            } else {
                                ESP_LOGW(TAG, "base64 decode failed");
                            }
                            free(sig_bin);
                        }
                    }
                }
            }
            cJSON_Delete(root);
            free(payload);
        }
        break;
    }
    default:
        break;
    }
}

esp_err_t aws_mqtt_init(void)
{
    // storage init is required for loading certs if they are stored
    esp_err_t err = storage_init();
    if (err != ESP_OK) return err;
    // Try to load and cache TLV partition (CA / cert / key)
    if (!s_pem_blob) {
        uint8_t *ca = NULL, *cert = NULL, *key = NULL; size_t ca_len=0, cert_len=0, key_len=0;
        if (secure_part_read(&s_pem_blob, &s_pem_blob_len, &ca, &ca_len, &cert, &cert_len, &key, &key_len) == ESP_OK) {
            // parse signer cert(s) from CA TLV
            if (ca) {
                mbedtls_x509_crt_init(&s_signer_cert);
                if (mbedtls_x509_crt_parse(&s_signer_cert, ca, ca_len+1) == 0) {
                    s_signer_loaded = true;
                    ESP_LOGI(TAG, "loaded signer cert(s) from esp_secure_cert partition");
                } else {
                    mbedtls_x509_crt_free(&s_signer_cert);
                    ESP_LOGW(TAG, "failed to parse signer cert(s) from partition");
                }
            }
            // keep raw pointers for mqtt tls config by pointing into single cached blob
            secure_part_free(NULL, ca, cert, key); // ca/cert/key copied; keep s_pem_blob as authoritative
        } else {
            ESP_LOGW(TAG, "esp_secure_cert partition not found or invalid TLV");
        }
    }
    ESP_LOGI(TAG, "aws_mqtt initialized (endpoint=%s)", CONFIG_AWS_IOT_ENDPOINT);
    return ESP_OK;
}

esp_err_t aws_mqtt_connect(void)
{
    if (s_client) return ESP_OK;
    // If s_pem_blob is available from aws_mqtt_init, use it to populate cert pointers
    const char *ca_pem = NULL; const char *client_cert_pem = NULL; const char *client_key_pem = NULL;
    if (s_pem_blob) {
        uint8_t *ca = NULL, *cert = NULL, *key = NULL; size_t ca_len=0, cert_len=0, key_len=0;
        if (secure_part_read(&s_pem_blob, &s_pem_blob_len, &ca, &ca_len, &cert, &cert_len, &key, &key_len) == ESP_OK) {
            // secure_part_read returns separate allocated copies; free them after taking pointers into s_pem_blob
            // We will search s_pem_blob for the PEM markers instead to get pointer offsets that remain valid.
            char *s = (char*)s_pem_blob;
            char *pca = strstr(s, "-----BEGIN CERTIFICATE-----");
            if (pca) ca_pem = pca;
            char *pcert = NULL;
            if (pca) pcert = strstr(pca + 1, "-----BEGIN CERTIFICATE-----");
            if (pcert) client_cert_pem = pcert;
            char *pkey = strstr(s, "-----BEGIN PRIVATE KEY-----");
            if (pkey) client_key_pem = pkey;
            secure_part_free(NULL, ca, cert, key);
        }
    }
    esp_mqtt_client_config_t cfg = { 0 };
    cfg.broker.address.uri = "mqtts://" CONFIG_AWS_IOT_ENDPOINT ":8883";
    cfg.broker.verification.certificate = (const char*)ca_pem;
    cfg.credentials.client_id = CONFIG_AWS_CLIENT_ID;
    cfg.credentials.authentication.certificate = (const char*)client_cert_pem;
    cfg.credentials.authentication.key = (const char*)client_key_pem;
    cfg.session.keepalive = 60;
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) return ESP_FAIL;
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t ret = esp_mqtt_client_start(s_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to start mqtt: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "mqtt client started");
    return ESP_OK;
}

esp_err_t aws_publish_shadow(const char *reported_json)
{
    if (!s_client) return ESP_ERR_INVALID_STATE;
    if (!reported_json) return ESP_ERR_INVALID_ARG;
    // Build topic: $aws/things/<thingName>/shadow/update
    char topic[256];
    snprintf(topic, sizeof(topic), "$aws/things/%s/shadow/update", CONFIG_AWS_CLIENT_ID);
    int msg_id = esp_mqtt_client_publish(s_client, topic, reported_json, 0, 1, 0);
    ESP_LOGI(TAG, "published shadow update id=%d", msg_id);
    return ESP_OK;
}

esp_err_t aws_handle_job(const char *job_id, const char *job_doc)
{
    ESP_LOGI(TAG, "AWS Job received: id=%s doc=%s", job_id ? job_id : "(null)", job_doc ? job_doc : "(null)");
    // Parse job_doc for OTA URL and signature and call ota_request_update(url, sig, len)
    // Omitted: actual parsing and security checks; provided as integration hook.
    return ESP_OK;
}

esp_err_t aws_mqtt_publish(const char *topic, const char *data, int len, int qos)
{
    if (!s_client || !topic || !data) return ESP_ERR_INVALID_STATE;
    int msg_id = esp_mqtt_client_publish(s_client, topic, data, len, qos, 0);
    if (msg_id < 0) return ESP_FAIL;
    return ESP_OK;
}
