#include "unity.h"
#include "ota.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_ota_check_version_policy_newer(void) {
    TEST_ASSERT_TRUE(ota_check_version_policy(1, 2, 0, false, false));
}

void test_ota_check_version_policy_rollback_block(void) {
    TEST_ASSERT_FALSE(ota_check_version_policy(5, 3, 0, false, false));
}

void test_ota_parse_manifest_min_required(void) {
    const char *m = "{\"version\": 4, \"min_required\": 3, \"url\":\"https://x\", \"digest\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\", \"signature\": \"AA==\"}";
    ota_manifest_t mf;
    TEST_ASSERT_EQUAL(ESP_OK, ota_parse_manifest(m, &mf));
    TEST_ASSERT_EQUAL_UINT32(4, mf.version);
    TEST_ASSERT_TRUE(mf.has_min_required);
    TEST_ASSERT_EQUAL_UINT32(3, mf.min_required);
}

void test_ota_compute_keyid_empty() {
    char full[65]; char shortid[17];
    TEST_ASSERT_NOT_EQUAL(0, ota_compute_keyid_from_der((const unsigned char*)"", 0, full, sizeof(full), shortid, sizeof(shortid), 8));
}
