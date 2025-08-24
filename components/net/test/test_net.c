#include "unity.h"
#include "net.h"

void setUp(void) {}
void tearDown(void) {}

void test_event_bits_defined(void)
{
    TEST_ASSERT_EQUAL_INT((1<<0), NET_BIT_WIFI_UP);
    TEST_ASSERT_EQUAL_INT((1<<2), NET_BIT_TIME_SYNCED);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_event_bits_defined);
    return UNITY_END();
}
