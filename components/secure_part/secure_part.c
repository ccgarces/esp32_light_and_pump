#include "secure_part.h"
#include <stdlib.h>
#include <string.h>
#include "esp_partition.h"
#include "esp_log.h"

static const char *TAG = "secure_part";

esp_err_t secure_part_read(uint8_t **out_blob, size_t *out_len,
                           uint8_t **out_ca, size_t *out_ca_len,
                           uint8_t **out_cert, size_t *out_cert_len,
                           uint8_t **out_key, size_t *out_key_len)
{
    if (out_blob) {
        *out_blob = NULL;
    }
    if (out_len) {
        *out_len = 0;
    }
    if (out_ca) {
        *out_ca = NULL;
    }
    if (out_ca_len) {
        *out_ca_len = 0;
    }
    if (out_cert) {
        *out_cert = NULL;
    }
    if (out_cert_len) {
        *out_cert_len = 0;
    }
    if (out_key) {
        *out_key = NULL;
    }
    if (out_key_len) {
        *out_key_len = 0;
    }

    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "esp_secure_cert");
    if (!p) { ESP_LOGW(TAG, "secure partition not found"); return ESP_ERR_NOT_FOUND; }
    size_t psz = p->size;
    uint8_t *buf = malloc(psz);
    if (!buf) return ESP_ERR_NO_MEM;
    if (esp_partition_read(p, 0, buf, psz) != ESP_OK) { free(buf); return ESP_FAIL; }

    // simple TLV parser
    size_t idx = 0;
    // check magic
    if (psz < 5) { free(buf); return ESP_ERR_INVALID_SIZE; }
    if (buf[0]!='S' || buf[1]!='P' || buf[2]!='C' || buf[3]!='F') { free(buf); ESP_LOGW(TAG, "bad magic"); return ESP_ERR_INVALID_STATE; }
    idx = 5;
    while (idx + 5 <= psz) {
        uint8_t t = buf[idx];
        uint32_t l = 0;
        l = buf[idx+1] | (buf[idx+2]<<8) | (buf[idx+3]<<16) | (buf[idx+4]<<24);
        idx += 5;
        if (l == 0 || idx + l > psz) break;
        uint8_t *copy = malloc(l+1);
        if (!copy) break;
        memcpy(copy, buf+idx, l);
        copy[l] = '\0';
        switch (t) {
            case SPCF_TLV_TYPE_CA:
                if (out_ca) { *out_ca = copy; *out_ca_len = l; } else free(copy);
                break;
            case SPCF_TLV_TYPE_CERT:
                if (out_cert) { *out_cert = copy; *out_cert_len = l; } else free(copy);
                break;
            case SPCF_TLV_TYPE_KEY:
                if (out_key) { *out_key = copy; *out_key_len = l; } else free(copy);
                break;
            default:
                free(copy);
                break;
        }
        idx += l;
    }

    if (out_blob) { *out_blob = buf; *out_len = psz; } else free(buf);
    return ESP_OK;
}

void secure_part_free(uint8_t *blob, uint8_t *ca, uint8_t *cert, uint8_t *key)
{
    if (blob) free(blob);
    if (ca) free(ca);
    if (cert) free(cert);
    if (key) free(key);
}

esp_err_t secure_part_create_image(const uint8_t *ca, size_t ca_len,
                                  const uint8_t *cert, size_t cert_len,
                                  const uint8_t *key, size_t key_len,
                                  uint8_t **out_img, size_t *out_len, size_t pad_to_size)
{
    if (!out_img || !out_len) return ESP_ERR_INVALID_ARG;
    // compute needed size: header(5) + for each TLV: 1 + 4 + len
    size_t need = 5;
    if (ca && ca_len>0) need += 1 + 4 + ca_len;
    if (cert && cert_len>0) need += 1 + 4 + cert_len;
    if (key && key_len>0) need += 1 + 4 + key_len;
    size_t final = need;
    if (pad_to_size && pad_to_size > final) final = pad_to_size;
    uint8_t *buf = calloc(1, final);
    if (!buf) return ESP_ERR_NO_MEM;
    size_t idx = 0;
    buf[0]='S'; buf[1]='P'; buf[2]='C'; buf[3]='F'; buf[4]=1; idx = 5;
    #define WRITE_TLV(type, data, dlen) \
        do { \
            buf[idx++] = (type); \
            uint32_t _l = (uint32_t)(dlen); \
            buf[idx++] = (_l & 0xff); buf[idx++] = ((_l>>8)&0xff); buf[idx++] = ((_l>>16)&0xff); buf[idx++] = ((_l>>24)&0xff); \
            memcpy(buf+idx, data, dlen); idx += dlen; \
        } while(0)
    if (ca && ca_len>0) WRITE_TLV(SPCF_TLV_TYPE_CA, ca, ca_len);
    if (cert && cert_len>0) WRITE_TLV(SPCF_TLV_TYPE_CERT, cert, cert_len);
    if (key && key_len>0) WRITE_TLV(SPCF_TLV_TYPE_KEY, key, key_len);
    #undef WRITE_TLV
    *out_img = buf; *out_len = final;
    return ESP_OK;
}
