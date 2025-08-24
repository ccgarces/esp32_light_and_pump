#include "crypto.h"
#include <string.h>
#include "esp_log.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/sha256.h"
#include "mbedtls/gcm.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/md.h"

static const char *TAG = "crypto";

typedef struct {
    mbedtls_ecp_group grp;
    mbedtls_mpi d;            // private key
    mbedtls_ecp_point Q;      // public key
    mbedtls_mpi z;            // shared secret
    mbedtls_entropy_context *entropy;
    mbedtls_ctr_drbg_context *drbg;
} ectx_t;

int crypto_ecdh_generate_keypair(uint8_t *out_pub, size_t *pub_len, void **ctx)
{
    if (!out_pub || !pub_len || !ctx) return -1;
    int ret = 0;
    ectx_t *bundle = calloc(1, sizeof(*bundle));
    if (!bundle) return -1;
    mbedtls_ecp_group_init(&bundle->grp);
    mbedtls_mpi_init(&bundle->d);
    mbedtls_mpi_init(&bundle->z);
    mbedtls_ecp_point_init(&bundle->Q);
    mbedtls_entropy_context *entropy = calloc(1, sizeof(*entropy));
    mbedtls_ctr_drbg_context *drbg = calloc(1, sizeof(*drbg));
    if (!entropy || !drbg) { ret = -1; goto cleanup; }
    mbedtls_entropy_init(entropy);
    mbedtls_ctr_drbg_init(drbg);
    const char *pers = "ecdh_gen";
    if ((ret = mbedtls_ctr_drbg_seed(drbg, mbedtls_entropy_func, entropy, (const unsigned char*)pers, strlen(pers))) != 0) goto cleanup;
    ret = mbedtls_ecp_group_load(&bundle->grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret) goto cleanup;
    ret = mbedtls_ecdh_gen_public(&bundle->grp, &bundle->d, &bundle->Q, mbedtls_ctr_drbg_random, drbg);
    if (ret) goto cleanup;
    // export Q in uncompressed form
    size_t olen = 0;
    ret = mbedtls_ecp_point_write_binary(&bundle->grp, &bundle->Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, out_pub, *pub_len);
    if (ret) goto cleanup;
    *pub_len = olen;
    bundle->entropy = entropy; bundle->drbg = drbg;
    *ctx = bundle;
    return 0;
cleanup:
    if (bundle) {
        mbedtls_ecp_group_free(&bundle->grp);
        mbedtls_mpi_free(&bundle->d);
        mbedtls_mpi_free(&bundle->z);
        mbedtls_ecp_point_free(&bundle->Q);
        free(bundle);
    }
    if (drbg) { mbedtls_ctr_drbg_free(drbg); free(drbg); }
    if (entropy) { mbedtls_entropy_free(entropy); free(entropy); }
    return ret;
}

int crypto_ecdh_compute_shared(void *ctx, const uint8_t *peer_pub, size_t peer_pub_len, uint8_t *out_secret, size_t *out_len)
{
    if (!ctx || !peer_pub || !out_secret || !out_len) return -1;
    int ret = 0;
    ectx_t *bundle = (ectx_t*)ctx;
    mbedtls_ecp_point Qp;
    mbedtls_ecp_point_init(&Qp);
    ret = mbedtls_ecp_point_read_binary(&bundle->grp, &Qp, peer_pub, peer_pub_len);
    if (ret) goto cleanup;
    // compute shared secret
    ret = mbedtls_ecdh_compute_shared(&bundle->grp, &bundle->z, &Qp, &bundle->d, mbedtls_ctr_drbg_random, bundle->drbg);
    if (ret) goto cleanup;
    // export shared secret as big-endian MPI to buffer
    ret = mbedtls_mpi_write_binary(&bundle->z, out_secret, *out_len);
    if (ret) goto cleanup;
    // set out_len to actual size (32 for secp256r1)
    *out_len = 32;
cleanup:
    mbedtls_ecp_point_free(&Qp);
    return ret;
}

void crypto_ecdh_free(void *ctx)
{
    if (!ctx) return;
    ectx_t *bundle = (ectx_t*)ctx;
    mbedtls_ecp_group_free(&bundle->grp);
    mbedtls_mpi_free(&bundle->d);
    mbedtls_mpi_free(&bundle->z);
    mbedtls_ecp_point_free(&bundle->Q);
    if (bundle->drbg) { mbedtls_ctr_drbg_free(bundle->drbg); free(bundle->drbg); }
    if (bundle->entropy) { mbedtls_entropy_free(bundle->entropy); free(bundle->entropy); }
    free(bundle);
}

int crypto_derive_key(const uint8_t *secret, size_t secret_len, uint8_t *out_key, size_t key_len)
{
    if (!secret || !out_key) return -1;
    // Prefer HKDF for key derivation
    const uint8_t salt[] = {0};
    if (key_len > 32) return -1;
    return crypto_hkdf_sha256(salt, 0, secret, secret_len, (const uint8_t*)"BLE-KDF", 7, out_key, key_len);
}

// HKDF-SHA256 implementation (extract-then-expand)
int crypto_hkdf_sha256(const uint8_t *salt, size_t salt_len,
                       const uint8_t *ikm, size_t ikm_len,
                       const uint8_t *info, size_t info_len,
                       uint8_t *out_key, size_t key_len)
{
    uint8_t prk[32];
    // extract: PRK = HMAC-SHA256(salt, IKM)
    if (!ikm || !out_key) return -1;
    // use mbedtls_md_hmac
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return -1;
    if (mbedtls_md_hmac(md, salt, salt_len, ikm, ikm_len, prk) != 0) return -1;

    // expand
    size_t hash_len = 32;
    uint8_t T[32];
    uint8_t previous[32];
    size_t n = (key_len + hash_len - 1) / hash_len;
    uint8_t counter = 1;
    size_t out_pos = 0;
    size_t prev_len = 0;
    for (size_t i = 0; i < n; ++i) {
        mbedtls_md_context_t ctx;
        mbedtls_md_init(&ctx);
        if (mbedtls_md_setup(&ctx, md, 1) != 0) return -1;
        if (mbedtls_md_hmac_starts(&ctx, prk, hash_len) != 0) return -1;
        if (prev_len > 0) mbedtls_md_hmac_update(&ctx, previous, prev_len);
        if (info && info_len > 0) mbedtls_md_hmac_update(&ctx, info, info_len);
        mbedtls_md_hmac_update(&ctx, &counter, 1);
        mbedtls_md_hmac_finish(&ctx, T);
        mbedtls_md_free(&ctx);
        size_t take = (out_pos + hash_len > key_len) ? (key_len - out_pos) : hash_len;
        memcpy(out_key + out_pos, T, take);
        out_pos += take;
        memcpy(previous, T, hash_len);
        prev_len = hash_len;
        counter++;
    }
    return 0;
}

int crypto_aes_gcm_encrypt(const uint8_t *key, size_t key_len,
                           const uint8_t *iv, size_t iv_len,
                           const uint8_t *plaintext, size_t pt_len,
                           const uint8_t *aad, size_t aad_len,
                           uint8_t *out_ct, uint8_t *out_tag, size_t tag_len)
{
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    int ret = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, key_len*8);
    if (ret) goto cleanup;
    ret = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, pt_len, iv, iv_len, aad, aad_len, plaintext, out_ct, tag_len, out_tag);
cleanup:
    mbedtls_gcm_free(&ctx);
    return ret;
}

int crypto_aes_gcm_decrypt(const uint8_t *key, size_t key_len,
                           const uint8_t *iv, size_t iv_len,
                           const uint8_t *ciphertext, size_t ct_len,
                           const uint8_t *aad, size_t aad_len,
                           const uint8_t *tag, size_t tag_len,
                           uint8_t *out_pt)
{
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    int ret = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, key_len*8);
    if (ret) goto cleanup;
    ret = mbedtls_gcm_auth_decrypt(&ctx, ct_len, iv, iv_len, aad, aad_len, tag, tag_len, ciphertext, out_pt);
cleanup:
    mbedtls_gcm_free(&ctx);
    return ret;
}
