#pragma once

#include <stdint.h>
#include <stddef.h>

// Minimal crypto helpers using mbedtls for ECDH (secp256r1) and AES-GCM AEAD.

// Generate an ephemeral ECDH keypair, returns public key in uncompressed format (65 bytes) in out_pub, pub_len set to 65.
int crypto_ecdh_generate_keypair(uint8_t *out_pub, size_t *pub_len, void **ctx);

// Compute shared secret given local context and peer public key (uncompressed). Shared secret is written to out_secret (32 bytes) and out_len set.
int crypto_ecdh_compute_shared(void *ctx, const uint8_t *peer_pub, size_t peer_pub_len, uint8_t *out_secret, size_t *out_len);

// Free ECDH context
void crypto_ecdh_free(void *ctx);

// Derive AES-GCM key from shared secret using a simple KDF (SHA256). out_key should be 32 bytes.
int crypto_derive_key(const uint8_t *secret, size_t secret_len, uint8_t *out_key, size_t key_len);

// AES-GCM encrypt/decrypt helpers. iv_len must be 12, tag_len typically 16.
int crypto_aes_gcm_encrypt(const uint8_t *key, size_t key_len,
                           const uint8_t *iv, size_t iv_len,
                           const uint8_t *plaintext, size_t pt_len,
                           const uint8_t *aad, size_t aad_len,
                           uint8_t *out_ct, uint8_t *out_tag, size_t tag_len);

int crypto_aes_gcm_decrypt(const uint8_t *key, size_t key_len,
                           const uint8_t *iv, size_t iv_len,
                           const uint8_t *ciphertext, size_t ct_len,
                           const uint8_t *aad, size_t aad_len,
                           const uint8_t *tag, size_t tag_len,
                           uint8_t *out_pt);

// HKDF-SHA256: derive key material of key_len bytes from ikm, salt, and info
int crypto_hkdf_sha256(const uint8_t *salt, size_t salt_len,
                       const uint8_t *ikm, size_t ikm_len,
                       const uint8_t *info, size_t info_len,
                       uint8_t *out_key, size_t key_len);
