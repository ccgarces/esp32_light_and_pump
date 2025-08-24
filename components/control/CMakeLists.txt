set(COMPONENT_SRCS "control.c")
set(COMPONENT_INCLUDES "include")

idf_component_register(SRCS "${COMPONENT_SRCS}"
                    INCLUDE_DIRS "${COMPONENT_INCLUDES}"
                    PRIV_INCLUDE_DIRS "."
                    REQUIRES log freertos driver esp_system
                    PRIV_REQUIRES main)
