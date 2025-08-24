#include "unity.h"
#include "ota.h"

void setUp(void) {}
void tearDown(void) {}

void test_ota_init(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, ota_init());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ota_init);
    return UNITY_END();
}
