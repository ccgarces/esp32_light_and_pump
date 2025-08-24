idf_component_register(SRCS "ota.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_https_ota log mbedtls json app_update secure_part storage)

# Note: unit tests are not compiled into the firmware by default
