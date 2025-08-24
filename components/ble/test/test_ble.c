#include "unity.h"
#include "ble.h"

void setUp(void) {}
void tearDown(void) {}

void test_ble_api_exists(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, ble_init());
    TEST_ASSERT_EQUAL_INT(ESP_OK, ble_stop());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ble_api_exists);
    return UNITY_END();
}
