// ble_nimble.c – NimBLE-based BLE commissioning (ESP32-C3, ESP-IDF v5.5)
// -----------------------------------------------------------------------------
// Pre-production commissioning flow:
//   • On boot, try Wi‑Fi with stored creds. BLE stays OFF initially.
//   • If Wi‑Fi+time aren’t up within CONFIG_NET_BLE_FALLBACK_SEC → enable BLE ads.
//   • Mobile/app writes JSON {ssid, psk, tz} to provisioning characteristic:
//       - Stop advertising, clear NET_BIT_BLE_ACTIVE
//       - Invoke user callback to persist creds & kick Wi‑Fi
//   • After Wi‑Fi stays connected for CONFIG_NET_WIFI_STABLE_MIN seconds → keep BLE OFF.
//   • If provisioning happened but Wi‑Fi still not connected after 3 minutes → re‑enable BLE.
//   • GATT DB registered only after host sync.
//   • Controller+HCI brought up via nimble_port_init() (IDF ≥ 5).
//
// Test credentials (build-time injection):
//   Define these at compile time to simulate app provisioning without BLE:
//     -D SIM_WIFI_SSID="YourSSID" [-D SIM_WIFI_PSK="YourPass"] [-D SIM_TZ="Region/City"]
//   (Optionally via Kconfig symbols CONFIG_SIM_WIFI_SSID/PSK/TZ—handled below.)
//   Injection reuses the *exact* post-provisioning codepath.
//
// Security:
//   Default characteristic is writable without encryption to simplify field
//   commissioning. To require pairing before write, build with -D BLE_PROV_REQUIRE_ENC=1
// -----------------------------------------------------------------------------

#include "ble.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_bt.h"
#ifdef __has_include
#  if __has_include("esp_bt_controller.h")
#    include "esp_bt_controller.h"   // (types/debug only; controller is init'd by NimBLE port)
#  endif
#endif
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_gatt.h"
#include "host/ble_hs_id.h"

#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#ifdef __has_include
#  if __has_include("host/ble_store_util.h")
#    include "host/ble_store_util.h"
#    define BLE_HAVE_STORE_UTIL 1
#  endif
#endif
#ifdef __has_include
#  if __has_include("nimble/store/ble_store_config.h")
#    include "nimble/store/ble_store_config.h"
#    define BLE_HAVE_STORE_CONFIG 1
#  elif __has_include("host/ble_store_config.h")
#    include "host/ble_store_config.h"
#    define BLE_HAVE_STORE_CONFIG 1
#  elif __has_include("store/ble_store_config.h")
#    include "store/ble_store_config.h"
#    define BLE_HAVE_STORE_CONFIG 1
#  endif
#endif

#include "ipc.h"
#include "net.h"
#include "cJSON.h"

#if CONFIG_BLE_PROV_USE_ESP_PROV
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include "protocomm.h"
#include "esp_event.h"
#include "esp_mac.h"
#endif

// -------------------- Module settings --------------------
static const char *TAG = "ble_nimble";

#ifndef BLE_PROV_TEST
#define BLE_PROV_TEST 0   // pre-prod default: no test backdoors
#endif
#ifndef BLE_PROV_REQUIRE_ENC
#define BLE_PROV_REQUIRE_ENC 0   // set to 1 to require encryption before writes
#endif

// Event bits (provide safe fallbacks so this compiles even if net.h changes)
#ifndef NET_BIT_BLE_ACTIVE
#  define NET_BIT_BLE_ACTIVE (1u << 0)
#endif
// Alias WIFI_CONNECTED to WIFI_UP if available in net.h
#ifndef NET_BIT_WIFI_CONNECTED
#  ifdef NET_BIT_WIFI_UP
#    define NET_BIT_WIFI_CONNECTED NET_BIT_WIFI_UP
#  else
#    define NET_BIT_WIFI_CONNECTED (1u << 1)
#  endif
#endif
#ifndef NET_BIT_TIME_SYNCED
#  define NET_BIT_TIME_SYNCED (1u << 2)
#endif

// Build-time test creds via either -D SIM_* or Kconfig CONFIG_SIM_*
#if defined(CONFIG_SIM_WIFI_SSID) && !defined(SIM_WIFI_SSID)
#  define SIM_WIFI_SSID CONFIG_SIM_WIFI_SSID
#endif
#if defined(CONFIG_SIM_WIFI_PSK) && !defined(SIM_WIFI_PSK)
#  define SIM_WIFI_PSK CONFIG_SIM_WIFI_PSK
#endif
#if defined(CONFIG_SIM_TZ) && !defined(SIM_TZ)
#  define SIM_TZ CONFIG_SIM_TZ
#endif
#if defined(SIM_WIFI_SSID)
#  define BLE_HAS_TEST_CREDS 1
#else
#  define BLE_HAS_TEST_CREDS 0
#endif

#if !CONFIG_BLE_PROV_USE_ESP_PROV
// 128-bit UUIDs: Service A000..., Characteristic A001 for provisioning JSON
static const ble_uuid128_t prov_svc_uuid =
    BLE_UUID128_INIT(0x00,0xA0,0x00,0x00,0x00,0x00,0x10,0x00,0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB);
static const ble_uuid128_t prov_chr_uuid =
    BLE_UUID128_INIT(0x01,0xA0,0x00,0x00,0x00,0x00,0x10,0x00,0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB);
static const ble_uuid128_t prov_uuid = BLE_UUID128_INIT(
    0xf0,0xde,0xbc,0x9a,0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12);
#endif

// -------------------- State --------------------
#if !CONFIG_BLE_PROV_USE_ESP_PROV
static uint8_t own_addr_type;
#endif
static ble_prov_cb_t s_prov_cb = NULL; void *s_prov_arg = NULL;
static volatile bool s_should_adv = false;
static volatile bool s_adv_running = false;
static volatile bool s_svcs_registered = false;   // gate ads until GATT ready
static volatile bool s_ble_boot_ok = false;       // set after host sync
static bool s_ble_started = false;                // idempotent guard
#if !CONFIG_BLE_PROV_USE_ESP_PROV
static char s_devname[32] = {0};
static uint8_t s_mac[6] = {0};
#endif
static bool s_provisioned_recently = false;
static TickType_t s_prov_time_ticks = 0;

#if CONFIG_BLE_PROV_USE_ESP_PROV
static bool s_prov_mgr_started = false;
static char s_last_ssid[33] = {0};
static char s_last_psk[65] = {0};
static char s_last_tz[64] = {0};
static bool s_custom_ep_registered = false;
#endif

// Forward decls
#if !CONFIG_BLE_PROV_USE_ESP_PROV
static void start_advertising(void);
#endif
static void ble_mgr_task(void *param);
static void ble_commission_orchestrator(void *param);
static inline void set_adv_flag(bool on) { s_should_adv = on; }
#if BLE_HAS_TEST_CREDS
static void ble_inject_provision(const char *ssid, const char *psk, const char *tz);
#endif
#if CONFIG_BLE_PROV_USE_ESP_PROV
static void start_provisioning_mgr(void);
static void stop_provisioning_mgr(void);
#endif

#if CONFIG_BLE_PROV_USE_ESP_PROV
// Provisioning manager helpers
static esp_err_t custom_data_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
                                     uint8_t **outbuf, ssize_t *outlen, void *priv)
{
    (void)session_id; (void)priv;
    const char *ok = "{\"status\":\"ok\"}";
    if (outbuf && outlen) { *outbuf = (uint8_t*)strdup(ok); *outlen = strlen(ok); }
    if (!inbuf || inlen <= 0) return ESP_OK;
    cJSON *root = cJSON_ParseWithLength((const char*)inbuf, (size_t)inlen);
    if (!root) return ESP_OK;
    cJSON *tz = cJSON_GetObjectItemCaseSensitive(root, "tz");
    if (cJSON_IsString(tz) && tz->valuestring) {
        strncpy(s_last_tz, tz->valuestring, sizeof(s_last_tz)-1);
        s_last_tz[sizeof(s_last_tz)-1] = '\0';
        ESP_LOGI(TAG, "custom-data: tz=%s", s_last_tz);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static void prov_event_handler(void *user_data, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)user_data; (void)event_base;
    switch ((wifi_prov_cb_event_t)event_id) {
    case WIFI_PROV_START:
        ESP_LOGI(TAG, "Provisioning started");
        // Create custom endpoint when provisioning stack is fully started
        if (wifi_prov_mgr_endpoint_create("custom-data") != ESP_OK) {
            ESP_LOGW(TAG, "custom endpoint create failed: %s", esp_err_to_name(ESP_FAIL));
        } else {
            esp_err_t er = wifi_prov_mgr_endpoint_register("custom-data", custom_data_handler, NULL);
            if (er != ESP_OK) {
                ESP_LOGW(TAG, "custom endpoint register failed: %s", esp_err_to_name(er));
            }
        }
        break;
    case WIFI_PROV_CRED_RECV: {
        wifi_sta_config_t *cfg = (wifi_sta_config_t *)event_data;
        strncpy(s_last_ssid, (const char*)cfg->ssid, sizeof(s_last_ssid)-1);
        s_last_ssid[sizeof(s_last_ssid)-1] = '\0';
        strncpy(s_last_psk, (const char*)cfg->password, sizeof(s_last_psk)-1);
        s_last_psk[sizeof(s_last_psk)-1] = '\0';
        ESP_LOGI(TAG, "Received Wi-Fi creds for SSID '%s'", s_last_ssid);
        break; }
    case WIFI_PROV_CRED_FAIL:
        ESP_LOGW(TAG, "Provisioning failed");
        break;
    case WIFI_PROV_CRED_SUCCESS:
        ESP_LOGI(TAG, "Provisioning successful");
        if (s_prov_cb) {
            s_prov_cb(s_last_ssid, (s_last_psk[0]?s_last_psk:NULL), (s_last_tz[0]?s_last_tz:NULL), s_prov_arg);
        }
        if (g_net_state_event_group) xEventGroupClearBits(g_net_state_event_group, NET_BIT_BLE_ACTIVE);
        wifi_prov_mgr_stop_provisioning();
        break;
    case WIFI_PROV_END:
        ESP_LOGI(TAG, "Provisioning ended");
        wifi_prov_mgr_deinit();
        s_prov_mgr_started = false;
        s_custom_ep_registered = false;
        // Now unregister our event handler
        esp_err_t _e = esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler);
        if (_e != ESP_OK) {
            ESP_LOGD(TAG, "event_handler_unregister (END): %s", esp_err_to_name(_e));
        }
        break;
    default: break;
    }
}

static void start_provisioning_mgr(void)
{
    if (s_prov_mgr_started) return;
    wifi_prov_mgr_config_t cfg = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };
    ESP_ERROR_CHECK(wifi_prov_mgr_init(cfg));
    // Register provisioning events on the default event loop (works across IDF versions)
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler, NULL));

    uint8_t mac_local[6]; esp_read_mac(mac_local, ESP_MAC_WIFI_STA);
    char service_name[20];
    snprintf(service_name, sizeof(service_name), "%s_%02X%02X", CONFIG_BLE_PROV_SERVICE_NAME, mac_local[4], mac_local[5]);

    const char *pop = CONFIG_BLE_PROV_POP;
    ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, (const void*)pop, service_name, NULL));
    s_prov_mgr_started = true;
}

static void stop_provisioning_mgr(void)
{
    if (!s_prov_mgr_started) return;
    wifi_prov_mgr_stop_provisioning();
    // Deinit happens on WIFI_PROV_END event
}
#endif // CONFIG_BLE_PROV_USE_ESP_PROV

#if !CONFIG_BLE_PROV_USE_ESP_PROV
// -------------------- GAP events --------------------
static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "gap connect status=%d", event->connect.status);
        if (event->connect.status != 0) { if (s_should_adv) start_advertising(); }
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

static int set_adv_and_rsp_fields(const char *short_name,
                                  const uint8_t *mfg, uint8_t mfg_len)
{
    int rc;
    struct ble_hs_adv_fields adv = {0};
    struct ble_hs_adv_fields rsp = {0};

    /* --- ADV: keep it tiny and always valid --- */
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    // Option A (recommended): advertise a short name only
    adv.name = (uint8_t *)short_name;
    adv.name_len = (uint8_t)strlen(short_name);
    adv.name_is_complete = 1;

    // No UUIDs in ADV if name isn’t super short; put UUIDs in SCAN RESP instead.

    rc = ble_gap_adv_set_fields(&adv);
    if (rc) {
        // Fallback: drop the name entirely if even that failed
        memset(&adv, 0, sizeof(adv));
        adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        rc = ble_gap_adv_set_fields(&adv);
        if (rc) return rc; // still invalid -> bail so caller can log rc
    }

    /* --- SCAN RESPONSE: keep under 31 bytes (UUID + small mfg) --- */
    rsp.uuids128 = (ble_uuid128_t *)&prov_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;

    if (mfg && mfg_len) {
        rsp.mfg_data = mfg;
        rsp.mfg_data_len = mfg_len; // keep this modest (<= ~10 bytes)
    }
    // Do NOT include a long name here; UUID(16B) + name often exceeds 31B.

    rc = ble_gap_adv_rsp_set_fields(&rsp);
    return rc;
}

// -------------------- Advertising --------------------
static void start_advertising(void)
{
    // Only start if host is up and services are registered
    if (!s_ble_boot_ok || !s_svcs_registered) {
        return;
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;        // connectable
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;        // general discoverable
    params.itvl_min  = 0x00a0; // 100 ms
    params.itvl_max  = 0x00f0; // 150 ms

    const char *short_name = "C3-PROV";              // <= 8 chars is a good target
    uint8_t mfg[6] = {0x4C,0x00, /*Apple example vendor ID? use your own*/
                      0x01, 0x02, 0x03, 0x04};      // keep small; or omit

    int rc = set_adv_and_rsp_fields(short_name, mfg, sizeof(mfg));
    if (rc) {
        ESP_LOGE("BLE_ADV", "adv fields invalid rc=%d", rc);
        return;
    }

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &params, gap_event, NULL);
    if (rc) {
        ESP_LOGE("BLE_ADV", "adv_start failed rc=%d", rc);
    } else {
        ESP_LOGI("BLE_ADV", "Advertising started.");
        s_adv_running = true;
    }
}

// -------------------- Provisioning GATT --------------------
static int prov_chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
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
            set_adv_flag(false);
            ble_gap_adv_stop(); s_adv_running = false;
            if (g_net_state_event_group) xEventGroupClearBits(g_net_state_event_group, NET_BIT_BLE_ACTIVE);
            s_provisioned_recently = true; s_prov_time_ticks = xTaskGetTickCount();
        }
        cJSON_Delete(root);
        return 0;
    }
#if BLE_PROV_TEST
    case BLE_GATT_ACCESS_OP_READ_CHR: {
#     ifdef BLE_PROV_TEST_EMBED_SYM
        extern const uint8_t _ble_prov_test_start[] asm("_binary_" BLE_PROV_TEST_EMBED_SYM "_start");
        extern const uint8_t _ble_prov_test_end[]   asm("_binary_" BLE_PROV_TEST_EMBED_SYM "_end");
        const uint8_t *data = _ble_prov_test_start;
        size_t data_len = (size_t)(_ble_prov_test_end - _ble_prov_test_start);
#     else
        const uint8_t *data = NULL; size_t data_len = 0;
#     endif
        if (!data || data_len == 0) {
            static const char fallback[] = "{\"ssid\":\"YourSSID\",\"psk\":\"YourPassword\",\"tz\":\"America/Los_Angeles\"}";
            data = (const uint8_t*)fallback; data_len = sizeof(fallback) - 1;
        }
        int rc = os_mbuf_append(ctxt->om, data, (uint16_t)data_len);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
#endif
    default: return BLE_ATT_ERR_UNLIKELY;
    }
}

// GATT DB
static const struct ble_gatt_chr_def s_chrs[] = {
    { .uuid=&prov_chr_uuid.u, .access_cb=prov_chr_access,
      .flags =
#if BLE_PROV_TEST
              BLE_GATT_CHR_F_READ |
#endif
#if BLE_PROV_REQUIRE_ENC
              BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_NO_RSP,
#else
              BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
#endif
    },
    { 0 }
};
static const struct ble_gatt_svc_def s_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &prov_svc_uuid.u, .characteristics = s_chrs },
    { 0 }
};

// -------------------- Host lifecycle --------------------
static void host_reset_cb(int reason) { ESP_LOGW(TAG, "host reset reason=%d", reason); }

static bool s_gap_gatt_inited = false;
static void host_sync_cb(void) {
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer addr rc=%d", rc); return; }
    uint8_t addr[6]; ble_hs_id_copy_addr(own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "BLE addr %02X:%02X:%02X:%02X:%02X:%02X", addr[5],addr[4],addr[3],addr[2],addr[1],addr[0]);
    memcpy(s_mac, addr, 6);

    if (!s_gap_gatt_inited) {
        ble_svc_gap_init();
        ble_svc_gatt_init();
        s_gap_gatt_inited = true;
    }

    snprintf(s_devname, sizeof(s_devname), "ESP-C3-PROV-%02X%02X%02X", addr[2], addr[1], addr[0]);
    ble_svc_gap_device_name_set(s_devname);

    if (!s_svcs_registered) {
        rc = ble_gatts_count_cfg((struct ble_gatt_svc_def*)s_svcs);
        if (rc) { ESP_LOGE(TAG, "gatts_count rc=%d", rc); return; }
        rc = ble_gatts_add_svcs((struct ble_gatt_svc_def*)s_svcs);
        if (rc) { ESP_LOGE(TAG, "gatts_add rc=%d", rc); return; }
        s_svcs_registered = true;
        ESP_LOGI(TAG, "GATT services registered");
    }

    s_ble_boot_ok = true;
    if (s_should_adv) start_advertising();
}

static void ble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
    vTaskDelete(NULL);
}
#endif // !CONFIG_BLE_PROV_USE_ESP_PROV

// Watches NET_BIT_BLE_ACTIVE and (de)starts advertising accordingly
static void ble_mgr_task(void *param) {
    (void)param;
    bool last_desired = false;
    for (;;) {
        if (g_net_state_event_group) {
            EventBits_t bits = xEventGroupGetBits(g_net_state_event_group);
            bool desired = (bits & NET_BIT_BLE_ACTIVE) != 0;
            if (desired != last_desired) {
                ESP_LOGI(TAG, "BLE_ACTIVE -> %d", desired);
                set_adv_flag(desired);
                #if CONFIG_BLE_PROV_USE_ESP_PROV
                if (desired) start_provisioning_mgr(); else stop_provisioning_mgr();
                #else
                if (!desired && s_adv_running) { ble_gap_adv_stop(); s_adv_running = false; }
                if (desired && !s_adv_running) { start_advertising(); }
                #endif
                last_desired = desired;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

// Orchestrates commissioning windows based on Wi‑Fi state/time
static void ble_commission_orchestrator(void *param) {
    (void)param;
    const TickType_t fallback_ticks = pdMS_TO_TICKS((uint32_t)CONFIG_NET_BLE_FALLBACK_SEC * 1000u);
    const TickType_t stable_ticks   = pdMS_TO_TICKS((uint32_t)CONFIG_NET_WIFI_STABLE_MIN * 1000u);

    TickType_t boot = xTaskGetTickCount();
    bool ble_enabled_once = false;
    TickType_t connected_since = 0;

    for (;;) {
        EventBits_t bits = g_net_state_event_group ? xEventGroupGetBits(g_net_state_event_group) : 0;
        bool wifi_connected = (bits & NET_BIT_WIFI_CONNECTED) != 0;
        bool time_synced    = (bits & NET_BIT_TIME_SYNCED)    != 0;

        // Initial fallback: if not up in time, open commissioning
        if (!ble_enabled_once) {
            if (!(wifi_connected && time_synced) && (xTaskGetTickCount() - boot >= fallback_ticks)) {
                if (g_net_state_event_group) xEventGroupSetBits(g_net_state_event_group, NET_BIT_BLE_ACTIVE);
                ble_enabled_once = true;
                ESP_LOGI(TAG, "Fallback window opened: BLE commissioning enabled");
            }
        }

        // Track stability
        if (wifi_connected) {
            if (connected_since == 0) connected_since = xTaskGetTickCount();
            if (xTaskGetTickCount() - connected_since >= stable_ticks) {
                // Wi‑Fi considered stable → ensure BLE is off
                if (g_net_state_event_group) xEventGroupClearBits(g_net_state_event_group, NET_BIT_BLE_ACTIVE);
                set_adv_flag(false);
                if (s_adv_running) { ble_gap_adv_stop(); s_adv_running = false; }
            }
        } else {
            connected_since = 0; // lost connectivity → reset stability window
        }

        // If provisioning just happened but Wi‑Fi still not connecting after 3 minutes, re-open BLE
        if (s_provisioned_recently && !wifi_connected) {
            if (xTaskGetTickCount() - s_prov_time_ticks >= pdMS_TO_TICKS(180000)) {
                if (g_net_state_event_group) xEventGroupSetBits(g_net_state_event_group, NET_BIT_BLE_ACTIVE);
                s_provisioned_recently = false;
                ESP_LOGW(TAG, "Provisioning connect timeout → re-enabling BLE commissioning");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

// Build-time injection task (fires once when callback exists)
#if BLE_HAS_TEST_CREDS
static void ble_test_inject_task(void *param) {
    (void)param;
    // Wait briefly for app to register callback
    for (int i = 0; i < 40; ++i) { // ~2s max
        if (s_prov_cb) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
#  ifndef SIM_WIFI_PSK
#    define SIM_WIFI_PSK NULL
#  endif
#  ifndef SIM_TZ
#    define SIM_TZ NULL
#  endif
    if (s_prov_cb) {
        ESP_LOGW(TAG, "SIMULATION: injecting Wi‑Fi creds from build-time macros (ssid=\"%s\")", SIM_WIFI_SSID);
        ble_inject_provision(SIM_WIFI_SSID, SIM_WIFI_PSK, SIM_TZ);
    } else {
        ESP_LOGW(TAG, "SIMULATION: skipped (provision callback not registered)");
    }
    vTaskDelete(NULL);
}
#endif

// -------------------- Public API --------------------
esp_err_t ble_init(void) {
    ESP_LOGI(TAG, "init NimBLE");
    ESP_LOGI(TAG, "heap free: 8bit=%u, 32bit=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_32BIT));

    // Free Classic BT RAM (harmless on C3)
    (void)esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    // NVS for keys/bonds
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    #if CONFIG_BLE_PROV_USE_ESP_PROV
    // Manager: reacts to NET_BIT_BLE_ACTIVE
    xTaskCreate(ble_mgr_task, "ble_mgr", 3072, NULL, 5, NULL);
    // Orchestrator: timing windows & stability logic
    xTaskCreate(ble_commission_orchestrator, "ble_comm", 3072, NULL, 5, NULL);
    s_ble_started = true;
    return ESP_OK;
    #else

    int rc = nimble_port_init(); // IDF ≥5: handles controller + HCI
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", rc);
        return ESP_FAIL;
    }

#if defined(BLE_HAVE_STORE_CONFIG)
    ble_store_config_init();
#elif defined(BLE_HAVE_STORE_UTIL)
    ble_store_util_init();
#endif

    ble_hs_cfg.reset_cb = host_reset_cb;
    ble_hs_cfg.sync_cb  = host_sync_cb;

    // Start NimBLE host thread
    nimble_port_freertos_init(ble_host_task);

    // Manager: reacts to NET_BIT_BLE_ACTIVE
    xTaskCreate(ble_mgr_task, "ble_mgr", 3072, NULL, 5, NULL);

    // Orchestrator: timing windows & stability logic
    xTaskCreate(ble_commission_orchestrator, "ble_comm", 3072, NULL, 5, NULL);

#if BLE_HAS_TEST_CREDS
    // Fire-and-forget injector that waits for the app to register callback
    xTaskCreate(ble_test_inject_task, "ble_inject", 2048, NULL, 5, NULL);
#endif

    s_ble_started = true;
    return ESP_OK;
    #endif
}

esp_err_t ble_stop(void) {
    if (!s_ble_started) return ESP_OK;
    set_adv_flag(false);
#if CONFIG_BLE_PROV_USE_ESP_PROV
    stop_provisioning_mgr();
#else
    if (s_adv_running) { ble_gap_adv_stop(); s_adv_running = false; }
#endif
    return ESP_OK;
}

void ble_register_prov_callback(ble_prov_cb_t cb, void *arg) { s_prov_cb = cb; s_prov_arg = arg; }

// -------------------- Internal helpers --------------------
#if BLE_HAS_TEST_CREDS
static void ble_inject_provision(const char *ssid, const char *psk, const char *tz) {
    if (ssid && s_prov_cb) {
        s_prov_cb(ssid, psk, tz, s_prov_arg);
        set_adv_flag(false);
        if (s_adv_running) { ble_gap_adv_stop(); s_adv_running = false; }
        if (g_net_state_event_group) xEventGroupClearBits(g_net_state_event_group, NET_BIT_BLE_ACTIVE);
        s_provisioned_recently = true;
        s_prov_time_ticks = xTaskGetTickCount();
        ESP_LOGI(TAG, "ble_inject_provision: injected creds (ssid len=%d, tz=%s)",
                 (int)strlen(ssid), tz ? tz : "");
    } else {
        ESP_LOGW(TAG, "ble_inject_provision: ignored (no ssid or no callback)");
    }
}
#endif
