set(TEST_NAME "ble_test")
list(APPEND SRC_FILES "test_ble.c")
register_test(${TEST_NAME} SRCS "${SRC_FILES}")
