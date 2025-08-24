#include "unity.h"
#include "control.h"

// Unit tests for control component logic that do not depend on hardware.

void setUp(void) {}
void tearDown(void) {}

void test_calc_step_count_zero_ramp(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, control_calc_step_count(0, 50));
}

void test_calc_step_count_small_ramp(void)
{
    // ramp shorter than step_ms should still return at least 1
    TEST_ASSERT_EQUAL_UINT32(1, control_calc_step_count(10, 50));
}

void test_calc_step_count_multiple_steps(void)
{
    TEST_ASSERT_EQUAL_UINT32(20, control_calc_step_count(1000, 50));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_calc_step_count_zero_ramp);
    RUN_TEST(test_calc_step_count_small_ramp);
    RUN_TEST(test_calc_step_count_multiple_steps);
    return UNITY_END();
}
