idf_component_register(SRCS "provision_cli.c"
                       INCLUDE_DIRS "include"
                       REQUIRES console log nvs_flash secure_part mbedtls)
