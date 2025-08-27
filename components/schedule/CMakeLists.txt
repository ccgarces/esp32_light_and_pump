idf_component_register(SRCS "schedule.c"
                       INCLUDE_DIRS "include"
                       REQUIRES storage esp_timer log esp_system control
                       PRIV_REQUIRES main)
