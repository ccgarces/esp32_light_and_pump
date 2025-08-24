#include "unity.h"
#include "storage.h"
#include <string.h>
#include <stdint.h>

// This unit test exercises the BLE replay/window persistence logic by directly
// setting and reading the NVS keys used by the component. It simulates the
// monotonic counter progression and ensures the saved values are as expected.

void setUp(void) {
    storage_init();
}

void tearDown(void) {
}

static uint32_t read_saved_counter(void) {
    uint32_t v = 0; size_t got = 0;
    if (storage_load_config("ble_peer_counter", &v, sizeof(v), &got) == ESP_OK && got == sizeof(v)) return v;
    return 0;
}

static uint64_t read_saved_window(void) {
    uint64_t v = 0; size_t got = 0;
    if (storage_load_config("ble_peer_window", &v, sizeof(v), &got) == ESP_OK && got == sizeof(v)) return v;
    return 0;
}

void test_initial_state(void) {
    // Expect no saved counter initially
    uint32_t c = read_saved_counter();
    TEST_ASSERT_EQUAL_UINT32(0, c);
}

void test_persist_counter_and_window(void) {
    // simulate saving counter = 5 and window = 0x1
    uint32_t c = 5; uint64_t w = 1ULL;
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_save_config("ble_peer_counter", &c, sizeof(c)));
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_save_config("ble_peer_window", &w, sizeof(w)));
    // reload
    uint32_t rc = read_saved_counter();
    uint64_t rw = read_saved_window();
    TEST_ASSERT_EQUAL_UINT32(5, rc);
    TEST_ASSERT_EQUAL_UINT64(1ULL, rw);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_initial_state);
    RUN_TEST(test_persist_counter_and_window);
    return UNITY_END();
}
