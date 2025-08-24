#include "provision_cli.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "secure_part.h"
#include "esp_partition.h"
#include <stdlib.h>
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"

static const char *TAG = "prov_cli";

// Helper: read file or stdin
static int read_blob_from_path(const char *path, uint8_t **out, size_t *out_len)
{
    if (!path || !out || !out_len) return -1;
    if (strcmp(path, "-") == 0) {
        // read from stdin until EOF
        size_t cap = 4096; uint8_t *buf = malloc(cap); if (!buf) return -1;
        size_t len = 0; int c;
        while ((c = getchar()) != EOF) {
            if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); if (!buf) return -1; }
            buf[len++] = (uint8_t)c;
        }
        *out = buf; *out_len = len; return 0;
    } else {
        // try open file on VFS
        FILE *f = fopen(path, "rb");
        if (!f) return -1;
        fseek(f, 0, SEEK_END); size_t len = ftell(f); fseek(f,0,SEEK_SET);
        uint8_t *buf = malloc(len);
        if (!buf) { fclose(f); return -1; }
        if (fread(buf,1,len,f) != len) { free(buf); fclose(f); return -1; }
        fclose(f);
        *out = buf; *out_len = len; return 0;
    }
}

// convert PEM blob (may contain headers) into raw DER bytes; returns 0 on success
static int pem_to_der(const uint8_t *pem, size_t pem_len, uint8_t **out_der, size_t *out_der_len)
{
    if (!pem || pem_len==0 || !out_der || !out_der_len) return -1;
    const char *buf = (const char*)pem;
    const char *beg = strstr(buf, "-----BEGIN CERTIFICATE-----");
    const char *end = strstr(buf, "-----END CERTIFICATE-----");
    if (!beg || !end) {
        // treat as raw DER
        uint8_t *copy = malloc(pem_len);
        if (!copy) return -1;
        memcpy(copy, pem, pem_len);
        *out_der = copy; *out_der_len = pem_len; return 0;
    }
    beg = strchr(beg, '\n'); if (!beg) return -1; beg++;
    size_t b64len = end - beg;
    char *b64 = malloc(b64len+1); if (!b64) return -1;
    memcpy(b64, beg, b64len); b64[b64len]=0;
    // strip whitespace
    char *r = b64, *w = b64;
    while (*r) { if (*r!='\r' && *r!='\n' && *r!=' ' && *r!='\t') *w++ = *r; r++; }
    size_t cleanlen = w - b64;
    // allocate output buffer conservatively
    size_t outcap = (cleanlen * 3) / 4 + 8;
    uint8_t *der = malloc(outcap);
    if (!der) { free(b64); return -1; }
    size_t der_len = 0;
    int rc = mbedtls_base64_decode(der, outcap, &der_len, (const unsigned char*)b64, cleanlen);
    free(b64);
    if (rc != 0) { free(der); return -1; }
    *out_der = der; *out_der_len = der_len; return 0;
}

// prov_pem --ca <path|-> --cert <path|-> --key <path|->
static struct {
    struct arg_str *ca;
    struct arg_str *cert;
    struct arg_str *key;
    struct arg_end *end;
} prov_args;

static int cmd_prov_pem(int argc, char **argv)
{
    prov_args.ca = arg_str1(NULL, "ca", "CA", "CA PEM path or - for stdin");
    prov_args.cert = arg_str1(NULL, "cert", "CERT", "Device cert path or - for stdin");
    prov_args.key = arg_str0(NULL, "key", "KEY", "Private key path or - for stdin (optional)");
    prov_args.end = arg_end(5);
    int nerrors = arg_parse(argc, argv, (void **)&prov_args);
    if (nerrors != 0) { arg_print_errors(stderr, prov_args.end, argv[0]); return 1; }
    uint8_t *ca=NULL,*cert=NULL,*key=NULL; size_t ca_len=0,cert_len=0,key_len=0;
    if (read_blob_from_path(prov_args.ca->sval[0], &ca, &ca_len) !=0) { ESP_LOGE(TAG, "failed to read ca"); return 1; }
    if (read_blob_from_path(prov_args.cert->sval[0], &cert, &cert_len) !=0) { ESP_LOGE(TAG, "failed to read cert"); free(ca); return 1; }
    if (prov_args.key->count && read_blob_from_path(prov_args.key->sval[0], &key, &key_len) !=0) { ESP_LOGW(TAG, "no key provided or failed to read key"); key=NULL; key_len=0; }

    // Convert PEM -> DER for fingerprint
    uint8_t *ca_der = NULL, *cert_der = NULL, *key_der = NULL;
    size_t ca_der_len = 0, cert_der_len = 0, key_der_len = 0;
    bool ca_der_alloc = false, cert_der_alloc = false, key_der_alloc = false;
    if (pem_to_der(ca, ca_len, &ca_der, &ca_der_len) == 0) { if (ca_der != ca) ca_der_alloc = true; }
    else { ca_der = ca; ca_der_len = ca_len; }
    if (pem_to_der(cert, cert_len, &cert_der, &cert_der_len) == 0) { if (cert_der != cert) cert_der_alloc = true; }
    else { cert_der = cert; cert_der_len = cert_len; }
    if (key && key_len>0) {
        if (pem_to_der(key, key_len, &key_der, &key_der_len) == 0) { if (key_der != key) key_der_alloc = true; }
        else { key_der = key; key_der_len = key_len; }
    }

    // compute short key-id from cert DER
    unsigned char fp[32]; char fp_hex[65]; char short_hex[17];
    if (mbedtls_sha256(cert_der, cert_der_len, fp, 0) == 0) {
        for (int i=0;i<32;i++) {
            sprintf(fp_hex + i*2, "%02x", fp[i]);
        }
        fp_hex[64] = '\0';
        memcpy(short_hex, fp_hex, 16);
        short_hex[16] = '\0';
        ESP_LOGI(TAG, "Computed cert key-id: %s (short %s)", fp_hex, short_hex);
    } else {
        ESP_LOGW(TAG, "failed to compute cert fingerprint");
    }

    // Confirm
    printf("This will ERASE and PROVISION the secure partition. Type YES to proceed: "); fflush(stdout);
    char answer[16]; if (!fgets(answer, sizeof(answer), stdin)) { ESP_LOGW(TAG, "no input"); goto cleanup_and_exit; }
    if (strncmp(answer, "YES", 3) != 0) { ESP_LOGI(TAG, "aborted by user"); goto cleanup_and_exit; }

    uint8_t *img = NULL; size_t img_len = 0;
    if (secure_part_create_image(ca, ca_len, cert, cert_len, key, key_len, &img, &img_len, 0) != ESP_OK) {
        ESP_LOGE(TAG, "failed to create TLV image"); goto cleanup_and_exit;
    }

    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "esp_secure_cert");
    if (!p) { ESP_LOGE(TAG, "secure partition not found"); free(img); goto cleanup_and_exit; }

    // transactional write: perform chunked backup to temp file to avoid large RAM usage
    const char *backup_path = "/spifss/secure_part.bak"; // adjust if your device uses different VFS
    const size_t CHUNK = 4096;
    uint8_t *chunk = malloc(CHUNK);
    if (!chunk) { ESP_LOGE(TAG, "alloc chunk failed"); free(img); goto cleanup_and_exit; }

    FILE *bf = fopen(backup_path, "wb");
    bool using_file_backup = false;
    uint8_t *ram_backup = NULL;
    if (bf) {
        using_file_backup = true;
        size_t remaining = p->size;
        size_t offset = 0;
        while (remaining > 0) {
            size_t to_read = remaining > CHUNK ? CHUNK : remaining;
            if (esp_partition_read(p, offset, chunk, to_read) != ESP_OK) {
                ESP_LOGW(TAG, "partition read failed at offset %u", (unsigned)offset);
                fclose(bf); remove(backup_path); free(chunk); free(img); goto cleanup_and_exit;
            }
            if (fwrite(chunk, 1, to_read, bf) != to_read) { ESP_LOGW(TAG, "backup file write failed"); fclose(bf); remove(backup_path); free(chunk); free(img); goto cleanup_and_exit; }
            remaining -= to_read; offset += to_read;
        }
        fflush(bf); fclose(bf);
    } else {
        // fallback: if partition reasonably small, keep RAM backup
        if (p->size <= 64*1024) {
            ram_backup = malloc(p->size);
            if (!ram_backup) { ESP_LOGE(TAG, "cannot create backup file and RAM fallback failed"); free(chunk); free(img); goto cleanup_and_exit; }
            if (esp_partition_read(p, 0, ram_backup, p->size) != ESP_OK) { ESP_LOGE(TAG, "partition read failed for RAM fallback"); free(ram_backup); free(chunk); free(img); goto cleanup_and_exit; }
        } else {
            ESP_LOGE(TAG, "cannot create backup file and partition too large for RAM fallback"); free(chunk); free(img); goto cleanup_and_exit;
        }
    }

    // Erase and write new image
    if (esp_partition_erase_range(p, 0, p->size) != ESP_OK) {
        ESP_LOGE(TAG, "erase failed");
        // attempt restore
        if (using_file_backup) {
            FILE *rf = fopen(backup_path, "rb");
            if (rf) {
                size_t remaining = p->size; size_t offset = 0;
                while (remaining > 0) {
                    size_t to_write = remaining > CHUNK ? CHUNK : remaining;
                    if (fread(chunk, 1, to_write, rf) != to_write) break;
                    esp_partition_write(p, offset, chunk, to_write);
                    remaining -= to_write; offset += to_write;
                }
                fclose(rf);
            }
            remove(backup_path);
        } else if (ram_backup) {
            esp_partition_write(p, 0, ram_backup, p->size);
            free(ram_backup);
        }
        free(chunk); free(img); goto cleanup_and_exit;
    }

    if (esp_partition_write(p, 0, img, img_len) != ESP_OK) {
        ESP_LOGE(TAG, "write failed, attempting restore");
        if (using_file_backup) {
            FILE *rf = fopen(backup_path, "rb");
            if (rf) {
                size_t remaining = p->size; size_t offset = 0;
                while (remaining > 0) {
                    size_t to_write = remaining > CHUNK ? CHUNK : remaining;
                    if (fread(chunk, 1, to_write, rf) != to_write) break;
                    esp_partition_write(p, offset, chunk, to_write);
                    remaining -= to_write; offset += to_write;
                }
                fclose(rf);
            }
            remove(backup_path);
        } else if (ram_backup) {
            esp_partition_write(p, 0, ram_backup, p->size);
            free(ram_backup);
        }
        free(chunk); free(img); goto cleanup_and_exit;
    }

    // verify header
    uint8_t check[5]; if (esp_partition_read(p,0,check,5) != ESP_OK) { ESP_LOGW(TAG, "verify read failed"); }
    if (!(check[0]=='S' && check[1]=='P' && check[2]=='C' && check[3]=='F')) {
        ESP_LOGE(TAG, "written image invalid, attempting restore");
        if (using_file_backup) {
            FILE *rf = fopen(backup_path, "rb");
            if (rf) {
                size_t remaining = p->size; size_t offset = 0;
                while (remaining > 0) {
                    size_t to_write = remaining > CHUNK ? CHUNK : remaining;
                    if (fread(chunk, 1, to_write, rf) != to_write) break;
                    esp_partition_write(p, offset, chunk, to_write);
                    remaining -= to_write; offset += to_write;
                }
                fclose(rf);
            }
            remove(backup_path);
        } else if (ram_backup) {
            esp_partition_write(p, 0, ram_backup, p->size);
            free(ram_backup);
        }
        free(chunk); free(img); goto cleanup_and_exit;
    }

    // success
    if (using_file_backup) remove(backup_path);
    if (ram_backup) free(ram_backup);
    free(chunk);
    ESP_LOGI(TAG, "provisioned secure partition (%u bytes)", (unsigned)img_len);
    free(img);

cleanup_and_exit:
    if (ca_der_alloc && ca_der) free(ca_der);
    if (cert_der_alloc && cert_der) free(cert_der);
    if (key_der_alloc && key_der) free(key_der);
    free(ca); free(cert); if (key) free(key);
    return 0;
}

esp_err_t provision_cli_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "prov_pem",
        .help = "Provision CA/device cert/key into secure TLV partition",
        .hint = "--ca <path|-> --cert <path|-> [--key <path|->]",
        .func = &cmd_prov_pem,
    };
    return esp_console_cmd_register(&cmd);
}

// register at startup if component is linked
__attribute__((constructor)) static void register_me(void) {
    provision_cli_register();
}
