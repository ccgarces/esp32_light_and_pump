#include "ble.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_gatt.h"
#include "host/ble_hs_id.h"
#include "esp_nimble_hci.h"
#include "esp_bt.h"
#if __has_include("esp_bt_controller.h")
#include "esp_bt_controller.h"
#endif
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
// NimBLE store headers vary across IDF versions; prefer nimble/store paths in IDF 5.5
#if __has_include("nimble/store/ble_store_config.h")
#include "nimble/store/ble_store_config.h"
#define BLE_HAVE_STORE_CONFIG 1
#elif __has_include("nimble/store/ble_store.h")
#include "nimble/store/ble_store.h"
#define BLE_HAVE_STORE_API 1
#elif __has_include("store/ble_store_config.h")
#include "store/ble_store_config.h"
#define BLE_HAVE_STORE_CONFIG 1
#elif __has_include("store/ble_store.h")
#include "store/ble_store.h"
#define BLE_HAVE_STORE_API 1
#else
#warning "No NimBLE store headers found; persistence will be disabled"
#endif
#include "esp_task_wdt.h"
#include "ipc.h"
#include "net.h"
#include "cJSON.h"

// NimBLE-based commissioning for ESP32-C3
static const char *TAG = "ble_nimble";

// Hard-coded TEST flag: set to 1 to enable readback of provisioning JSON from an embedded file.
#ifndef BLE_PROV_TEST
#define BLE_PROV_TEST 1
#endif

// 128-bit UUIDs: Service A000..., Characteristic A001 for provisioning JSON
static const ble_uuid128_t prov_svc_uuid = BLE_UUID128_INIT(0x00,0xA0,0x00,0x00,0x00,0x00,0x10,0x00,0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB);
static const ble_uuid128_t prov_chr_uuid = BLE_UUID128_INIT(0x01,0xA0,0x00,0x00,0x00,0x00,0x10,0x00,0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB);

static uint8_t own_addr_type;
static ble_prov_cb_t s_prov_cb = NULL; void *s_prov_arg = NULL;
static volatile bool s_should_adv = false;
static volatile bool s_adv_running = false;
static char s_devname[32] = {0};
static uint8_t s_mac[6] = {0};

// Forward decls
static void start_advertising(void);

static int gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "gap connect status=%d", event->connect.status);
        if (event->connect.status != 0) {
            if (s_should_adv) start_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "gap disconnect");
        if (s_should_adv) start_advertising();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "adv complete");
        s_adv_running = false;
        if (s_should_adv) start_advertising();
        break;
    default: break;
    }
    return 0;
}

static void set_adv_flag(bool on) { s_should_adv = on; }

static void start_advertising(void) {
    int rc;
    struct ble_hs_adv_fields fields; memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const char *name = s_devname[0] ? s_devname : "ESP-C3-PROV";
#ifdef CONFIG_BT_NIMBLE_SVC_GAP_DEVICE_NAME
    if (!s_devname[0] && CONFIG_BT_NIMBLE_SVC_GAP_DEVICE_NAME[0]) name = CONFIG_BT_NIMBLE_SVC_GAP_DEVICE_NAME;
#endif
    fields.name = (uint8_t*)name; fields.name_len = (uint8_t)strlen(name); fields.name_is_complete = 1;
    ble_uuid_any_t svc; memcpy(&svc.u128, &prov_svc_uuid, sizeof(prov_svc_uuid)); svc.u.type = BLE_UUID_TYPE_128;
    fields.uuids128 = &svc.u128; fields.num_uuids128 = 1; fields.uuids128_is_complete = 1;
    rc = ble_gap_adv_set_fields(&fields); if (rc) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return; }

    // Provide BLE MAC in scan response via manufacturer data (0xFFFF + MAC)
    struct ble_hs_adv_fields rsp; memset(&rsp, 0, sizeof(rsp));
    uint8_t mfg[8] = { 0xFF, 0xFF, 0,0,0,0,0,0 };
    memcpy(&mfg[2], s_mac, 6);
    rsp.mfg_data = mfg; rsp.mfg_data_len = sizeof(mfg);
    rc = ble_gap_adv_rsp_set_fields(&rsp); if (rc) { ESP_LOGW(TAG, "adv_rsp_set_fields rc=%d", rc); }

    struct ble_gap_adv_params advp = {0};
    advp.conn_mode = BLE_GAP_CONN_MODE_UND;
    advp.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &advp, gap_event, NULL);
    if (rc == 0) { s_adv_running = true; ESP_LOGI(TAG, "advertising started (name=%s)", name); }
    else if (rc == BLE_HS_EALREADY) { s_adv_running = true; }
    else if (rc == BLE_HS_EBUSY || rc == BLE_HS_EINVAL) { ESP_LOGW(TAG, "adv_start busy/invalid rc=%d", rc); }
    else if (rc == BLE_HS_EUNKNOWN || rc == BLE_HS_ENOTSYNCED) { ESP_LOGW(TAG, "adv_start not ready rc=%d", rc); }
    else { ESP_LOGE(TAG, "adv_start rc=%d", rc); }
}

static int prov_chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint8_t buf[256]; uint16_t len = 0; int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
        if (rc != 0) return BLE_ATT_ERR_UNLIKELY;
        ESP_LOGI(TAG, "prov write len=%u", (unsigned)len);
        cJSON *root = cJSON_ParseWithLength((const char*)buf, len);
        if (!root) return BLE_ATT_ERR_UNLIKELY;
        cJSON *jssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
        cJSON *jpsk  = cJSON_GetObjectItemCaseSensitive(root, "psk");
        cJSON *jtz   = cJSON_GetObjectItemCaseSensitive(root, "tz");
        const char *ssid = cJSON_IsString(jssid) ? jssid->valuestring : NULL;
        const char *psk  = cJSON_IsString(jpsk)  ? jpsk->valuestring  : NULL;
        const char *tz   = cJSON_IsString(jtz)   ? jtz->valuestring   : NULL;
        if (ssid && s_prov_cb) {
            s_prov_cb(ssid, psk, tz, s_prov_arg);
            // Stop advertising and clear BLE_ACTIVE so system proceeds
            set_adv_flag(false);
            ble_gap_adv_stop(); s_adv_running = false;
            if (g_net_state_event_group) xEventGroupClearBits(g_net_state_event_group, NET_BIT_BLE_ACTIVE);
        }
        cJSON_Delete(root);
        return 0;
    }
#if BLE_PROV_TEST
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        // In TEST mode, respond with embedded JSON from a file ignored by git (.gitignore)
        #ifdef BLE_PROV_TEST_EMBED_SYM
        extern const uint8_t _ble_prov_test_start[] asm("_binary_" BLE_PROV_TEST_EMBED_SYM "_start");
        extern const uint8_t _ble_prov_test_end[]   asm("_binary_" BLE_PROV_TEST_EMBED_SYM "_end");
        const uint8_t *data = _ble_prov_test_start;
        size_t data_len = (size_t)(_ble_prov_test_end - _ble_prov_test_start);
        #else
        const uint8_t *data = NULL; size_t data_len = 0;
        #endif
        if (!data || data_len == 0) {
            static const char fallback[] = "{\"ssid\":\"YourSSID\",\"psk\":\"YourPassword\",\"tz\":\"America/Los_Angeles\"}";
            data = (const uint8_t*)fallback; data_len = sizeof(fallback) - 1;
        }
        int rc = os_mbuf_append(ctxt->om, data, (uint16_t)data_len);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
#endif
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static void host_reset_cb(int reason) { ESP_LOGW(TAG, "host reset reason=%d", reason); }

static void host_sync_cb(void) {
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer addr rc=%d", rc); return; }
    uint8_t addr[6]; ble_hs_id_copy_addr(own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "BLE addr %02X:%02X:%02X:%02X:%02X:%02X", addr[5],addr[4],addr[3],addr[2],addr[1],addr[0]);
    memcpy(s_mac, addr, 6);
    // Add MAC suffix to device name for easy identification without serial logs
    snprintf(s_devname, sizeof(s_devname), "ESP-C3-PROV-%02X%02X%02X", addr[2], addr[1], addr[0]);
    ble_svc_gap_device_name_set(s_devname);
    if (s_should_adv) start_advertising();
}

static void ble_host_task(void *param) {
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    nimble_port_run();
    nimble_port_freertos_deinit();
    vTaskDelete(NULL);
}

static void ble_mgr_task(void *param) {
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    bool last_desired = false;
    for (;;) {
        if (g_net_state_event_group) {
            EventBits_t bits = xEventGroupGetBits(g_net_state_event_group);
            bool desired = (bits & NET_BIT_BLE_ACTIVE) != 0;
            if (desired != last_desired) {
                ESP_LOGI(TAG, "BLE_ACTIVE -> %d", desired);
                set_adv_flag(desired);
                if (!desired && s_adv_running) { ble_gap_adv_stop(); s_adv_running = false; }
                if (desired && !s_adv_running) { start_advertising(); }
                last_desired = desired;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

esp_err_t ble_init(void) {
    ESP_LOGI(TAG, "init NimBLE");
    // Initialize controller + HCI and NimBLE host
    esp_err_t err;
    // Init and enable BT controller (BLE mode)
#if defined(ESP_BT_CONTROLLER_INIT_CONFIG_DEFAULT)
    esp_bt_controller_config_t bt_cfg = ESP_BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#elif defined(BT_CONTROLLER_INIT_CONFIG_DEFAULT)
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#else
    esp_bt_controller_config_t bt_cfg = { 0 };
#endif
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { ESP_LOGE(TAG, "bt ctrl init err=%d", err); return err; }
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { ESP_LOGE(TAG, "bt ctrl enable err=%d", err); return err; }
    // Attach NimBLE HCI
    err = esp_nimble_hci_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { ESP_LOGE(TAG, "nimble hci init err=%d", err); return err; }
    nimble_port_init();
    // GAP/GATT default services and NVS-backed store
    ble_svc_gap_init();
    const char *name_cfg = "ESP-C3-PROV";
#ifdef CONFIG_BT_NIMBLE_SVC_GAP_DEVICE_NAME
    if (CONFIG_BT_NIMBLE_SVC_GAP_DEVICE_NAME[0]) name_cfg = CONFIG_BT_NIMBLE_SVC_GAP_DEVICE_NAME;
#endif
    ble_svc_gap_device_name_set(name_cfg);
    ble_svc_gatt_init();
    // Initialize NimBLE store: NVS persistence if available, otherwise RAM store
#if defined(BLE_HAVE_STORE_CONFIG)
    ble_store_config_init();
#endif
    ble_hs_cfg.reset_cb = host_reset_cb;
    ble_hs_cfg.sync_cb = host_sync_cb;

    static const struct ble_gatt_chr_def chrs[] = {
        { .uuid=&prov_chr_uuid.u, .access_cb=prov_chr_access, .flags=
#if BLE_PROV_TEST
            BLE_GATT_CHR_F_READ |
#endif
            BLE_GATT_CHR_F_WRITE|BLE_GATT_CHR_F_WRITE_NO_RSP },
        { 0 }
    };
    static const struct ble_gatt_svc_def svcs[] = {
        { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &prov_svc_uuid.u, .characteristics = chrs },
        { 0 }
    };
    int rc = ble_gatts_count_cfg((struct ble_gatt_svc_def*)svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatts_count rc=%d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs((struct ble_gatt_svc_def*)svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatts_add rc=%d", rc); return ESP_FAIL; }
    nimble_port_freertos_init(ble_host_task);

    // Start manager that follows NET_BIT_BLE_ACTIVE set by net.c
    xTaskCreate(ble_mgr_task, "ble_mgr", 3072, NULL, 5, NULL);

#if BLE_PROV_TEST
    // Force commissioning active in TEST mode regardless of stored creds
    if (g_net_state_event_group) {
        ESP_LOGI(TAG, "TEST mode: forcing BLE commissioning active");
        xEventGroupSetBits(g_net_state_event_group, NET_BIT_BLE_ACTIVE);
    }
#endif
    return ESP_OK;
}

esp_err_t ble_stop(void) {
    set_adv_flag(false);
    if (s_adv_running) { ble_gap_adv_stop(); s_adv_running = false; }
    // Do not fully deinit NimBLE to keep footprint minimal during runtime
    return ESP_OK;
}

void ble_register_prov_callback(ble_prov_cb_t cb, void *arg) { s_prov_cb = cb; s_prov_arg = arg; }
