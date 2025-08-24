set(TEST_NAME "safety_test")
list(APPEND SRC_FILES "test_safety.c")
register_test(${TEST_NAME} SRCS "${SRC_FILES}")
