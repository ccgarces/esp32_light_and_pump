idf_component_register(SRCS "aws_mqtt.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_event esp_netif nvs_flash mqtt esp-tls secure_part
                       PRIV_REQUIRES main)
