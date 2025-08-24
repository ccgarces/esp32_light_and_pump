set(COMPONENT_SRCS "net.c")
set(COMPONENT_INCLUDES "include")

idf_component_register(SRCS "${COMPONENT_SRCS}"
                    INCLUDE_DIRS "${COMPONENT_INCLUDES}"
                    PRIV_INCLUDE_DIRS "."
                    REQUIRES log freertos esp_event esp_netif esp_wifi nvs_flash esp_system
                    PRIV_REQUIRES main)
