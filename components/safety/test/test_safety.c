#include "unity.h"
#include "safety.h"

void setUp(void) {}
void tearDown(void) {}

void test_safety_init(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, safety_init());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_safety_init);
    return UNITY_END();
}
