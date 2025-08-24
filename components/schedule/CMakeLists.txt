idf_component_register(SRCS "schedule.c"
                       INCLUDE_DIRS "include"
                       REQUIRES storage esp_timer log esp_system
                       PRIV_REQUIRES main)
