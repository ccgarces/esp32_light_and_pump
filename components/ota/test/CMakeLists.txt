set(TEST_NAME "ota_test")
list(APPEND SRC_FILES "test_manifest.c")
register_test(${TEST_NAME} SRCS "${SRC_FILES}")
