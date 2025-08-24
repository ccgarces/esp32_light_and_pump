idf_component_register(SRCS "app_main.c" "ipc.h"
                    INCLUDE_DIRS "."
                    REQUIRES freertos log safety storage control schedule net ble telemetry ota aws_mqtt)
