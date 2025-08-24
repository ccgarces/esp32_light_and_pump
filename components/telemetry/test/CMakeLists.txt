set(TEST_NAME "telemetry_test")
list(APPEND SRC_FILES "test_telemetry.c")
register_test(${TEST_NAME} SRCS "${SRC_FILES}")
