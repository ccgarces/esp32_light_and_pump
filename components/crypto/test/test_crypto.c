#include "unity.h"
#include "crypto.h"

void setUp(void) {}
void tearDown(void) {}

void test_ecdh_and_kdf(void)
{
    uint8_t pub[65]; size_t pub_len = sizeof(pub);
    void *ctx = NULL;
    TEST_ASSERT_EQUAL_INT(0, crypto_ecdh_generate_keypair(pub, &pub_len, &ctx));
    uint8_t shared[32]; size_t shared_len = sizeof(shared);
    // compute shared with self pub to ensure compute function runs (not meaningful)
    TEST_ASSERT_EQUAL_INT(0, crypto_ecdh_compute_shared(ctx, pub, pub_len, shared, &shared_len));
    uint8_t key[32];
    TEST_ASSERT_EQUAL_INT(0, crypto_derive_key(shared, shared_len, key, sizeof(key)));
    crypto_ecdh_free(ctx);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ecdh_and_kdf);
    return UNITY_END();
}
