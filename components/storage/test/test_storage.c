#include "unity.h"
#include "storage.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_crc32_known_vector(void)
{
    const char *s = "123456789"; // standard CRC32 test vector
    uint32_t crc = storage_crc32(s, strlen(s));
    // expected CRC32 for "123456789" is 0xCBF43926
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc);
}

void test_backup_key_format(void)
{
    char out[32];
    char *p = storage_make_backup_key("config", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("config_bak", out);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc32_known_vector);
    RUN_TEST(test_backup_key_format);
    return UNITY_END();
}
