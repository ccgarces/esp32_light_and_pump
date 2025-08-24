#include "unity.h"
#include "schedule.h"
#include <time.h>

void setUp(void) {}
void tearDown(void) {}

void test_default_schedule_next_events(void)
{
    schedule_t s;
    s.on_hour = 7; s.on_min = 0;
    s.off_hour = 21; s.off_min = 0;
    strcpy(s.tz, "UTC");

    time_t now = 0; // epoch (1970-01-01 00:00:00 UTC)
    time_t next_on, next_off;
    esp_err_t err = schedule_compute_next_events(now, &s, &next_on, &next_off);
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
    // next on should be at 07:00 UTC same day -> 7*3600
    TEST_ASSERT_EQUAL_INT64(7*3600, next_on);
    TEST_ASSERT_EQUAL_INT64(21*3600, next_off);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_schedule_next_events);
    return UNITY_END();
}
