#pragma once

#include <stddef.h>
#include <esp_err.h>
#include <stdint.h>

// Types used in TLV partition
#define SPCF_TLV_TYPE_CA     1
#define SPCF_TLV_TYPE_CERT   2
#define SPCF_TLV_TYPE_KEY    3

// Read the esp_secure_cert partition and return pointers into newly allocated
// blobs. Caller must free the returned pointers (they are copies). Any of the
// out pointers may be NULL if not present.
esp_err_t secure_part_read(uint8_t **out_blob, size_t *out_len,
                           uint8_t **out_ca, size_t *out_ca_len,
                           uint8_t **out_cert, size_t *out_cert_len,
                           uint8_t **out_key, size_t *out_key_len);

// Free memory returned by secure_part_read
void secure_part_free(uint8_t *blob, uint8_t *ca, uint8_t *cert, uint8_t *key);

// Create TLV partition image with optional ca/cert/key. Returned buffer must be free()'d by caller.
esp_err_t secure_part_create_image(const uint8_t *ca, size_t ca_len,
                                  const uint8_t *cert, size_t cert_len,
                                  const uint8_t *key, size_t key_len,
                                  uint8_t **out_img, size_t *out_len, size_t pad_to_size);
