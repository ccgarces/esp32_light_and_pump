idf_component_register(SRCS "telemetry.c"
                       INCLUDE_DIRS "include"
                       REQUIRES log esp_timer json aws_mqtt esp_wifi net
                       PRIV_REQUIRES main)
