#pragma once

#include <stdbool.h>

void aht10_init(void);
bool aht10_read(float *temperature, float *humidity);
void aht10_hourly_task(void *arg);

// mqtt helper referenced by aht10.c
void mqtt_publish_sensor(int64_t timestamp, float temperature, float humidity);
