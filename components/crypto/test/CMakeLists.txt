set(TEST_NAME "crypto_test")
list(APPEND SRC_FILES "test_crypto.c")
register_test(${TEST_NAME} SRCS "${SRC_FILES}")
