#include "ota.h"
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_app_format.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "secure_part.h"
#include <strings.h>
#include "storage.h"

static const char *TAG = "ota";

#define OTA_TASK_STACK_SIZE (8192)
#define OTA_TASK_PRIORITY   (5)
#define OTA_JOB_QUEUE_LEN   (1)

static QueueHandle_t s_ota_job_queue = NULL;

// Forward declarations for static functions
static esp_err_t ota_verify_manifest_signature(const cJSON *manifest, const char *digest_hex, const char *signature_b64);
static esp_err_t ota_download_and_verify(esp_https_ota_handle_t ota_handle, const char *expected_digest_hex);
static void ota_task(void *pvParameters);

// Minimal helper implementations for tests
esp_err_t ota_parse_manifest(const char *json, ota_manifest_t *out) {
    if (!json || !out) return ESP_ERR_INVALID_ARG;
    cJSON *root = cJSON_Parse(json);
    if (!root) return ESP_FAIL;
    memset(out, 0, sizeof(*out));
    cJSON *v = cJSON_GetObjectItem(root, "version");
    if (!cJSON_IsNumber(v)) { cJSON_Delete(root); return ESP_FAIL; }
    out->version = (uint32_t)cJSON_GetNumberValue(v);
    cJSON *mr = cJSON_GetObjectItem(root, "min_required");
    if (cJSON_IsNumber(mr)) { out->has_min_required = true; out->min_required = (uint32_t)cJSON_GetNumberValue(mr); }
    cJSON_Delete(root);
    return ESP_OK;
}

int ota_compute_keyid_from_der(const unsigned char *der, size_t der_len,
                               char *out_full_hex, size_t full_sz,
                               char *out_short_hex, size_t short_sz,
                               size_t short_nibbles) {
    if (!der || der_len == 0 || !out_full_hex || full_sz < 65) return -1;
    unsigned char hash[32];
    mbedtls_sha256(der, der_len, hash, 0);
    for (int i = 0; i < 32; ++i) sprintf(out_full_hex + i*2, "%02x", hash[i]);
    out_full_hex[64] = '\0';
    if (out_short_hex && short_sz > short_nibbles) {
        memcpy(out_short_hex, out_full_hex, short_nibbles);
        out_short_hex[short_nibbles] = '\0';
    }
    return 0;
}

bool ota_check_version_policy(uint32_t current, uint32_t newv, uint32_t min_required, bool allow_equal, bool allow_rollback) {
    if (!allow_rollback && newv < current) return false;
    if (!allow_equal && newv == current) return false;
    if (min_required && current < min_required) {
        // Force update if current below min_required
        return true;
    }
    return true;
}

/**
 * @brief Public API to trigger an OTA update.
 */
esp_err_t ota_trigger_update(const char *manifest_json) {
    if (!s_ota_job_queue) {
        ESP_LOGE(TAG, "OTA component not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!manifest_json) {
        return ESP_ERR_INVALID_ARG;
    }

    char *manifest_copy = strdup(manifest_json);
    if (!manifest_copy) {
        return ESP_ERR_NO_MEM;
    }

    if (xQueueSend(s_ota_job_queue, &manifest_copy, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGE(TAG, "Failed to queue OTA job, queue might be full");
        free(manifest_copy);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief Main OTA task.
 */
// TODO: Implement real signature verification using signer CA from secure partition
// Convert hex ASCII to bytes; expects even-length hex string
static int hex_to_bytes(const char *hex, uint8_t *out, size_t out_len) {
    size_t n = strlen(hex);
    if ((n % 2) != 0 || out_len < n/2) return -1;
    for (size_t i = 0; i < n/2; ++i) {
        char c1 = hex[2*i];
        char c2 = hex[2*i+1];
        uint8_t v1 = (uint8_t)((c1 >= '0' && c1 <= '9') ? (c1 - '0') : (c1 >= 'a' && c1 <= 'f') ? (c1 - 'a' + 10) : (c1 >= 'A' && c1 <= 'F') ? (c1 - 'A' + 10) : 255);
        uint8_t v2 = (uint8_t)((c2 >= '0' && c2 <= '9') ? (c2 - '0') : (c2 >= 'a' && c2 <= 'f') ? (c2 - 'a' + 10) : (c2 >= 'A' && c2 <= 'F') ? (c2 - 'A' + 10) : 255);
        if (v1 == 255 || v2 == 255) return -1;
        out[i] = (uint8_t)((v1 << 4) | v2);
    }
    return 0;
}

// Verify manifest signature over the raw 32-byte image SHA-256 digest.
// Manifest may include optional field "signer_cert_b64" (DER) for the signer.
static esp_err_t ota_verify_manifest_signature(const cJSON *manifest, const char *digest_hex, const char *signature_b64) {
    if (!manifest || !digest_hex || !signature_b64) return ESP_ERR_INVALID_ARG;

    // 1) Decode expected digest (hex -> 32 bytes)
    uint8_t digest[32];
    if (strlen(digest_hex) != 64 || hex_to_bytes(digest_hex, digest, sizeof(digest)) != 0) {
        ESP_LOGE(TAG, "Invalid digest hex length or format");
        return ESP_ERR_INVALID_ARG;
    }

    // 2) Base64-decode signature
    const unsigned char *sig_b64 = (const unsigned char*)signature_b64;
    size_t sig_b64_len = strlen(signature_b64);
    size_t sig_len = 0;
    int rc = mbedtls_base64_decode(NULL, 0, &sig_len, sig_b64, sig_b64_len);
    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        ESP_LOGE(TAG, "Invalid signature base64");
        return ESP_ERR_INVALID_ARG;
    }
    unsigned char *sig = malloc(sig_len);
    if (!sig) return ESP_ERR_NO_MEM;
    if (mbedtls_base64_decode(sig, sig_len, &sig_len, sig_b64, sig_b64_len) != 0) {
        free(sig);
        ESP_LOGE(TAG, "Signature base64 decode failed");
        return ESP_FAIL;
    }

    // 3) Load signer certificate
    // Try manifest-provided signer cert first (DER in base64 under key signer_cert_b64)
    unsigned char *signer_der = NULL; size_t signer_der_len = 0;
    cJSON *signer_b64 = cJSON_GetObjectItemCaseSensitive(manifest, "signer_cert_b64");
    mbedtls_x509_crt signer_crt; mbedtls_x509_crt_init(&signer_crt);
    bool have_signer = false;
    if (cJSON_IsString(signer_b64) && signer_b64->valuestring) {
        const unsigned char *der_b64 = (const unsigned char*)signer_b64->valuestring;
        size_t der_b64_len = strlen(signer_b64->valuestring);
        size_t need = 0;
        if (mbedtls_base64_decode(NULL, 0, &need, der_b64, der_b64_len) == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
            signer_der = malloc(need);
            if (signer_der && mbedtls_base64_decode(signer_der, need, &signer_der_len, der_b64, der_b64_len) == 0) {
                if (mbedtls_x509_crt_parse_der(&signer_crt, signer_der, signer_der_len) == 0) {
                    have_signer = true;
                }
            }
        }
    }

    // 4) Load CA from secure partition and verify signer chain if signer provided
    uint8_t *blob=NULL,*ca=NULL,*cert=NULL,*key=NULL; size_t blob_len=0,ca_len=0,cert_len=0,key_len=0;
    (void)cert; (void)cert_len; (void)key; (void)key_len;
    esp_err_t sp = secure_part_read(&blob, &blob_len, &ca, &ca_len, &cert, &cert_len, &key, &key_len);
    mbedtls_x509_crt ca_chain; mbedtls_x509_crt_init(&ca_chain);
    if (sp == ESP_OK && ca && ca_len) {
        // Attempt to parse CA list (DER or PEM)
        int perr = mbedtls_x509_crt_parse(&ca_chain, ca, ca_len);
        if (perr != 0) {
            // If stored DER of a single cert, try DER parse fallback
            mbedtls_x509_crt_free(&ca_chain);
            mbedtls_x509_crt_init(&ca_chain);
            mbedtls_x509_crt_parse_der(&ca_chain, ca, ca_len);
        }
    }

    uint32_t flags = 0;
    if (have_signer) {
        // Optional: pin signer key-id if provided
        cJSON *kid = cJSON_GetObjectItemCaseSensitive(manifest, "signer_keyid_hex");
        if (cJSON_IsString(kid) && kid->valuestring) {
            unsigned char skid[32];
            if (signer_der && signer_der_len > 0) {
                mbedtls_sha256(signer_der, signer_der_len, skid, 0);
                char skid_hex[65];
                for (int i=0;i<32;i++) sprintf(skid_hex + i*2, "%02x", skid[i]);
                skid_hex[64] = '\0';
                if (strcasecmp(skid_hex, kid->valuestring) != 0) {
                    ESP_LOGE(TAG, "Signer key-id mismatch");
                    if (blob || ca) secure_part_free(blob, ca, cert, key);
                    if (signer_der) free(signer_der);
                    mbedtls_x509_crt_free(&signer_crt);
                    mbedtls_x509_crt_free(&ca_chain);
                    free(sig);
                    return ESP_ERR_INVALID_RESPONSE;
                }
            }
        }

        // Verify signer against CA (if CA present); if no CA, fail (we require trust chain)
        if (ca_chain.raw.len > 0) {
            int vrc = mbedtls_x509_crt_verify(&signer_crt, &ca_chain, NULL, NULL, &flags, NULL, NULL);
            if (vrc != 0) {
                ESP_LOGE(TAG, "Signer certificate chain verification failed (flags=0x%x)", (unsigned)flags);
                if (blob || ca) secure_part_free(blob, ca, cert, key);
                if (signer_der) free(signer_der);
                mbedtls_x509_crt_free(&signer_crt);
                mbedtls_x509_crt_free(&ca_chain);
                free(sig);
                return ESP_ERR_INVALID_STATE;
            }
        } else {
            ESP_LOGE(TAG, "No CA in secure partition to verify signer");
            if (blob || ca) secure_part_free(blob, ca, cert, key);
            if (signer_der) free(signer_der);
            mbedtls_x509_crt_free(&signer_crt);
            mbedtls_x509_crt_free(&ca_chain);
            free(sig);
            return ESP_ERR_INVALID_STATE;
        }
    }

    // 5) Select public key for verification
    mbedtls_pk_context *pk = NULL;
    if (have_signer) {
        pk = &signer_crt.pk;
    }

    if (!pk || !mbedtls_pk_get_type(pk)) {
        ESP_LOGE(TAG, "No signer key available for manifest verification");
        if (blob || ca) secure_part_free(blob, ca, cert, key);
        if (signer_der) free(signer_der);
        mbedtls_x509_crt_free(&signer_crt);
        mbedtls_x509_crt_free(&ca_chain);
        free(sig);
        return ESP_ERR_INVALID_STATE;
    }

    // 6) Verify signature over the raw digest using SHA-256
    int v = mbedtls_pk_verify(pk, MBEDTLS_MD_SHA256, digest, sizeof(digest), sig, sig_len);
    free(sig);
    if (blob || ca) secure_part_free(blob, ca, cert, key);
    if (signer_der) free(signer_der);
    mbedtls_x509_crt_free(&signer_crt);
    mbedtls_x509_crt_free(&ca_chain);
    if (v != 0) {
        ESP_LOGE(TAG, "Manifest signature invalid (err=%d)", v);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "Manifest signature verified");
    return ESP_OK;
}

/**
 * @brief Main OTA task.
 */
static void ota_task(void *pvParameters) {
    ESP_LOGI(TAG, "OTA task started");

    while (1) {
        char *manifest_str = NULL;
        if (xQueueReceive(s_ota_job_queue, &manifest_str, portMAX_DELAY) == pdPASS) {
            ESP_LOGI(TAG, "Received OTA job");

            cJSON *manifest = cJSON_Parse(manifest_str);
            if (!manifest) {
                ESP_LOGE(TAG, "Failed to parse manifest JSON");
                free(manifest_str);
                continue;
            }

            // Extract manifest fields
            const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(manifest, "url"));
            const char *digest_hex = cJSON_GetStringValue(cJSON_GetObjectItem(manifest, "digest"));
            const char *signature_b64 = cJSON_GetStringValue(cJSON_GetObjectItem(manifest, "signature"));
            cJSON *version_item = cJSON_GetObjectItem(manifest, "version");

            if (!url || !digest_hex || !signature_b64 || !version_item) {
                ESP_LOGE(TAG, "Manifest missing required fields");
                cJSON_Delete(manifest);
                free(manifest_str);
                continue;
            }
            
            // 1. Verify manifest signature
            if (ota_verify_manifest_signature(manifest, digest_hex, signature_b64) != ESP_OK) {
                ESP_LOGE(TAG, "Manifest signature verification failed");
                cJSON_Delete(manifest);
                free(manifest_str);
                continue;
            }
            ESP_LOGI(TAG, "Manifest signature OK");

            // 2. Check versioning and rollback policy
            uint32_t current_version = 0;
            uint32_t new_version = (uint32_t)cJSON_GetNumberValue(version_item);
            storage_load_uint32("ota_version", &current_version);

            bool allow_rollback = cJSON_IsTrue(cJSON_GetObjectItem(manifest, "allow_rollback"));
            if (!allow_rollback && new_version <= current_version) {
                ESP_LOGE(TAG, "Rollback protection: new version (%d) is not greater than current version (%d)", new_version, current_version);
                cJSON_Delete(manifest);
                free(manifest_str);
                continue;
            }

            // 3. Perform the OTA update
            // Load CA from secure partition for TLS pinning if PEM content exists
            uint8_t *sp_blob=NULL,*sp_ca=NULL,*sp_cert=NULL,*sp_key=NULL; size_t sp_blob_len=0,sp_ca_len=0,sp_cert_len=0,sp_key_len=0;
            esp_err_t sp_err = secure_part_read(&sp_blob, &sp_blob_len, &sp_ca, &sp_ca_len, &sp_cert, &sp_cert_len, &sp_key, &sp_key_len);
            const char *ca_pem_ptr = NULL;
            if (sp_err == ESP_OK && sp_ca && sp_ca_len >= 27 && strstr((const char*)sp_ca, "-----BEGIN") != NULL) {
                ca_pem_ptr = (const char*)sp_ca; // PEM expected by http client
            }

            esp_http_client_config_t http_cfg = {
                .url = url,
                .cert_pem = ca_pem_ptr,
                .timeout_ms = 15000,
                .keep_alive_enable = true,
            };
            esp_https_ota_config_t ota_cfg = {
                .http_config = &http_cfg,
            };

            esp_https_ota_handle_t ota_handle = NULL;
            esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
                cJSON_Delete(manifest);
                free(manifest_str);
                continue;
            }

            // 4. Verify image digest during download
            err = ota_download_and_verify(ota_handle, digest_hex);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Image verification failed");
                esp_https_ota_abort(ota_handle);
                cJSON_Delete(manifest);
                free(manifest_str);
                if (sp_err == ESP_OK) secure_part_free(sp_blob, sp_ca, sp_cert, sp_key);
                continue;
            }
            ESP_LOGI(TAG, "Image digest OK");

            // 5. Finalize OTA
            err = esp_https_ota_finish(ota_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "OTA update successful, persisting version and rebooting...");
                storage_save_uint32("ota_version", new_version);
                esp_restart();
            }

            cJSON_Delete(manifest);
            free(manifest_str);
            if (sp_err == ESP_OK) secure_part_free(sp_blob, sp_ca, sp_cert, sp_key);
        }
    }
}

/**
 * @brief Verifies the signature of the manifest.
 */
// (no second definition; function implemented above)

/**
 * @brief Reads image from OTA handle, computes SHA256, and verifies against expected digest.
 */
static esp_err_t ota_download_and_verify(esp_https_ota_handle_t ota_handle, const char *expected_digest_hex) {
    // Download via standard perform loop
    esp_err_t err;
    while ((err = esp_https_ota_perform(ota_handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        // keep looping
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_perform failed: %s", esp_err_to_name(err));
        return err;
    }
    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG, "Incomplete OTA image received");
        return ESP_ERR_INVALID_SIZE;
    }

    // Compute SHA-256 of the written update partition
    size_t img_len = esp_https_ota_get_image_len_read(ota_handle);
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update || img_len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);
    const size_t CHUNK = 4096;
    uint8_t *buf = malloc(CHUNK);
    if (!buf) { mbedtls_sha256_free(&sha_ctx); return ESP_ERR_NO_MEM; }
    size_t offset = 0;
    while (offset < img_len) {
        size_t to_read = (img_len - offset) > CHUNK ? CHUNK : (img_len - offset);
        if (esp_partition_read(update, offset, buf, to_read) != ESP_OK) { free(buf); mbedtls_sha256_free(&sha_ctx); return ESP_FAIL; }
    if (mbedtls_sha256_update(&sha_ctx, buf, to_read) != 0) { free(buf); mbedtls_sha256_free(&sha_ctx); return ESP_FAIL; }
        offset += to_read;
    }
    free(buf);
    unsigned char computed_hash[32];
    mbedtls_sha256_finish(&sha_ctx, computed_hash);
    mbedtls_sha256_free(&sha_ctx);
    char computed_hash_hex[65];
    for (int i = 0; i < 32; ++i) sprintf(computed_hash_hex + i*2, "%02x", computed_hash[i]);
    computed_hash_hex[64] = '\0';
    if (strcasecmp(computed_hash_hex, expected_digest_hex) != 0) {
        ESP_LOGE(TAG, "Image digest mismatch\nExpected: %s\nComputed: %s", expected_digest_hex, computed_hash_hex);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

/**
 * @brief Public API to initialize the OTA component.
 */
esp_err_t ota_init(void) {
    s_ota_job_queue = xQueueCreate(OTA_JOB_QUEUE_LEN, sizeof(char *));
    if (!s_ota_job_queue) {
        ESP_LOGE(TAG, "Failed to create OTA job queue");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(ota_task, "ota_task", OTA_TASK_STACK_SIZE, NULL, OTA_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        vQueueDelete(s_ota_job_queue);
        s_ota_job_queue = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA component initialized");
    return ESP_OK;
}
