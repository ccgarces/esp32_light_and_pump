set(TEST_NAME "schedule_test")

list(APPEND SRC_FILES "test_schedule.c")

register_test(${TEST_NAME} SRCS "${SRC_FILES}")
