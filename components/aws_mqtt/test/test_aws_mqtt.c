#include "unity.h"
#include "aws_mqtt.h"

void setUp(void) {}
void tearDown(void) {}

void test_aws_init(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, aws_mqtt_init());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_aws_init);
    return UNITY_END();
}
