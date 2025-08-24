#include "ble.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_bt_defs.h"
#include "esp_bt_controller.h"
#include "esp_bt_main.h"
#include "esp_gatts_api.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_task_wdt.h"
#include "crypto.h"
#include "storage.h"
#include "cJSON.h"
#include "ipc.h"
#include "sdkconfig.h"

static const char *TAG = "ble";

// GATT Service and Characteristic UUIDs
#define PROV_SERVICE_UUID           0xA000
#define CHAR_SSID_UUID              0xA001
#define CHAR_PSK_UUID               0xA002
#define CHAR_TZ_UUID                0xA003
#define CHAR_CONTROL_UUID           0xA004 // Write encrypted commands
#define CHAR_TELEMETRY_UUID         0xA005 // Notify encrypted telemetry

// Advertising parameters
static esp_ble_adv_params_t adv_params = {
    .adv_int_min        = 0x20,
    .adv_int_max        = 0x40,
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// GATT Profile and handles
#define PROFILE_NUM                 1
#define PROFILE_APP_ID              0
#define SVC_INST_ID                 0

struct gatts_profile_inst {
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_handle;
    esp_gatt_srvc_id_t service_id;
    uint16_t char_ssid_handle;
    uint16_t char_psk_handle;
    uint16_t char_tz_handle;
    uint16_t char_control_handle;
    uint16_t char_telemetry_handle;
};

static struct gatts_profile_inst profile_tab[PROFILE_NUM] = {
    [PROFILE_APP_ID] = {
        .gatts_cb = gatts_profile_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,
    },
};

// State
static TaskHandle_t s_ble_task_handle = NULL;
static ble_prov_cb_t s_prov_cb = NULL;
static void *s_prov_arg = NULL;
static char s_ssid[32], s_psk[64], s_tz[64];

// Secure session state
static uint8_t s_session_key[32];
static bool s_session_ready = false;
static uint32_t s_peer_counter = 0;       // last accepted counter
static uint64_t s_peer_window = 0;        // 64-bit sliding window bitmask
static const char *STO_KEY_COUNTER = "ble_peer_counter";
static const char *STO_KEY_WINDOW  = "ble_peer_window";

static void replay_state_load(void) {
    uint32_t c = 0; size_t len = sizeof(c);
    if (storage_load_config(STO_KEY_COUNTER, &c, &len) == ESP_OK && len == sizeof(c)) {
        s_peer_counter = c;
    }
    uint64_t w = 0; len = sizeof(w);
    if (storage_load_config(STO_KEY_WINDOW, &w, &len) == ESP_OK && len == sizeof(w)) {
        s_peer_window = w;
    }
}

static void replay_state_save(void) {
    storage_save_config(STO_KEY_COUNTER, &s_peer_counter, sizeof(s_peer_counter));
    storage_save_config(STO_KEY_WINDOW, &s_peer_window, sizeof(s_peer_window));
}

// Returns true if counter is new within window; updates window
static bool replay_accept_and_update(uint32_t ctr) {
    if (!s_session_ready) return false;
    if (ctr > s_peer_counter) {
        uint32_t delta = ctr - s_peer_counter;
        if (delta >= 64) {
            s_peer_window = 1ULL; // only latest bit set
        } else {
            s_peer_window <<= delta;
            s_peer_window |= 1ULL;
        }
        s_peer_counter = ctr;
        replay_state_save();
        return true;
    }
    // ctr <= s_peer_counter: check within window
    uint32_t back = s_peer_counter - ctr;
    if (back >= 64) return false; // too old
    uint64_t mask = (1ULL << back);
    if (s_peer_window & mask) return false; // already seen
    s_peer_window |= mask;
    replay_state_save();
    return true;
}

// Handle handshake JSON on control characteristic when session not ready
// Expect: { "cmd":"handshake", "client_pub": <65-byte uncompressed hex>, "pop":"..." }
static bool handle_handshake(const uint8_t *buf, size_t len) {
    cJSON *root = cJSON_ParseWithLength((const char*)buf, len);
    if (!root) return false;
    bool ok = false;
    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "handshake") == 0) {
        cJSON *pubhex = cJSON_GetObjectItemCaseSensitive(root, "client_pub");
        cJSON *pop = cJSON_GetObjectItemCaseSensitive(root, "pop");
        if (cJSON_IsString(pubhex) && pubhex->valuestring && cJSON_IsString(pop) && pop->valuestring) {
            // Convert pubkey hex
            const char *hx = pubhex->valuestring;
            size_t hlen = strlen(hx);
            if (hlen == 130) {
                uint8_t peer_pub[65];
                for (int i=0;i<65;i++) {
                    char c1=hx[i*2], c2=hx[i*2+1];
                    uint8_t v1 = (uint8_t)((c1>='0'&&c1<='9')?c1-'0':(c1>='a'&&c1<='f')?c1-'a'+10:(c1>='A'&&c1<='F')?c1-'A'+10:255);
                    uint8_t v2 = (uint8_t)((c2>='0'&&c2<='9')?c2-'0':(c2>='a'&&c2<='f')?c2-'a'+10:(c2>='A'&&c2<='F')?c2-'A'+10:255);
                    if (v1==255||v2==255) { cJSON_Delete(root); return false; }
                    peer_pub[i] = (uint8_t)((v1<<4)|v2);
                }
                // Generate ephemeral keypair and compute shared
                uint8_t our_pub[65]; size_t our_pub_len = sizeof(our_pub);
                void *ctx = NULL;
                if (crypto_ecdh_generate_keypair(our_pub, &our_pub_len, &ctx) == 0) {
                    uint8_t secret[32]; size_t secret_len = sizeof(secret);
                    if (crypto_ecdh_compute_shared(ctx, peer_pub, sizeof(peer_pub), secret, &secret_len) == 0) {
                        // Derive session key with HKDF, bind PoP in info
                        if (crypto_hkdf_sha256((const uint8_t*)"BLE-POP", 7, secret, secret_len,
                                               (const uint8_t*)pop->valuestring, strlen(pop->valuestring),
                                               s_session_key, sizeof(s_session_key)) == 0) {
                            s_session_ready = true;
                            s_peer_counter = 0; s_peer_window = 0; replay_state_save();
                            ok = true;
                            ESP_LOGI(TAG, "BLE secure session established");
                        }
                    }
                    crypto_ecdh_free(ctx);
                }
            }
        }
    }
    cJSON_Delete(root);
    return ok;
}

// Decrypt and apply control message; format: {"ctr":N,"ramp_ms":ms,"light":p,"pump":p}
static void handle_encrypted_control(const uint8_t *data, size_t len) {
    if (!s_session_ready || len < (12+16)) { ESP_LOGW(TAG, "control: no session or too short"); return; }
    const uint8_t *iv = data;           // 12-byte nonce
    const uint8_t *ct = data + 12;      // ciphertext
    size_t ct_len = len - 12 - 16;
    const uint8_t *tag = data + 12 + ct_len; // 16-byte tag
    uint8_t pt[256]; if (ct_len > sizeof(pt)) { ESP_LOGW(TAG, "control: msg too large"); return; }
    int rc = crypto_aes_gcm_decrypt(s_session_key, sizeof(s_session_key), iv, 12, NULL, 0, tag, 16, pt);
    if (rc != 0) { ESP_LOGW(TAG, "control: decrypt fail"); return; }
    // parse JSON
    cJSON *root = cJSON_ParseWithLength((const char*)pt, ct_len);
    if (!root) { ESP_LOGW(TAG, "control: bad JSON"); return; }
    cJSON *ctr = cJSON_GetObjectItemCaseSensitive(root, "ctr");
    if (!cJSON_IsNumber(ctr)) { cJSON_Delete(root); return; }
    uint32_t ctrv = (uint32_t)cJSON_GetNumberValue(ctr);
    if (!replay_accept_and_update(ctrv)) { ESP_LOGW(TAG, "control: replay rejected"); cJSON_Delete(root); return; }
    control_cmd_t cmd = {0};
    cmd.actor = ACTOR_BLE; cmd.ts = 0; cmd.seq = ctrv;
    cJSON *ramp = cJSON_GetObjectItemCaseSensitive(root, "ramp_ms");
    cJSON *light = cJSON_GetObjectItemCaseSensitive(root, "light");
    cJSON *pump = cJSON_GetObjectItemCaseSensitive(root, "pump");
    if (cJSON_IsNumber(ramp)) cmd.ramp_ms = (uint32_t)cJSON_GetNumberValue(ramp);
    if (cJSON_IsNumber(light)) cmd.light_pct = (uint8_t)cJSON_GetNumberValue(light);
    if (cJSON_IsNumber(pump)) cmd.pump_pct = (uint8_t)cJSON_GetNumberValue(pump);
    xQueueSend(g_cmd_queue, &cmd, 0);
    cJSON_Delete(root);
}

// Forward declarations
static void ble_manager_task(void *arg);
static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
        // Create service and set up advertising
        replay_state_load();
        profile_tab[PROFILE_APP_ID].gatts_if = gatts_if;
        profile_tab[PROFILE_APP_ID].service_id.is_primary = true;
        profile_tab[PROFILE_APP_ID].service_id.id.inst_id = SVC_INST_ID;
        profile_tab[PROFILE_APP_ID].service_id.id.uuid.len = ESP_UUID_LEN_16;
        profile_tab[PROFILE_APP_ID].service_id.id.uuid.uuid.uuid16 = PROV_SERVICE_UUID;
        esp_ble_gatts_create_service(gatts_if, &profile_tab[PROFILE_APP_ID].service_id, 10);
        // Simple advertising payload (name + service UUID)
        esp_ble_gap_set_device_name(CONFIG_BLE_DEVICE_NAME);
        esp_ble_adv_data_t adv = {0};
        adv.name_type = BLE_ADV_NAME_COMPLETE;
        adv.include_txpower = false;
        adv.flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
        adv.service_uuid_len = 2;
        uint8_t svc16[2] = { (uint8_t)(PROV_SERVICE_UUID & 0xFF), (uint8_t)(PROV_SERVICE_UUID >> 8) };
        adv.p_service_uuid = svc16;
        esp_ble_gap_config_adv_data(&adv);
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
    case ESP_GATTS_CREATE_EVT: {
        profile_tab[PROFILE_APP_ID].service_handle = param->create.service_handle;
        esp_ble_gatts_start_service(profile_tab[PROFILE_APP_ID].service_handle);
        // Add characteristics with write properties
        esp_bt_uuid_t uuid;
        uuid.len = ESP_UUID_LEN_16;
        // SSID
        uuid.uuid.uuid16 = CHAR_SSID_UUID;
        esp_gatt_char_prop_t prop = ESP_GATT_CHAR_PROP_BIT_WRITE;
        esp_ble_gatts_add_char(profile_tab[PROFILE_APP_ID].service_handle, &uuid, ESP_GATT_PERM_WRITE, prop, NULL, NULL);
        // PSK
        uuid.uuid.uuid16 = CHAR_PSK_UUID; esp_ble_gatts_add_char(profile_tab[PROFILE_APP_ID].service_handle, &uuid, ESP_GATT_PERM_WRITE, prop, NULL, NULL);
        // TZ
        uuid.uuid.uuid16 = CHAR_TZ_UUID; esp_ble_gatts_add_char(profile_tab[PROFILE_APP_ID].service_handle, &uuid, ESP_GATT_PERM_WRITE, prop, NULL, NULL);
        // CONTROL
        uuid.uuid.uuid16 = CHAR_CONTROL_UUID; esp_ble_gatts_add_char(profile_tab[PROFILE_APP_ID].service_handle, &uuid, ESP_GATT_PERM_WRITE, prop, NULL, NULL);
        // TELEMETRY notify char could be added similarly if needed
        break; }
    case ESP_GATTS_ADD_CHAR_EVT: {
        // Track handles based on UUID
        uint16_t uuid16 = param->add_char.char_uuid.uuid.uuid16;
        if (uuid16 == CHAR_SSID_UUID) profile_tab[PROFILE_APP_ID].char_ssid_handle = param->add_char.attr_handle;
        else if (uuid16 == CHAR_PSK_UUID) profile_tab[PROFILE_APP_ID].char_psk_handle = param->add_char.attr_handle;
        else if (uuid16 == CHAR_TZ_UUID) profile_tab[PROFILE_APP_ID].char_tz_handle = param->add_char.attr_handle;
        else if (uuid16 == CHAR_CONTROL_UUID) profile_tab[PROFILE_APP_ID].char_control_handle = param->add_char.attr_handle;
        break; }
        esp_ble_gap_start_advertising(&adv_params);
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising start failed");
        } else {
            ESP_LOGI(TAG, "Advertising started");
        }
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising stop failed");
        } else {
            ESP_LOGI(TAG, "Advertising stopped");
        }
        break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        ESP_LOGI(TAG, "update connection params status = %d, min_int = %d, max_int = %d,conn_int = %d,latency = %d, timeout = %d",
                 param->update_conn_params.status,
                 param->update_conn_params.min_int,
                 param->update_conn_params.max_int,
                 param->update_conn_params.conn_int,
                 param->update_conn_params.latency,
                 param->update_conn_params.timeout);
        break;
    default:
        break;
    }
}

static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        // Service and advertising configuration
        break;
    case ESP_GATTS_READ_EVT:
        // Read events (if any characteristics were readable)
        break;
    case ESP_GATTS_WRITE_EVT: {
        if (!param->write.is_prep) {
            ESP_LOGI(TAG, "GATT_WRITE_EVT, handle = %d, value len = %d", param->write.handle, param->write.len);
            if (param->write.handle == profile_tab[PROFILE_APP_ID].char_ssid_handle) {
                size_t n = param->write.len < sizeof(s_ssid)-1 ? param->write.len : sizeof(s_ssid)-1;
                memcpy(s_ssid, param->write.value, n); s_ssid[n] = '\0';
            } else if (param->write.handle == profile_tab[PROFILE_APP_ID].char_psk_handle) {
                size_t n = param->write.len < sizeof(s_psk)-1 ? param->write.len : sizeof(s_psk)-1;
                memcpy(s_psk, param->write.value, n); s_psk[n] = '\0';
            } else if (param->write.handle == profile_tab[PROFILE_APP_ID].char_tz_handle) {
                size_t n = param->write.len < sizeof(s_tz)-1 ? param->write.len : sizeof(s_tz)-1;
                memcpy(s_tz, param->write.value, n); s_tz[n] = '\0';
                // This is the last piece of data, trigger the callback
                if (s_prov_cb) {
                    s_prov_cb(s_ssid, s_psk, s_tz, s_prov_arg);
                }
            } else if (param->write.handle == profile_tab[PROFILE_APP_ID].char_control_handle) {
                // Placeholder for handling encrypted control commands
                ESP_LOGI(TAG, "Control command received (decryption needed)");
            }
            // Send response if needed
            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
            }
        }
        break;
    }
    // ... other GATT events
    default:
        break;
    }
}

static void ble_manager_task(void *arg)
{
    ESP_LOGI(TAG, "ble_manager_task starting");
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    bool is_ble_active = false;

    switch (event) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        // Service and advertising configuration (skeleton; real descriptors omitted for brevity)
        replay_state_load();
        break;

        if ((bits & NET_BIT_BLE_ACTIVE) && !is_ble_active) {
            ESP_LOGI(TAG, "Starting BLE services...");
            // Initialize stack and register callbacks
            ESP_ERROR_CHECK(esp_bluedroid_init());
            ESP_ERROR_CHECK(esp_bluedroid_enable());
            ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_profile_event_handler));
            ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
            ESP_ERROR_CHECK(esp_ble_gatts_app_register(PROFILE_APP_ID));
            is_ble_active = true;
        } else if (!(bits & NET_BIT_BLE_ACTIVE) && is_ble_active) {
            ESP_LOGI(TAG, "Stopping BLE services...");
            esp_ble_gap_stop_advertising();
            esp_bluedroid_disable();
            esp_bluedroid_deinit();
            esp_bt_controller_disable();
            esp_bt_controller_deinit();
            esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
            is_ble_active = false;
        }
                // If not established, treat as handshake JSON
                if (!s_session_ready) {
                    if (!handle_handshake(param->write.value, param->write.len)) {
                        ESP_LOGW(TAG, "handshake failed");
                    }
                } else {
                    handle_encrypted_control(param->write.value, param->write.len);
                }

esp_err_t ble_init(void)
{
    // Initialize BT controller
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "%s initialize controller failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }

    // Create the manager task
    BaseType_t r = xTaskCreate(ble_manager_task, "ble_mgr_task", 4096, NULL, 5, &s_ble_task_handle);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ble_manager_task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BLE component initialized");
    return ESP_OK;
}

esp_err_t ble_stop(void)
{
    if (s_ble_task_handle) {
        vTaskDelete(s_ble_task_handle);
        s_ble_task_handle = NULL;
    }
    // The task itself will handle de-initialization.
    return ESP_OK;
}

void ble_register_prov_callback(ble_prov_cb_t cb, void *arg)
{
    s_prov_cb = cb;
    s_prov_arg = arg;
}

