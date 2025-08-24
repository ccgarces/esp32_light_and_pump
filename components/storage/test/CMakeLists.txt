set(TEST_NAME "storage_test")

list(APPEND SRC_FILES "test_storage.c")

register_test(${TEST_NAME} SRCS "${SRC_FILES}")
