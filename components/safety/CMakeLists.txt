idf_component_register(SRCS "safety.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_system esp_timer driver
                       PRIV_REQUIRES freertos main)
