set(TEST_NAME "control_test")

list(APPEND SRC_FILES "test_control.c")

idf_component_get_property(IDF_TARGET IDF_TARGET)

# Use unity from IDF
register_test(${TEST_NAME} SRCS "${SRC_FILES}")
