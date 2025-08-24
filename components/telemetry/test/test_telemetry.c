#include "unity.h"
#include "telemetry.h"

void setUp(void) {}
void tearDown(void) {}

void test_telemetry_init(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, telemetry_init());
}

void test_telemetry_heartbeat(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, telemetry_publish_heartbeat());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_telemetry_init);
    RUN_TEST(test_telemetry_heartbeat);
    return UNITY_END();
}
