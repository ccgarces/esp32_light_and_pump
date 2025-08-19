#pragma once

#include <stdint.h>

void pwm_ctrl_init(void);
void pwm_set_light_duty(uint8_t percent);
void pwm_set_pump_duty(uint8_t percent);
